// SPDX-License-Identifier: GPL-2.0

/*
 * Liveupdate support for VFIO devices.
 *
 * Copyright (c) 2025, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/liveupdate.h>
#include <linux/liveupdate/abi/vfio_pci.h>
#include <linux/errno.h>

#include "vfio_pci_priv.h"

static bool vfio_pci_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					     struct file *file)
{
	return false;
}

static const struct liveupdate_file_ops vfio_pci_liveupdate_file_ops = {
	.can_preserve = vfio_pci_liveupdate_can_preserve,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler vfio_pci_liveupdate_fh = {
	.ops = &vfio_pci_liveupdate_file_ops,
	.compatible = VFIO_PCI_LUO_FH_COMPATIBLE,
};

int __init vfio_pci_liveupdate_init(void)
{
	if (!liveupdate_enabled())
		return 0;

	return liveupdate_register_file_handler(&vfio_pci_liveupdate_fh);
}

void vfio_pci_liveupdate_cleanup(void)
{
	/*
	 * TODO: Unregister vfio_pci_liveupdate_fh from LUO once that is
	 * supported.
	 */
	BUG_ON(liveupdate_enabled());
}
