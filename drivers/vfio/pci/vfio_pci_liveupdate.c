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
#include <linux/vfio.h>
#include <linux/errno.h>
#include <linux/kexec_handover.h>

#include "../vfio.h"
#include "vfio_pci_priv.h"

static bool vfio_pci_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					     struct file *file)
{
	struct vfio_device_file *df = to_vfio_device_file(file);

	if (!df)
		return false;

	/* Live Update support is limited to cdev files. */
	if (df->group)
		return false;

	return df->device->ops == &vfio_pci_ops;
}

static int vfio_pci_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;
	struct folio *folio;
	int err;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM))
		return -EINVAL;

	if (vfio_pci_is_intel_display(pdev))
		return -EINVAL;

	/*
	 * Require devices that support function-level resets to simplify
	 * vfio_pci_liveupdate_freeze() below and allow the next kernel to
	 * skip reset support detection. This could be relaxed in the future if
	 * there's appetite for preserving other types of devices.
	 */
	if (!vdev->reset_works)
		return -EIO;

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, get_order(sizeof(*ser)));
	if (!folio)
		return -ENOMEM;

	ser = folio_address(folio);

	ser->bdf = pci_dev_id(pdev);
	ser->domain = pci_domain_nr(pdev->bus);

	err = kho_preserve_folio(folio);
	if (err)
		goto error;

	args->serialized_data = virt_to_phys(ser);
	return 0;

error:
	folio_put(folio);
	return err;
}

static void vfio_pci_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device_ser *ser = phys_to_virt(args->serialized_data);
	struct folio *folio = virt_to_folio(ser);

	kho_unpreserve_folio(folio);
	folio_put(folio);
}

static int vfio_pci_liveupdate_freeze(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;
	int ret;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	guard(mutex)(&device->dev_set->lock);

	/*
	 * Userspace must disable interrupts on the device prior to freeze so
	 * that the device does not send any interrupts until new interrupt
	 * handlers have been established by the next kernel.
	 */
	if (vdev->irq_type != VFIO_PCI_NUM_IRQS) {
		pci_err(pdev, "Freeze failed! Interrupts are still enabled.\n");
		return -EINVAL;
	}

	if (!pci_dev_trylock(pdev))
		return -EBUSY;

	/*
	 * Reset the device and load the original state. Eventually this should
	 * all be dropped and the device should be kept running with its current
	 * state across the Live Update.
	 */
	ret = pci_load_and_free_saved_state(pdev, &vdev->pci_saved_state);
	if (ret)
		goto out;

	ret = __pci_reset_function_locked(pdev);
	pci_restore_state(pdev);

out:
	pci_dev_unlock(pdev);
	return ret;
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static const struct liveupdate_file_ops vfio_pci_liveupdate_file_ops = {
	.can_preserve = vfio_pci_liveupdate_can_preserve,
	.preserve = vfio_pci_liveupdate_preserve,
	.unpreserve = vfio_pci_liveupdate_unpreserve,
	.freeze = vfio_pci_liveupdate_freeze,
	.retrieve = vfio_pci_liveupdate_retrieve,
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
