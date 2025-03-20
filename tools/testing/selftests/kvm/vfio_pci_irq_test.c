// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"
#include "apic.h"
#include "processor.h"

#include <pthread.h>
#include <ctype.h>
#include <time.h>
#include <linux/vfio.h>
#include <linux/sizes.h>
#include <sys/sysinfo.h>

#include <vfio_util.h>

static bool x2apic = true;
static bool done;
static bool block;

static bool guest_ready_for_irqs[KVM_MAX_VCPUS];
static bool guest_received_irq[KVM_MAX_VCPUS];
static bool guest_received_nmi[KVM_MAX_VCPUS];

static pid_t vcpu_tids[KVM_MAX_VCPUS];

#define TIMEOUT_NS (2ULL * 1000 * 1000 * 1000)

#define READ_FROM_GUEST(_vm, _variable) ({		\
	sync_global_from_guest(_vm, _variable);		\
	READ_ONCE(_variable);				\
})

#define WRITE_TO_GUEST(_vm, _variable, _value) do {	\
	WRITE_ONCE(_variable, _value);			\
	sync_global_to_guest(_vm, _variable);		\
} while (0)

static u32 guest_get_vcpu_id(void)
{
	if (x2apic)
		return x2apic_read_reg(APIC_ID);
	else
		return xapic_read_reg(APIC_ID) >> 24;
}

static void guest_enable_interrupts(void)
{
	if (x2apic)
		x2apic_enable();
	else
		xapic_enable();

	sti_nop();
}

static void guest_irq_handler(struct ex_regs *regs)
{
	WRITE_ONCE(guest_received_irq[guest_get_vcpu_id()], true);

	if (x2apic)
		x2apic_write_reg(APIC_EOI, 0);
	else
		xapic_write_reg(APIC_EOI, 0);
}

static void guest_nmi_handler(struct ex_regs *regs)
{
	WRITE_ONCE(guest_received_nmi[guest_get_vcpu_id()], true);
}

static void guest_code(void)
{
	guest_enable_interrupts();
	WRITE_ONCE(guest_ready_for_irqs[guest_get_vcpu_id()], true);

	while (!READ_ONCE(done)) {
		if (block)
			hlt();
	}

	GUEST_DONE();
}

static void *vcpu_thread_main(void *arg)
{
	struct kvm_vcpu *vcpu = arg;
	struct ucall uc;

	WRITE_ONCE(vcpu_tids[vcpu->id], syscall(__NR_gettid));

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(UCALL_DONE, get_ucall(vcpu, &uc));

	return NULL;
}

static int get_cpu(struct kvm_vcpu *vcpu)
{
	pid_t tid = vcpu_tids[vcpu->id];
	cpu_set_t cpus;
	int cpu = -1;
	int i, ret;

	ret = sched_getaffinity(tid, sizeof(cpus), &cpus);
	TEST_ASSERT(ret == 0, "sched_getaffinity() failed");

	for (i = 0; i < get_nprocs(); i++) {
		if (!CPU_ISSET(i, &cpus))
			continue;

		if (cpu != -1) {
			cpu = i;
		} else {
			/* vCPU is pinned to multiple CPUs */
			return -1;
		}
	}

	return cpu;
}

static void pin_vcpu_threads(int nr_vcpus, int start_cpu, cpu_set_t *available_cpus)
{
	const size_t size = sizeof(cpu_set_t);
	int nr_cpus, cpu, vcpu_index = 0;
	cpu_set_t target_cpu;
	int r;

	nr_cpus = get_nprocs();
	CPU_ZERO(&target_cpu);

	for (cpu = start_cpu;; cpu = (cpu + 1) % nr_cpus) {
		if (vcpu_index == nr_vcpus)
			break;

		if (!CPU_ISSET(cpu, available_cpus))
			continue;

		CPU_SET(cpu, &target_cpu);

		r = sched_setaffinity(vcpu_tids[vcpu_index], size, &target_cpu);
		TEST_ASSERT(r == 0, "sched_setaffinity() failed (cpu: %d)", cpu);

		CPU_CLR(cpu, &target_cpu);

		vcpu_index++;
	}
}

static FILE *open_proc_interrupts(void)
{
	FILE *fp;

	fp = fopen("/proc/interrupts", "r");
	TEST_ASSERT(fp, "fopen(/proc/interrupts) failed");

	return fp;
}

static int get_irq_number(const char *device_bdf, int msi)
{
	char search_string[64];
	char line[4096];
	int irq = -1;
	FILE *fp;

	fp = open_proc_interrupts();

	snprintf(search_string, sizeof(search_string), "vfio-msix[%d]", msi);

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, device_bdf) && strstr(line, search_string)) {
			TEST_ASSERT_EQ(1, sscanf(line, "%d:", &irq));
			break;
		}
	}

	fclose(fp);

	TEST_ASSERT(irq != -1, "Failed to locate IRQ for %s %s", device_bdf, search_string);
	return irq;
}

