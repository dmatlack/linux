// SPDX-License-Identifier: GPL-2.0-only
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/align.h>
#include <linux/io.h>
#include <linux/iommufd.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/log2.h>
#include <linux/mman.h>
#include <linux/overflow.h>
#include <linux/pci_ids.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/vfio.h>

#include <uuid/uuid.h>

#include "kselftest.h"
#include <libvfio.h>

static void vfio_pci_irq_set(struct vfio_pci_device *device,
			     u32 index, u32 vector, u32 count, int *fds)
{
	size_t argsz = sizeof(struct vfio_irq_set) + sizeof(int) * count;
	struct vfio_irq_set *irq;

	irq = calloc_assert(1, argsz);
	irq->argsz = argsz;
	irq->flags = VFIO_IRQ_SET_ACTION_TRIGGER;
	irq->index = index;
	irq->start = vector;
	irq->count = count;

	if (count) {
		irq->flags |= VFIO_IRQ_SET_DATA_EVENTFD;
		memcpy(irq->data, fds, sizeof(int) * count);
	} else {
		irq->flags |= VFIO_IRQ_SET_DATA_NONE;
	}

	ioctl_assert(device->fd, VFIO_DEVICE_SET_IRQS, irq);
	free(irq);
}

void vfio_pci_irq_trigger(struct vfio_pci_device *device, u32 index, u32 vector)
{
	struct vfio_irq_set irq = {
		.argsz = sizeof(irq),
		.flags = VFIO_IRQ_SET_ACTION_TRIGGER | VFIO_IRQ_SET_DATA_NONE,
		.index = index,
		.start = vector,
		.count = 1,
	};

	ioctl_assert(device->fd, VFIO_DEVICE_SET_IRQS, &irq);
}

static void check_supported_irq_index(u32 index)
{
	/* VFIO selftests only supports MSI and MSI-x for now. */
	VFIO_ASSERT_TRUE(index == VFIO_PCI_MSI_IRQ_INDEX ||
			 index == VFIO_PCI_MSIX_IRQ_INDEX,
			 "Unsupported IRQ index: %u\n", index);
}

void vfio_pci_irq_enable(struct vfio_pci_device *device, u32 index, u32 vector,
			 int count)
{
	int i;

	check_supported_irq_index(index);

	for (i = vector; i < vector + count; i++) {
		VFIO_ASSERT_LT(device->msi_eventfds[i], 0);
		device->msi_eventfds[i] = eventfd(0, 0);
		VFIO_ASSERT_GE(device->msi_eventfds[i], 0);
	}

	vfio_pci_irq_set(device, index, vector, count, device->msi_eventfds + vector);
}

void vfio_pci_irq_disable(struct vfio_pci_device *device, u32 index)
{
	int i;

	check_supported_irq_index(index);

	for (i = 0; i < ARRAY_SIZE(device->msi_eventfds); i++) {
		if (device->msi_eventfds[i] < 0)
			continue;

		VFIO_ASSERT_EQ(close(device->msi_eventfds[i]), 0);
		device->msi_eventfds[i] = -1;
	}

	vfio_pci_irq_set(device, index, 0, 0, NULL);
}

/*
 * Re-issue VFIO_DEVICE_SET_IRQS for an already-enabled vector range using
 * the existing eventfds.  Intended for drivers that need to re-arm device
 * interrupts after a VFIO_DEVICE_RESET, which tears down the kernel-side
 * IRQ trigger but leaves user-side eventfds intact.  Recreating the
 * eventfds would invalidate any test-fixture cache of the fd, so this
 * helper deliberately preserves them.
 */
void vfio_pci_irq_reenable(struct vfio_pci_device *device, u32 index,
			   u32 vector, int count)
{
	int i;

	check_supported_irq_index(index);

	for (i = vector; i < vector + count; i++)
		VFIO_ASSERT_GE(device->msi_eventfds[i], 0,
			       "vector %d eventfd not allocated\n", i);

	vfio_pci_irq_set(device, index, vector, count, device->msi_eventfds + vector);
}

