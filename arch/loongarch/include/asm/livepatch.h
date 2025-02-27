/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LIVEPATCH_H
#define _ASM_LIVEPATCH_H

#include <asm/inst.h>

static inline int klp_check_compiler_support(void)
{
	return 0;
}

static inline void klp_arch_set_pc(struct ftrace_regs *fregs, unsigned long ip)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);

	regs->csr_era = ip;
}
#endif /* _ASM_LIVEPATCH_H */
