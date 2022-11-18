/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_TRACE_KVM_MMUTRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KVM_MMUTRACE_H

#ifdef CONFIG_X86
#include "../../arch/x86/kvm/mmu/mmutrace.h"
#else
#define trace_mark_mmio_spte(...)		do {} while (0)
#define trace_kvm_mmu_get_page(...)		do {} while (0)
#define trace_kvm_mmu_prepare_zap_page(...)	do {} while (0)
#define trace_kvm_mmu_set_spte(...)		do {} while (0)
#define trace_kvm_mmu_spte_requested(...)	do {} while (0)
#define trace_kvm_mmu_split_huge_page(...)	do {} while (0)
#define trace_kvm_tdp_mmu_spte_changed(...)	do {} while (0)
#endif

#endif /* _TRACE_KVM_MMUTRACE_H */
