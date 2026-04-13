// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

/**
 * DOC: PCI Live Update
 *
 * The PCI subsystem participates in the Live Update process to enable drivers
 * to preserve their PCI devices across kexec.
 *
 * Device preservation across Live Update is built on top of the Live Update
 * Orchestrator (LUO) support for file preservation across kexec. Userspace
 * indicates that a device should be preserved by preserving the file associated
 * with the device with ``ioctl(LIVEUPDATE_SESSION_PRESERVE_FD)``.
 *
 * .. note::
 *    The support for preserving PCI devices across Live Update is currently
 *    *partial* and should be considered *experimental*. It should only be
 *    used by developers working on the implementation for the time being.
 *
 *    To enable the support, enable ``CONFIG_PCI_LIVEUPDATE``.
 *
 * Driver API
 * ==========
 *
 * Drivers that support file-based device preservation must register their
 * ``liveupdate_file_handler`` with the PCI subsystem by calling
 * ``pci_liveupdate_register_flb()``. This ensures the PCI subsystem will be
 * notified whenever a device file is preserved so that ``struct pci_ser``
 * can be allocated to track all preserved devices. This struct is an ABI
 * and is eventually handed off to the next kernel via Kexec-Handover (KHO).
 *
 * In the "outgoing" kernel (before kexec), drivers should then notify the PCI
 * subsystem directly whenever the preservation status for a device changes:
 *
 *  * ``pci_liveupdate_preserve(pci_dev)``: The device is being preserved.
 *
 *  * ``pci_liveupdate_unpreserve(pci_dev)``: The device is no longer being
 *    preserved (preservation is cancelled).
 *
 * In the "incoming" kernel (after kexec), drivers should notify the PCI
 * subsystem with the following calls:
 *
 *  * ``pci_liveupdate_finish(pci_dev)``: The device is done participating in
 *    Live Update. After this point the device may no longer be even associated
 *    with the same driver.
 *
 * Restrictions
 * ============
 *
 * Preserved devices currently have the following restrictions. Each of these
 * may be relaxed in the future.
 *
 *  * The device must not be a Virtual Function (VF).
 *
 *  * The device must not be a Physical Function (PF).
 *
 * Preservation Behavior
 * =====================
 *
 * The kernel preserves the following state for devices preserved across a Live
 * Update:
 *
 *  * The PCI Segment, Bus, Device, and Function numbers assigned to the device
 *    are guaranteed to remain the same across Live Update. Note that this is
 *    true even if pci=assign-busses is set on the command line. The kernel will
 *    always inherit bus numbers assigned by the previous kernel during a Live
 *    Update.
 *
 * This list will be extended in the future as new support is added.
 *
 * Driver Binding
 * ==============
 *
 * It is the driver's responsibility for ensuring that preserved devices are not
 * released or bound to a different driver for as long as they are preserved. In
 * practice, this is enforced by LUO taking an extra referenced to the preserved
 * device file for as long as it is preserved.
 *
 * However, there is a window of time in the incoming kernel when a device is
 * first probed and when userspace retrieves the device file with
 * ``LIVEUPDATE_SESSION_RETRIEVE_FD`` when the device could be bound to any
 * driver.
 *
 * It is currently userspace's responsibility to ensure that the device is bound
 * to the correct driver in this window.
 */

#include <linux/bsearch.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/pci.h>
#include <linux/liveupdate.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/sort.h>

#include "pci.h"

static DEFINE_MUTEX(pci_flb_outgoing_lock);

static int pci_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct pci_dev *dev = NULL;
	int max_nr_devices = 0;
	struct pci_ser *ser;
	unsigned long size;

	/*
	 * Allocate enough space to preserve all of the devices that are
	 * currently present on the system. Extra padding can be added to this
	 * in the future to increase the chances that there is enough room to
	 * preserve devices that are not yet present on the system (e.g. VFs,
	 * hot-plugged devices).
	 */
	for_each_pci_dev(dev)
		max_nr_devices++;

	size = struct_size_t(struct pci_ser, devices, max_nr_devices);

	ser = kho_alloc_preserve(size);
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	ser->max_nr_devices = max_nr_devices;

	args->obj = ser;
	args->data = virt_to_phys(ser);
	return 0;
}

