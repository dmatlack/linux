// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * David Matlack <dmatlack@google.com>
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

static DEFINE_MUTEX(pci_flb_outgoing_lock);
static DEFINE_MUTEX(pci_flb_incoming_lock);

static int pci_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct pci_dev *dev = NULL;
	struct pci_ser *ser;
	unsigned long size;
	int nr_devices = 0;

	/*
	 * Calculate the maximum number of devices based on what's present
	 * on the system currently (including VFs) to size the folio holding
	 * struct pci_ser. This is not perfect given devices could be
	 * hotplugged, but it's also unlikely that all devices in the system are
	 * going to be preserved anyway.
	 */
	for_each_pci_dev(dev) {
		if (dev->is_virtfn)
			continue;

		nr_devices += 1 + pci_sriov_get_totalvfs(dev);
	}

	size = struct_size_t(struct pci_ser, devices, nr_devices);

	ser = kho_alloc_preserve(size);
	if (IS_ERR(ser))
		return PTR_ERR(ser);

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
	struct pci_ser *ser = args->obj;

	/*
	 * Sanity check that all devices have been finished via
	 * pci_liveupdate_incoming_finish().
	 */
	WARN_ON_ONCE(ser->nr_devices);
	kho_restore_free(ser);
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
}

static int pci_dev_ser_cmp(const void *__a, const void *__b)
{
	const struct pci_dev_ser *a = __a, *b = __b;

	return cmp_int(a->domain << 16 | a->bdf, b->domain << 16 | b->bdf);
}

static struct pci_dev_ser *pci_ser_find(struct pci_ser *ser,
					struct pci_dev *dev)
{
	const struct pci_dev_ser key = INIT_PCI_DEV_SER(dev);

	return bsearch(&key, ser->devices, ser->nr_devices,
		       sizeof(key), pci_dev_ser_cmp);
}

static int pci_ser_delete(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser;
	int i;

	dev_ser = pci_ser_find(ser, dev);
	if (!dev_ser)
		return -ENOENT;

	for (i = dev_ser - ser->devices; i < ser->nr_devices - 1; i++)
		ser->devices[i] = ser->devices[i + 1];

	ser->nr_devices--;
	return 0;
}

static int max_nr_devices(struct pci_ser *ser)
{
	u64 size;

	size = folio_size(virt_to_folio(ser));
	size -= offsetof(struct pci_ser, devices);

	return size / sizeof(struct pci_dev_ser);
}

int pci_liveupdate_outgoing_preserve(struct pci_dev *dev)
{
	struct pci_dev_ser new = INIT_PCI_DEV_SER(dev);
	struct pci_ser *ser;
	int i, ret;

	/* VFs are not supported due to bus number instability across kexec */
	if (dev->is_virtfn)
		return -EINVAL;

	guard(mutex)(&pci_flb_outgoing_lock);

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);
	if (ret)
		return ret;

	if (ser->nr_devices == max_nr_devices(ser))
		return -E2BIG;

	for (i = ser->nr_devices; i > 0; i--) {
		struct pci_dev_ser *prev = &ser->devices[i - 1];
		int cmp = pci_dev_ser_cmp(&new, prev);

		/* This device is already preserved. */
		if (cmp == 0)
			return 0;

		if (cmp > 0)
			break;

		ser->devices[i] = *prev;
	}

	ser->devices[i] = new;
	ser->nr_devices++;
	return 0;
}
EXPORT_SYMBOL_GPL(pci_liveupdate_outgoing_preserve);

void pci_liveupdate_outgoing_unpreserve(struct pci_dev *dev)
{
	struct pci_ser *ser;
	int ret;

	guard(mutex)(&pci_flb_outgoing_lock);

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);
	if (WARN_ON_ONCE(ret))
		return;

	WARN_ON_ONCE(pci_ser_delete(ser, dev));
}
EXPORT_SYMBOL_GPL(pci_liveupdate_outgoing_unpreserve);

u32 pci_liveupdate_incoming_nr_devices(void)
{
	struct pci_ser *ser;
	int ret;

	guard(mutex)(&pci_flb_incoming_lock);

	ret = liveupdate_flb_get_incoming(&pci_liveupdate_flb, (void **)&ser);
	if (ret)
		return 0;

	return ser->nr_devices;
}
EXPORT_SYMBOL_GPL(pci_liveupdate_incoming_nr_devices);

bool pci_liveupdate_incoming_is_preserved(struct pci_dev *dev)
{
	struct pci_ser *ser;
	int ret;

	guard(mutex)(&pci_flb_incoming_lock);

	ret = liveupdate_flb_get_incoming(&pci_liveupdate_flb, (void **)&ser);
	if (ret)
		return false;

	return pci_ser_find(ser, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_incoming_is_preserved);

void pci_liveupdate_incoming_finish(struct pci_dev *dev)
{
	struct pci_ser *ser;
	int ret;

	guard(mutex)(&pci_flb_incoming_lock);

	ret = liveupdate_flb_get_incoming(&pci_liveupdate_flb, (void **)&ser);
	if (WARN_ON_ONCE(ret))
		return;

	WARN_ON_ONCE(pci_ser_delete(ser, dev));
}
EXPORT_SYMBOL_GPL(pci_liveupdate_incoming_finish);

int pci_liveupdate_register_fh(struct liveupdate_file_handler *fh)
{
	return liveupdate_register_flb(fh, &pci_liveupdate_flb);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_register_fh);

int pci_liveupdate_unregister_fh(struct liveupdate_file_handler *fh)
{
	return liveupdate_unregister_flb(fh, &pci_liveupdate_flb);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unregister_fh);