static void vfio_pci_irq_get(struct vfio_pci_device *device, u32 index,
			     struct vfio_irq_info *irq_info)
{
	irq_info->argsz = sizeof(*irq_info);
	irq_info->index = index;

	ioctl_assert(device->fd, VFIO_DEVICE_GET_IRQ_INFO, irq_info);
}

static int vfio_device_feature_ioctl(int fd, u32 flags, void *data,
				     size_t data_size)
{
	size_t argsz = sizeof(struct vfio_device_feature) + data_size;
	struct vfio_device_feature *feature;
	int ret;

	feature = calloc_assert(1, argsz);
	memcpy(feature->data, data, data_size);

	feature->argsz = argsz;
	feature->flags = flags;

	ret = ioctl(fd, VFIO_DEVICE_FEATURE, feature);
	if (ret < 0)
		ret = -errno;
	free(feature);

	return ret;
}

int __vfio_pci_dmabuf_create(struct vfio_pci_device *device, u32 region_index,
			     u64 offset, u64 length, u32 open_flags)
{
	u8 buf[sizeof(struct vfio_device_feature_dma_buf) +
	       sizeof(struct vfio_region_dma_range)] = {};
	struct vfio_device_feature_dma_buf *dma_buf = (void *)buf;

	dma_buf->region_index = region_index;
	dma_buf->open_flags = open_flags;
	dma_buf->nr_ranges = 1;
	dma_buf->dma_ranges[0].offset = offset;
	dma_buf->dma_ranges[0].length = length;

	return vfio_device_feature_ioctl(device->fd,
					 VFIO_DEVICE_FEATURE_GET |
					 VFIO_DEVICE_FEATURE_DMA_BUF,
					 buf, sizeof(buf));
}

int vfio_pci_dmabuf_create(struct vfio_pci_device *device, u32 region_index,
			   u64 offset, u64 length, u32 open_flags)
{
	int fd;

	fd = __vfio_pci_dmabuf_create(device, region_index, offset, length,
				      open_flags);
	VFIO_ASSERT_GE(fd, 0, "VFIO_DEVICE_FEATURE_DMA_BUF failed: %d\n", fd);

	return fd;
}

static void vfio_device_feature_set(int fd, u16 feature, void *data, size_t data_size)
{
	u32 flags = VFIO_DEVICE_FEATURE_SET | feature;
	int ret;

	ret = vfio_device_feature_ioctl(fd, flags, data, data_size);
	VFIO_ASSERT_EQ(ret, 0, "Failed to set feature %u\n", feature);
}

void vfio_device_set_vf_token(int fd, const char *vf_token)
{
	uuid_t token_uuid = {0};

	VFIO_ASSERT_NOT_NULL(vf_token, "vf_token is NULL");
	VFIO_ASSERT_EQ(uuid_parse(vf_token, token_uuid), 0);

	vfio_device_feature_set(fd, VFIO_DEVICE_FEATURE_PCI_VF_TOKEN,
				token_uuid, sizeof(uuid_t));
}

static void vfio_pci_region_get(struct vfio_pci_device *device, int index,
				struct vfio_region_info *info)
{
	memset(info, 0, sizeof(*info));

	info->argsz = sizeof(*info);
	info->index = index;

	ioctl_assert(device->fd, VFIO_DEVICE_GET_REGION_INFO, info);
}