static void pci_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	struct pci_ser *ser = args->obj;

	WARN_ON_ONCE(ser->nr_devices);
	kho_unpreserve_free(ser);
}

static int pci_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	args->obj = phys_to_virt(args->data);
	return 0;
}

static void pci_flb_finish(struct liveupdate_flb_op_args *args)
{
	kho_restore_free(args->obj);
}

static struct liveupdate_flb_ops pci_liveupdate_flb_ops = {
	.preserve = pci_flb_preserve,
	.unpreserve = pci_flb_unpreserve,
	.retrieve = pci_flb_retrieve,
	.finish = pci_flb_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_flb pci_liveupdate_flb = {
	.ops = &pci_liveupdate_flb_ops,
	.compatible = PCI_LUO_FLB_COMPATIBLE,
};

#define INIT_PCI_DEV_SER(_dev) {		\
	.domain = pci_domain_nr((_dev)->bus),	\
	.bdf = pci_dev_id(_dev),		\
	.refcount = 1,				\
}

static int pci_dev_ser_cmp(const void *__a, const void *__b)
{
	const struct pci_dev_ser *a = __a, *b = __b;

	return cmp_int((u64)a->domain << 16 | a->bdf,
		       (u64)b->domain << 16 | b->bdf);
}

static struct pci_dev_ser *pci_ser_find(struct pci_ser *ser,
					struct pci_dev *dev)
{
	const struct pci_dev_ser key = INIT_PCI_DEV_SER(dev);

	return bsearch(&key, ser->devices, ser->nr_devices,
		       sizeof(key), pci_dev_ser_cmp);
}

static void pci_ser_delete(struct pci_ser *ser, struct pci_dev_ser *dev_ser)
{
	int i;

	for (i = dev_ser - ser->devices; i < ser->nr_devices - 1; i++)
		ser->devices[i] = ser->devices[i + 1];

	ser->nr_devices--;
}

static void __pci_liveupdate_unpreserve(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev *upstream_bridge = dev->bus->self;
	struct pci_dev_ser *dev_ser;

	if (upstream_bridge)
		__pci_liveupdate_unpreserve(ser, upstream_bridge);

	dev_ser = dev->liveupdate_outgoing;
	if (!dev_ser) {
		pci_WARN_ONCE(dev, true, "Device is not preserved!");
		return;
	}

	if (--dev_ser->refcount == 0)
		pci_ser_delete(ser, dev_ser);
}

static int pci_liveupdate_preserve_one(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev_ser new = INIT_PCI_DEV_SER(dev);
	int i;

	if (dev->liveupdate_outgoing) {
		dev->liveupdate_outgoing->refcount++;
		return 0;
	}

	if (ser->nr_devices == ser->max_nr_devices)
		return -ENOSPC;

	for (i = ser->nr_devices; i > 0; i--) {
		struct pci_dev_ser *prev = &ser->devices[i - 1];
		int cmp = pci_dev_ser_cmp(&new, prev);

		if (!cmp)
			return -EBUSY;

		if (cmp > 0)
			break;

		ser->devices[i] = *prev;
	}

	ser->devices[i] = new;
	ser->nr_devices++;
	dev->liveupdate_outgoing = &ser->devices[i];
	return 0;
}

static int __pci_liveupdate_preserve(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev *upstream_bridge = dev->bus->self;
	int ret = 0;

	/* SR-IOV is not yet supported. */
	if (dev->is_virtfn || dev->is_physfn)
		return -EINVAL;

	if (upstream_bridge) {
		ret = __pci_liveupdate_preserve(ser, upstream_bridge);
		if (ret)
			return ret;
	} else if (!pci_is_root_bus(dev->bus)) {
		pci_err(dev, "Failed to preserve up to root port\n");
		return -EINVAL;
	}

	ret = pci_liveupdate_preserve_one(ser, dev);
	if (ret)
		goto err;

	return 0;

err:
	if (upstream_bridge)
		__pci_liveupdate_unpreserve(ser, upstream_bridge);

	return ret;
}

int pci_liveupdate_preserve(struct pci_dev *dev)
{
	struct pci_ser *ser;
	int ret;

	guard(mutex)(&pci_flb_outgoing_lock);

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);
	if (ret)
		return ret;

	if (!ser)
		return -ENOENT;

	return __pci_liveupdate_preserve(ser, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_preserve);

