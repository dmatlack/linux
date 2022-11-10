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

#endif /* !__KVM_MMU_TYPES_H */
