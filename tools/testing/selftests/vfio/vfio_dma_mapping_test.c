// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/limits.h>
#include <linux/mman.h>
#include <linux/sizes.h>
#include <linux/vfio.h>

#include <vfio_util.h>

#include "../kselftest_harness.h"

static struct {
	u64 size;
	u64 iova;
	int mmap_flags;
	const char *bdf;
} test_config;

FIXTURE(vfio_dma_mapping_test)
{
	struct vfio_pci_device *device;
};

FIXTURE_SETUP(vfio_dma_mapping_test)
{
	self->device = vfio_pci_device_init(test_config.bdf, VFIO_TYPE1_IOMMU);
}

FIXTURE_TEARDOWN(vfio_dma_mapping_test)
{
	vfio_pci_device_cleanup(self->device);
}

TEST_F(vfio_dma_mapping_test, dma_map_unmap)
{
	void *mem;

	mem = mmap(NULL, test_config.size, PROT_READ | PROT_WRITE,
		   test_config.mmap_flags, -1, 0);
	ASSERT_NE(mem, MAP_FAILED);

	vfio_pci_dma_map(self->device, test_config.iova, test_config.size, mem);
	printf("Mapped HVA %p (size 0x%lx) at IOVA 0x%lx\n", mem,
	       test_config.size, test_config.iova);

	vfio_pci_dma_unmap(self->device, test_config.iova, test_config.size);

	ASSERT_TRUE(!munmap(mem, test_config.size));
}

static void help(const char *name)
{
	printf("Usage: %s [-b backing_src] segment:bus:device.function\n"
	       "  -b: Which backing memory to use (default: anonymous)\n"
	       "\n"
	       "      anonymous\n"
	       "      anonymous_hugetlb_2mb\n"
	       "      anonymous_hugetlb_1gb\n",
	       name);
	exit(1);
}

static int set_backing_src(const char *backing_src)
{
	if (!backing_src)
		return 1;

	test_config.mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;

	if (!strcmp(backing_src, "anonymous")) {
		test_config.size = SZ_4K;
		return 0;
	} else if (!strcmp(backing_src, "anonymous_hugetlb_2mb")) {
		test_config.size = SZ_2M;
		test_config.mmap_flags |= MAP_HUGETLB | MAP_HUGE_2MB;
		return 0;
	} else if (!strcmp(backing_src, "anonymous_hugetlb_1gb")) {
		test_config.size = SZ_1G;
		test_config.mmap_flags |= MAP_HUGETLB | MAP_HUGE_1GB;
		return 0;
	}

	fprintf(stderr, "Unrecognized value for -b: %s\n", backing_src);
	return 1;
}

int main(int argc, char *argv[])
{
	const char *backing_src = "anonymous";
	int c;

	while ((c = getopt(argc, argv, "b:")) != -1) {
		switch (c) {
		case 'b':
			backing_src = optarg;
			break;
		default:
			help(argv[0]);
		}
	}

	if (set_backing_src(backing_src))
		help(argv[0]);

	if (optind >= argc)
		help(argv[0]);

	test_config.bdf = argv[optind];

	return test_harness_run(0, NULL);
}
