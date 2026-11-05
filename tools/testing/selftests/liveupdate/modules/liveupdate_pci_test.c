// SPDX-License-Identifier: GPL-2.0
/*
 * Test-only PCI driver for the PCI Live Update selftests.
 *
 * Binds to Intel 82576 (IGB) devices and lets userspace preserve them across a
 * Live Update. Userspace gets a file for a bound device from the
 * /dev/liveupdate_pci_test misc device, and preserves that file in a Live
 * Update session. Other than enabling the device and bus mastering, the driver
 * leaves the device alone so that the selftests can check how the PCI core
 * treats it.
 *
 * Like VFIO, the driver captures each device's state with
 * pci_store_saved_state() when it first probes the device, and preserves that
 * state along with the device, so that the selftests can check that the PCI
 * core hands it to the next kernel intact.
 *
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/pci.h>
#include <linux/liveupdate.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_liveupdate.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "../pci/liveupdate_pci_test.h"

#define LUPT_FILE_NAME "[liveupdate_pci_test]"

struct lupt_device {
	bool incoming_at_probe;
	/* Protected by the device lock */
	struct pci_saved_state *saved_state;
};

/*
 * Preserved for each device, at the physical address in the file's
 * serialized_data.
 */
struct lupt_ser {
	u32 domain;
	u16 bdf;
	u16 reserved;
	u64 saved_state;	/* From pci_liveupdate_preserve_saved_state() */
} __packed;

static_assert(LIVEUPDATE_PCI_TEST_CFG_DWORDS == PCI_SAVED_STATE_SER_DWORDS);
static_assert(LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD ==
	      PCI_SAVED_STATE_SER_DWORDS - 1);

static struct pci_driver lupt_driver;

/* Returns a reference to a device bound to this driver, or NULL. */
static struct pci_dev *lupt_get_bound_device(u32 domain, u16 bdf)
{
	struct pci_dev *pdev;

	pdev = pci_get_domain_bus_and_slot(domain, PCI_BUS_NUM(bdf), bdf & 0xff);
	if (!pdev)
		return NULL;

	if (READ_ONCE(pdev->driver) != &lupt_driver) {
		pci_dev_put(pdev);
		return NULL;
	}

	return pdev;
}

/* Returns the driver data of @pdev with its device lock held, or NULL. */
static struct lupt_device *lupt_lock_device(struct pci_dev *pdev)
{
	device_lock(&pdev->dev);
	if (pdev->driver == &lupt_driver)
		return pci_get_drvdata(pdev);

	device_unlock(&pdev->dev);
	return NULL;
}

static void lupt_unlock_device(struct pci_dev *pdev)
{
	device_unlock(&pdev->dev);
}

static long lupt_get_saved_state(struct pci_dev *pdev, void __user *arg)
{
	struct liveupdate_pci_test_saved_state *out;
	struct pci_saved_state_ser *ser;
	struct lupt_device *ldev;
	unsigned int i, nr = 0;
	long ret = -ENOENT;
	u64 phys;

	out = kzalloc_obj(*out);
	if (!out)
		return -ENOMEM;

	/*
	 * struct pci_saved_state is opaque, so read it back through the form in
	 * which it is preserved across Live Update.
	 */
	ldev = lupt_lock_device(pdev);
	if (!ldev) {
		ret = -ENODEV;
		goto out;
	}
	if (ldev->saved_state)
		ret = pci_liveupdate_preserve_saved_state(ldev->saved_state, &phys);
	lupt_unlock_device(pdev);
	if (ret)
		goto out;

	ser = phys_to_virt(phys);
	for (i = 0; i < ARRAY_SIZE(ser->dword_map); i++) {
		out->dword_map[i] = ser->dword_map[i];
		nr += hweight64(ser->dword_map[i]);
	}
	memcpy(out->dword_val, ser->dword_val, nr * sizeof(u32));
	pci_liveupdate_unpreserve_saved_state(phys);

	ret = copy_to_user(arg, out, sizeof(*out)) ? -EFAULT : 0;
out:
	kfree(out);
	return ret;
}

static long lupt_restore_saved_state(struct pci_dev *pdev)
{
	struct lupt_device *ldev;
	long ret = -ENOENT;

	ldev = lupt_lock_device(pdev);
	if (!ldev)
		return -ENODEV;

	if (ldev->saved_state) {
		ret = pci_load_saved_state(pdev, ldev->saved_state);
		if (!ret)
			pci_restore_state(pdev);
	}

	lupt_unlock_device(pdev);
	return ret;
}

