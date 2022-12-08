/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_KVM_TDP_PGTABLE_H
#define __ASM_KVM_TDP_PGTABLE_H

#include <linux/types.h>
#include <linux/kvm_types.h>
#include <kvm/mmu_types.h>

struct kvm_mmu_page *tdp_mmu_root(struct kvm_vcpu *vcpu);

/*
 * Use a semi-arbitrary value that doesn't set RWX bits, i.e. is not-present on
 * both AMD and Intel CPUs, and doesn't set PFN bits, i.e. doesn't create a L1TF
 * vulnerability.  Use only low bits to avoid 64-bit immediates.
 */
#define REMOVED_TDP_PTE		0x5a0ULL

#define TDP_PTE_WRITABLE_MASK	BIT_ULL(1)
#define TDP_PTE_HUGE_PAGE_MASK	BIT_ULL(7)
#define TDP_PTE_PRESENT_MASK	BIT_ULL(11)

static inline bool tdp_pte_is_writable(u64 pte)
{
	return pte & TDP_PTE_WRITABLE_MASK;
}

static inline bool tdp_pte_is_huge(u64 pte)
{
	return pte & TDP_PTE_HUGE_PAGE_MASK;
}

static inline bool tdp_pte_is_present(u64 pte)
{
	return pte & TDP_PTE_PRESENT_MASK;
}

bool tdp_pte_is_accessed(u64 pte);
bool tdp_pte_is_dirty(u64 pte);
bool tdp_pte_is_mmio(u64 pte);
bool tdp_pte_is_volatile(u64 pte);

static inline u64 tdp_pte_clear_writable(u64 pte)
{
	return pte & ~TDP_PTE_WRITABLE_MASK;
}

static inline u64 tdp_pte_clear_mmu_writable(u64 pte)
{
	extern u64 __read_mostly shadow_mmu_writable_mask;

	return pte & ~(TDP_PTE_WRITABLE_MASK | shadow_mmu_writable_mask);
}

u64 tdp_pte_clear_dirty(u64 pte, bool force_wrprot);
u64 tdp_pte_clear_accessed(u64 pte);

kvm_pfn_t tdp_pte_to_pfn(u64 pte);

void tdp_pte_check_leaf_invariants(u64 pte);

struct tdp_iter;

u64 tdp_mmu_make_leaf_pte(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault,
			  struct tdp_iter *iter, bool *wrprot);
u64 tdp_mmu_make_nonleaf_pte(struct kvm_mmu_page *sp);
u64 tdp_mmu_make_changed_pte_notifier_pte(struct tdp_iter *iter,
					  struct kvm_gfn_range *range);
u64 tdp_mmu_make_huge_page_split_pte(struct kvm *kvm, u64 huge_spte,
				     struct kvm_mmu_page *sp, int index);

gfn_t tdp_mmu_max_gfn_exclusive(void);

#endif /* !__ASM_KVM_TDP_PGTABLE_H */
