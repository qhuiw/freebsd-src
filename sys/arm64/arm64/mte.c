/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024-2026 Arm Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/proc.h>

#include <machine/cpu_feat.h>
#include <machine/pcb.h>
#include <machine/pte.h>
#include <machine/sysarch.h>
#include <vm/vm.h>
#include <vm/vm_page.h>

/* Version of MTE implemented. 0 == unimplemented */
static u_int __read_mostly mte_version = 0;

/*
 * FEAT_MTE (mte_version == 1) has userspace instructions, but no tag
 * checking. May of the registers/fields need FEAT_MTE2 to be implemented
 * before we can access them.
 */
#define	MTE_HAS_TAG_CHECK	(mte_version >= 2)

static u_int __read_mostly mte_flags = 0;
#define	MTE_HAS_ASYNC		1

struct thread *mte_switch(struct thread *);

#define load_tags(addr) ({						\
	uint64_t __val;							\
	asm volatile(							\
	    ".arch_extension memtag	\n"				\
	    "ldgm %0, [%1]		\n"				\
	    ".arch_extension nomemtag" : "=r" (__val) : "r" (addr));	\
	__val;								\
})

#define set_tags(tags, addr) do {					\
	asm volatile(							\
	    ".arch_extension memtag	\n"				\
	    "stgm %0, [%1]		\n"				\
	    ".arch_extension nomemtag" : "=r" (tags) : "r" (addr));	\
} while (0)

/* Fetch the block size used by tag load and store instructions */
static inline size_t
mte_block_size(void)
{
	return (sizeof(int) << GMID_BS_SIZE(READ_SPECIALREG(GMID_EL1_REG)));
}

static void
mte_update_sctlr(struct thread *td, uint64_t sctlr)
{
	MPASS((sctlr & ~(SCTLR_ATA0 | SCTLR_TCF0_MASK)) == 0);
	td->td_md.md_sctlr &= ~(SCTLR_ATA0 | SCTLR_TCF0_MASK);
	td->td_md.md_sctlr |= sctlr;
}

int
mte_sysarch_ctrl(struct thread *td, uint64_t flags)
{
	uint64_t mask, sctlr;
	bool clear_tfsr;

	if (!MTE_HAS_TAG_CHECK)
		return (0);

	clear_tfsr = false;
	switch (flags & MTE_CTRL_TCF_MASK) {
	case MTE_CTRL_TCF_NONE:
		sctlr = SCTLR_TCF0_NONE;
		break;
	case MTE_CTRL_TCF_SYNC:
		sctlr = SCTLR_TCF0_SYNC;
		break;
	case MTE_CTRL_TCF_ASYNC:
		if ((mte_flags & MTE_HAS_ASYNC) == 0)
			return (EINVAL);

		sctlr = SCTLR_TCF0_ASYNC;
		clear_tfsr = true;
		break;
	default:
		/* TODO: Support FEAT_MTE_ASYM_FAULT */
		return (EINVAL);
	}

	if ((flags & MTE_CTRL_ENABLE) != 0)
		sctlr |= SCTLR_ATA0;

	/* Tag Exclusion Mask */
	mask = (flags & MTE_CTRL_EXCLUDE_MASK) >> MTE_CTRL_EXCLUDE_SHIFT;
	td->td_md.md_gcr = mask << GCR_Exclude_SHIFT | GCR_RRND;
	/* MTE mode */
	mte_update_sctlr(td, sctlr);

	if (td == curthread) {
		printf("writing new sctlr val: 0x%lx\n", sctlr);
		WRITE_SPECIALREG(sctlr_el1,
		    (READ_SPECIALREG(sctlr_el1) & ~SCTLR_USER_MASK) | sctlr);
		WRITE_SPECIALREG(GCR_EL1_REG, td->td_md.md_gcr);
		if (clear_tfsr)
			WRITE_SPECIALREG(TFSRE0_EL1_REG, 0);
		isb();
	}

	return (0);
}

/**
 * Clear/sync the allocation tags for a given page. This should be done on
 * allocation of a page to ensure a tag check fault does not occur immediately
 * after accessing newly tagged memory.
 */
void
mte_sync_tags(vm_page_t page)
{
	size_t block_size;
	vm_offset_t addr;

	if (!MTE_HAS_TAG_CHECK)
		return;

	/* don't clear the tags on a page that's already setup for mte */
	if ((page->md.pv_flags & PV_MTE_TAGGED) != 0)
		return;

	block_size = mte_block_size();
	addr = PHYS_TO_DMAP(page->phys_addr);

	for (size_t count = 0; count < PAGE_SIZE;
	    count += block_size, addr += block_size)
		asm volatile(
		    ".arch_extension memtag	\n"
		    "stgm xzr, [%0]		\n"
		    ".arch_extension nomemtag" : : "r" (addr));

	page->md.pv_flags |= PV_MTE_TAGGED;
}

