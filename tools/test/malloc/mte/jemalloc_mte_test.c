/* SPDX-License-Identifier: BSD-2-Clause */
/* Run in the FreeBSD AArch64 guest with the prototype libc; see README.md. */
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <malloc_np.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__aarch64__) && !defined(__CHERI_PURE_CAPABILITY__)
#define CHECK(condition) do { \
	if (!(condition)) \
		errx(1, "%s:%d: %s", __func__, __LINE__, #condition); \
} while (0)

static bool tagging;
static volatile sig_atomic_t fault_armed;
static volatile sig_atomic_t fault_report_fd;

struct fault_report {
	int sig;
	int code;
	int armed;
};

enum fault_exit {
	FAULT_EXPECTED = 0,
	FAULT_UNEXPECTED = 2,
	FAULT_MISSING = 3,
	FAULT_REPORT_FAILED = 4
};

static uintptr_t
address(const void *ptr)
{
	return ((uintptr_t)ptr & ~(UINT64_C(0xff) << 56));
}

static unsigned
tag(const void *ptr)
{
	return (((uintptr_t)ptr >> 56) & 0xf);
}

static bool
read_bool(const char *name)
{
	bool value;
	size_t len = sizeof(value);
	CHECK(mallctl(name, &value, &len, NULL, 0) == 0);
	CHECK(len == sizeof(value));
	return (value);
}

static void
check_allocation(void *ptr, size_t requested)
{
	CHECK(ptr != NULL);
	CHECK((address(ptr) & 15) == 0);
	CHECK(tagging ? tag(ptr) != 0 : tag(ptr) == 0);
	size_t usable = malloc_usable_size(ptr);
	CHECK(usable >= requested && usable >= 16 && (usable & 15) == 0);
	CHECK(sallocx(ptr, 0) == usable);
	CHECK(nallocx(requested == 0 ? 1 : requested, 0) == usable);
	/* Touch every usable byte, including the final tag granule. */
	memset(ptr, 0xa5, usable);
	for (size_t i = 0; i < usable; i++)
		CHECK(((volatile unsigned char *)ptr)[i] == 0xa5);
}

static void
ordinary_allocations(void)
{
	static const size_t sizes[] = {0, 1, 8, 15, 16, 17, 31, 64, 1024,
	    16384, 65536};
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		void *ptr = malloc(sizes[i]);
		check_allocation(ptr, sizes[i]);
		free(ptr);
	}
	/* Exercise cache hits repeatedly, as well as tiny adjacent objects. */
	for (unsigned i = 0; i < 512; i++) {
		void *a = malloc(1), *b = malloc(8);
		check_allocation(a, 1);
		check_allocation(b, 8);
		CHECK((address(a) >> 4) != (address(b) >> 4));
		free(a);
		CHECK(*(volatile unsigned char *)b == 0xa5);
		free(b);
	}
	unsigned char *zero = calloc(3, 17);
	CHECK(zero != NULL);
	for (size_t i = 0; i < 51; i++)
		CHECK(zero[i] == 0);
	check_allocation(zero, 51);
	free(zero);
	volatile size_t huge = SIZE_MAX;
	CHECK(calloc(huge, 2) == NULL);
	void *aligned = NULL;
	CHECK(posix_memalign(&aligned, 4096, 65) == 0);
	CHECK((address(aligned) & 4095) == 0);
	memset(aligned, 0, 65);
	CHECK(tagging ? tag(aligned) != 0 : tag(aligned) == 0);
	free(aligned);
	aligned = aligned_alloc(64, 128);
	CHECK(aligned != NULL && (address(aligned) & 63) == 0);
	memset(aligned, 1, 128);
	free(aligned);
	free(NULL);
}

