/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_MMU_TYPES_H
#define __KVM_MMU_TYPES_H

#include <linux/bug.h>
#include <linux/kvm_types.h>
#include <linux/refcount.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <asm/kvm/mmu_types.h>

/*
 * kvm_mmu_page_role tracks the properties of a shadow page (where shadow page
 * also includes TDP pages) to determine whether or not a page can be used in
 * the given MMU context.
 */
union kvm_mmu_page_role {
	u32 word;
	struct {
		struct {
			/* The address space ID mapped by the page. */
			u16 as_id:8;

			/* The level of the page in the page table hierarchy. */
			u16 level:4;

			/* Whether the page is invalid, i.e. pending destruction. */
			u16 invalid:1;
		};

		/* Architecture-specific properties. */
		struct kvm_mmu_page_role_arch arch;
	};
};

static_assert(sizeof(union kvm_mmu_page_role) == sizeof_field(union kvm_mmu_page_role, word));

typedef u64 __rcu *tdp_ptep_t;

struct kvm_mmu_page {
	struct list_head link;

	union kvm_mmu_page_role role;

	/* The start of the GFN region mapped by this shadow page. */
	gfn_t gfn;

	/* The page table page. */
	u64 *spt;

	/* The PTE that points to this shadow page. */
	tdp_ptep_t ptep;

	/* Used for freeing TDP MMU pages asynchronously. */
	struct rcu_head rcu_head;

	/* The number of references to this shadow page as a root. */
	refcount_t root_refcount;

	/* Used for tearing down an entire page table tree. */
	struct work_struct tdp_mmu_async_work;
	void *tdp_mmu_async_data;

	struct kvm_mmu_page_arch arch;
};

struct kvm_page_fault {
	/* The raw faulting address. */
	const gpa_t addr;

	/* Whether the fault was synthesized to prefetch a mapping. */
	const bool prefetch;

	/* Information about the cause of the fault. */
	const bool write;
	const bool exec;

	/* Shifted addr, or result of guest page table walk if shadow paging. */
	gfn_t gfn;

	/* The memslot that contains @gfn. May be NULL. */
	struct kvm_memory_slot *slot;

	/* Maximum page size that can be created for this fault. */
	u8 max_level;

	/*
	 * Page size that can be created based on the max_level and the page
	 * size used by the host mapping.
	 */
	u8 req_level;

	/* Final page size that will be created. */
	u8 goal_level;

	/*
	 * The value of kvm->mmu_invalidate_seq before fetching the host
	 * mapping. Used to verify that the host mapping has not changed
	 * after grabbing the MMU lock.
	 */
	unsigned long mmu_seq;

	/* Information about the host mapping. */
	kvm_pfn_t pfn;
	hva_t hva;
	bool map_writable;

	struct kvm_page_fault_arch arch;
};

/*
 * Return values for page fault handling routines.
 *
 * RET_PF_CONTINUE: So far, so good, keep handling the page fault.
 * RET_PF_RETRY: let CPU fault again on the address.
 * RET_PF_EMULATE: mmio page fault, emulate the instruction directly.
 * RET_PF_INVALID: the spte is invalid, let the real page fault path update it.
 * RET_PF_FIXED: The faulting entry has been fixed.
 * RET_PF_SPURIOUS: The faulting entry was already fixed, e.g. by another vCPU.
 *
 * Any names added to this enum should be exported to userspace for use in
 * tracepoints via TRACE_DEFINE_ENUM() in arch/x86/kvm/mmu/mmutrace.h.
 *
 * Note, all values must be greater than or equal to zero so as not to encroach
 * on -errno return values.  Somewhat arbitrarily use '0' for CONTINUE, which
 * will allow for efficient machine code when checking for CONTINUE.
 */
enum {
	RET_PF_CONTINUE = 0,
	RET_PF_RETRY,
	RET_PF_EMULATE,
	RET_PF_INVALID,
	RET_PF_FIXED,
	RET_PF_SPURIOUS,
};

struct tdp_mmu {
	/* The number of TDP MMU pages across all roots. */
	atomic64_t pages;

	/*
	 * List of kvm_mmu_page structs being used as roots.
	 * All kvm_mmu_page structs in the list should have
	 * tdp_mmu_page set.
	 *
	 * For reads, this list is protected by:
	 *	the MMU lock in read mode + RCU or
	 *	the MMU lock in write mode
	 *
	 * For writes, this list is protected by:
	 *	the MMU lock in read mode + the tdp_mmu_pages_lock or
	 *	the MMU lock in write mode
	 *
	 * Roots will remain in the list until their tdp_mmu_root_count
	 * drops to zero, at which point the thread that decremented the
	 * count to zero should removed the root from the list and clean
	 * it up, freeing the root after an RCU grace period.
	 */
	struct list_head roots;

	/*
	 * Protects accesses to the following fields when the MMU lock
	 * is held in read mode:
	 *  - roots (above)
	 *  - the link field of kvm_mmu_page structs used by the TDP MMU
	 *  - (x86-only) possible_nx_huge_pages;
	 *  - (x86-only) the arch.possible_nx_huge_page_link field of
	 *    kvm_mmu_page structs used by the TDP MMU
	 * It is acceptable, but not necessary, to acquire this lock when
	 * the thread holds the MMU lock in write mode.
	 */
	spinlock_t pages_lock;

	struct workqueue_struct *zap_wq;
};

#endif /* !__KVM_MMU_TYPES_H */
