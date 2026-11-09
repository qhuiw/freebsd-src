/* SPDX-License-Identifier: BSD-2-Clause */
/* Run in the FreeBSD AArch64 guest with the prototype libc; see README.md. */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/thr.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc_np.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#if defined(__aarch64__) && !defined(__CHERI_PURE_CAPABILITY__)
#include <machine/sysarch.h>

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
		/* Exercise freeing an inherited allocation before allocating again. */
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
check_bytes(const void *ptr, size_t size, unsigned char value)
{
	const volatile unsigned char *bytes = ptr;

	for (size_t i = 0; i < size; i++)
		CHECK(bytes[i] == value);
}

static void
wait_success(pid_t child)
{
	pid_t waited;
	int status;

	do {
		waited = waitpid(child, &status, 0);
	} while (waited == -1 && errno == EINTR);
	CHECK(waited == child);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		errx(1, "fork stress: child %d failed, wait status=%#x",
		    (int)child, status);
}

#define STRESS_THREADS 4
#define STRESS_SLOTS 16
#define STRESS_ROUNDS 16
#define STRESS_WAVES 4

static const size_t stress_sizes[] = {
	1, 15, 16, 17, 63, 64, 65, 255, 256, 257, 1024, 4096, 16384, 65536
};

struct stress_slot {
	void *ptr;
	size_t size;
	size_t usable;
	unsigned char pattern;
};

struct stress_group {
	pthread_barrier_t barrier;
	struct stress_slot slots[STRESS_THREADS][STRESS_SLOTS];
};

struct stress_worker {
	struct stress_group *group;
	unsigned id;
};