static void vfio_pci_bar_map(struct vfio_pci_device *device, int index)
{
	struct vfio_pci_bar *bar = &device->bars[index];
	size_t align, size;
	int prot = 0;
	void *vaddr;

	VFIO_ASSERT_LT(index, PCI_STD_NUM_BARS);
	VFIO_ASSERT_NULL(bar->vaddr);
	VFIO_ASSERT_TRUE(bar->info.flags & VFIO_REGION_INFO_FLAG_MMAP);
	VFIO_ASSERT_TRUE(is_power_of_2(bar->info.size));

	if (bar->info.flags & VFIO_REGION_INFO_FLAG_READ)
		prot |= PROT_READ;
	if (bar->info.flags & VFIO_REGION_INFO_FLAG_WRITE)
		prot |= PROT_WRITE;

	size = bar->info.size;

	/*
	 * Align BAR mmaps to improve page fault granularity during potential
	 * subsequent IOMMU mapping of these BAR vaddr. 1G for x86 is the
	 * largest hugepage size across any architecture, so no benefit from
	 * larger alignment. BARs smaller than 1G will be aligned by their
	 * power-of-two size, guaranteeing sufficient alignment for smaller
	 * hugepages, if present.
	 */
	align = min_t(size_t, size, SZ_1G);

	vaddr = mmap_reserve(size, align, 0);
	bar->vaddr = mmap(vaddr, size, prot, MAP_SHARED | MAP_FIXED,
			  device->fd, bar->info.offset);
	VFIO_ASSERT_NE(bar->vaddr, MAP_FAILED);

	madvise(bar->vaddr, size, MADV_HUGEPAGE);
}

static void vfio_pci_bar_unmap(struct vfio_pci_device *device, int index)
{
	struct vfio_pci_bar *bar = &device->bars[index];

	VFIO_ASSERT_LT(index, PCI_STD_NUM_BARS);
	VFIO_ASSERT_NOT_NULL(bar->vaddr);

	VFIO_ASSERT_EQ(munmap(bar->vaddr, bar->info.size), 0);
	bar->vaddr = NULL;
}

static void vfio_pci_bar_unmap_all(struct vfio_pci_device *device)
{
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (device->bars[i].vaddr)
			vfio_pci_bar_unmap(device, i);
	}
}

#define PCI_DEVICE_ID_REDHAT_TEST	0x0005

struct vfio_pci_mem_bar_desc {
	u16 vendor_id;
	u16 device_id;
	u8 bar_index;
	u64 offset;
	u64 size;
};

static const struct vfio_pci_mem_bar_desc mem_bar_descs[] = {
	/* QEMU pci-testdev with membar=<size>,membar-backed=on */
	{
		.vendor_id = PCI_VENDOR_ID_REDHAT,
		.device_id = PCI_DEVICE_ID_REDHAT_TEST,
		.bar_index = 2,
	},
};

static bool vfio_pci_mem_bar_verify(struct vfio_pci_device *device, void *vaddr)
{
	const u64 pattern = 0x55aa55aa55aa55aaULL;
	u16 orig_cmd;
	u64 orig;
	bool ok;

	orig_cmd = vfio_pci_config_readw(device, PCI_COMMAND);
	if (!(orig_cmd & PCI_COMMAND_MEMORY))
		vfio_pci_config_writew(device, PCI_COMMAND,
				       orig_cmd | PCI_COMMAND_MEMORY);

	orig = readq(vaddr);
	writeq(pattern, vaddr);
	ok = (readq(vaddr) == pattern);
	if (ok) {
		writeq(~pattern, vaddr);
		ok = (readq(vaddr) == ~pattern);
	}
	writeq(orig, vaddr);

	if (!(orig_cmd & PCI_COMMAND_MEMORY))
		vfio_pci_config_writew(device, PCI_COMMAND, orig_cmd);

	return ok;
}

bool vfio_pci_find_mem_bar(struct vfio_pci_device *device, u64 min_size,
			   struct vfio_pci_mem_bar *mem_bar)
{
	const u32 rw_mmap_flags = VFIO_REGION_INFO_FLAG_MMAP |
				  VFIO_REGION_INFO_FLAG_READ |
				  VFIO_REGION_INFO_FLAG_WRITE;
	size_t i;

	min_size = max_t(u64, min_size, sizeof(u64));

