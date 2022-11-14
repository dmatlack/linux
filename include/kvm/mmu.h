/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_MMU_H
#define __KVM_MMU_H

#include <kvm/mmu_types.h>

static inline struct kvm_mmu_page *to_shadow_page(hpa_t shadow_page)
{
	struct page *page = pfn_to_page((shadow_page) >> PAGE_SHIFT);

	return (struct kvm_mmu_page *)page_private(page);
}

static inline struct kvm_mmu_page *sptep_to_sp(u64 *sptep)
{
	return to_shadow_page(__pa(sptep));
}

#endif /* !__KVM_MMU_H */