static void
reallocations(void)
{
	unsigned char *ptr = realloc(NULL, 64);
	check_allocation(ptr, 64);
	volatile size_t huge = SIZE_MAX;
	errno = 0;
	CHECK(realloc(ptr, huge) == NULL);
	CHECK(errno == ENOMEM);
	for (size_t i = 0; i < 64; i++)
		CHECK(ptr[i] == 0xa5);
	unsigned char *next = realloc(ptr, 65536);
	CHECK(next != NULL);
	for (size_t i = 0; i < 64; i++)
		CHECK(next[i] == 0xa5);
	check_allocation(next, 65536);
	ptr = realloc(next, 8);
	CHECK(ptr != NULL);
	for (size_t i = 0; i < 8; i++)
		CHECK(ptr[i] == 0xa5);
	free(ptr);

	ptr = mallocx(64, MALLOCX_ZERO);
	CHECK(ptr != NULL);
	memset(ptr, 0x42, 64);
	next = rallocx(ptr, 96, MALLOCX_ZERO);
	CHECK(next != NULL);
	for (size_t i = 0; i < 64; i++)
		CHECK(next[i] == 0x42);
	for (size_t i = 64; i < 96; i++)
		CHECK(next[i] == 0);
	if (tagging) {
		size_t usable = sallocx(next, 0);
		CHECK(xallocx(next, 65536, 0, 0) == usable);
		CHECK(next[0] == 0x42);
	}
	sdallocx(next, 96, 0);
	ptr = mallocx(17, MALLOCX_TCACHE_NONE);
	check_allocation(ptr, 17);
	dallocx(ptr, MALLOCX_TCACHE_NONE);
}

static void *
thread_free(void *ptr)
{
	/* The thread must be able to access a handed-off tagged allocation. */
	CHECK(*(volatile unsigned char *)ptr == 0xa5);
	free(ptr);
	ptr = malloc(64);
	check_allocation(ptr, 64);
	return (ptr);
}