static void
stress_barrier(pthread_barrier_t *barrier)
{
	int error = pthread_barrier_wait(barrier);
	CHECK(error == 0 || error == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *
stress_worker(void *arg)
{
	struct stress_worker *worker = arg;
	struct stress_group *group = worker->group;
	unsigned id = worker->id;

	for (unsigned round = 0; round < STRESS_ROUNDS; round++) {
		for (unsigned i = 0; i < STRESS_SLOTS; i++) {
			struct stress_slot *slot = &group->slots[id][i];
			slot->size = stress_sizes[(round + id + i) %
			    (sizeof(stress_sizes) / sizeof(stress_sizes[0]))];
			slot->ptr = malloc(slot->size);
			check_allocation(slot->ptr, slot->size);
			slot->usable = malloc_usable_size(slot->ptr);
			slot->pattern = (unsigned char)(1 + round + id * 17 + i);
			memset(slot->ptr, slot->pattern, slot->usable);
		}
		stress_barrier(&group->barrier);
		/* Each published batch is consumed by exactly one other thread. */
		for (unsigned i = 0; i < STRESS_SLOTS; i++) {
			struct stress_slot *slot =
			    &group->slots[(id + 1) % STRESS_THREADS][i];
			check_bytes(slot->ptr, slot->usable, slot->pattern);
			if ((i & 1) != 0) {
				size_t grown = slot->size * 2 + 17;
				void *next = realloc(slot->ptr, grown);
				CHECK(next != NULL);
				check_bytes(next, slot->size, slot->pattern);
				check_allocation(next, grown);
				slot->ptr = next;
				slot->size = grown;
			}
			switch (i % 3) {
			case 0:
				free(slot->ptr);
				break;
			case 1:
				dallocx(slot->ptr, 0);
				break;
			default:
				sdallocx(slot->ptr, slot->size, 0);
				break;
			}
		}
		if (read_bool("thread.tcache.enabled"))
			CHECK(mallctl("thread.tcache.flush", NULL, NULL,
			    NULL, 0) == 0);
		stress_barrier(&group->barrier);
	}
	return (NULL);
}

static void
thread_stress(void)
{
	struct stress_group group;
	struct stress_worker workers[STRESS_THREADS];
	pthread_t threads[STRESS_THREADS];

	for (unsigned wave = 0; wave < STRESS_WAVES; wave++) {
		CHECK(pthread_barrier_init(&group.barrier, NULL,
		    STRESS_THREADS) == 0);
		for (unsigned i = 0; i < STRESS_THREADS; i++) {
			workers[i].group = &group;
			workers[i].id = i;
			CHECK(pthread_create(&threads[i], NULL, stress_worker,
			    &workers[i]) == 0);
		}
		for (unsigned i = 0; i < STRESS_THREADS; i++)
			CHECK(pthread_join(threads[i], NULL) == 0);
		CHECK(pthread_barrier_destroy(&group.barrier) == 0);
	}
}

static void
explicit_cache_stress(void)
{
	unsigned cache;
	size_t len = sizeof(cache);
	void *ptrs[STRESS_SLOTS];

	CHECK(mallctl("tcache.create", &cache, &len, NULL, 0) == 0);
	CHECK(len == sizeof(cache));
	int flags = MALLOCX_TCACHE(cache);
	for (unsigned round = 0; round < STRESS_ROUNDS; round++) {
		for (unsigned i = 0; i < STRESS_SLOTS; i++) {
			size_t size = (i + 1) * 16;
			ptrs[i] = mallocx(size, flags);
			check_allocation(ptrs[i], size);
		}
		for (unsigned i = 0; i < STRESS_SLOTS; i++) {
			check_bytes(ptrs[i], malloc_usable_size(ptrs[i]), 0xa5);
			dallocx(ptrs[i], flags);
		}
		CHECK(mallctl("tcache.flush", NULL, NULL,
		    &cache, sizeof(cache)) == 0);
	}
	CHECK(mallctl("tcache.destroy", NULL, NULL, &cache, sizeof(cache)) == 0);
}

static void
fork_stress(void)
{
	struct stress_slot slots[STRESS_SLOTS];

	/* Fork only after all stress workers have joined. */
	for (unsigned round = 0; round < 8; round++) {
		for (unsigned i = 0; i < STRESS_SLOTS; i++) {
			slots[i].size = stress_sizes[(round + i) %
			    (sizeof(stress_sizes) / sizeof(stress_sizes[0]))];
			slots[i].ptr = malloc(slots[i].size);
			check_allocation(slots[i].ptr, slots[i].size);
			slots[i].usable = malloc_usable_size(slots[i].ptr);
		}
		pid_t child = fork();
		CHECK(child != -1);
		if (child == 0) {
			for (unsigned i = 0; i < STRESS_SLOTS; i++) {
				check_bytes(slots[i].ptr, slots[i].usable, 0xa5);
				/* Free first: exercise tag-control restoration. */
				free(slots[i].ptr);
				void *next = malloc(slots[i].size + 32);
				check_allocation(next, slots[i].size + 32);
				memset(next, 0x24, malloc_usable_size(next));
				free(next);
			}
			_exit(0);
		}
		wait_success(child);
		for (unsigned i = 0; i < STRESS_SLOTS; i++) {
			/* Child writes and tag invalidation must be private. */
			check_bytes(slots[i].ptr, slots[i].usable, 0xa5);
			free(slots[i].ptr);
		}
	}
}

static void
arena_command(unsigned arena, const char *command, void *value, size_t size)
{
	char name[80];
	int n = snprintf(name, sizeof(name), "arena.%u.%s", arena, command);
	CHECK(n > 0 && (size_t)n < sizeof(name));
	int error = mallctl(name, NULL, NULL, value, size);
	if (error != 0)
		errx(1, "%s: %s", name, strerror(error));
}

static void
arena_stat(unsigned arena, const char *stat, void *value, size_t size)
{
	char name[80];
	int n = snprintf(name, sizeof(name), "stats.arenas.%u.%s", arena, stat);
	CHECK(n > 0 && (size_t)n < sizeof(name));
	size_t len = size;
	int error = mallctl(name, value, &len, NULL, 0);
	if (error != 0)
		errx(1, "%s: %s", name, strerror(error));
	CHECK(len == size);
}

static void
refresh_stats(void)
{
	uint64_t epoch = 1;
	CHECK(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)) == 0);
}

