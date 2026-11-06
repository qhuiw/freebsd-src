
#ifndef _MACHINE_PTRACE_H_
#define _MACHINE_PTRACE_H_

#define __HAVE_PTRACE_MACHDEP

#define PT_GETVFPREGS32	(PT_FIRSTMACH + 0)
#define PT_SETVFPREGS32	(PT_FIRSTMACH + 1)

#define	PT_GETMTETAGS	(PT_FIRSTMACH + 2)
#define	PT_SETMTETAGS	(PT_FIRSTMACH + 3)

struct ptrace_mte_tag_desc {
	void	*pmted_addr;	/* address in traced process */
	void	*pmted_tags;	/* tracer buffer, one 4-bit tag per byte */
	size_t	pmted_len;	/* number of tags to transfer. */
};

#endif /* _MACHINE_PTRACE_H_ */
