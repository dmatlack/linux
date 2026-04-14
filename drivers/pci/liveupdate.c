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
 * is unpreserved. After kexec, the PCI core can fetch the pci_ser struct (which
 * was constructed by the previous kernel) from LUO at any time (e.g. during
 * enumeration) so that it knows which devices were preserved.
 *
 * To enable the PCI core to be notified whenever a file representing a device
 * is preserved, drivers must register their liveupdate_file_handler struct with
 * the PCI core by using the following APIs:
 *
 *  * ``pci_liveupdate_register_flb()``: Register a driver's
 *    liveupdate_file_handler with the PCI core.
 *
 *  * ``pci_liveupdate_unregister_flb(): Unregister a driver's
 *    liveupdate_file_handler struct with the PCI core.
 *
 * Device Tracking
 * ===============
 *
 * Drivers must notify the PCI core when specific devices are preserved so that
 * the PCI core can keep it's FLB data (struct pci_ser) up to date and track the
 * list of **outgoing** devices (devices being preserved for the next kernel).
 *
 *  * ``pci_liveupdate_preserve(pci_dev)``: Notifies the PCI core that
 *    ``@pci_dev`` must be preserved across Live Update.
 *
 *  * ``pci_liveupdate_unpreserve(pci_dev)``: Notifies the PCI core that
 *    ``@pci_dev`` is no longer being preserved across Live Update.
 *
 * After kexec, drivers must notify the PCI core when an **incoming** device
 * (i.e. a device that was preserved by the previous kernel) is done
 * participating in the incoming Live Update:
 *
 *  * ``pci_liveupdate_finish(pci_dev)``: Notifies the PCI core that
 *    ``@pci_dev`` is no longer participating in Live Update.
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
 *
 * BDF Stability
 * =============
 *
 * The PCI core guarantees that incoming preserved devices can be identified by
 * the same bus, device, and function numbers as prior to kexec. To accomplish
 * this, the PCI core always inherits the secondary and subordinate bus numbers
 * assigned to bridges during enumeration, rather than assigning new ones (the
 * PCI core assumes that the previous kernel established a sane topology).
 *
 * If a misconfigured or unconfigured bridge is encountered during enumeration
 * while there are incoming preserved devices, it's secondary and subordinate
 * bus numbers will be cleared and devices below it will not be enumerated.
 *
 * PCI-to-PCI Bridges
 * ==================
 *
 * Any PCI-to-PCI bridges upstream of a preserved device are automatically
 * preserved when the device is preserved. The PCI core keeps track of the
 * number of downstream devices that are preserved under a bridge so that the
 * bridge is only unpreserved once all downstream devices are unpreserved.
 *
 * This enables the PCI core and any drivers bound to the bridge to participate
 * in the Live Update so that preserved endpoints can continue issuing memory
 * transactions during the Live Update.
 *
 * Handling Preserved Devices
 * ==========================
 *
 * The PCI core treats preserved devices differently than non-preserved devices.
 * This section enumerates those differences.
 *
 *  * The PCI core does not disable bus mastering on outoing preserved devices
 *    during kexec. This allows preserved devices to issue memory transactions
 *    throughout the Live Update.
 *
 *  * The PCI core inherits all ACS flags enabled on incoming preserved devices
 *    rather than assigning new ones. This ensures that TLPs are routed the same
 *    way after Live Update and that IOMMU groups do not change.
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

static int pci_liveupdate_preserve_device(struct pci_ser *ser, struct pci_dev *dev)
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

static void pci_liveupdate_unpreserve_path(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev *upstream_bridge = dev->bus->self;
	struct pci_dev_ser *dev_ser;

	if (upstream_bridge)
		pci_liveupdate_unpreserve_path(ser, upstream_bridge);

	dev_ser = dev->liveupdate_outgoing;
	if (!dev_ser) {
		pci_WARN_ONCE(dev, true, "Device is not preserved!");
		return;
	}

	if (--dev_ser->refcount == 0)
		pci_ser_delete(ser, dev_ser);
}

static int pci_liveupdate_preserve_path(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev *upstream_bridge = dev->bus->self;
	int ret = 0;

	if (upstream_bridge) {
		ret = pci_liveupdate_preserve_path(ser, upstream_bridge);
		if (ret)
			return ret;
	} else if (!pci_is_root_bus(dev->bus)) {
		pci_err(dev, "Failed to preserve up to root port\n");
		return -EINVAL;
	}

	ret = pci_liveupdate_preserve_device(ser, dev);
	if (ret)
		goto err;

	return 0;

err:
	if (upstream_bridge)
		pci_liveupdate_unpreserve_path(ser, upstream_bridge);

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

	if (dev->is_virtfn)
		return -EINVAL;

	if (dev->liveupdate_outgoing)
		return -EBUSY;

	return pci_liveupdate_preserve_path(ser, dev);
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

	pci_liveupdate_unpreserve_path(ser, dev);
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
	 * Decrement the refcount so this device does not get treated as an
	 * incoming device again, e.g. in case pci_liveupdate_setup_device()
	 * gets called again becase the device is hot-plugged.
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