static void
reclamation_stress(void)
{
	enum { BLOCKS = 8, BLOCK_SIZE = 1024 * 1024 };
	void *ptrs[BLOCKS];
	unsigned arena;
	size_t len = sizeof(arena);
	ssize_t disabled = -1, immediate = 0;

	CHECK(read_bool("config.stats"));
	CHECK(mallctl("arenas.create", &arena, &len, NULL, 0) == 0);
	CHECK(len == sizeof(arena));
	arena_command(arena, "dirty_decay_ms", &disabled, sizeof(disabled));
	arena_command(arena, "muzzy_decay_ms", &immediate, sizeof(immediate));
	int flags = MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE;
	for (unsigned round = 0; round < 4; round++) {
		for (unsigned i = 0; i < BLOCKS; i++) {
			ptrs[i] = mallocx(BLOCK_SIZE, flags | MALLOCX_ZERO);
			CHECK(ptrs[i] != NULL);
			check_bytes(ptrs[i], BLOCK_SIZE, 0);
			check_allocation(ptrs[i], BLOCK_SIZE);
		}
		for (unsigned i = 0; i < BLOCKS; i++)
			dallocx(ptrs[i], flags);
		refresh_stats();
		size_t dirty, after_dirty, after_muzzy;
		uint64_t before_purged, after_purged;
		arena_stat(arena, "pdirty", &dirty, sizeof(dirty));
		arena_stat(arena, "dirty_purged", &before_purged,
		    sizeof(before_purged));
		CHECK(dirty > 0);
		arena_command(arena, "purge", NULL, 0);
		refresh_stats();
		arena_stat(arena, "pdirty", &after_dirty, sizeof(after_dirty));
		arena_stat(arena, "pmuzzy", &after_muzzy, sizeof(after_muzzy));
		arena_stat(arena, "dirty_purged", &after_purged,
		    sizeof(after_purged));
		CHECK(after_dirty == 0 && after_muzzy == 0);
		CHECK(after_purged > before_purged);
		printf("INFO: purge round %u: dirty %zu -> %zu, "
		    "purged %" PRIu64 " pages\n", round + 1, dirty,
		    after_dirty, after_purged - before_purged);
	}
	/* Verify allocation/zeroing once more after the final purge. */
	void *ptr = mallocx(BLOCK_SIZE, flags | MALLOCX_ZERO);
	CHECK(ptr != NULL);
	check_bytes(ptr, BLOCK_SIZE, 0);
	check_allocation(ptr, BLOCK_SIZE);
	dallocx(ptr, flags);
	arena_command(arena, "purge", NULL, 0);
	/* Arena destruction is intentionally unsupported with MTE enabled. */
}

static void
run_check(const char *name, void (*check)(void))
{
	printf("RUN: %s\n", name);
	CHECK(fflush(stdout) == 0);
	check();
	printf("PASS: %s\n", name);
	CHECK(fflush(stdout) == 0);
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

/*
 * Bypass libthr startup, atfork callbacks and signal trampolines: any of
 * those could allocate and hide broken kernel MTE inheritance.  The raw
 * thread deliberately has no TLS.  Its entry point and signal handler use
 * only inline instructions and direct syscalls, including error reporting.
 */
static __inline __attribute__((always_inline)) long
inherit_syscall(long number, long a0, long a1, long a2)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x8 __asm__("x8") = number;

	/* FreeBSD reports syscall errors in x0 with carry set. */
	__asm__ volatile("svc #0\n\tcneg %0, %0, cs"
	    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x8)
	    : : "cc", "memory");
	return (x0);
}

enum inherit_phase {
	INHERIT_SETUP,
	INHERIT_TAG,
	INHERIT_MATCHING,
	INHERIT_WRONG
};

struct inherit_report {
	int phase;
	int sig;
	int code;
	int error;
};

static volatile sig_atomic_t inherit_phase;
static volatile sig_atomic_t inherit_fd;
static volatile sig_atomic_t inherit_fault_expected;
static bool inherit_tags_enabled;

