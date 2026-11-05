/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Interface between the PCI Live Update selftests and the liveupdate_pci_test
 * kernel module.
 *
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */
#ifndef SELFTESTS_LIVEUPDATE_PCI_LIVEUPDATE_PCI_TEST_H
#define SELFTESTS_LIVEUPDATE_PCI_LIVEUPDATE_PCI_TEST_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define LIVEUPDATE_PCI_TEST_DEVICE	"/dev/liveupdate_pci_test"
#define LIVEUPDATE_PCI_TEST_MODULE	"/liveupdate_pci_test.ko"

/* The device was incoming-preserved when the driver probed it. */
#define LIVEUPDATE_PCI_TEST_INCOMING_AT_PROBE	(1 << 0)
/* The device is currently incoming-preserved. */
#define LIVEUPDATE_PCI_TEST_INCOMING		(1 << 1)

/**
 * struct liveupdate_pci_test_device - Identifies a PCI device
 * @domain: PCI domain (segment) number
 * @bdf: PCI bus, device and function number
 * @flags: LIVEUPDATE_PCI_TEST_* flags, set by LIVEUPDATE_PCI_TEST_GET_STATUS
 */
struct liveupdate_pci_test_device {
	__u32 domain;
	__u16 bdf;
	__u16 flags;
};

#define LIVEUPDATE_PCI_TEST_IOC_TYPE	0xB8

/*
 * Issued on LIVEUPDATE_PCI_TEST_DEVICE. Returns a new file descriptor for a
 * device bound to the liveupdate_pci_test driver, which can be preserved in a
 * Live Update session.
 */
#define LIVEUPDATE_PCI_TEST_GET_DEVICE_FD \
	_IOW(LIVEUPDATE_PCI_TEST_IOC_TYPE, 0, struct liveupdate_pci_test_device)

/* Issued on a device file descriptor. */
#define LIVEUPDATE_PCI_TEST_GET_STATUS \
	_IOR(LIVEUPDATE_PCI_TEST_IOC_TYPE, 1, struct liveupdate_pci_test_device)

/* Number of DWORDs in a PCI Express configuration space. */
#define LIVEUPDATE_PCI_TEST_CFG_DWORDS	1024

/*
 * The configuration space DWORD that the test driver adds to the saved state
 * it preserves, to stand in for a register that the previous kernel saved but
 * the next kernel does not restore. No capability of the test device reserves
 * it, so the next kernel must drop it.
 */
#define LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD	(LIVEUPDATE_PCI_TEST_CFG_DWORDS - 1)
#define LIVEUPDATE_PCI_TEST_UNRESERVED_VALUE	0x4c55dead

/**
 * struct liveupdate_pci_test_saved_state - A device's saved state
 * @dword_map: One bit per configuration space DWORD, set if it was saved
 * @dword_val: The saved DWORDs, one per bit set in @dword_map, in ascending
 *             offset order
 *
 * The saved state is what the test driver captured with
 * pci_store_saved_state() when it first probed the device, or retrieved from
 * the previous kernel if the device was preserved.
 */
struct liveupdate_pci_test_saved_state {
	__u64 dword_map[LIVEUPDATE_PCI_TEST_CFG_DWORDS / 64];
	__u32 dword_val[LIVEUPDATE_PCI_TEST_CFG_DWORDS];
};

/* Issued on a device file descriptor. Returns the device's saved state. */
#define LIVEUPDATE_PCI_TEST_GET_SAVED_STATE \
	_IOR(LIVEUPDATE_PCI_TEST_IOC_TYPE, 2, struct liveupdate_pci_test_saved_state)

/*
 * Issued on a device file descriptor. Loads the device's saved state with
 * pci_load_saved_state() and writes it to the device with pci_restore_state().
 */
#define LIVEUPDATE_PCI_TEST_RESTORE_SAVED_STATE \
	_IO(LIVEUPDATE_PCI_TEST_IOC_TYPE, 3)

#endif /* SELFTESTS_LIVEUPDATE_PCI_LIVEUPDATE_PCI_TEST_H */