void pci_liveupdate_unpreserve(struct pci_dev *dev)
{
	struct pci_ser *ser;
	int ret;

	guard(mutex)(&pci_flb_outgoing_lock);

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);

	if (WARN_ON_ONCE(ret) || WARN_ON_ONCE(!ser))
		return;

	__pci_liveupdate_unpreserve(ser, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unpreserve);

static struct pci_ser *pci_liveupdate_flb_get_incoming(void)
{
	void *ser;
	int ret;

	ret = liveupdate_flb_get_incoming(&pci_liveupdate_flb, &ser);

	/* Live Update is not enabled. */
	if (ret == -EOPNOTSUPP)
		return NULL;

	/* Live Update is enabled, but there is no incoming FLB data. */
	if (ret == -ENODATA)
		return NULL;

	/*
	 * Live Update is enabled and there is incoming FLB data, but none of it
	 * matches pci_liveupdate_flb.compatible.
	 *
	 * This could mean that no PCI FLB data was passed by the previous
	 * kernel, but it could also mean the previous kernel used a different
	 * compatibility string (i.e. a different ABI). The latter deserves at
	 * least a WARN_ON_ONCE() but it cannot be distinguished from the
	 * former.
	 */
	if (ret == -ENOENT) {
		pr_info_once("PCI: No Live Update incoming FLB matched %s",
			     pci_liveupdate_flb.compatible);
		return NULL;
	}

	/*
	 * There is incoming FLB data that matches pci_liveupdate_flb.compatible
	 * but it cannot be retrieved. Proceed with standard initialization as
	 * if there was no incoming PCI FLB data.
	 */
	if (ret) {
		WARN_ONCE(ret, "PCI: Failed to retrieve incoming FLB data during Live Update");
		return NULL;
	}

	return ser;
}

static void pci_liveupdate_flb_put_incoming(void)
{
	liveupdate_flb_put_incoming(&pci_liveupdate_flb);
}

void pci_liveupdate_setup_device(struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser;
	struct pci_ser *ser;

	ser = pci_liveupdate_flb_get_incoming();
	if (!ser)
		return;

	/*
	 * During a Live Update, preserved devices are allowed to continue
	 * performing memory transactions. The kernel must not change the fabric
	 * topology, including bus numbers, since that would require disabling
	 * and flushing any memory transactions first.
	 *
	 * To keep things simple, inherit the secondary and subordinate bus
	 * numbers on _all_ bridges if _any_ PCI devices were preserved (i.e.
	 * even bridges without any downstream endpoints that were preserved).
	 * This avoids accidentally assigning a bridge a new window that
	 * overlaps with a preserved device that is downstream of a different
	 * bridge.
	 */
	dev->liveupdate_inherit_buses = true;

	dev_ser = pci_ser_find(ser, dev);
	if (!dev_ser || !dev_ser->refcount) {
		pci_liveupdate_flb_put_incoming();
		return;
	}

	/*
	 * Hold the ref on the incoming FLB until pci_liveupdate_finish() so
	 * that dev_ser does not get freed while it is in use.
	 */
	dev->liveupdate_incoming = dev_ser;
}

void pci_liveupdate_finish(struct pci_dev *dev)
{
	struct pci_dev *upstream_bridge = dev->bus->self;

	if (upstream_bridge)
		pci_liveupdate_finish(upstream_bridge);

	/*
	 * Decrement the refcount so it does not get reassociated with this
	 * device again, e.g. if the device it hot-unplugged and then
	 * hot-plugged.
	 */
	if (--dev->liveupdate_incoming->refcount)
		return;

	dev->liveupdate_incoming = NULL;
	pci_liveupdate_flb_put_incoming();
}
EXPORT_SYMBOL_GPL(pci_liveupdate_finish);

int pci_liveupdate_register_flb(struct liveupdate_file_handler *fh)
{
	return liveupdate_register_flb(fh, &pci_liveupdate_flb);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_register_flb);

void pci_liveupdate_unregister_flb(struct liveupdate_file_handler *fh)
{
	liveupdate_unregister_flb(fh, &pci_liveupdate_flb);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unregister_flb);