static int parse_interrupt_count(char *token)
{
	char *c;

	for (c = token; *c; c++) {
		if (!isdigit(*c))
			return 0;
	}

	return atoi_non_negative("interrupt count", token);
}

static u64 __get_irq_count(const char *search_string)
{
	u64 total_count = 0;
	char line[4096];
	FILE *fp;

	fp = open_proc_interrupts();

	while (fgets(line, sizeof(line), fp)) {
		char *token = strtok(line, " ");

		if (strcmp(token, search_string))
			continue;

		while ((token = strtok(NULL, " ")))
			total_count += parse_interrupt_count(token);

		break;
	}

	fclose(fp);
	return total_count;
}

static u64 get_irq_count(int irq)
{
	char search_string[32];

	snprintf(search_string, sizeof(search_string), "%d:", irq);
	return __get_irq_count(search_string);
}

static void kvm_clear_gsi_routes(struct kvm_vm *vm)
{
	struct kvm_irq_routing routes = {};

	vm_ioctl(vm, KVM_SET_GSI_ROUTING, &routes);
}

static void kvm_route_msi(struct kvm_vm *vm, u32 gsi, struct kvm_vcpu *vcpu,
			  u8 vector, bool do_nmi)
{
	u8 buf[sizeof(struct kvm_irq_routing) + sizeof(struct kvm_irq_routing_entry)] = {};
	struct kvm_irq_routing *routes = (void *)&buf;

	routes->nr = 1;
	routes->entries[0].gsi = gsi;
	routes->entries[0].type = KVM_IRQ_ROUTING_MSI;
	routes->entries[0].u.msi.address_lo = 0xFEE00000 | (vcpu->id << 12);
	routes->entries[0].u.msi.data = do_nmi ? NMI_VECTOR | (4 << 8) : vector;

	vm_ioctl(vm, KVM_SET_GSI_ROUTING, routes);
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
		TEST_ASSERT_EQ(msi, device->driver.msi);
		vfio_pci_driver_send_msi(device);
	} else {
		vfio_pci_irq_trigger(device, VFIO_PCI_MSIX_IRQ_INDEX, msi);
	}
}

static void help(const char *name)
{
	printf("Usage: %s [-a] [-b] [-d] [-e] [-h] [-i nr_irqs] [-n] [-p] [-v nr_vcpus] [-x] segment:bus:device.function\n",
	       name);
	printf("\n");
	printf("  -a: Randomly affinitize the device IRQ to different CPUs\n"
	       "      throughout the test.\n");
	printf("  -b: Block vCPUs (e.g. HLT) instead of spinning in guest-mode\n");
	printf("  -d: Use the device to trigger the IRQ instead of emulating\n"
	       "      it with an eventfd write.\n");
	printf("  -e: Destroy and recreate KVM's GSI routing table in between\n"
	       "      some interrupts.\n");
	printf("  -i: The number of IRQs to generate during the test.\n");
	printf("  -n: Route some of the device interrupts to be delivered as\n"
	       "      an NMI into the guest.\n");
	printf("  -p: Pin vCPU threads to random pCPUs throughout the test.\n");
	printf("  -v: Set the number of vCPUs that the test should create.\n"
	       "      Interrupts will be round-robined among vCPUs.\n");
	printf("  -x: Use xAPIC mode instead of x2APIC mode in the guest.\n");
	printf("\n");
	exit(KSFT_FAIL);
}