	for (i = 0; i < ARRAY_SIZE(mem_bar_descs); i++) {
		const struct vfio_pci_mem_bar_desc *desc = &mem_bar_descs[i];
		struct vfio_pci_bar *bar;
		u64 size;

		if (!vfio_pci_device_match(device, desc->vendor_id,
					   desc->device_id))
			continue;

		VFIO_ASSERT_LT(desc->bar_index, PCI_STD_NUM_BARS);
		bar = &device->bars[desc->bar_index];

		if ((bar->info.flags & rw_mmap_flags) != rw_mmap_flags)
			continue;

		VFIO_ASSERT_NOT_NULL(bar->vaddr);

		if (bar->info.size <= desc->offset)
			continue;

		size = bar->info.size - desc->offset;
		if (desc->size)
			size = min(size, desc->size);

		if (size < min_size)
			continue;

		if (!vfio_pci_mem_bar_verify(device, bar->vaddr + desc->offset))
			continue;

		*mem_bar = (struct vfio_pci_mem_bar){
			.bar = bar,
			.vaddr = bar->vaddr + desc->offset,
			.offset = desc->offset,
			.size = size,
		};
		return true;
	}

	return false;
}

static bool vfio_pci_has_p2p_path(struct vfio_pci_device *initiator,
				  struct vfio_pci_device *target)
{
	/*
	 * TODO: Check PCI topology / ACS / host bridge support to verify that
	 * P2P DMA transactions from @initiator can reach @target.
	 */
	return initiator != target;
}

bool vfio_pci_find_p2p_pair(struct vfio_pci_device **devices, int nr_devices,
			    u64 min_bar_size, struct vfio_pci_p2p_pair *pair)
{
	struct vfio_pci_mem_bar mem_bar;
	int i, j;

	for (i = 0; i < nr_devices; i++) {
		struct vfio_pci_device *initiator = devices[i];
		const struct vfio_pci_driver_ops *ops = initiator->driver.ops;

		if (!ops || !ops->memcpy_start || !ops->memcpy_wait)
			continue;

		for (j = 0; j < nr_devices; j++) {
			struct vfio_pci_device *target = devices[j];

			if (i == j)
				continue;

			if (!vfio_pci_has_p2p_path(initiator, target))
				continue;

			if (!vfio_pci_find_mem_bar(target, min_bar_size,
						   &mem_bar))
				continue;

			*pair = (struct vfio_pci_p2p_pair){
				.initiator = initiator,
				.target = target,
				.mem_bar = mem_bar,
			};
			return true;
		}
	}

	return false;
}

void vfio_pci_config_access(struct vfio_pci_device *device, bool write,
			    size_t config, size_t size, void *data)
{
	struct vfio_region_info *config_space = &device->config_space;
	int ret;

	if (write)
		ret = pwrite(device->fd, data, size, config_space->offset + config);
	else
		ret = pread(device->fd, data, size, config_space->offset + config);

	VFIO_ASSERT_EQ(ret, size, "Failed to %s PCI config space: 0x%lx\n",
		       write ? "write to" : "read from", config);
}

int __vfio_pci_device_reset(struct vfio_pci_device *device)
{
	if (ioctl(device->fd, VFIO_DEVICE_RESET, NULL))
		return -errno;

	return 0;
}

void vfio_pci_device_reset(struct vfio_pci_device *device)
{
	int retries = 20;
	int r;

	do {
		r = __vfio_pci_device_reset(device);
		if (r == -EAGAIN)
			usleep(10000);
	} while (r == -EAGAIN && retries-- > 0);

	VFIO_ASSERT_EQ(r, 0, "ioctl(device->fd, VFIO_DEVICE_RESET) failed\n");
}

void vfio_pci_group_setup(struct vfio_pci_device *device, const char *bdf)
{
	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status),
	};
	char group_path[32];
	int group;

	group = sysfs_iommu_group_get(bdf);
	snprintf_assert(group_path, sizeof(group_path), "/dev/vfio/%d", group);

	device->group_fd = open(group_path, O_RDWR);
	VFIO_ASSERT_GE(device->group_fd, 0, "open(%s) failed\n", group_path);

	ioctl_assert(device->group_fd, VFIO_GROUP_GET_STATUS, &group_status);
	VFIO_ASSERT_TRUE(group_status.flags & VFIO_GROUP_FLAGS_VIABLE);

	ioctl_assert(device->group_fd, VFIO_GROUP_SET_CONTAINER, &device->iommu->container_fd);
}