static __attribute__((noreturn, no_stack_protector)) void
inherit_finish(int sig, int code, int error)
{
	struct inherit_report report;
	long written;

	report.phase = inherit_phase;
	report.sig = sig;
	report.code = code;
	report.error = error;
	do {
		written = inherit_syscall(SYS_write, inherit_fd,
		    (long)&report, sizeof(report));
	} while (written == -EINTR);
	(void)inherit_syscall(SYS_exit,
	    written != (long)sizeof(report) ? FAULT_REPORT_FAILED :
	    error != 0 ? FAULT_UNEXPECTED : FAULT_EXPECTED, 0, 0);
	__builtin_trap();
}

static __attribute__((no_stack_protector)) void
inherit_signal(int sig, siginfo_t *info, void *context)
{
	(void)context;
	inherit_finish(sig, info->si_code,
	    inherit_fault_expected && inherit_phase == INHERIT_WRONG &&
	    sig == SIGSEGV && info->si_code == SEGV_MTESERR ? 0 : EFAULT);
}

static __attribute__((noreturn, no_stack_protector)) void
inherit_access(void *ptr)
{
	uintptr_t loaded, wrong;
	unsigned value;

	/* LDG proves allocation-tag access survived, independently of TCF0. */
	inherit_phase = INHERIT_TAG;
	if (inherit_tags_enabled) {
		loaded = (uintptr_t)ptr & ~(UINT64_C(0xff) << 56);
		__asm__ volatile(".arch_extension memtag\n\t"
		    "ldg %0, [%1]" : "+r" (loaded) : "r" (ptr) : "memory");
		if (((loaded ^ (uintptr_t)ptr) & (UINT64_C(0xf) << 56)) != 0)
			inherit_finish(0, 0, EINVAL);
	}
	inherit_phase = INHERIT_MATCHING;
	__asm__ volatile("ldrb %w0, [%1]"
	    : "=r" (value) : "r" (ptr) : "memory");
	if (value != 0xa5)
		inherit_finish(0, 0, EIO);
	wrong = (uintptr_t)ptr ^ (UINT64_C(1) << 56);
	inherit_phase = INHERIT_WRONG;
	__asm__ volatile("ldrb wzr, [%0]" : : "r" (wrong) : "memory");
	inherit_finish(0, 0, inherit_fault_expected ? ENOMSG : 0);
}