int main(int argc, char **argv)
{
	/* Random non-reserved vector and GSI to use for the device IRQ */
	const u8 vector = 0xe0;
	const u32 gsi = 32;

	/* Test configuration (overridable by command line flags). */
	bool use_device_msi = false, irq_affinity = false, pin_vcpus = false;
	bool empty = false, nmi = false;
	int nr_irqs = 1000;
	int nr_vcpus = 1;

	struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
	pthread_t vcpu_threads[KVM_MAX_VCPUS];
	u64 irq_count, pin_count, piw_count;
	struct vfio_pci_device *device;
	cpu_set_t available_cpus;
	const char *device_bdf;
	FILE *irq_affinity_fp;
	int i, j, c, msi, irq;
	struct kvm_vm *vm;
	int irq_cpu;
	int ret;

	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	while ((c = getopt(argc, argv, "abdehi:npv:x")) != -1) {
		switch (c) {
		case 'a':
			irq_affinity = true;
			break;
		case 'b':
			block = true;
			break;
		case 'd':
			use_device_msi = true;
			break;
		case 'e':
			empty = true;
			break;
		case 'i':
			nr_irqs = atoi_positive("Number of IRQs", optarg);
			break;
		case 'n':
			nmi = true;
			break;
		case 'p':
			pin_vcpus = true;
			break;
		case 'v':
			nr_vcpus = atoi_positive("nr_vcpus", optarg);
			break;
		case 'x':
			x2apic = false;
			break;
		case 'h':
		default:
			help(argv[0]);
		}
	}

	vm = vm_create_with_vcpus(nr_vcpus, guest_code, vcpus);
	vm_install_exception_handler(vm, vector, guest_irq_handler);
	vm_install_exception_handler(vm, NMI_VECTOR, guest_nmi_handler);

	if (!x2apic)
		virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);

	device = vfio_pci_device_init(device_bdf, default_iommu_mode);
	msi = setup_msi(device, use_device_msi);
	irq = get_irq_number(device_bdf, msi);

	irq_count = get_irq_count(irq);
	pin_count = __get_irq_count("PIN:");
	piw_count = __get_irq_count("PIW:");

	printf("%s %s MSI-X[%d] (IRQ-%d) %d times\n",
	       use_device_msi ? "Triggering" : "Notifying the eventfd for",
	       device_bdf, msi, irq, nr_irqs);

	kvm_assign_irqfd(vm, gsi, device->msi_eventfds[msi]);

	sync_global_to_guest(vm, x2apic);
	sync_global_to_guest(vm, block);

	for (i = 0; i < nr_vcpus; i++)
		pthread_create(&vcpu_threads[i], NULL, vcpu_thread_main, vcpus[i]);

	for (i = 0; i < nr_vcpus; i++) {
		struct kvm_vcpu *vcpu = vcpus[i];

		while (!READ_FROM_GUEST(vm, guest_ready_for_irqs[vcpu->id]))
			continue;
	}

	if (pin_vcpus) {
		ret = sched_getaffinity(vcpu_tids[0], sizeof(available_cpus), &available_cpus);
		TEST_ASSERT(ret == 0, "sched_getaffinity() failed");

		if (nr_vcpus > CPU_COUNT(&available_cpus)) {
			printf("There are more vCPUs than pCPUs; refusing to pin.\n");
			pin_vcpus = false;
		}
	}

	if (irq_affinity) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
		irq_affinity_fp = fopen(path, "w");
		TEST_ASSERT(irq_affinity_fp, "fopen(%s) failed", path);
	}

	/* Set a consistent seed so that test are repeatable. */
	srand(0);

	for (i = 0; i < nr_irqs; i++) {
		struct kvm_vcpu *vcpu = vcpus[i % nr_vcpus];
		const bool do_nmi = nmi && (i & BIT(2));
		const bool do_empty = empty && (i & BIT(3));
		struct timespec start;

		if (do_empty)
			kvm_clear_gsi_routes(vm);

		kvm_route_msi(vm, gsi, vcpu, vector, do_nmi);

		if (irq_affinity && vcpu->id == 0) {
			irq_cpu = rand() % get_nprocs();

			ret = fprintf(irq_affinity_fp, "%d\n", irq_cpu);
			TEST_ASSERT(ret > 0, "Failed to affinitize IRQ-%d to CPU %d", irq, irq_cpu);
		}

		if (pin_vcpus && vcpu->id == 0)
			pin_vcpu_threads(nr_vcpus, rand() % get_nprocs(), &available_cpus);

		for (j = 0; j < nr_vcpus; j++) {
			TEST_ASSERT_EQ(READ_FROM_GUEST(vm, guest_received_irq[vcpu->id]), false);
			TEST_ASSERT_EQ(READ_FROM_GUEST(vm, guest_received_nmi[vcpu->id]), false);
		}

		send_msi(device, use_device_msi, msi);

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (;;) {
			if (do_nmi && READ_FROM_GUEST(vm, guest_received_nmi[vcpu->id]))
				break;

			if (!do_nmi && READ_FROM_GUEST(vm, guest_received_irq[vcpu->id]))
				break;

			if (timespec_to_ns(timespec_elapsed(start)) > TIMEOUT_NS) {
				printf("Timeout waiting for interrupt!\n");
				printf("  vCPU: %d\n", vcpu->id);
				printf("  do_nmi: %d\n", do_nmi);
				printf("  do_empty: %d\n", do_empty);
				if (irq_affinity)
					printf("  irq_cpu: %d\n", irq_cpu);
				if (pin_vcpus)
					printf("  vcpu_cpu: %d\n", get_cpu(vcpu));

				TEST_FAIL("vCPU never received IRQ!\n");
			}
		}

		if (do_nmi)
			WRITE_TO_GUEST(vm, guest_received_nmi[vcpu->id], false);
		else
			WRITE_TO_GUEST(vm, guest_received_irq[vcpu->id], false);
	}

	WRITE_TO_GUEST(vm, done, true);

	for (i = 0; i < nr_vcpus; i++) {
		if (block) {
			kvm_route_msi(vm, gsi, vcpus[i], vector, false);
			send_msi(device, false, msi);
		}

		pthread_join(vcpu_threads[i], NULL);
	}

	if (irq_affinity)
		fclose(irq_affinity_fp);

	printf("Host interrupts handled:\n");
	printf("  IRQ-%d: %lu\n", irq, get_irq_count(irq) - irq_count);
	printf("  Posted-interrupt notification events: %lu\n",
	       __get_irq_count("PIN:") - pin_count);
	printf("  Posted-interrupt wakeup events: %lu\n",
	       __get_irq_count("PIW:") - piw_count);

	vfio_pci_device_cleanup(device);

	return 0;
}