static void
thread_and_fork(void)
{
	pthread_t thread;
	void *ptr = malloc(64), *result;
	check_allocation(ptr, 64);
	CHECK(pthread_create(&thread, NULL, thread_free, ptr) == 0);
	CHECK(pthread_join(thread, &result) == 0);
	check_allocation(result, 64);
	pid_t child = fork();
	CHECK(child != -1);
	if (child == 0) {
		/* No child allocation before freeing: exercise ATA0 restoration. */
		free(result);
		_exit(0);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	check_allocation(result, 64);
	free(result);
}

static void
fault_handler(int sig, siginfo_t *info, void *context)
{
	struct fault_report report;
	ssize_t written;

	(void)context;
	report.sig = sig;
	report.code = info->si_code;
	report.armed = fault_armed;
	/* Only async-signal-safe operations here; the parent formats diagnostics. */
	do {
		written = write(fault_report_fd, &report, sizeof(report));
	} while (written == -1 && errno == EINTR);
	if (written != (ssize_t)sizeof(report))
		_exit(FAULT_REPORT_FAILED);
	_exit(report.armed && sig == SIGSEGV && report.code == SEGV_MTESERR ?
	    FAULT_EXPECTED : FAULT_UNEXPECTED);
}

static const char *
fault_code_name(int sig, int code)
{
	if (sig == SIGSEGV) {
		switch (code) {
		case SEGV_MAPERR:
			return ("SEGV_MAPERR");
		case SEGV_ACCERR:
			return ("SEGV_ACCERR");
		case SEGV_MTEAERR:
			return ("SEGV_MTEAERR");
		case SEGV_MTESERR:
			return ("SEGV_MTESERR");
		}
	}
	return ("see signal(3)");
}

static bool
expect_fault(bool after_free)
{
	const char *name = after_free ? "use-after-free" : "wrong-tag";
	int fds[2];

	if (pipe(fds) == -1)
		err(1, "%s: pipe", name);
	/* A child setup failure uses errx(), which flushes inherited stdio. */
	if (fflush(stdout) == EOF)
		err(1, "%s: fflush", name);
	pid_t child = fork();
	if (child == -1)
		err(1, "%s: fork", name);
	if (child == 0) {
		static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE,
		    SIGABRT};
		close(fds[0]);
		fault_report_fd = fds[1];
		fault_armed = 0;
		struct sigaction action = {0};
		action.sa_sigaction = fault_handler;
		action.sa_flags = SA_SIGINFO;
		sigfillset(&action.sa_mask);
		for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
			if (sigaction(signals[i], &action, NULL) == -1)
				err(1, "%s: sigaction(%d)", name, signals[i]);
		}
		void *ptr = malloc(64);
		check_allocation(ptr, 64);
		uintptr_t stale = (uintptr_t)ptr;
		if (after_free)
			free(ptr);
		else
			stale ^= UINT64_C(1) << 56;
		/* No allocations between free and the access: no tag-reuse lottery. */
		fault_armed = 1;
		__asm__ volatile("ldrb wzr, [%0]" : : "r" (stale) : "memory");
		fault_armed = 0;
		_exit(FAULT_MISSING);
	}
	close(fds[1]);
	/* The sole report fits in the pipe, so waiting cannot block its writer. */
	int status;
	pid_t waited;
	do {
		waited = waitpid(child, &status, 0);
	} while (waited == -1 && errno == EINTR);
	if (waited == -1)
		err(1, "%s: waitpid", name);
	struct fault_report report;
	size_t received = 0;
	while (received < sizeof(report)) {
		ssize_t n = read(fds[0], (char *)&report + received,
		    sizeof(report) - received);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			err(1, "%s: read fault report", name);
		}
		if (n == 0)
			break;
		received += n;
	}
	close(fds[0]);
	if (WIFEXITED(status) && WEXITSTATUS(status) == FAULT_EXPECTED &&
	    received == sizeof(report) && report.armed && report.sig == SIGSEGV &&
	    report.code == SEGV_MTESERR) {
		printf("PASS: %s: SIGSEGV, si_code=SEGV_MTESERR (%d)\n",
		    name, report.code);
		return (true);
	}
	fprintf(stderr, "FAIL: %s: expected SIGSEGV, si_code=SEGV_MTESERR (%d) "
	    "during deliberate access; ", name, SEGV_MTESERR);
	if (received == sizeof(report)) {
		fprintf(stderr, "got signal=%d (%s), si_code=%d (%s), %s; ",
		    report.sig, strsignal(report.sig), report.code,
		    fault_code_name(report.sig, report.code),
		    report.armed ? "during deliberate access" :
		    "outside deliberate access");
	} else {
		fprintf(stderr, "signal details unavailable (report %zu/%zu bytes); ",
		    received, sizeof(report));
	}
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "child terminated by signal=%d (%s)\n",
		    WTERMSIG(status), strsignal(WTERMSIG(status)));
	} else if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);
		fprintf(stderr, "child exit=%d%s\n", code,
		    code == FAULT_MISSING ? " (access completed without fault)" :
		    code == FAULT_REPORT_FAILED ? " (fault report write failed)" :
		    code == 1 ? " (child setup/check failed)" : "");
	} else {
		fprintf(stderr, "unexpected wait status=%#x\n", status);
	}
	return (false);
}

int
main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "on") != 0 &&
	    strcmp(argv[1], "off") != 0))
		errx(1, "usage: %s on|off (set MALLOC_CONF before exec)", argv[0]);
	CHECK(read_bool("config.mte"));
	tagging = read_bool("opt.mte");
	CHECK(tagging == (strcmp(argv[1], "on") == 0));
	struct rlimit no_core = {0, 0};
	CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0);
	ordinary_allocations();
	reallocations();
	thread_and_fork();
	if (tagging) {
		bool wrong_tag_ok = expect_fault(false);
		bool after_free_ok = expect_fault(true);
		if (!wrong_tag_ok || !after_free_ok)
			return (1);
	}
	printf("PASS: jemalloc MTE %s\n", tagging ? "on" : "off");
	return (0);
}
#else
int
main(void)
{
	fprintf(stderr, "This test requires FreeBSD AArch64 without capabilities\n");
	return (77);
}
#endif
