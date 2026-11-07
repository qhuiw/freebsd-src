#ifndef JEMALLOC_INTERNAL_MTE_H
#define JEMALLOC_INTERNAL_MTE_H

/* MTE allocation tags cover naturally aligned 16-byte granules. */
#define JEMALLOC_MTE_GRANULE ((size_t)16)

static const bool config_mte =
#ifdef JEMALLOC_MTE
    true
#else
    false
#endif
    ;

extern bool opt_mte;

/* Boot before the first arena/base mapping; the option is then immutable. */
bool mte_boot(void);
void *mte_alloc(void *ptr, size_t usize);
bool mte_validate(const void *ptr);
void mte_dalloc(void *ptr, size_t usize);

/*
 * Only strip tags for address bookkeeping.  An untagged pointer cannot access
 * a live, nonzero-tagged payload until its allocation tags have been reset.
 */
static inline void *
mte_untag(const void *ptr) {
#ifdef JEMALLOC_MTE
	if (opt_mte) {
		return (void *)((uintptr_t)ptr & ~((uintptr_t)0xff << 56));
	}
#endif
	return (void *)ptr;
}

static inline int
mte_prot(int prot) {
#ifdef JEMALLOC_MTE
	if (opt_mte) {
		return prot | PROT_MTE;
	}
#endif
	return prot;
}

#endif /* JEMALLOC_INTERNAL_MTE_H */
