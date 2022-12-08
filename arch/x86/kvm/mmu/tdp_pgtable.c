// SPDX-License-Identifier: GPL-2.0

#include <linux/kvm_types.h>
#include <kvm/tdp_pgtable.h>
#include <kvm/tdp_iter.h>

#include "mmu.h"
#include "spte.h"

/* Removed SPTEs must not be misconstrued as shadow present PTEs. */
static_assert(!(REMOVED_TDP_PTE & SPTE_MMU_PRESENT_MASK));

static_assert(TDP_PTE_WRITABLE_MASK == PT_WRITABLE_MASK);
static_assert(TDP_PTE_HUGE_PAGE_MASK == PT_PAGE_SIZE_MASK);
static_assert(TDP_PTE_PRESENT_MASK == SPTE_MMU_PRESENT_MASK);

struct kvm_mmu_page *tdp_mmu_root(struct kvm_vcpu *vcpu)
{
	return to_shadow_page(vcpu->arch.mmu->root.hpa);
}

bool tdp_pte_is_accessed(u64 pte)
{
	return is_accessed_spte(pte);
}

bool tdp_pte_is_dirty(u64 pte)
{
	return is_dirty_spte(pte);
}

bool tdp_pte_is_mmio(u64 pte)
{
	return is_mmio_spte(pte);
}

bool tdp_pte_is_volatile(u64 pte)
{
	return spte_has_volatile_bits(pte);
}

u64 tdp_pte_clear_dirty(u64 pte, bool force_wrprot)
{
	if (force_wrprot || spte_ad_need_write_protect(pte)) {
		if (tdp_pte_is_writable(pte))
			pte &= ~PT_WRITABLE_MASK;
	} else if (pte & shadow_dirty_mask) {
		pte &= ~shadow_dirty_mask;
	}

	return pte;
}

u64 tdp_pte_clear_accessed(u64 old_spte)
{
	if (spte_ad_enabled(old_spte))
		return old_spte & ~shadow_accessed_mask;

	/*
	 * Capture the dirty status of the page, so that it doesn't get lost
	 * when the SPTE is marked for access tracking.
	 */
	if (tdp_pte_is_writable(old_spte))
		kvm_set_pfn_dirty(tdp_pte_to_pfn(old_spte));

	return mark_spte_for_access_track(old_spte);
}

kvm_pfn_t tdp_pte_to_pfn(u64 pte)
{
	return spte_to_pfn(pte);
}

void tdp_pte_check_leaf_invariants(u64 pte)
{
	check_spte_writable_invariants(pte);
}

u64 tdp_mmu_make_leaf_pte(struct kvm_vcpu *vcpu,
			  struct kvm_page_fault *fault,
			  struct tdp_iter *iter,
			  bool *wrprot)
{
	struct kvm_mmu_page *sp = sptep_to_sp(rcu_dereference(iter->sptep));
	u64 new_spte;

	if (unlikely(!fault->slot))
		return make_mmio_spte(vcpu, iter->gfn, ACC_ALL);

	*wrprot = make_spte(vcpu, sp, fault->slot, ACC_ALL, iter->gfn,
			    fault->pfn, iter->old_spte, fault->prefetch, true,
			    fault->map_writable, &new_spte);

	return new_spte;
}

u64 tdp_mmu_make_nonleaf_pte(struct kvm_mmu_page *sp)
{
	return make_nonleaf_spte(sp->spt, !kvm_ad_enabled());
}

u64 tdp_mmu_make_changed_pte_notifier_pte(struct tdp_iter *iter,
					  struct kvm_gfn_range *range)
{
	return kvm_mmu_changed_pte_notifier_make_spte(iter->old_spte,
						      pte_pfn(range->pte));
}

u64 tdp_mmu_make_huge_page_split_pte(struct kvm *kvm, u64 huge_spte,
				     struct kvm_mmu_page *sp, int index)
{
	return make_huge_page_split_spte(kvm, huge_spte, sp->role, index);
}

void tdp_mmu_arch_adjust_map_level(struct kvm_page_fault *fault,
				   struct tdp_iter *iter)
{
	if (fault->arch.nx_huge_page_workaround_enabled)
		disallowed_hugepage_adjust(fault, iter->old_spte, iter->level);
}

void tdp_mmu_arch_init_sp(struct kvm_mmu_page *sp)
{
	INIT_LIST_HEAD(&sp->arch.possible_nx_huge_page_link);
}

void tdp_mmu_arch_pre_link_sp(struct kvm *kvm,
			      struct kvm_mmu_page *sp,
			      struct kvm_page_fault *fault)
{
	sp->arch.nx_huge_page_disallowed = fault->arch.huge_page_disallowed;
}

void tdp_mmu_arch_post_link_sp(struct kvm *kvm,
			       struct kvm_mmu_page *sp,
			       struct kvm_page_fault *fault)
{
	if (!fault->arch.huge_page_disallowed)
		return;

	if (fault->req_level < sp->role.level)
		return;

	spin_lock(&kvm->tdp_mmu.pages_lock);
	track_possible_nx_huge_page(kvm, sp);
	spin_unlock(&kvm->tdp_mmu.pages_lock);
}

void tdp_mmu_arch_unlink_sp(struct kvm *kvm, struct kvm_mmu_page *sp,
			    bool shared)
{
	if (!sp->arch.nx_huge_page_disallowed)
		return;

	if (shared)
		spin_lock(&kvm->tdp_mmu.pages_lock);
	else
		lockdep_assert_held_write(&kvm->mmu_lock);

	sp->arch.nx_huge_page_disallowed = false;
	untrack_possible_nx_huge_page(kvm, sp);

	if (shared)
		spin_unlock(&kvm->tdp_mmu.pages_lock);
}

int tdp_mmu_max_mapping_level(struct kvm *kvm,
			      const struct kvm_memory_slot *slot,
			      struct tdp_iter *iter)
{
	return kvm_mmu_max_mapping_level(kvm, slot, iter->gfn, PG_LEVEL_NUM);
}

gfn_t tdp_mmu_max_gfn_exclusive(void)
{
	/*
	 * Bound TDP MMU walks at host.MAXPHYADDR.  KVM disallows memslots with
	 * a gpa range that would exceed the max gfn, and KVM does not create
	 * MMIO SPTEs for "impossible" gfns, instead sending such accesses down
	 * the slow emulation path every time.
	 */
	return kvm_mmu_max_gfn() + 1;
}
