/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_MMU_INTERNAL_H
#define __KVM_X86_MMU_INTERNAL_H

#include <linux/types.h>
#include <linux/kvm_host.h>
#include <asm/kvm_host.h>

#undef MMU_DEBUG

#ifdef MMU_DEBUG
extern bool dbg;

#define pgprintk(x...) do { if (dbg) printk(x); } while (0)
#define rmap_printk(fmt, args...) do { if (dbg) printk("%s: " fmt, __func__, ## args); } while (0)
#define MMU_WARN_ON(x) WARN_ON(x)
#else
#define pgprintk(x...) do { } while (0)
#define rmap_printk(x...) do { } while (0)
#define MMU_WARN_ON(x) do { } while (0)
#endif

/* Page table builder macros common to shadow (host) PTEs and guest PTEs. */
#define __PT_LEVEL_SHIFT(level, bits_per_level)	\
	(PAGE_SHIFT + ((level) - 1) * (bits_per_level))
#define __PT_INDEX(address, level, bits_per_level) \
	(((address) >> __PT_LEVEL_SHIFT(level, bits_per_level)) & ((1 << (bits_per_level)) - 1))

#define __PT_LVL_ADDR_MASK(base_addr_mask, level, bits_per_level) \
	((base_addr_mask) & ~((1ULL << (PAGE_SHIFT + (((level) - 1) * (bits_per_level)))) - 1))

#define __PT_LVL_OFFSET_MASK(base_addr_mask, level, bits_per_level) \
	((base_addr_mask) & ((1ULL << (PAGE_SHIFT + (((level) - 1) * (bits_per_level)))) - 1))

#define __PT_ENT_PER_PAGE(bits_per_level)  (1 << (bits_per_level))

/*
 * Unlike regular MMU roots, PAE "roots", a.k.a. PDPTEs/PDPTRs, have a PRESENT
 * bit, and thus are guaranteed to be non-zero when valid.  And, when a guest
 * PDPTR is !PRESENT, its corresponding PAE root cannot be set to INVALID_PAGE,
 * as the CPU would treat that as PRESENT PDPTR with reserved bits set.  Use
 * '0' instead of INVALID_PAGE to indicate an invalid PAE root.
 */
#define INVALID_PAE_ROOT	0
#define IS_VALID_PAE_ROOT(x)	(!!(x))

static inline bool kvm_mmu_page_ad_need_write_protect(struct kvm_mmu_page *sp)
{
	/*
	 * When using the EPT page-modification log, the GPAs in the CPU dirty
	 * log would come from L2 rather than L1.  Therefore, we need to rely
	 * on write protection to record dirty pages, which bypasses PML, since
	 * writes now result in a vmexit.  Note, the check on CPU dirty logging
	 * being enabled is mandatory as the bits used to denote WP-only SPTEs
	 * are reserved for PAE paging (32-bit KVM).
	 */
	return kvm_x86_ops.cpu_dirty_log_size && sp->role.arch.guest_mode;
}

int mmu_try_to_unsync_pages(struct kvm *kvm, const struct kvm_memory_slot *slot,
			    gfn_t gfn, bool can_unsync, bool prefetch);

void kvm_mmu_gfn_disallow_lpage(const struct kvm_memory_slot *slot, gfn_t gfn);
void kvm_mmu_gfn_allow_lpage(const struct kvm_memory_slot *slot, gfn_t gfn);
bool kvm_mmu_slot_gfn_write_protect(struct kvm *kvm,
				    struct kvm_memory_slot *slot, u64 gfn,
				    int min_level);
void kvm_flush_remote_tlbs_with_address(struct kvm *kvm,
					u64 start_gfn, u64 pages);
unsigned int pte_list_count(struct kvm_rmap_head *rmap_head);

extern int nx_huge_pages;
static inline bool is_nx_huge_page_enabled(struct kvm *kvm)
{
	return READ_ONCE(nx_huge_pages) && !kvm->arch.disable_nx_huge_pages;
}

int kvm_tdp_page_fault(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault);

static inline int kvm_mmu_do_page_fault(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa,
					u32 err, bool prefetch)
{
	bool is_tdp = likely(vcpu->arch.mmu->page_fault == kvm_tdp_page_fault);
	struct kvm_page_fault fault = {
		.addr = cr2_or_gpa,
		.prefetch = prefetch,

		.write = err & PFERR_WRITE_MASK,
		.exec = err & PFERR_FETCH_MASK,

		.max_level = KVM_MAX_HUGEPAGE_LEVEL,
		.req_level = PG_LEVEL_4K,
		.goal_level = PG_LEVEL_4K,

		.arch = {
			.error_code = err,
			.present = err & PFERR_PRESENT_MASK,
			.rsvd = err & PFERR_RSVD_MASK,
			.user = err & PFERR_USER_MASK,
			.is_tdp = is_tdp,
			.nx_huge_page_workaround_enabled =
				is_nx_huge_page_enabled(vcpu->kvm),
		},
	};
	int r;

	if (vcpu->arch.mmu->root_role.arch.direct) {
		fault.gfn = fault.addr >> PAGE_SHIFT;
		fault.slot = kvm_vcpu_gfn_to_memslot(vcpu, fault.gfn);
	}

	/*
	 * Async #PF "faults", a.k.a. prefetch faults, are not faults from the
	 * guest perspective and have already been counted at the time of the
	 * original fault.
	 */
	if (!prefetch)
		vcpu->stat.pf_taken++;

	if (IS_ENABLED(CONFIG_RETPOLINE) && fault.arch.is_tdp)
		r = kvm_tdp_page_fault(vcpu, &fault);
	else
		r = vcpu->arch.mmu->page_fault(vcpu, &fault);

	/*
	 * Similar to above, prefetch faults aren't truly spurious, and the
	 * async #PF path doesn't do emulation.  Do count faults that are fixed
	 * by the async #PF handler though, otherwise they'll never be counted.
	 */
	if (r == RET_PF_FIXED)
		vcpu->stat.pf_fixed++;
	else if (prefetch)
		;
	else if (r == RET_PF_EMULATE)
		vcpu->stat.pf_emulate++;
	else if (r == RET_PF_SPURIOUS)
		vcpu->stat.pf_spurious++;
	return r;
}

int kvm_mmu_max_mapping_level(struct kvm *kvm,
			      const struct kvm_memory_slot *slot, gfn_t gfn,
			      int max_level);
void kvm_mmu_hugepage_adjust(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault);
void disallowed_hugepage_adjust(struct kvm_page_fault *fault, u64 spte, int cur_level);

void *mmu_memory_cache_alloc(struct kvm_mmu_memory_cache *mc);

void track_possible_nx_huge_page(struct kvm *kvm, struct kvm_mmu_page *sp);
void untrack_possible_nx_huge_page(struct kvm *kvm, struct kvm_mmu_page *sp);

#endif /* __KVM_X86_MMU_INTERNAL_H */
