/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_TDP_PGTABLE_H
#define __KVM_TDP_PGTABLE_H

#include <linux/log2.h>
#include <linux/mm_types.h>

#include <asm/kvm/tdp_pgtable.h>

#define TDP_ROOT_MAX_LEVEL	5
#define TDP_MAX_HUGEPAGE_LEVEL	PG_LEVEL_PUD
#define TDP_PTES_PER_PAGE	(PAGE_SIZE / sizeof(u64))
#define TDP_LEVEL_BITS		ilog2(TDP_PTES_PER_PAGE)
#define TDP_LEVEL_MASK		((1UL << TDP_LEVEL_BITS) - 1)

#define TDP_LEVEL_SHIFT(level) (((level) - 1) * TDP_LEVEL_BITS)

#define TDP_PAGES_PER_LEVEL(level) (1UL << TDP_LEVEL_SHIFT(level))

#define TDP_PTE_INDEX(gfn, level) \
	(((gfn) >> TDP_LEVEL_SHIFT(level)) & TDP_LEVEL_MASK)

/*
 * If a thread running without exclusive control of the MMU lock must perform a
 * multi-part operation on a PTE, it can set the PTE to REMOVED_TDP_PTE as a
 * non-present intermediate value. Other threads which encounter this value
 * should not modify the PTE.
 */
static inline bool tdp_pte_is_removed(u64 pte)
{
	return pte == REMOVED_TDP_PTE;
}

static inline bool tdp_pte_is_leaf(u64 pte, int level)
{
	return tdp_pte_is_huge(pte) || level == PG_LEVEL_PTE;
}

#endif /* !__KVM_TDP_PGTABLE_H */