static long lupt_device_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct liveupdate_pci_test_device dev = {};
	struct pci_dev *pdev = file->private_data;
	struct lupt_device *ldev;

	if (cmd == LIVEUPDATE_PCI_TEST_GET_SAVED_STATE)
		return lupt_get_saved_state(pdev, (void __user *)arg);

	if (cmd == LIVEUPDATE_PCI_TEST_RESTORE_SAVED_STATE)
		return lupt_restore_saved_state(pdev);

	if (cmd != LIVEUPDATE_PCI_TEST_GET_STATUS)
		return -ENOTTY;

	dev.domain = pci_domain_nr(pdev->bus);
	dev.bdf = pci_dev_id(pdev);

	device_lock(&pdev->dev);
	if (pdev->driver == &lupt_driver) {
		ldev = pci_get_drvdata(pdev);
		if (ldev->incoming_at_probe)
			dev.flags |= LIVEUPDATE_PCI_TEST_INCOMING_AT_PROBE;
	}
	device_unlock(&pdev->dev);

	if (pci_liveupdate_is_incoming(pdev))
		dev.flags |= LIVEUPDATE_PCI_TEST_INCOMING;

	if (copy_to_user((void __user *)arg, &dev, sizeof(dev)))
		return -EFAULT;

	return 0;
}

static int lupt_device_release(struct inode *inode, struct file *file)
{
	pci_dev_put(file->private_data);
	return 0;
}

static const struct file_operations lupt_device_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = lupt_device_ioctl,
	.release = lupt_device_release,
};

static long lupt_ctl_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct liveupdate_pci_test_device dev;
	struct pci_dev *pdev;
	int fd;

	if (cmd != LIVEUPDATE_PCI_TEST_GET_DEVICE_FD)
		return -ENOTTY;

	if (copy_from_user(&dev, (void __user *)arg, sizeof(dev)))
		return -EFAULT;

	pdev = lupt_get_bound_device(dev.domain, dev.bdf);
	if (!pdev)
		return -ENODEV;

	/* On success, the reference to pdev is dropped when the file is released. */
	fd = anon_inode_getfd(LUPT_FILE_NAME, &lupt_device_fops, pdev,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0)
		pci_dev_put(pdev);

	return fd;
}

static const struct file_operations lupt_ctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = lupt_ctl_ioctl,
};

static struct miscdevice lupt_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "liveupdate_pci_test",
	.fops = &lupt_ctl_fops,
};

static bool lupt_can_preserve(struct liveupdate_file_handler *handler,
			      struct file *file)
{
	return file->f_op == &lupt_device_fops;
}

/*
 * Add LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD to the preserved saved state at
 * *@phys, to stand in for a register that the previous kernel saved but the
 * next kernel does not restore. It is the last DWORD of configuration space,
 * so its value goes after all the others.
 */
static int lupt_add_unreserved_dword(u64 *phys)
{
	struct pci_saved_state_ser *old = phys_to_virt(*phys), *new;
	DECLARE_BITMAP(map, PCI_SAVED_STATE_SER_DWORDS);
	unsigned int nr;

	bitmap_from_arr64(map, old->dword_map, PCI_SAVED_STATE_SER_DWORDS);
	if (test_bit(LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD, map))
		return -EEXIST;

	nr = bitmap_weight(map, PCI_SAVED_STATE_SER_DWORDS);
	new = kho_alloc_preserve(struct_size(new, dword_val, nr + 1));
	if (IS_ERR(new))
		return PTR_ERR(new);

	__set_bit(LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD, map);
	bitmap_to_arr64(new->dword_map, map, PCI_SAVED_STATE_SER_DWORDS);
	memcpy(new->dword_val, old->dword_val, nr * sizeof(u32));
	new->dword_val[nr] = LIVEUPDATE_PCI_TEST_UNRESERVED_VALUE;

	pci_liveupdate_unpreserve_saved_state(*phys);
	*phys = virt_to_phys(new);
	return 0;
}

static int lupt_preserve_saved_state(struct pci_dev *pdev, u64 *phys)
{
	struct lupt_device *ldev;
	int ret = -ENOENT;

	ldev = lupt_lock_device(pdev);
	if (!ldev)
		return -ENODEV;

	if (ldev->saved_state)
		ret = pci_liveupdate_preserve_saved_state(ldev->saved_state, phys);
	lupt_unlock_device(pdev);
	if (ret)
		return ret;

	ret = lupt_add_unreserved_dword(phys);
	if (ret)
		pci_liveupdate_unpreserve_saved_state(*phys);

	return ret;
}

static int lupt_preserve(struct liveupdate_file_op_args *args)
{
	struct pci_dev *pdev = args->file->private_data;
	struct lupt_ser *ser;
	int ret;

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	ser->domain = pci_domain_nr(pdev->bus);
	ser->bdf = pci_dev_id(pdev);

	ret = lupt_preserve_saved_state(pdev, &ser->saved_state);
	if (ret)
		goto err_free_ser;

	ret = pci_liveupdate_preserve(pdev);
	if (ret)
		goto err_free_saved_state;

	args->serialized_data = virt_to_phys(ser);
	return 0;

err_free_saved_state:
	pci_liveupdate_unpreserve_saved_state(ser->saved_state);
err_free_ser:
	kho_unpreserve_free(ser);
	return ret;
}