void __vfio_pci_group_get_device_fd(struct vfio_pci_device *device,
				    const char *bdf, const char *vf_token)
{
	char arg[64];

	/*
	 * If a vf_token exists, argument to VFIO_GROUP_GET_DEVICE_FD
	 * will be in the form of the following example:
	 * "0000:04:10.0 vf_token=bd8d9d2b-5a5f-4f5a-a211-f591514ba1f3"
	 */
	if (vf_token)
		snprintf_assert(arg, ARRAY_SIZE(arg), "%s vf_token=%s", bdf, vf_token);
	else
		snprintf_assert(arg, ARRAY_SIZE(arg), "%s", bdf);

	device->fd = ioctl(device->group_fd, VFIO_GROUP_GET_DEVICE_FD, arg);
}

static void vfio_pci_group_get_device_fd(struct vfio_pci_device *device,
					 const char *bdf, const char *vf_token)
{
	__vfio_pci_group_get_device_fd(device, bdf, vf_token);
	VFIO_ASSERT_GE(device->fd, 0);
}

void vfio_container_set_iommu(struct vfio_pci_device *device)
{
	struct iommu *iommu = device->iommu;
	unsigned long iommu_type = iommu->mode->iommu_type;
	int ret;

	ret = ioctl(iommu->container_fd, VFIO_CHECK_EXTENSION, iommu_type);
	VFIO_ASSERT_GT(ret, 0, "VFIO IOMMU type %lu not supported\n", iommu_type);

	/*
	 * Allow multiple threads to race to set the IOMMU type on the
	 * container. The first will succeed and the rest should fail
	 * because the IOMMU type is already set.
	 */
	(void)ioctl(iommu->container_fd, VFIO_SET_IOMMU, (void *)iommu_type);
}

static void vfio_pci_container_setup(struct vfio_pci_device *device,
				     const char *bdf, const char *vf_token)
{
	vfio_pci_group_setup(device, bdf);
	vfio_container_set_iommu(device);
	vfio_pci_group_get_device_fd(device, bdf, vf_token);
}

static void vfio_pci_device_setup(struct vfio_pci_device *device)
{
	int i;

	device->info.argsz = sizeof(device->info);
	ioctl_assert(device->fd, VFIO_DEVICE_GET_INFO, &device->info);

	vfio_pci_region_get(device, VFIO_PCI_CONFIG_REGION_INDEX, &device->config_space);

	/* Sanity check VFIO does not advertise mmap for config space */
	VFIO_ASSERT_TRUE(!(device->config_space.flags & VFIO_REGION_INFO_FLAG_MMAP),
			 "PCI config space should not support mmap()\n");

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		struct vfio_pci_bar *bar = device->bars + i;

		vfio_pci_region_get(device, i, &bar->info);
		if (bar->info.flags & VFIO_REGION_INFO_FLAG_MMAP)
			vfio_pci_bar_map(device, i);
	}

	vfio_pci_irq_get(device, VFIO_PCI_MSI_IRQ_INDEX, &device->msi_info);
	vfio_pci_irq_get(device, VFIO_PCI_MSIX_IRQ_INDEX, &device->msix_info);

	for (i = 0; i < ARRAY_SIZE(device->msi_eventfds); i++)
		device->msi_eventfds[i] = -1;
}

const char *vfio_pci_get_cdev_path(const char *bdf)
{
	char dir_path[PATH_MAX];
	struct dirent *entry;
	char *cdev_path;
	DIR *dir;

	cdev_path = calloc_assert(PATH_MAX, 1);

	snprintf_assert(dir_path, sizeof(dir_path), "/sys/bus/pci/devices/%s/vfio-dev/", bdf);

	dir = opendir(dir_path);
	VFIO_ASSERT_NOT_NULL(dir, "Failed to open directory %s\n", dir_path);

	while ((entry = readdir(dir)) != NULL) {
		/* Find the file that starts with "vfio" */
		if (strncmp("vfio", entry->d_name, 4))
			continue;

		snprintf_assert(cdev_path, PATH_MAX, "/dev/vfio/devices/%s", entry->d_name);
		break;
	}

	VFIO_ASSERT_NE(cdev_path[0], 0, "Failed to find vfio cdev file.\n");
	VFIO_ASSERT_EQ(closedir(dir), 0);

	return cdev_path;
}