static bool
inherit_case(bool thread, uint64_t flags, const char *mode)
{
	static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
	static const char *const phases[] = {"setup", "allocation-tag load",
	    "matching-tag read", "wrong-tag read"};
	const bool expect_fault = (flags & MTE_CTRL_ENABLE) != 0 &&
	    (flags & MTE_CTRL_TCF_MASK) == MTE_CTRL_TCF_SYNC;
	struct inherit_report report;
	int fds[2], status;
	pid_t child, waited;
	size_t received = 0;

	CHECK(pipe(fds) == 0);
	printf("RUN: inherit-%s-%s\n", thread ? "thread" : "fork", mode);
	CHECK(fflush(stdout) == 0);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		struct sigaction action = {0};
		struct thr_param param = {0};
		void *ptr;
		long result;

		close(fds[0]);
		/* fork does not inherit alarm timers; bound each isolated probe. */
		alarm(15);
		inherit_fd = fds[1];
		inherit_phase = INHERIT_SETUP;
		inherit_fault_expected = expect_fault;
		inherit_tags_enabled = (flags & MTE_CTRL_ENABLE) != 0;
		action.sa_sigaction = inherit_signal;
		action.sa_flags = SA_SIGINFO;
		sigfillset(&action.sa_mask);
		for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
			result = inherit_syscall(SYS_sigaction, signals[i],
			    (long)&action, 0);
			if (result < 0)
				inherit_finish(0, 0, -result);
		}
		ptr = malloc(64);
		check_allocation(ptr, 64);
		if (thread) {
			param.stack_size = 64 * 1024;
			param.stack_base = mmap(NULL, param.stack_size,
			    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
			CHECK(param.stack_base != MAP_FAILED);
			param.start_func = inherit_access;
			param.arg = ptr;
			/* tls_base stays NULL; the raw entry never uses libc/TLS. */
		}
		/* Last control write: no allocator/library calls in the probe. */
		result = inherit_syscall(SYS_sysarch, ARM64_MTE_CTRL,
		    (long)&flags, 0);
		if (result < 0)
			inherit_finish(0, 0, -result);
		if (thread) {
			result = inherit_syscall(SYS_thr_new, (long)&param,
			    sizeof(param), 0);
			if (result < 0)
				inherit_finish(0, 0, -result);
			/* The probe reports and exits the entire isolated process. */
			(void)inherit_syscall(SYS_thr_exit, 0, 0, 0);
			__builtin_trap();
		}
		result = inherit_syscall(SYS_fork, 0, 0, 0);
		if (result < 0)
			inherit_finish(0, 0, -result);
		if (result == 0)
			inherit_access(ptr);
		/* Only this coordinator may use libc after the tested fork. */
		do {
			waited = waitpid(result, &status, 0);
		} while (waited == -1 && errno == EINTR);
		_exit(waited == result && WIFEXITED(status) ?
		    WEXITSTATUS(status) : FAULT_UNEXPECTED);
	}
	close(fds[1]);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited == -1 && errno == EINTR);
	CHECK(waited == child);
	while (received < sizeof(report)) {
		ssize_t n = read(fds[0], (char *)&report + received,
		    sizeof(report) - received);
		if (n == -1 && errno == EINTR)
			continue;
		CHECK(n >= 0);
		if (n == 0)
			break;
		received += n;
	}
	close(fds[0]);
	if (WIFEXITED(status) && WEXITSTATUS(status) == FAULT_EXPECTED &&
	    received == sizeof(report) && report.phase == INHERIT_WRONG &&
	    report.error == 0 && (expect_fault ?
	    report.sig == SIGSEGV && report.code == SEGV_MTESERR :
	    report.sig == 0 && report.code == 0)) {
		printf("PASS: inherit-%s-%s: %s\n", thread ? "thread" : "fork",
		    mode, expect_fault ? "SIGSEGV, si_code=SEGV_MTESERR" :
		    "wrong-tag read completed as configured");
		return (true);
	}
	fprintf(stderr, "FAIL: inherit-%s-%s: expected %s; wait status=%#x; ",
	    thread ? "thread" : "fork", mode,
	    expect_fault ? "SIGSEGV/SEGV_MTESERR on wrong-tag read" :
	    "wrong-tag read without a fault", status);
	if (received == sizeof(report)) {
		fprintf(stderr, "phase=%s, signal=%d, si_code=%d, error=%d\n",
		    report.phase >= INHERIT_SETUP && report.phase <= INHERIT_WRONG ?
		    phases[report.phase] : "invalid", report.sig, report.code,
		    report.error);
	} else {
		fprintf(stderr, "incomplete report (%zu/%zu bytes)\n", received,
		    sizeof(report));
	}
	return (false);
}

static bool
inheritance_tests(void)
{
	static const struct {
		uint64_t flags;
		const char *name;
	} modes[] = {
		{ MTE_CTRL_ENABLE | MTE_CTRL_TCF_SYNC, "sync" },
		{ MTE_CTRL_ENABLE | MTE_CTRL_TCF_NONE, "none" },
		{ MTE_CTRL_TCF_SYNC, "disabled" },
	};
	bool ok = true;

	/* Each case starts from a fresh isolated process with a live allocation. */
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (!inherit_case(false, modes[i].flags, modes[i].name))
			ok = false;
		if (!inherit_case(true, modes[i].flags, modes[i].name))
			ok = false;
	}
	return (ok);
}

typedef char *(*string_search_fn)(const char *, int);