static void lupt_unpreserve(struct liveupdate_file_op_args *args)
{
	struct lupt_ser *ser = phys_to_virt(args->serialized_data);

	pci_liveupdate_unpreserve(args->file->private_data);
	pci_liveupdate_unpreserve_saved_state(ser->saved_state);
	kho_unpreserve_free(ser);
}

static int lupt_retrieve(struct liveupdate_file_op_args *args)
{
	struct lupt_ser *ser = phys_to_virt(args->serialized_data);
	struct pci_saved_state *saved_state;
	struct lupt_device *ldev;
	struct pci_dev *pdev;
	struct file *file;

	/* Userspace must load this driver before retrieving the device. */
	pdev = lupt_get_bound_device(ser->domain, ser->bdf);
	if (!pdev)
		return -ENODEV;

	file = anon_inode_getfile(LUPT_FILE_NAME, &lupt_device_fops, pdev, O_RDWR);
	if (IS_ERR(file)) {
		pci_dev_put(pdev);
		return PTR_ERR(file);
	}

	saved_state = pci_liveupdate_retrieve_saved_state(pdev, ser->saved_state);
	if (IS_ERR(saved_state)) {
		fput(file);
		return PTR_ERR(saved_state);
	}

	kho_restore_free(ser);

	ldev = lupt_lock_device(pdev);
	if (ldev) {
		swap(ldev->saved_state, saved_state);
		lupt_unlock_device(pdev);
	}
	kfree(saved_state);

	args->file = file;
	return 0;
}

static bool lupt_can_finish(struct liveupdate_file_op_args *args)
{
	return args->retrieve_status > 0;
}

static void lupt_finish(struct liveupdate_file_op_args *args)
{
	pci_liveupdate_finish(args->file->private_data);
}

static const struct liveupdate_file_ops lupt_file_ops = {
	.can_preserve = lupt_can_preserve,
	.preserve = lupt_preserve,
	.unpreserve = lupt_unpreserve,
	.retrieve = lupt_retrieve,
	.can_finish = lupt_can_finish,
	.finish = lupt_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler lupt_fh = {
	.ops = &lupt_file_ops,
	.compatible = "liveupdate-pci-test-v1",
};

static int lupt_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct lupt_device *ldev;
	int ret;

	ldev = devm_kzalloc(&pdev->dev, sizeof(*ldev), GFP_KERNEL);
	if (!ldev)
		return -ENOMEM;

	ldev->incoming_at_probe = pci_liveupdate_is_incoming(pdev);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	/*
	 * Capture the state the device was probed in, as VFIO does when a
	 * device is first opened. A preserved device gets the state its
	 * previous kernel captured when the device is retrieved instead.
	 */
	if (!ldev->incoming_at_probe) {
		pci_save_state(pdev);
		ldev->saved_state = pci_store_saved_state(pdev);
		if (!ldev->saved_state)
			return -ENOMEM;
	}

	pci_set_drvdata(pdev, ldev);

	pci_info(pdev, "Bound to %s device\n",
		 ldev->incoming_at_probe ? "incoming-preserved" : "non-preserved");
	return 0;
}

static void lupt_remove(struct pci_dev *pdev)
{
	struct lupt_device *ldev = pci_get_drvdata(pdev);

	kfree(ldev->saved_state);
}

static const struct pci_device_id lupt_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x10c9) },	/* 82576 (QEMU igb) */
	{}
};

static struct pci_driver lupt_driver = {
	.name = "liveupdate_pci_test",
	.id_table = lupt_ids,
	.probe = lupt_probe,
	.remove = lupt_remove,
};

static int __init lupt_init(void)
{
	int ret;

	ret = liveupdate_register_file_handler(&lupt_fh);
	if (ret)
		return ret;

	ret = pci_liveupdate_register_flb(&lupt_fh);
	if (ret)
		goto err_unregister_fh;

	ret = misc_register(&lupt_miscdev);
	if (ret)
		goto err_unregister_flb;

	ret = pci_register_driver(&lupt_driver);
	if (ret)
		goto err_deregister_misc;

	return 0;

err_deregister_misc:
	misc_deregister(&lupt_miscdev);
err_unregister_flb:
	pci_liveupdate_unregister_flb(&lupt_fh);
err_unregister_fh:
	liveupdate_unregister_file_handler(&lupt_fh);
	return ret;
}
module_init(lupt_init);

static void __exit lupt_exit(void)
{
	pci_unregister_driver(&lupt_driver);
	misc_deregister(&lupt_miscdev);
	pci_liveupdate_unregister_flb(&lupt_fh);
	liveupdate_unregister_file_handler(&lupt_fh);
}
module_exit(lupt_exit);

MODULE_DESCRIPTION("Test-only PCI driver for the PCI Live Update selftests");
MODULE_LICENSE("GPL");