int __vfio_device_bind_iommufd(int device_fd, int iommufd, const char *vf_token)
{
	struct vfio_device_bind_iommufd args = {
		.argsz = sizeof(args),
		.iommufd = iommufd,
	};
	uuid_t token_uuid;

	if (vf_token) {
		VFIO_ASSERT_EQ(uuid_parse(vf_token, token_uuid), 0);
		args.flags |= VFIO_DEVICE_BIND_FLAG_TOKEN;
		args.token_uuid_ptr = (u64)token_uuid;
	}

	if (ioctl(device_fd, VFIO_DEVICE_BIND_IOMMUFD, &args))
		return -errno;

	return 0;
}

static void vfio_device_bind_iommufd(int device_fd, int iommufd,
				     const char *vf_token)
{
	int ret = __vfio_device_bind_iommufd(device_fd, iommufd, vf_token);

	VFIO_ASSERT_EQ(ret, 0, "Failed VFIO_DEVICE_BIND_IOMMUFD ioctl\n");
}

static void vfio_device_attach_iommufd_pt(int device_fd, u32 pt_id)
{
	struct vfio_device_attach_iommufd_pt args = {
		.argsz = sizeof(args),
		.pt_id = pt_id,
	};

	ioctl_assert(device_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &args);
}

void vfio_pci_cdev_open(struct vfio_pci_device *device, const char *bdf)
{
	const char *cdev_path = vfio_pci_get_cdev_path(bdf);

	device->fd = open(cdev_path, O_RDWR);
	VFIO_ASSERT_GE(device->fd, 0);
	free((void *)cdev_path);
}

static void vfio_pci_iommufd_setup(struct vfio_pci_device *device,
				   const char *bdf, const char *vf_token)
{
	vfio_pci_cdev_open(device, bdf);
	vfio_device_bind_iommufd(device->fd, device->iommu->iommufd, vf_token);
	vfio_device_attach_iommufd_pt(device->fd, device->iommu->ioas_id);
}

struct vfio_pci_device *vfio_pci_device_alloc(const char *bdf, struct iommu *iommu)
{
	struct vfio_pci_device *device;

	device = calloc_assert(1, sizeof(*device));

	VFIO_ASSERT_NOT_NULL(iommu);
	device->iommu = iommu;
	device->bdf = bdf;

	return device;
}

void vfio_pci_device_free(struct vfio_pci_device *device)
{
	free(device);
}

struct vfio_pci_device *vfio_pci_device_init(const char *bdf, struct iommu *iommu)
{
	struct vfio_pci_device *device;

	device = vfio_pci_device_alloc(bdf, iommu);

	if (iommu->mode->container_path)
		vfio_pci_container_setup(device, bdf, NULL);
	else
		vfio_pci_iommufd_setup(device, bdf, NULL);

	vfio_pci_device_setup(device);
	vfio_pci_driver_probe(device);

	return device;
}

void vfio_pci_device_cleanup(struct vfio_pci_device *device)
{
	int i;

	if (device->driver.initialized)
		vfio_pci_driver_remove(device);

	vfio_pci_bar_unmap_all(device);

	VFIO_ASSERT_EQ(close(device->fd), 0);

	for (i = 0; i < ARRAY_SIZE(device->msi_eventfds); i++) {
		if (device->msi_eventfds[i] < 0)
			continue;

		VFIO_ASSERT_EQ(close(device->msi_eventfds[i]), 0);
	}

	if (device->group_fd)
		VFIO_ASSERT_EQ(close(device->group_fd), 0);

	vfio_pci_device_free(device);
}
