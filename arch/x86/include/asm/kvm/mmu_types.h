/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_KVM_MMU_TYPES_H
#define __ASM_KVM_MMU_TYPES_H

#include <linux/types.h>

/*
 * This is a subset of the overall kvm_cpu_role to minimize the size of
 * kvm_memory_slot.arch.gfn_track, i.e. allows allocating 2 bytes per gfn
 * instead of 4 bytes per gfn.
 *
 * Upper-level shadow pages having gptes are tracked for write-protection via
 * gfn_track.  As above, gfn_track is a 16 bit counter, so KVM must not create
 * more than 2^16-1 upper-level shadow pages at a single gfn, otherwise
 * gfn_track will overflow and explosions will ensure.
 *
 * A unique shadow page (SP) for a gfn is created if and only if an existing SP
 * cannot be reused.  The ability to reuse a SP is tracked by its role, which
 * incorporates various mode bits and properties of the SP.  Roughly speaking,
 * the number of unique SPs that can theoretically be created is 2^n, where n
 * is the number of bits that are used to compute the role.
 *
 * Note, not all combinations of modes and flags below are possible:
 *
 *   - invalid shadow pages are not accounted, so the bits are effectively 18
 *
 *   - quadrant will only be used if has_4_byte_gpte=1 (non-PAE paging);
 *     execonly and ad_disabled are only used for nested EPT which has
 *     has_4_byte_gpte=0.  Therefore, 2 bits are always unused.
 *
 *   - the 4 bits of level are effectively limited to the values 2/3/4/5,
 *     as 4k SPs are not tracked (allowed to go unsync).  In addition non-PAE
 *     paging has exactly one upper level, making level completely redundant
 *     when has_4_byte_gpte=1.
 *
 *   - on top of this, smep_andnot_wp and smap_andnot_wp are only set if
 *     cr0_wp=0, therefore these three bits only give rise to 5 possibilities.
 *
 * Therefore, the maximum number of possible upper-level shadow pages for a
 * single gfn is a bit less than 2^13.
 */
struct kvm_mmu_page_role_arch {
	u16 has_4_byte_gpte:1;
	u16 quadrant:2;
	u16 direct:1;
	u16 access:3;
	u16 efer_nx:1;
	u16 cr0_wp:1;
	u16 smep_andnot_wp:1;
	u16 smap_andnot_wp:1;
	u16 ad_disabled:1;
	u16 guest_mode:1;
	u16 passthrough:1;
};

#endif /* !__ASM_KVM_MMU_TYPES_H */
