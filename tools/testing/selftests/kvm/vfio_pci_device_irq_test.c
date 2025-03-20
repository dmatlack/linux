// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"
#include "apic.h"
#include "processor.h"

#include <pthread.h>
#include <time.h>
#include <linux/vfio.h>
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
	printf("Usage: %s [-i iommu_mode] [segment:bus:device.function]\n", name);
	exit(KSFT_FAIL);
}

int main(int argc, char **argv)
{
	/* Random non-reserved vector and GSI to use for the device IRQ */
	const u8 vector = 0xe0;
	const u32 gsi = 32;

	struct timespec start, elapsed;
	struct vfio_pci_device *device;
	const char *iommu_mode = NULL;
	const char *device_bdf;
	struct kvm_vcpu *vcpu;
	pthread_t vcpu_thread;
	struct kvm_vm *vm;
	int c;

	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	while ((c = getopt(argc, argv, "i:")) != -1) {
		switch (c) {
		case 'i':
			iommu_mode = optarg;
			break;
		default:
			help(argv[0]);
		}
	}

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	vm_install_exception_handler(vm, vector, guest_irq_handler);

	device = vfio_pci_device_init(device_bdf, iommu_mode);
	TEST_REQUIRE(device->msix_info.count > 0);

	vfio_pci_msix_enable(device, 0, 1);
	kvm_add_irqfd(vm, gsi, device->msi_eventfds[0]);
	kvm_route_msi(vm, gsi, vcpu, vector);

	pthread_create(&vcpu_thread, NULL, vcpu_thread_main, vcpu);

	while (!READ_ONCE(guest_ready_for_irq))
		sync_global_from_guest(vm, guest_ready_for_irq);

	vfio_pci_irq_trigger(device, VFIO_PCI_MSIX_IRQ_INDEX, 0);

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