static char *
string_test_tags(void *memory, size_t size, unsigned value)
{
	char *ptr = (char *)(address(memory) | ((uintptr_t)value << 56));

	for (size_t i = 0; i < size; i += 16) {
		char *granule = ptr + i;
		__asm__ volatile(".arch_extension memtag\n\t"
		    "stg %0, [%0]" : : "r" (granule) : "memory");
	}
	return (ptr);
}

static void
string_search_case(string_search_fn search, bool last, bool return_nul)
{
	static const int needles[] = {0, 'a', 'b', 'z', 0xe1, 0x1e1, -31};
	static const unsigned char pattern[] = {'a', 'b', 0xe1};
	size_t pagesize = getpagesize();
	int mte_prot = tagging ? PROT_MTE : 0;
	char *mapping = mmap(NULL, 3 * pagesize,
	    PROT_READ | PROT_WRITE | mte_prot, MAP_ANON | MAP_PRIVATE, -1, 0);
	CHECK(mapping != MAP_FAILED);
	CHECK(mprotect(mapping, pagesize, PROT_NONE | mte_prot) == 0);
	CHECK(mprotect(mapping + 2 * pagesize, pagesize,
	    PROT_NONE | mte_prot) == 0);
	char *page = mapping + pagesize;
	if (tagging) {
		uint64_t flags = MTE_CTRL_ENABLE | MTE_CTRL_TCF_SYNC;
		CHECK(syscall(SYS_sysarch, ARM64_MTE_CTRL, &flags) == 0);
		(void)string_test_tags(page, pagesize, 0);
	}
	memset(page, 0xa5, pagesize);

	/*
	 * Exercise both halves of a 32-byte boundary and a page end.  Only
	 * the granules containing the string have tag 1; both neighbours
	 * keep tag 0.  This makes even a single extra granule read fail
	 * deterministically, without depending on random allocator tags.
	 */
	for (unsigned placement = 0; placement < 3; placement++) {
		for (size_t offset = 0; offset < 16; offset++) {
			for (size_t len = 0; len <= 80; len++) {
				size_t size = (offset + len + 1 + 15) & ~(size_t)15;
				char *base = page + (placement == 2 ? pagesize - size :
				    64 + placement * 16);
				char *span = tagging ? string_test_tags(base, size, 1) :
				    base;
				char *str = span + offset;
				memset(span, 0, size); /* Ignore NULs before the start. */
				for (size_t i = 0; i < len; i++)
					str[i] = pattern[i % sizeof(pattern)];
				for (size_t n = 0; n < sizeof(needles) / sizeof(needles[0]);
				    n++) {
					unsigned char needle = needles[n];
					char *expected = NULL;
					for (size_t i = 0; i <= len; i++) {
						if ((unsigned char)str[i] == needle) {
							expected = str + i;
							if (!last)
								break;
						}
					}
					if (expected == NULL && return_nul)
						expected = str + len;
					char *actual = search(str, needles[n]);
					if (actual != expected)
						errx(1, "placement=%u offset=%zu len=%zu "
						    "needle=%d: got %p expected %p", placement,
						    offset, len, needles[n], (void *)actual,
						    (void *)expected);
				}
				if (tagging)
					(void)string_test_tags(base, size, 0);
			}
		}
	}
	CHECK(munmap(mapping, 3 * pagesize) == 0);
}

static void
strerror_case(void)
{
	char buffer[128];

	/* First call opens the locale catalogue; later calls exercise its cache. */
	for (unsigned i = 0; i < 4; i++) {
		errno = EBUSY;
		const char *message = strerror(EINVAL);
		CHECK(message != NULL && strcmp(message, "Invalid argument") == 0);
		CHECK(errno == EBUSY);
		CHECK(snprintf(buffer, sizeof(buffer), "%s", message) == 16);
		CHECK(strcmp(buffer, "Invalid argument") == 0);
		CHECK(strerror_r(EINVAL, buffer, sizeof(buffer)) == 0);
		CHECK(strcmp(buffer, "Invalid argument") == 0);
	}
}

