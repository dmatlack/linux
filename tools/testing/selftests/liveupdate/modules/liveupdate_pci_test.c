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
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/anon_inodes.h>
#include <linux/fs.h>
#include <linux/liveupdate.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_liveupdate.h>
#include <linux/uaccess.h>

#include "../pci/liveupdate_pci_test.h"

#define LUPT_FILE_NAME "[liveupdate_pci_test]"

struct lupt_device {
	bool incoming_at_probe;
};

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

static long lupt_device_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct liveupdate_pci_test_device dev = {};
	struct pci_dev *pdev = file->private_data;
	struct lupt_device *ldev;

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

static int lupt_preserve(struct liveupdate_file_op_args *args)
{
	struct pci_dev *pdev = args->file->private_data;
	int ret;

	ret = pci_liveupdate_preserve(pdev);
	if (ret)
		return ret;

	args->serialized_data = (u64)pci_domain_nr(pdev->bus) << 16 | pci_dev_id(pdev);
	return 0;
}

static void lupt_unpreserve(struct liveupdate_file_op_args *args)
{
	pci_liveupdate_unpreserve(args->file->private_data);
}

static int lupt_retrieve(struct liveupdate_file_op_args *args)
{
	u32 domain = args->serialized_data >> 16;
	u16 bdf = args->serialized_data & 0xffff;
	struct pci_dev *pdev;
	struct file *file;

	/* Userspace must load this driver before retrieving the device. */
	pdev = lupt_get_bound_device(domain, bdf);
	if (!pdev)
		return -ENODEV;

	file = anon_inode_getfile(LUPT_FILE_NAME, &lupt_device_fops, pdev, O_RDWR);
	if (IS_ERR(file)) {
		pci_dev_put(pdev);
		return PTR_ERR(file);
	}

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
	pci_set_drvdata(pdev, ldev);

	pci_info(pdev, "Bound to %s device\n",
		 ldev->incoming_at_probe ? "incoming-preserved" : "non-preserved");
	return 0;
}

static const struct pci_device_id lupt_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x10c9) },	/* 82576 (QEMU igb) */
	{}
};

static struct pci_driver lupt_driver = {
	.name = "liveupdate_pci_test",
	.id_table = lupt_ids,
	.probe = lupt_probe,
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
