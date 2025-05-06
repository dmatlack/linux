// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"
#include "apic.h"
#include "processor.h"

#include <pthread.h>
#include <time.h>
#include <linux/vfio.h>
#include <linux/sizes.h>

#include <vfio_util.h>

static bool guest_ready_for_irq;
static bool guest_received_irq;

#define TIMEOUT_NS (10ULL * 1000 * 1000 * 1000)

static void guest_enable_interrupts(void)
{
	x2apic_enable();
	asm volatile ("sti");
}

void kvm_route_msi(struct kvm_vm *vm, u32 gsi, struct kvm_vcpu *vcpu, u8 vector)
{
	struct kvm_irq_routing_entry entry = {
		.gsi = gsi,
		.type = KVM_IRQ_ROUTING_MSI,
		.u.msi.address_lo = 0xFEE00000 | (vcpu->id << 12),
		.u.msi.data = vector,
	};

	kvm_route_gsi(vm, &entry);
}

static void guest_irq_handler(struct ex_regs *regs)
{
	WRITE_ONCE(guest_received_irq, true);
	GUEST_DONE();
}

static void guest_code(void)
{
	guest_enable_interrupts();
	WRITE_ONCE(guest_ready_for_irq, true);

	for (;;)
		continue;
}

void *vcpu_thread_main(void *arg)
{
	struct kvm_vcpu *vcpu = arg;
	struct ucall uc;

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(UCALL_DONE, get_ucall(vcpu, &uc));

	return NULL;
}

static void help(const char *name)
{
	printf("Usage: %s [-i iommu_mode] [-d] segment:bus:device.function\n", name);
	printf("  -d: Send a real MSI from the device, rather than synthesizing\n"
	       "      an eventfd signal from VFIO. Note that this option requires\n"
	       "      a VFIO selftests driver that supports the device.\n");
	iommu_mode_help("-i");
	exit(1);
}

static int setup_msi(struct vfio_pci_device *device, bool use_device_msi)
{
	const int flags = MAP_SHARED | MAP_ANONYMOUS;
	const int prot = PROT_READ | PROT_WRITE;
	struct vfio_dma_region *region;

	if (use_device_msi) {
		/* A driver is required to generate an MSI. */
		TEST_REQUIRE(device->driver.ops);

		/* Set up a DMA-able region for the driver to use. */
		region = &device->driver.region;
		region->iova = 0;
		region->size = SZ_2M;
		region->vaddr = mmap(NULL, region->size, prot, flags, -1, 0);
		TEST_ASSERT(region->vaddr != MAP_FAILED, "mmap() failed\n");
		vfio_pci_dma_map(device, region);

		vfio_pci_driver_init(device);

		return device->driver.msi;
	}

	TEST_REQUIRE(device->msix_info.count > 0);
	vfio_pci_msix_enable(device, 0, 1);
	return 0;
}

static void send_msi(struct vfio_pci_device *device, bool use_device_msi, int msi)
{
	if (use_device_msi) {
		printf("Sending MSI %d from the device\n", msi);
		TEST_ASSERT_EQ(msi, device->driver.msi);
		vfio_pci_driver_send_msi(device);
	} else {
		printf("Notifying the eventfd for MSI %d from VFIO\n", msi);
		vfio_pci_irq_trigger(device, VFIO_PCI_MSIX_IRQ_INDEX, msi);
	}
}

int main(int argc, char **argv)
{
	/* Random non-reserved vector and GSI to use for the device IRQ */
	const u8 vector = 0xe0;
	const u32 gsi = 32;

	struct timespec start, elapsed;
	struct vfio_pci_device *device;
	const char *iommu_mode = NULL;
	bool use_device_msi = false;
	struct kvm_vcpu *vcpu;
	pthread_t vcpu_thread;
	struct kvm_vm *vm;
	int msi;
	int c;

	while ((c = getopt(argc, argv, "i:d")) != -1) {
		switch (c) {
		case 'i':
			iommu_mode = optarg;
			break;
		case 'd':
			use_device_msi = true;
			break;
		default:
			help(argv[0]);
		}
	}

	if (optind >= argc)
		help(argv[0]);

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	vm_install_exception_handler(vm, vector, guest_irq_handler);

	device = vfio_pci_device_init(argv[optind], iommu_mode);
	msi = setup_msi(device, use_device_msi);

	kvm_add_irqfd(vm, gsi, device->msi_eventfds[msi]);
	kvm_route_msi(vm, gsi, vcpu, vector);

	pthread_create(&vcpu_thread, NULL, vcpu_thread_main, vcpu);

	while (!READ_ONCE(guest_ready_for_irq))
		sync_global_from_guest(vm, guest_ready_for_irq);

	send_msi(device, use_device_msi, msi);

	clock_gettime(CLOCK_MONOTONIC, &start);

	while (!READ_ONCE(guest_received_irq)) {
		elapsed = timespec_elapsed(start);
		TEST_ASSERT(timespec_to_ns(elapsed) < TIMEOUT_NS, "vCPU never received IRQ\n");
		sync_global_from_guest(vm, guest_received_irq);
	}

	pthread_join(vcpu_thread, NULL);
	vfio_pci_device_cleanup(device);

	return 0;
}