static bool
libc_compatibility_tests(void)
{
	static const struct {
		const char *name;
		string_search_fn search;
		bool last;
		bool return_nul;
	} cases[] = {
		{ "strchr", strchr, false, false },
		{ "index", index, false, false },
		{ "strchrnul", strchrnul, false, true },
		{ "strrchr", strrchr, true, false },
		{ "rindex", rindex, true, false },
		{ "strerror/strerror_r/formatting", NULL, false, false },
	};
	bool ok = true;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		printf("RUN: libc %s\n", cases[i].name);
		CHECK(fflush(stdout) == 0);
		pid_t child = fork();
		CHECK(child != -1);
		if (child == 0) {
			alarm(30);
			if (cases[i].search != NULL)
				string_search_case(cases[i].search, cases[i].last,
				    cases[i].return_nul);
			else
				strerror_case();
			_exit(0);
		}
		int status;
		pid_t waited;
		do {
			waited = waitpid(child, &status, 0);
		} while (waited == -1 && errno == EINTR);
		CHECK(waited == child);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			printf("PASS: libc %s\n", cases[i].name);
			continue;
		}
		/* Do not call strerror/strsignal while testing those libc paths. */
		fprintf(stderr, "FAIL: libc %s: wait status=%#x, signal=%d\n",
		    cases[i].name, status,
		    WIFSIGNALED(status) ? WTERMSIG(status) : 0);
		ok = false;
	}
	return (ok);
}

int
main(int argc, char **argv)
{
	if ((argc != 2 && argc != 3) || (strcmp(argv[1], "on") != 0 &&
	    strcmp(argv[1], "off") != 0) ||
	    (argc == 3 && strcmp(argv[2], "stress") != 0 &&
	    strcmp(argv[2], "inherit") != 0 && strcmp(argv[2], "libc") != 0))
		errx(1, "usage: %s on|off [stress|inherit|libc] "
		    "(set MALLOC_CONF before exec)", argv[0]);
	bool stress = argc == 3 && strcmp(argv[2], "stress") == 0;
	bool inherit = argc == 3 && strcmp(argv[2], "inherit") == 0;
	bool libc_checks = argc == 3 && strcmp(argv[2], "libc") == 0;
	CHECK(read_bool("config.mte"));
	tagging = read_bool("opt.mte");
	CHECK(tagging == (strcmp(argv[1], "on") == 0));
	if (inherit && !tagging)
		errx(1, "inherit mode requires MALLOC_CONF=mte:true and on");
	struct rlimit no_core = {0, 0};
	CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0);
	/* Bound failed barriers/waits as well as unexpectedly slow runs. */
	alarm(stress ? 300 : libc_checks ? 240 : inherit ? 120 : 60);
	printf("INFO: tcache=%s zero=%s mode=%s\n",
	    read_bool("opt.tcache") ? "on" : "off",
	    read_bool("opt.zero") ? "on" : "off",
	    stress ? "stress" : inherit ? "inherit" : libc_checks ? "libc" : "smoke");
	if (libc_checks) {
		if (!libc_compatibility_tests())
			return (1);
		printf("PASS: jemalloc MTE %s (libc compatibility)\n",
		    tagging ? "on" : "off");
		return (0);
	}
	if (inherit) {
		if (!inheritance_tests())
			return (1);
		printf("PASS: jemalloc MTE on (inheritance)\n");
		return (0);
	}
	run_check("ordinary allocations", ordinary_allocations);
	run_check("realloc preservation", reallocations);
	run_check("thread/fork handoff", thread_and_fork);
	if (tagging) {
		bool wrong_tag_ok = expect_fault(false);
		bool after_free_ok = expect_fault(true);
		if (!wrong_tag_ok || !after_free_ok)
			return (1);
	}
	if (stress) {
		run_check("concurrent ownership/thread lifecycle", thread_stress);
		run_check("explicit cache flush/reuse", explicit_cache_stress);
		run_check("repeated fork/tag and data COW", fork_stress);
		run_check("arena purge/reallocation", reclamation_stress);
	}
	printf("PASS: jemalloc MTE %s%s\n", tagging ? "on" : "off",
	    stress ? " (stress)" : "");
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
