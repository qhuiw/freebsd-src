#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mte.h"
#include "jemalloc/internal/safety_check.h"

#ifdef JEMALLOC_MTE
#  if !defined(__FreeBSD__) || !defined(__aarch64__) || \
    defined(__CHERI_PURE_CAPABILITY__)
#    error "JEMALLOC_MTE requires FreeBSD AArch64 without capabilities"
#  endif
#  include <sys/auxv.h>
#  include <machine/armreg.h>
#  include <machine/elf.h>
#  include <machine/sysarch.h>
#endif

bool opt_mte = false;

#ifdef JEMALLOC_MTE
static unsigned
mte_pointer_tag(const void *ptr) {
	return ((uintptr_t)ptr >> 56) & 0xf;
}

/*
 * Reapply the allocator's synchronous policy before using tag instructions.
 * Keep this conservative path for kernels predating the SCTLR_ATA0 thread
 * inheritance fix.  fork() already inherits the complete saved md_sctlr in
 * cpu_fork().  Caching these controls is a separate optimization; the initial
 * prototype intentionally makes a syscall per operation.
 */
static bool
mte_thread_enable(void) {
	uint64_t flags = MTE_CTRL_ENABLE | MTE_CTRL_TCF_SYNC |
	    (UINT64_C(1) << MTE_CTRL_EXCLUDE_SHIFT);
	int saved_errno = get_errno();
	int ret = (int)syscall(SYS_sysarch, ARM64_MTE_CTRL, &flags);
	set_errno(saved_errno);
	return ret != 0;
}

static void
mte_thread_require(void) {
	if (mte_thread_enable()) {
		malloc_write("<jemalloc>: Cannot enable synchronous MTE\n");
		abort();
	}
}

static void *
mte_load_tag(const void *ptr) {
	void *tagged = (void *)ptr;
	__asm__ volatile(".arch_extension memtag\n\t"
	    "ldg %0, [%1]" : "+r" (tagged) : "r" (ptr) : "memory");
	return tagged;
}

static void *
mte_random_tag(void *ptr) {
	void *tagged;
	/* Exclude tag zero even if the thread's exclusion mask changes. */
	uint64_t exclude = 1;
	__asm__ volatile(".arch_extension memtag\n\t"
	    "irg %0, %1, %2" : "=r" (tagged) : "r" (ptr), "r" (exclude));
	return tagged;
}

static void
mte_tag_range(void *tagged, size_t usize) {
	assert(((uintptr_t)tagged & (JEMALLOC_MTE_GRANULE - 1)) == 0);
	assert(usize != 0 && (usize & (JEMALLOC_MTE_GRANULE - 1)) == 0);
	for (size_t offset = 0; offset < usize;
	    offset += JEMALLOC_MTE_GRANULE) {
		void *granule = (void *)((uintptr_t)tagged + offset);
		__asm__ volatile(".arch_extension memtag\n\t"
		    "stg %0, [%0]" : : "r" (granule) : "memory");
	}
}
#endif

bool
mte_boot(void) {
	if (!opt_mte) {
		return false;
	}
#ifdef JEMALLOC_MTE
	unsigned long hwcap, hwcap2;
	if (_elf_aux_info(AT_HWCAP, &hwcap, sizeof(hwcap)) != 0 ||
	    _elf_aux_info(AT_HWCAP2, &hwcap2, sizeof(hwcap2)) != 0 ||
	    (hwcap & HWCAP_CPUID) == 0 || (hwcap2 & HWCAP2_MTE) == 0) {
		malloc_write("<jemalloc>: mte requires kernel/CPU MTE support\n");
		return true;
	}
	/* This branch advertises HWCAP2_MTE for FEAT_MTE1 as well. */
	uint64_t pfr1 = READ_SPECIALREG(id_aa64pfr1_el1);
	if (ID_AA64PFR1_MTE_VAL(pfr1) < ID_AA64PFR1_MTE_MTE2) {
		malloc_write("<jemalloc>: mte requires FEAT_MTE2 tag checking\n");
		return true;
	}
	if (opt_prof || opt_hpa || have_dss) {
		malloc_write("<jemalloc>: mte does not support profiling, HPA "
		    "or DSS in this prototype\n");
		return true;
	}
	/*
	 * malloc_init_a0() can run while libc is still allocating TLS.  Do not
	 * access errno or enable a thread here; mte_alloc()/mte_validate() do
	 * that after the ordinary entry points have obtained their TSD.
	 */
	return false;
#else
	malloc_write("<jemalloc>: MTE support was not compiled in\n");
	return true;
#endif
}

void *
mte_alloc(void *ptr, size_t usize) {
#ifdef JEMALLOC_MTE
	assert(opt_mte && ptr != NULL);
	assert(ptr == mte_untag(ptr));
	mte_thread_require();
	void *tagged = mte_random_tag(ptr);
	if (mte_pointer_tag(tagged) == 0) {
		malloc_write("<jemalloc>: MTE generated the reserved tag zero\n");
		abort();
	}
	mte_tag_range(tagged, usize);
	return tagged;
#else
	(void)usize;
	return ptr;
#endif
}

bool
mte_validate(const void *ptr) {
#ifdef JEMALLOC_MTE
	assert(opt_mte && ptr != NULL);
	mte_thread_require();
	unsigned tag = mte_pointer_tag(ptr);
	if (((uintptr_t)ptr & (JEMALLOC_MTE_GRANULE - 1)) != 0 || tag == 0 ||
	    tag != mte_pointer_tag(mte_load_tag(ptr))) {
		safety_check_fail("<jemalloc>: Invalid MTE allocation pointer %p\n",
		    ptr);
		return false;
	}
#else
	(void)ptr;
#endif
	return true;
}

void
mte_dalloc(void *ptr, size_t usize) {
#ifdef JEMALLOC_MTE
	/* mte_validate() must precede this transition for an external free. */
	assert(opt_mte && ptr != NULL);
	assert(ptr == mte_untag(ptr));
	mte_tag_range(ptr, usize);
#else
	(void)ptr;
	(void)usize;
#endif
}
