/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_MMU_TYPES_H
#define __KVM_MMU_TYPES_H

#include <linux/bug.h>
#include <linux/types.h>
#include <linux/stddef.h>

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

#endif /* !__KVM_MMU_TYPES_H */
