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
 * .. note::
 *    The support for preserving PCI devices across Live Update is currently
 *    *partial* and should be considered *experimental*. It should only be
 *    used by developers working on the implementation for the time being.
 *
 *    To enable the support, enable ``CONFIG_PCI_LIVEUPDATE``.
 *
 * File-Lifecycle-Bound (FLB) Data
 * ===============================
 *
 * PCI device preservation across Live Update is built on top of the Live Update
 * Orchestrator's (LUO) support for file preservation across kexec. Drivers
 * are expected to expose a file to represent a single PCI device and support
 * preservation of that file with ``ioctl(LIVEUPDATE_SESSION_PRESERVE_FD)``.
 * This allows userspace to control the preservation of devices and ensure
 * proper lifecycle management while a device is preserved. The first intended
 * use-case is preserving vfio-pci device files.
 *
 * The PCI core maintains its own state about what devices are being preserved
 * across Live Update using a feature called File-Lifecycle-Bound (FLB) data in
 * LUO.  Essentially, this allows the PCI core to allocate struct pci_ser when
 * the first device (file) is preserved and free it when the last device (file)
 * is unpreserved. After kexec, the PCI core can fetch the struct pci_ser (which
 * was constructed by the previous kernel) from LUO at any time (e.g. during
 * enumeration) so that it knows which devices were preserved.
 *
 * To enable the PCI core to be notified whenever a file representing a device
 * is preserved, drivers must register their struct liveupdate_file_handler with
 * the PCI core by using the following APIs:
 *
 *  * ``pci_liveupdate_register_flb(driver_file_handler)``
 *  * ``pci_liveupdate_unregister_flb(driver_file_handler)``
 *
 * Device Tracking
 * ===============
 *
 * Drivers must notify the PCI core when specific devices are preserved or
 * unpreserved with the following APIs:
 *
 *  * ``pci_liveupdate_preserve(pci_dev)``
 *  * ``pci_liveupdate_unpreserve(pci_dev)``
 *
 * This allows the PCI core to keep it's FLB data (struct pci_ser) up to date
 * with the list of **outgoing** preserved devices for the next kernel.
 *
 * After kexec, whenever a device is enumerated, the PCI core will check if it
 * is an **incoming** preserved device (i.e. preserved by the previous kernel)
 * by checking the incoming FLB data (struct pci_ser).
 *
 * Drivers must notify the PCI core when an **incoming** device is done
 * participating in the incoming Live Update with the following API:
 *
 *  * ``pci_liveupdate_finish(pci_dev)``
 *
 * The PCI core does not enforce any ordering of ``pci_liveupdate_finish()`` and
 * ``pci_liveupdate_preserve()``. i.e. A PCI device can be **outgoing**
 * (preserved for next kernel) and **incoming** (preserved by previous kernel)
 * at the same time.
 *
 * Restrictions
 * ============
 *
 * The PCI core enforces the following restrictions on which devices can be
 * preserved. These may be relaxed in the future:
 *
 *  * The device cannot be a Virtual Function (VF).
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

#define INIT_PCI_DEV_SER(_dev) (struct pci_dev_ser ) {	\
	.domain = pci_domain_nr((_dev)->bus),		\
	.bdf = pci_dev_id(_dev),			\
	.refcount = 1,					\
}

static int pci_dev_ser_cmp(const void *__a, const void *__b)
{
	const struct pci_dev_ser *a = __a, *b = __b;

	/*
	 * If the refcount is zero then set bit 63 so all the "empty" elements
	 * of the array get sorted to the end by pci_ser_sort() and then
	 * pci_ser_find() can just binary search the non-empty elements.
	 */
	u64 a_int = ((u64)!a->refcount << 63) | (u64)a->domain << 16 | a->bdf;
	u64 b_int = ((u64)!b->refcount << 63) | (u64)b->domain << 16 | b->bdf;

	return cmp_int(a_int, b_int);
}

static struct pci_dev_ser *pci_ser_find(struct pci_ser *ser,
					struct pci_dev *dev)
{
	const struct pci_dev_ser key = INIT_PCI_DEV_SER(dev);

	return bsearch(&key, ser->devices, ser->nr_devices,
		       sizeof(key), pci_dev_ser_cmp);
}


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
	ser->nr_devices = 0;

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
	struct pci_ser *ser = phys_to_virt(args->data);

	/*
	 * Sort the devices array so that pci_liveupdate_setup_device() can
	 * use binary search to check if devices are preserved. Sort the entire
	 * array (ser->max_nr_devices) since it may be sparse.
	 */
	sort(ser->devices, ser->max_nr_devices, sizeof(ser->devices[0]),
	     pci_dev_ser_cmp, NULL);

	args->obj = ser;
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

int pci_liveupdate_preserve(struct pci_dev *dev)
{
	struct pci_ser *ser;
	int i, ret;

	guard(mutex)(&pci_flb_outgoing_lock);

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);
	if (ret)
		return ret;

	if (!ser)
		return -ENOENT;

	if (dev->is_virtfn)
		return -EINVAL;

	if (dev->liveupdate_outgoing)
		return -EBUSY;

	if (ser->nr_devices == ser->max_nr_devices)
		return -ENOSPC;

	for (i = 0; i < ser->max_nr_devices; i++) {
		/*
		 * Start searching at index ser->nr_devices. This should result
		 * in a constant time search under expected conditions (devices
		 * are not getting unpreserved).
		 */
		int index = (ser->nr_devices + i) % ser->max_nr_devices;
		struct pci_dev_ser *dev_ser = &ser->devices[index];

		if (dev_ser->refcount)
			continue;

		ser->nr_devices++;

		*dev_ser = INIT_PCI_DEV_SER(dev);

		dev->liveupdate_outgoing = dev_ser;
		return 0;
	}

	return -ENOSPC;
}
EXPORT_SYMBOL_GPL(pci_liveupdate_preserve);

void pci_liveupdate_unpreserve(struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser;
	struct pci_ser *ser = NULL;
	int ret;

	guard(mutex)(&pci_flb_outgoing_lock);

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);

	if (ret || !ser) {
		pci_warn(dev, "Cannot unpreserve device without outgoing Live Update state\n");
		return;

	}
	if (WARN_ON_ONCE(ret) || WARN_ON_ONCE(!ser))
		return;

	dev_ser = dev->liveupdate_outgoing;
	if (!dev_ser) {
		pci_warn(dev, "Cannot unpreserve device that is not preserved\n");
		return;
	}

	memset(dev_ser, 0, sizeof(*dev_ser));
	dev->liveupdate_outgoing = NULL;
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

void pci_liveupdate_cleanup_device(struct pci_dev *dev)
{
	/*
	 * Note: This cannot race with pci_liveupdate_finish() since it is only
	 * called in cleanup paths when there are no users of the pci_dev.
	 */
	if (dev->liveupdate_incoming)
		pci_liveupdate_flb_put_incoming();
}

void pci_liveupdate_finish(struct pci_dev *dev)
{
	if (!dev->liveupdate_incoming) {
		pci_warn(dev, "Cannot finish preserving an unpreserved device\n");
		return;
	}

	/*
	 * Drop the refcount so this device does not get treated as an incoming
	 * device again, e.g. in case pci_liveupdate_setup_device() gets called
	 * again becase the device is hot-plugged.
	 */
	dev->liveupdate_incoming->refcount = 0;
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
