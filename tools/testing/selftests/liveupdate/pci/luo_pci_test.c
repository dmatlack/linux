// SPDX-License-Identifier: GPL-2.0-only
/*
 * Preserve PCI devices across a Live Update and check that the PCI core keeps
 * them, and the bridges above them, running.
 *
 * The devices are selected on the kernel command line:
 *
 *   lupci.preserve=<path>[,<path>...]  Endpoints to preserve.
 *   lupci.control=<path>[,<path>...]   Endpoints to bind to the test driver
 *                                      but not preserve.
 *   lupci.marker=<bdf>                 Optional device whose config space is
 *                                      written to mark the phases of the test
 *                                      in a QEMU trace of config writes.
 *   lupci.control_changes=<reg>[,...]  Optional. Registers that must change
 *                                      across the Live Update on bridges above
 *                                      control endpoints (and not above
 *                                      preserved endpoints): "acs" for the ACS
 *                                      controls, "ari" for ARI Forwarding
 *                                      Enable.
 *
 * A <path> identifies a device independently of bus numbers: a device on the
 * root bus followed by the device.function of each device below it, e.g.
 * 0000:00:02.0/00.0 is function 0 of device 0 below the bridge at 0000:00:02.0.
 *
 * Stage 1 snapshots the config space of each preserved endpoint and of all
 * bridges above it, prints the snapshot so that a host-side checker can
 * validate the QEMU trace, and preserves the endpoints. Stage 2 checks the
 * devices against the snapshot before and after the driver retrieves and
 * finishes them.
 *
 * The test driver also preserves the state it captured for each endpoint with
 * pci_store_saved_state(). Stage 1 records that state and changes a saved
 * register on the device. Stage 2 checks that the driver got the same state
 * back, and that restoring it puts the register back.
 *
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/pci_regs.h>

#include <libliveupdate.h>

#include "liveupdate_pci_test.h"

#define STATE_SESSION_NAME	"lupci_state"
#define STATE_MEMFD_TOKEN	1
#define DATA_SESSION_NAME	"lupci_data"
#define DATA_MEMFD_TOKEN	1
#define DEVICE_SESSION_NAME	"lupci_devices"
#define UNPRESERVE_SESSION_NAME	"lupci_unpreserve"

/* Token of the memfd holding the saved state of devices[i] is this plus i. */
#define SAVED_STATE_MEMFD_TOKEN	16

/*
 * Bits in the PCIe capability control registers, which the PCI core saves for
 * every PCIe device, that stage 1 can flip on the device. It uses the first
 * one that the device implements.
 */
static const struct {
	uint16_t reg;
	uint16_t bit;
} saved_bits[] = {
	{ PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_CERE },
	{ PCI_EXP_DEVCTL2, PCI_EXP_DEVCTL2_COMP_TMOUT_DIS },
	{ PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_CCC },
	{ PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_ES },
	{ PCI_EXP_DEVCTL2, 0x8000 },	/* End-End TLP Prefix Blocking */
};

/* Values written to the marker device. Keep in sync with check_cfg_trace.py. */
#define MARKER_OFFSET		0x40
#define MARKER_STAGE1_DONE	0x4c550001
#define MARKER_STAGE2_START	0x4c550002
#define MARKER_STAGE2_DONE	0x4c550003

#define MANIFEST_PREFIX		"LUPCI-MANIFEST: "

#define CMDLINE_SIZE		4096
#define CONFIG_SIZE		4096
#define MAX_DEVICES		32
#define BDF_LEN			12	/* DDDD:BB:dd.f */

enum role {
	ROLE_PRESERVED,
	ROLE_CONTROL,
};

struct pci_snapshot {
	char bdf[BDF_LEN + 1];
	enum role role;
	int token;		/* LUO token, only for preserved endpoints */
	bool endpoint;		/* Named by a path, i.e. not an upstream bridge */
	bool bridge;
	uint16_t vendor;
	uint16_t device;
	uint16_t command;
	uint8_t secondary;
	uint8_t subordinate;
	uint16_t pcie;		/* Capability offsets, 0 if not present */
	uint16_t pm;
	uint16_t acs;
	uint16_t devctl2;
	uint16_t pmcsr;
	uint16_t acs_ctrl;
};

static struct pci_snapshot devices[MAX_DEVICES];
static int nr_devices;

/* Stage 2 snapshot, taken before the test driver is loaded. */
static struct pci_snapshot before[MAX_DEVICES];
static int nr_before;

/* Stage 1 saved state of each preserved endpoint, retrieved in stage 2. */
static struct liveupdate_pci_test_saved_state saved_states[MAX_DEVICES];

/* The register stage 1 changed in each preserved endpoint, and the bit. */
static uint16_t changed_pos[MAX_DEVICES];
static uint16_t changed_bit[MAX_DEVICES];

static char opt_preserve[CMDLINE_SIZE];
static char opt_control[CMDLINE_SIZE];
static char opt_marker[CMDLINE_SIZE];
static char opt_control_changes[CMDLINE_SIZE];