void
mte_save_tags(vm_page_t m, void *tags)
{
	vm_offset_t va;
	size_t block_size;
	uint64_t tag_word, tmp;

	block_size = mte_block_size();
	va = PHYS_TO_DMAP(m->phys_addr);
	for (int i = 0; i < PAGE_SIZE / 32; i += sizeof(uint64_t)) {
		tag_word = 0;
		__asm __volatile(
		    ".arch_extension memtag	\n"
		    "1:				\n"
		    /* Load the tag granule */
		    "ldgm	%1, [%2]	\n"
		    /* Store the parts in tag_word */
		    "orr	%0, %0, %1	\n"
		    /* Next tag granule */
		    "add	%2, %2, %3	\n"
		    /* Check if we are on the last granule for this word */
		    "tst	%2, 0xff	\n"
		    "b.ne	1b		\n"
		    ".arch_extension nomemtag" :
		    "+r" (tag_word), "=r"(tmp), "+r" (va) : "r" (block_size));

		*(uint64_t *)((uintptr_t)tags + i) = tag_word;
	}
}

void
mte_load_tags(vm_page_t m, const void *tags)
{
	vm_offset_t va;
	size_t block_size;
	uint64_t tag_word;

	block_size = mte_block_size();
	va = PHYS_TO_DMAP(m->phys_addr);
	for (int i = 0; i < PAGE_SIZE / 32; i += sizeof(uint64_t)) {
		tag_word = *(uint64_t *)((uintptr_t)tags + i);
		__asm __volatile(
		    ".arch_extension memtag	\n"
		    "1:				\n"
		    /* Store the tag granule */
		    "stgm	%1, [%0]	\n"
		    /* Next tag granule */
		    "add	%0, %0, %2	\n"
		    /* Check if we are on the last granule for this word */
		    "tst	%0, 0xff	\n"
		    "b.ne	1b		\n"
		    ".arch_extension nomemtag" :
		    "+r" (va) : "r" (tag_word), "r" (block_size));
	}
}

/**
 * Copy the allocation tags from given target to destination page. This is called
 * on a copy-on-write and anything that causes a pmap_copy_page call.
 */
void
mte_copy_tags(vm_page_t srcpage, vm_page_t dstpage, vm_offset_t src, vm_offset_t dst)
{
	size_t block_size;
	uint64_t tags;

	MPASS((srcpage->md.pv_flags & PV_MTE_TAGGED) != 0);

	/*
	 * Copy the tags from the source page to the destination page,
	 * incrementing by the block count read from GMID_EL1
	 */
	block_size = mte_block_size();
	for (size_t count = 0; count < PAGE_SIZE;
	    count += block_size, src += block_size, dst += block_size) {
		tags = load_tags(src);
		set_tags(tags, dst);
	}
	dstpage->md.pv_flags |= PV_MTE_TAGGED;
}

void
mte_fork(struct thread *new_td, struct thread *orig_td)
{
	if (!MTE_HAS_TAG_CHECK)
		return;

	mte_update_sctlr(new_td,
	    orig_td->td_md.md_sctlr & SCTLR_TCF0_MASK);
	new_td->td_md.md_gcr = orig_td->td_md.md_gcr;
}

void
mte_exec(struct thread *td)
{
	if (!MTE_HAS_TAG_CHECK)
		return;

	mte_update_sctlr(td, SCTLR_TCF0_NONE);
	td->td_md.md_gcr = GCR_RRND;
}

void
mte_copy_thread(struct thread *new_td, struct thread *orig_td)
{
	if (!MTE_HAS_TAG_CHECK)
		return;

	mte_update_sctlr(new_td,
	    orig_td->td_md.md_sctlr & SCTLR_TCF0_MASK);
	new_td->td_md.md_gcr = orig_td->td_md.md_gcr;
}

/* Only for kernel threads */
void
mte_thread_alloc(struct thread *td)
{
}

/* Only for a kernel thread */
void
mte_thread0(struct thread *td)
{
}


struct thread *
mte_switch(struct thread *td)
{
	if (MTE_HAS_TAG_CHECK) {
		WRITE_SPECIALREG(GCR_EL1_REG, td->td_md.md_gcr);
	}
	return (td);
}
