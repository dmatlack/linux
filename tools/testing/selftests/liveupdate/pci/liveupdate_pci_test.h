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

#endif /* SELFTESTS_LIVEUPDATE_PCI_LIVEUPDATE_PCI_TEST_H */