/* Length of the prefix of @s that contains none of @reject (nolibc lacks strcspn). */
static size_t span_until(const char *s, const char *reject)
{
	size_t len = 0;

	while (s[len] && !strchr(reject, s[len]))
		len++;

	return len;
}

static void get_cmdline_option(const char *cmdline, const char *name,
			       char *value, size_t size)
{
	const char *p = cmdline;
	size_t name_len = strlen(name);
	size_t len;

	value[0] = 0;

	while ((p = strstr(p, name))) {
		/* Only match whole options. */
		if ((p == cmdline || p[-1] == ' ') && p[name_len] == '=')
			break;
		p += name_len;
	}

	if (!p)
		return;

	p += name_len + 1;
	len = span_until(p, "\n ");
	if (len >= size)
		fail_exit("Option %s is too long", name);

	memcpy(value, p, len);
	value[len] = 0;
}

static void parse_cmdline(void)
{
	char cmdline[CMDLINE_SIZE] = {};
	ssize_t len;
	int fd;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0)
		fail_exit("open /proc/cmdline");

	len = read(fd, cmdline, sizeof(cmdline) - 1);
	if (len < 0)
		fail_exit("read /proc/cmdline");
	close(fd);

	get_cmdline_option(cmdline, "lupci.preserve", opt_preserve, sizeof(opt_preserve));
	get_cmdline_option(cmdline, "lupci.control", opt_control, sizeof(opt_control));
	get_cmdline_option(cmdline, "lupci.marker", opt_marker, sizeof(opt_marker));
	get_cmdline_option(cmdline, "lupci.control_changes", opt_control_changes,
			   sizeof(opt_control_changes));

	if (!opt_preserve[0])
		ksft_exit_skip("No devices to preserve (lupci.preserve=)\n");
}

static void mount_sysfs(void)
{
	mkdir("/sys", 0755);
	if (mount("sysfs", "/sys", "sysfs", 0, NULL) < 0 && errno != EBUSY)
		fail_exit("mount sysfs");
}

static void load_module(void)
{
	int fd, ret;

	fd = open(LIVEUPDATE_PCI_TEST_MODULE, O_RDONLY);
	if (fd < 0)
		fail_exit("open %s", LIVEUPDATE_PCI_TEST_MODULE);

	ret = syscall(__NR_finit_module, fd, "", 0);
	if (ret < 0 && errno != EEXIST)
		fail_exit("finit_module %s", LIVEUPDATE_PCI_TEST_MODULE);

	close(fd);
}

static void write_marker(uint32_t value)
{
	char path[64];
	int fd;

	if (!opt_marker[0])
		return;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", opt_marker);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		fail_exit("open %s", path);

	if (lseek(fd, MARKER_OFFSET, SEEK_SET) != MARKER_OFFSET ||
	    write(fd, &value, sizeof(value)) != sizeof(value))
		fail_exit("write marker 0x%x to %s", value, path);

	close(fd);
}

static int read_config(const char *bdf, uint8_t *cfg)
{
	char path[64];
	ssize_t n;
	int fd, len = 0;

	memset(cfg, 0, CONFIG_SIZE);

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	while (len < CONFIG_SIZE) {
		n = read(fd, cfg + len, CONFIG_SIZE - len);
		if (n <= 0)
			break;
		len += n;
	}

	close(fd);
	return len;
}

static uint16_t cfg16(const uint8_t *cfg, int pos)
{
	return cfg[pos] | cfg[pos + 1] << 8;
}

static uint32_t cfg32(const uint8_t *cfg, int pos)
{
	return cfg16(cfg, pos) | (uint32_t)cfg16(cfg, pos + 2) << 16;
}

static void write_config16(const char *bdf, int pos, uint16_t value)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		fail_exit("open %s", path);

	if (lseek(fd, pos, SEEK_SET) != pos ||
	    write(fd, &value, sizeof(value)) != sizeof(value))
		fail_exit("write 0x%x to %s at 0x%x", value, path, pos);

	close(fd);
}

static uint16_t find_cap(const uint8_t *cfg, uint8_t id)
{
	int ttl = 48;
	uint8_t pos;

	if (!(cfg16(cfg, PCI_STATUS) & PCI_STATUS_CAP_LIST))
		return 0;

	pos = cfg[PCI_CAPABILITY_LIST] & ~3;
	while (pos && ttl--) {
		if (cfg[pos + PCI_CAP_LIST_ID] == id)
			return pos;
		pos = cfg[pos + PCI_CAP_LIST_NEXT] & ~3;
	}

	return 0;
}

static uint16_t find_ext_cap(const uint8_t *cfg, int len, uint16_t id)
{
	int ttl = (CONFIG_SIZE - PCI_CFG_SPACE_SIZE) / 8;
	uint16_t pos = PCI_CFG_SPACE_SIZE;
	uint32_t header;

	while (pos && pos + 4 <= len && ttl--) {
		header = cfg32(cfg, pos);
		if (!header)
			return 0;
		if (PCI_EXT_CAP_ID(header) == id)
			return pos;
		pos = PCI_EXT_CAP_NEXT(header);
	}

	return 0;
}

static void take_snapshot(struct pci_snapshot *dev)
{
	uint8_t cfg[CONFIG_SIZE];
	int len;

	len = read_config(dev->bdf, cfg);
	if (len < PCI_CFG_SPACE_SIZE)
		fail_exit("Failed to read config space of %s", dev->bdf);

	dev->vendor = cfg16(cfg, PCI_VENDOR_ID);
	dev->device = cfg16(cfg, PCI_DEVICE_ID);
	dev->command = cfg16(cfg, PCI_COMMAND);
	dev->bridge = (cfg[PCI_HEADER_TYPE] & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE;
	dev->secondary = dev->bridge ? cfg[PCI_SECONDARY_BUS] : 0;
	dev->subordinate = dev->bridge ? cfg[PCI_SUBORDINATE_BUS] : 0;

	dev->pcie = find_cap(cfg, PCI_CAP_ID_EXP);
	dev->devctl2 = dev->pcie ? cfg16(cfg, dev->pcie + PCI_EXP_DEVCTL2) : 0;

	dev->pm = find_cap(cfg, PCI_CAP_ID_PM);
	dev->pmcsr = dev->pm ? cfg16(cfg, dev->pm + PCI_PM_CTRL) : 0;

	dev->acs = find_ext_cap(cfg, len, PCI_EXT_CAP_ID_ACS);
	dev->acs_ctrl = dev->acs ? cfg16(cfg, dev->acs + PCI_ACS_CTRL) : 0;
}

static struct pci_snapshot *find_device(const char *bdf)
{
	int i;

	for (i = 0; i < nr_devices; i++) {
		if (!strcmp(devices[i].bdf, bdf))
			return &devices[i];
	}

	return NULL;
}

static struct pci_snapshot *add_device(const char *bdf, enum role role)
{
	struct pci_snapshot *dev = find_device(bdf);

	if (dev) {
		/* Bridges above both preserved and control endpoints are preserved. */
		if (role == ROLE_PRESERVED)
			dev->role = ROLE_PRESERVED;
		return dev;
	}

	if (nr_devices == MAX_DEVICES)
		fail_exit("Too many devices");

	dev = &devices[nr_devices++];
	memset(dev, 0, sizeof(*dev));
	snprintf(dev->bdf, sizeof(dev->bdf), "%s", bdf);
	dev->role = role;
	return dev;
}

static bool is_bdf(const char *name)
{
	return strlen(name) == BDF_LEN && name[4] == ':' && name[7] == ':' &&
	       name[10] == '.';
}

/* Find the child of @parent with device.function @devfn (e.g. "00.0"). */
static void find_child(const char *parent, const char *devfn, size_t devfn_len,
		       char *child)
{
	struct dirent entry, *result;
	char path[64];
	DIR *dir;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", parent);
	dir = opendir(path);
	if (!dir)
		fail_exit("opendir %s", path);

	child[0] = 0;
	while (!readdir_r(dir, &entry, &result) && result) {
		if (is_bdf(entry.d_name) && strlen(entry.d_name + 8) == devfn_len &&
		    !strncmp(entry.d_name + 8, devfn, devfn_len)) {
			snprintf(child, BDF_LEN + 1, "%s", entry.d_name);
			break;
		}
	}

	closedir(dir);

	if (!child[0])
		fail_exit("No device %.*s below %s", (int)devfn_len, devfn, parent);
}

/*
 * Add every device along @path, which is @len characters long, and return the
 * device at the end of it.
 */
static struct pci_snapshot *add_path(const char *path, size_t len, enum role role)
{
	const char *end = path + len;
	char bdf[BDF_LEN + 1];
	const char *p = path;
	char sysfs[64];
	size_t n;

	n = span_until(p, "/,");
	if (n != BDF_LEN || n > len)
		fail_exit("Invalid device path %.*s", (int)len, path);

	memcpy(bdf, p, BDF_LEN);
	bdf[BDF_LEN] = 0;

	snprintf(sysfs, sizeof(sysfs), "/sys/bus/pci/devices/%s", bdf);
	if (access(sysfs, F_OK))
		fail_exit("No device %s", bdf);

	add_device(bdf, role);
	p += n;

	while (p < end && *p == '/') {
		char child[BDF_LEN + 1];

		p++;
		n = span_until(p, "/,");
		if (p + n > end)
			n = end - p;
		find_child(bdf, p, n, child);
		memcpy(bdf, child, sizeof(bdf));
		add_device(bdf, role);
		p += n;
	}

	return find_device(bdf);
}

static int add_paths(const char *paths, enum role role)
{
	struct pci_snapshot *dev;
	const char *p = paths;
	int nr_endpoints = 0;
	size_t len;

	while (*p) {
		len = span_until(p, ",");
		dev = add_path(p, len, role);
		if (dev->endpoint)
			fail_exit("Device %s is listed twice", dev->bdf);
		dev->endpoint = true;
		if (role == ROLE_PRESERVED)
			dev->token = ++nr_endpoints;
		p += len;
		if (*p == ',')
			p++;
	}

	return nr_endpoints;
}

static int format_snapshot(const struct pci_snapshot *dev, char *buf, size_t size)
{
	return snprintf(buf, size,
			"bdf=%s role=%s token=%d endpoint=%d bridge=%d id=0x%04x device=0x%04x "
			"cmd=0x%04x sec=0x%02x sub=0x%02x pcie=0x%x pm=0x%x acs=0x%x "
			"devctl2=0x%04x pmcsr=0x%04x acsctrl=0x%04x\n",
			dev->bdf, dev->role == ROLE_PRESERVED ? "preserved" : "control",
			dev->token, dev->endpoint, dev->bridge, dev->vendor, dev->device,
			dev->command, dev->secondary, dev->subordinate, dev->pcie, dev->pm,
			dev->acs, dev->devctl2, dev->pmcsr, dev->acs_ctrl);
}

static unsigned long get_field(const char *line, const char *key)
{
	const char *p = strstr(line, key);

	if (!p)
		fail_exit("Snapshot is missing %s", key);

	return strtoul(p + strlen(key), NULL, 0);
}

static void parse_snapshots(const char *text)
{
	struct pci_snapshot *dev;
	const char *p = text;
	char line[512];
	size_t len;

	while (*p) {
		len = span_until(p, "\n");
		if (len >= sizeof(line))
			fail_exit("Snapshot line is too long");

		memcpy(line, p, len);
		line[len] = 0;
		p += len;
		if (*p == '\n')
			p++;

		if (!strstr(line, "bdf="))
			continue;

		if (nr_devices == MAX_DEVICES)
			fail_exit("Too many devices in snapshot");

		dev = &devices[nr_devices++];
		memset(dev, 0, sizeof(*dev));
		memcpy(dev->bdf, strstr(line, "bdf=") + 4, BDF_LEN);
		dev->role = strstr(line, "role=preserved") ? ROLE_PRESERVED : ROLE_CONTROL;
		dev->token = get_field(line, "token=");
		dev->endpoint = get_field(line, "endpoint=");
		dev->bridge = get_field(line, "bridge=");
		dev->vendor = get_field(line, "id=");
		dev->device = get_field(line, "device=");
		dev->command = get_field(line, "cmd=");
		dev->secondary = get_field(line, "sec=");
		dev->subordinate = get_field(line, "sub=");
		dev->pcie = get_field(line, "pcie=");
		dev->pm = get_field(line, "pm=");
		dev->acs = get_field(line, "acs=");
		dev->devctl2 = get_field(line, "devctl2=");
		dev->pmcsr = get_field(line, "pmcsr=");
		dev->acs_ctrl = get_field(line, "acsctrl=");
	}
}

static int get_device_fd(int ctl_fd, const char *bdf)
{
	struct liveupdate_pci_test_device dev = {};
	unsigned int domain, bus, slot, func;
	int fd;

	domain = strtoul(bdf, NULL, 16);
	bus = strtoul(bdf + 5, NULL, 16);
	slot = strtoul(bdf + 8, NULL, 16);
	func = strtoul(bdf + 11, NULL, 16);

	dev.domain = domain;
	dev.bdf = bus << 8 | slot << 3 | func;

	fd = ioctl(ctl_fd, LIVEUPDATE_PCI_TEST_GET_DEVICE_FD, &dev);
	if (fd < 0)
		fail_exit("Failed to get a file for %s. Is it bound to the test driver?", bdf);

	return fd;
}

static uint16_t get_status(int fd, const char *bdf)
{
	struct liveupdate_pci_test_device dev = {};
	char actual[BDF_LEN + 1];

	if (ioctl(fd, LIVEUPDATE_PCI_TEST_GET_STATUS, &dev) < 0)
		fail_exit("LIVEUPDATE_PCI_TEST_GET_STATUS for %s", bdf);

	snprintf(actual, sizeof(actual), "%04x:%02x:%02x.%x", dev.domain,
		 dev.bdf >> 8, (dev.bdf >> 3) & 0x1f, dev.bdf & 7);
	if (strcmp(actual, bdf))
		fail_exit("Expected device %s, got %s", bdf, actual);

	return dev.flags;
}

static void get_saved_state(int fd, const char *bdf,
			    struct liveupdate_pci_test_saved_state *state)
{
	memset(state, 0, sizeof(*state));
	if (ioctl(fd, LIVEUPDATE_PCI_TEST_GET_SAVED_STATE, state) < 0)
		fail_exit("LIVEUPDATE_PCI_TEST_GET_SAVED_STATE for %s", bdf);
}

static bool dword_saved(const struct liveupdate_pci_test_saved_state *state,
			int dword)
{
	return state->dword_map[dword / 64] & (1ULL << (dword % 64));
}

/* Get the saved value of the DWORD at @pos. Returns false if it wasn't saved. */
static bool saved_value(const struct liveupdate_pci_test_saved_state *state,
			int pos, uint32_t *value)
{
	int i, n = 0;

	if (!dword_saved(state, pos / 4))
		return false;

	for (i = 0; i < pos / 4; i++)
		n += dword_saved(state, i);

	*value = state->dword_val[n];
	return true;
}

/*
 * Format the register stage 1 changed as "r=<offset>,<bit>", followed by @state
 * as one "<dword>=<value>" line per saved DWORD, all in hex.
 */
static void format_saved_state(const struct liveupdate_pci_test_saved_state *state,
			       int pos, int bit, char *buf, size_t size)
{
	size_t len;
	int i, n = 0;

	len = snprintf(buf, size, "r=%x,%x\n", pos, bit);
	for (i = 0; i < LIVEUPDATE_PCI_TEST_CFG_DWORDS; i++) {
		if (!dword_saved(state, i))
			continue;

		len += snprintf(buf + len, size - len, "%x=%x\n", i,
				state->dword_val[n++]);
		if (len >= size)
			fail_exit("Saved state is too large");
	}
}

static void parse_saved_state(const char *text,
			      struct liveupdate_pci_test_saved_state *state,
			      uint16_t *pos, uint16_t *bit)
{
	const char *p = text;
	unsigned long dword;
	char *end;
	int n = 0;

	if (strncmp(p, "r=", 2))
		fail_exit("Malformed saved state: %s", p);
	*pos = strtoul(p + 2, &end, 16);
	if (*end != ',')
		fail_exit("Malformed saved state: %s", p);
	*bit = strtoul(end + 1, &end, 16);
	p = end;
	if (*p == '\n')
		p++;

	memset(state, 0, sizeof(*state));
	while (*p) {
		dword = strtoul(p, &end, 16);
		if (*end != '=' || dword >= LIVEUPDATE_PCI_TEST_CFG_DWORDS)
			fail_exit("Malformed saved state: %s", p);

		state->dword_map[dword / 64] |= 1ULL << (dword % 64);
		state->dword_val[n++] = strtoul(end + 1, &end, 16);
		p = end;
		if (*p == '\n')
			p++;
	}
}

static uint16_t read_config16(const char *bdf, int pos)
{
	uint8_t cfg[CONFIG_SIZE];

	if (read_config(bdf, cfg) < PCI_CFG_SPACE_SIZE)
		fail_exit("Failed to read config space of %s", bdf);

	return cfg16(cfg, pos);
}

/* Get the saved value of the 16-bit register at @pos. */
static bool saved_value16(const struct liveupdate_pci_test_saved_state *state,
			  int pos, uint16_t *value)
{
	uint32_t dword;

	if (!saved_value(state, pos & ~3, &dword))
		return false;

	*value = dword >> (8 * (pos & 3));
	return true;
}

static int nr_failures;

/* Whether @reg is listed in lupci.control_changes. */
static bool control_changes(const char *reg)
{
	const char *p = opt_control_changes;
	size_t len;

	while (*p) {
		len = span_until(p, ",");
		if (len == strlen(reg) && !strncmp(p, reg, len))
			return true;
		p += len;
		if (*p == ',')
			p++;
	}

	return false;
}

#define check(cond, fmt, ...) do {						\
	if (!(cond)) {								\
		ksft_print_msg("FAIL: " fmt "\n", ##__VA_ARGS__);		\
		nr_failures++;							\
	}									\
} while (0)

/*
 * Check the devices against the stage 1 snapshot. If @taken is NULL, check
 * their current state. Otherwise check the snapshots in @taken, which were
 * taken in the same order as the stage 1 snapshot.
 */
static void check_devices(const char *when, const struct pci_snapshot *taken)
{
	struct pci_snapshot now;
	int i;

	ksft_print_msg("[STAGE 2] Checking devices %s\n", when);

	for (i = 0; i < nr_devices; i++) {
		const struct pci_snapshot *dev = &devices[i];

		if (taken) {
			now = taken[i];
			check(!strcmp(now.bdf, dev->bdf), "%s: Device moved to %s",
			      dev->bdf, now.bdf);
		} else {
			now = *dev;
			take_snapshot(&now);
		}

		check(now.vendor == dev->vendor && now.device == dev->device,
		      "%s: ID changed from %04x:%04x to %04x:%04x", dev->bdf,
		      dev->vendor, dev->device, now.vendor, now.device);

		if (dev->role == ROLE_CONTROL) {
			if (dev->endpoint && taken)
				check(!(now.command & PCI_COMMAND_MASTER),
				      "%s: Bus Master still enabled on control device",
				      dev->bdf);
			if (dev->bridge && dev->acs && control_changes("acs"))
				check(now.acs_ctrl != dev->acs_ctrl,
				      "%s: ACS controls did not change (0x%04x)",
				      dev->bdf, now.acs_ctrl);
			if (dev->bridge && dev->pcie && control_changes("ari"))
				check((now.devctl2 ^ dev->devctl2) & PCI_EXP_DEVCTL2_ARI,
				      "%s: ARI Forwarding Enable did not change (DEVCTL2 0x%04x)",
				      dev->bdf, now.devctl2);
			continue;
		}

		check(now.command & PCI_COMMAND_MASTER,
		      "%s: Bus Master disabled (Command 0x%04x)", dev->bdf, now.command);

		if (dev->bridge)
			check(now.secondary == dev->secondary &&
			      now.subordinate == dev->subordinate,
			      "%s: Bus numbers changed from [%02x-%02x] to [%02x-%02x]",
			      dev->bdf, dev->secondary, dev->subordinate,
			      now.secondary, now.subordinate);

		if (dev->bridge && dev->pcie)
			check((now.devctl2 & PCI_EXP_DEVCTL2_ARI) ==
			      (dev->devctl2 & PCI_EXP_DEVCTL2_ARI),
			      "%s: ARI Forwarding Enable changed (DEVCTL2 0x%04x -> 0x%04x)",
			      dev->bdf, dev->devctl2, now.devctl2);

		if (dev->acs)
			check(now.acs_ctrl == dev->acs_ctrl,
			      "%s: ACS controls changed from 0x%04x to 0x%04x",
			      dev->bdf, dev->acs_ctrl, now.acs_ctrl);

		if (dev->pm)
			check((now.pmcsr & PCI_PM_CTRL_STATE_MASK) == 0,
			      "%s: Not in D0 (PMCSR 0x%04x)", dev->bdf, now.pmcsr);
	}

	if (nr_failures)
		ksft_exit_fail_msg("%d check(s) failed %s\n", nr_failures, when);
}

/*
 * Record the saved state of a preserved endpoint in a memfd for stage 2, and
 * change one of its saved registers on the device so that stage 2 can tell
 * whether restoring the saved state wrote it back.
 */
static void preserve_saved_state(int session_fd, int ctl_fd, int i)
{
	struct liveupdate_pci_test_saved_state state;
	struct pci_snapshot *dev = &devices[i];
	uint16_t saved, value, bit = 0;
	char text[4096];
	int fd, pos = 0;
	size_t j;

	fd = get_device_fd(ctl_fd, dev->bdf);
	get_saved_state(fd, dev->bdf, &state);
	close(fd);

	if (dword_saved(&state, LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD))
		fail_exit("%s: DWORD 0x%x is saved", dev->bdf,
			  LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD);

	if (!dev->pcie)
		fail_exit("%s: Not a PCIe device", dev->bdf);

	for (j = 0; j < ARRAY_SIZE(saved_bits); j++) {
		pos = dev->pcie + saved_bits[j].reg;
		bit = saved_bits[j].bit;

		if (!saved_value16(&state, pos, &saved))
			fail_exit("%s: Register 0x%x is not saved", dev->bdf, pos);
		if (saved != read_config16(dev->bdf, pos))
			fail_exit("%s: Saved 0x%x at 0x%x, device has 0x%x",
				  dev->bdf, saved, pos, read_config16(dev->bdf, pos));

		write_config16(dev->bdf, pos, saved ^ bit);
		value = read_config16(dev->bdf, pos);
		if (value == (saved ^ bit))
			break;
		write_config16(dev->bdf, pos, saved);
		bit = 0;
	}
	if (!bit)
		fail_exit("%s: None of the saved bits can be changed", dev->bdf);

	ksft_print_msg("[STAGE 1] %s: Changed 0x%x from 0x%x to 0x%x\n", dev->bdf,
		       pos, saved, value);

	format_saved_state(&state, pos, bit, text, sizeof(text));
	if (create_and_preserve_memfd(session_fd, SAVED_STATE_MEMFD_TOKEN + i, text) < 0)
		fail_exit("Failed to preserve the saved state of %s", dev->bdf);
}

/*
 * Preserve each endpoint in a session that is closed again right away, so
 * that the test driver frees the saved state it preserved.
 */
static void preserve_and_unpreserve(int luo_fd, int ctl_fd)
{
	int session_fd, fd, i;

	session_fd = luo_create_session(luo_fd, UNPRESERVE_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_create_session %s", UNPRESERVE_SESSION_NAME);

	for (i = 0; i < nr_devices; i++) {
		struct pci_snapshot *dev = &devices[i];

		if (dev->role != ROLE_PRESERVED || !dev->endpoint)
			continue;

		fd = get_device_fd(ctl_fd, dev->bdf);
		if (luo_session_preserve_fd(session_fd, fd, dev->token) < 0)
			fail_exit("Failed to preserve %s", dev->bdf);
		close(fd);
	}

	close(session_fd);
}

static void run_stage_1(int luo_fd)
{
	char text[4096] = {};
	int session_fd, ctl_fd, fd, i;
	size_t len = 0;

	ksft_print_msg("[STAGE 1] Starting pre-kexec setup...\n");

	for (i = 0; i < nr_devices; i++) {
		struct pci_snapshot *dev = &devices[i];

		if (dev->role == ROLE_PRESERVED || dev->endpoint) {
			if (!(dev->command & PCI_COMMAND_MASTER))
				fail_exit("%s: Bus Master is not enabled", dev->bdf);
		}

		len += format_snapshot(dev, text + len, sizeof(text) - len);
		if (len >= sizeof(text))
			fail_exit("Snapshot is too large");
	}

	/* Print the snapshot for the host-side checker. */
	for (i = 0; i < nr_devices; i++) {
		char line[512];

		format_snapshot(&devices[i], line, sizeof(line));
		ksft_print_msg(MANIFEST_PREFIX "%s", line);
	}

	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, 2);

	ctl_fd = open(LIVEUPDATE_PCI_TEST_DEVICE, O_RDWR);
	if (ctl_fd < 0)
		fail_exit("open %s", LIVEUPDATE_PCI_TEST_DEVICE);

	preserve_and_unpreserve(luo_fd, ctl_fd);

	session_fd = luo_create_session(luo_fd, DATA_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_create_session %s", DATA_SESSION_NAME);
	if (create_and_preserve_memfd(session_fd, DATA_MEMFD_TOKEN, text) < 0)
		fail_exit("Failed to preserve snapshot");

	for (i = 0; i < nr_devices; i++) {
		if (devices[i].role == ROLE_PRESERVED && devices[i].endpoint)
			preserve_saved_state(session_fd, ctl_fd, i);
	}

	session_fd = luo_create_session(luo_fd, DEVICE_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_create_session %s", DEVICE_SESSION_NAME);

	for (i = 0; i < nr_devices; i++) {
		struct pci_snapshot *dev = &devices[i];

		if (dev->role != ROLE_PRESERVED || !dev->endpoint)
			continue;

		ksft_print_msg("[STAGE 1] Preserving %s (token %d)\n", dev->bdf, dev->token);
		fd = get_device_fd(ctl_fd, dev->bdf);
		if (luo_session_preserve_fd(session_fd, fd, dev->token) < 0)
			fail_exit("Failed to preserve %s", dev->bdf);
		close(fd);
	}

	close(ctl_fd);
	write_marker(MARKER_STAGE1_DONE);

	close(luo_fd);
	daemonize_and_wait();
}

/*
 * Check that the test driver got back the saved state it preserved for
 * devices[i], without the DWORD it added that this kernel doesn't reserve, and
 * that restoring it writes back the register that stage 1 changed.
 */
static void check_saved_state(int fd, int i)
{
	const struct liveupdate_pci_test_saved_state *expected = &saved_states[i];
	struct liveupdate_pci_test_saved_state state;
	struct pci_snapshot *dev = &devices[i];
	int dword, n = 0, pos = changed_pos[i];
	uint16_t bit = changed_bit[i], saved;
	uint32_t value;

	ksft_print_msg("[STAGE 2] Checking the saved state of %s\n", dev->bdf);

	get_saved_state(fd, dev->bdf, &state);

	check(!dword_saved(&state, LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD),
	      "%s: Unreserved DWORD 0x%x was not dropped", dev->bdf,
	      LIVEUPDATE_PCI_TEST_UNRESERVED_DWORD);

	for (dword = 0; dword < LIVEUPDATE_PCI_TEST_CFG_DWORDS; dword++) {
		bool was_saved = dword_saved(expected, dword);
		bool is_saved = dword_saved(&state, dword);

		check(was_saved == is_saved, "%s: DWORD 0x%x %s saved", dev->bdf,
		      dword, is_saved ? "is now" : "is no longer");
		if (!was_saved || !is_saved)
			continue;

		saved_value(&state, dword * 4, &value);
		check(value == expected->dword_val[n],
		      "%s: Saved DWORD 0x%x changed from 0x%x to 0x%x", dev->bdf,
		      dword, expected->dword_val[n], value);
		n++;
	}

	if (!saved_value16(expected, pos, &saved))
		fail_exit("%s: Register 0x%x is not saved", dev->bdf, pos);

	value = read_config16(dev->bdf, pos);
	check(value == (saved ^ bit),
	      "%s: Register 0x%x is 0x%x before restoring, expected 0x%x",
	      dev->bdf, pos, value, saved ^ bit);

	check(!ioctl(fd, LIVEUPDATE_PCI_TEST_RESTORE_SAVED_STATE, 0),
	      "%s: Restoring the saved state failed (errno %d)", dev->bdf, errno);

	value = read_config16(dev->bdf, pos);
	check(value == saved,
	      "%s: Register 0x%x is 0x%x after restoring, expected 0x%x",
	      dev->bdf, pos, value, saved);
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	int fds[MAX_DEVICES] = {};
	int session_fd, ctl_fd, mfd, stage, i;
	char text[4096] = {};
	uint16_t flags;

	ksft_print_msg("[STAGE 2] Starting post-kexec verification...\n");

	restore_and_read_stage(state_session_fd, STATE_MEMFD_TOKEN, &stage);
	if (stage != 2)
		fail_exit("Expected stage 2, but state file contains %d", stage);

	session_fd = luo_retrieve_session(luo_fd, DATA_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_retrieve_session %s", DATA_SESSION_NAME);

	mfd = restore_and_verify_memfd(session_fd, DATA_MEMFD_TOKEN, NULL);
	if (mfd < 0)
		fail_exit("Failed to restore snapshot");
	if (read(mfd, text, sizeof(text) - 1) < 0)
		fail_exit("Failed to read snapshot");
	close(mfd);

	parse_snapshots(text);

	for (i = 0; i < nr_devices; i++) {
		struct pci_snapshot *dev = &devices[i];

		if (dev->role != ROLE_PRESERVED || !dev->endpoint)
			continue;

		mfd = restore_and_verify_memfd(session_fd, SAVED_STATE_MEMFD_TOKEN + i, NULL);
		if (mfd < 0)
			fail_exit("Failed to restore the saved state of %s", dev->bdf);
		memset(text, 0, sizeof(text));
		if (read(mfd, text, sizeof(text) - 1) < 0)
			fail_exit("Failed to read the saved state of %s", dev->bdf);
		close(mfd);

		parse_saved_state(text, &saved_states[i], &changed_pos[i],
				  &changed_bit[i]);
	}

	if (luo_session_finish(session_fd) < 0)
		fail_exit("luo_session_finish %s", DATA_SESSION_NAME);
	close(session_fd);

	if (nr_devices != nr_before)
		fail_exit("Expected %d devices, found %d", nr_devices, nr_before);

	check_devices("before probe", before);

	session_fd = luo_retrieve_session(luo_fd, DEVICE_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_retrieve_session %s", DEVICE_SESSION_NAME);

	ctl_fd = open(LIVEUPDATE_PCI_TEST_DEVICE, O_RDWR);
	if (ctl_fd < 0)
		fail_exit("open %s", LIVEUPDATE_PCI_TEST_DEVICE);

	for (i = 0; i < nr_devices; i++) {
		struct pci_snapshot *dev = &devices[i];

		if (!dev->endpoint)
			continue;

		if (dev->role == ROLE_CONTROL) {
			fds[i] = get_device_fd(ctl_fd, dev->bdf);
			flags = get_status(fds[i], dev->bdf);
			check(!(flags & LIVEUPDATE_PCI_TEST_INCOMING_AT_PROBE),
			      "%s: Control device was incoming-preserved", dev->bdf);
			continue;
		}

		ksft_print_msg("[STAGE 2] Retrieving %s (token %d)\n", dev->bdf, dev->token);
		fds[i] = luo_session_retrieve_fd(session_fd, dev->token);
		if (fds[i] < 0)
			fail_exit("Failed to retrieve %s", dev->bdf);

		flags = get_status(fds[i], dev->bdf);
		check(flags & LIVEUPDATE_PCI_TEST_INCOMING_AT_PROBE,
		      "%s: Device was not incoming-preserved at probe", dev->bdf);
		check(flags & LIVEUPDATE_PCI_TEST_INCOMING,
		      "%s: Device is not incoming-preserved", dev->bdf);
	}

	check_devices("after probe", NULL);

	for (i = 0; i < nr_devices; i++) {
		if (devices[i].role == ROLE_PRESERVED && devices[i].endpoint)
			check_saved_state(fds[i], i);
	}
	if (nr_failures)
		ksft_exit_fail_msg("%d check(s) failed\n", nr_failures);

	check_devices("after restoring saved state", NULL);

	ksft_print_msg("[STAGE 2] Finishing %s...\n", DEVICE_SESSION_NAME);
	if (luo_session_finish(session_fd) < 0)
		fail_exit("luo_session_finish %s", DEVICE_SESSION_NAME);

	for (i = 0; i < nr_devices; i++) {
		struct pci_snapshot *dev = &devices[i];

		if (!dev->endpoint || dev->role != ROLE_PRESERVED)
			continue;

		flags = get_status(fds[i], dev->bdf);
		check(!(flags & LIVEUPDATE_PCI_TEST_INCOMING),
		      "%s: Device is still incoming-preserved after finish", dev->bdf);
	}

	check_devices("after finish", NULL);

	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish %s", STATE_SESSION_NAME);

	write_marker(MARKER_STAGE2_DONE);
	ksft_print_msg("\n--- PCI LIVE UPDATE TEST PASSED ---\n");
}

static int get_stage(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc - 1; i++) {
		if (!strcmp(argv[i], "-s"))
			return atoi(argv[i + 1]);
	}

	return 1;
}

int main(int argc, char *argv[])
{
	int stage = get_stage(argc, argv);
	int i;

	mount_sysfs();
	parse_cmdline();

	if (stage == 2)
		write_marker(MARKER_STAGE2_START);

	add_paths(opt_preserve, ROLE_PRESERVED);
	add_paths(opt_control, ROLE_CONTROL);

	/*
	 * In stage 2, snapshot the devices before the test driver is loaded to
	 * see how the PCI core left them after enumeration. They are checked
	 * against the stage 1 snapshot once that is retrieved.
	 */
	if (stage == 2) {
		for (i = 0; i < nr_devices; i++)
			take_snapshot(&devices[i]);

		memcpy(before, devices, sizeof(devices));
		nr_before = nr_devices;
		nr_devices = 0;
	}

	/*
	 * LUO deserializes all preserved files when /dev/liveupdate is first
	 * opened, which requires the test driver's file handler.
	 */
	load_module();

	/* In stage 1, snapshot the devices once the test driver is bound. */
	if (stage == 1) {
		for (i = 0; i < nr_devices; i++)
			take_snapshot(&devices[i]);
	}

	return luo_test(argc, argv, STATE_SESSION_NAME, run_stage_1, run_stage_2);
}
