#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2026, Google LLC.
# David Matlack <dmatlack@google.com>
"""Check PCI config writes against the devices preserved by luo_pci_test.

QEMU records every PCI config write with the pci_cfg_write trace event:

    pci_cfg_write igb 01:00.0 @0x4 <- 0x407

luo_pci_test prints a manifest of the preserved devices (endpoints and the
bridges above them) in stage 1, and writes markers to the config space of a
marker device:

    A: end of stage 1 (devices preserved, about to kexec)
    B: start of the stage 2 test binary
    C: end of stage 2 (devices finished)

Between A and C, i.e. during kexec, the next kernel's enumeration and the
driver's probe, no config write may disrupt a preserved device:

    - Bus Master Enable must not be cleared
    - The secondary and subordinate bus numbers of bridges must not change
    - Secondary Bus Reset must not be set on bridges
    - Function Level Reset must not be initiated
    - The device must not leave D0
    - ARI Forwarding Enable on bridges must not change
    - The ACS controls must not change

The trace only records the offset and value of each write, not its size. The
checks rely on the PCI core using natural alignment and the same access sizes
for these registers as it always does.

As a positive control, each control endpoint (bound to the test driver but not
preserved) must have Bus Master Enable cleared between A and B, by the
previous kernel during kexec.
"""

import argparse
import re
import sys

MANIFEST_PREFIX = "LUPCI-MANIFEST: "
MARKER_OFFSET = 0x40
MARKERS = {0x4C550001: "A", 0x4C550002: "B", 0x4C550003: "C"}

PCI_COMMAND = 0x04
PCI_COMMAND_MASTER = 0x4
PCI_PRIMARY_BUS = 0x18
PCI_SECONDARY_BUS = 0x19
PCI_SUBORDINATE_BUS = 0x1A
PCI_INTERRUPT_LINE = 0x3C
PCI_BRIDGE_CONTROL = 0x3E
PCI_BRIDGE_CTL_BUS_RESET = 0x40
PCI_EXP_DEVCTL = 0x08
PCI_EXP_DEVCTL_BCR_FLR = 0x8000
PCI_EXP_DEVCTL2 = 0x28
PCI_EXP_DEVCTL2_ARI = 0x20
PCI_PM_CTRL = 0x04
PCI_PM_CTRL_STATE_MASK = 0x3
PCI_ACS_CAP = 0x04
PCI_ACS_CTRL = 0x06

TRACE_RE = re.compile(
    r"pci_cfg_write (\S+) ([0-9a-f]{2}):([0-9a-f]{2})\.([0-7]) "
    r"@0x([0-9a-f]+) <- 0x([0-9a-f]+)"
)


class Device:
    def __init__(self, fields):
        self.bdf = fields["bdf"]
        domain, bus, devfn = self.bdf.split(":")
        slot, func = devfn.split(".")
        if int(domain, 16) != 0:
            raise ValueError(f"{self.bdf}: only domain 0 is supported")
        self.key = (int(bus, 16), int(slot, 16), int(func, 16))
        self.role = fields["role"]
        self.endpoint = int(fields["endpoint"]) != 0
        self.bridge = int(fields["bridge"]) != 0
        self.secondary = int(fields["sec"], 0)
        self.subordinate = int(fields["sub"], 0)
        self.pcie = int(fields["pcie"], 0)
        self.pm = int(fields["pm"], 0)
        self.acs = int(fields["acs"], 0)
        self.devctl2 = int(fields["devctl2"], 0)
        self.acs_ctrl = int(fields["acsctrl"], 0)
        self.nr_writes = 0
        self.cleared_master = False


def parse_manifest(path):
    devices = {}
    with open(path, errors="replace") as f:
        for line in f:
            idx = line.find(MANIFEST_PREFIX)
            if idx < 0:
                continue
            fields = dict(
                kv.split("=", 1)
                for kv in line[idx + len(MANIFEST_PREFIX):].split()
                if "=" in kv
            )
            dev = Device(fields)
            devices[dev.key] = dev
    return devices


def parse_trace(path):
    writes = []
    with open(path, errors="replace") as f:
        for line in f:
            m = TRACE_RE.search(line)
            if not m:
                continue
            name, bus, slot, func, offset, value = m.groups()
            writes.append(
                (
                    name,
                    (int(bus, 16), int(slot, 16), int(func, 16)),
                    int(offset, 16),
                    int(value, 16),
                )
            )
    return writes


def check_write(dev, offset, value):
    """Return a description of how the write disrupts dev, or None."""
    if offset == PCI_COMMAND and not value & PCI_COMMAND_MASTER:
        return "clears Bus Master Enable"

    if dev.bridge:
        if offset == PCI_PRIMARY_BUS:
            secondary = (value >> 8) & 0xFF
            subordinate = (value >> 16) & 0xFF
        elif offset == PCI_SECONDARY_BUS:
            secondary, subordinate = value & 0xFF, dev.subordinate
        elif offset == PCI_SUBORDINATE_BUS:
            secondary, subordinate = dev.secondary, value & 0xFF
        else:
            secondary, subordinate = dev.secondary, dev.subordinate

        if (secondary, subordinate) != (dev.secondary, dev.subordinate):
            return (
                f"changes bus numbers from [{dev.secondary:02x}-{dev.subordinate:02x}] "
                f"to [{secondary:02x}-{subordinate:02x}]"
            )

        if offset == PCI_BRIDGE_CONTROL and value & PCI_BRIDGE_CTL_BUS_RESET:
            return "sets Secondary Bus Reset"
        if offset == PCI_INTERRUPT_LINE and (value >> 16) & PCI_BRIDGE_CTL_BUS_RESET:
            return "sets Secondary Bus Reset"

    if dev.pcie:
        if offset == dev.pcie + PCI_EXP_DEVCTL and value & PCI_EXP_DEVCTL_BCR_FLR:
            return "initiates Function Level Reset"
        if (
            dev.bridge
            and offset == dev.pcie + PCI_EXP_DEVCTL2
            and (value ^ dev.devctl2) & PCI_EXP_DEVCTL2_ARI
        ):
            return "changes ARI Forwarding Enable"

    if dev.pm and offset == dev.pm + PCI_PM_CTRL and value & PCI_PM_CTRL_STATE_MASK:
        return f"puts the device in D{value & PCI_PM_CTRL_STATE_MASK}"

    if dev.acs:
        if offset == dev.acs + PCI_ACS_CTRL:
            acs_ctrl = value & 0xFFFF
        elif offset == dev.acs + PCI_ACS_CAP:
            acs_ctrl = value >> 16
        else:
            acs_ctrl = dev.acs_ctrl
        if acs_ctrl != dev.acs_ctrl:
            return f"changes ACS controls from 0x{dev.acs_ctrl:04x} to 0x{acs_ctrl:04x}"

    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True, help="Serial console log")
    parser.add_argument("--trace", required=True, help="QEMU pci_cfg_write trace")
    args = parser.parse_args()

    devices = parse_manifest(args.serial)
    if not devices:
        print("# No device manifest found in the serial log")
        return 1

    phase = None
    seen = set()
    failures = []

    for name, key, offset, value in parse_trace(args.trace):
        if offset == MARKER_OFFSET and value in MARKERS:
            phase = MARKERS[value]
            seen.add(phase)
            continue

        if phase not in ("A", "B") or key not in devices:
            continue

        dev = devices[key]
        if dev.role == "control":
            if (
                phase == "A"
                and dev.endpoint
                and offset == PCI_COMMAND
                and not value & PCI_COMMAND_MASTER
            ):
                dev.cleared_master = True
            continue

        dev.nr_writes += 1
        problem = check_write(dev, offset, value)
        if problem:
            failures.append(
                f"{dev.bdf} ({name}): write 0x{value:x} @0x{offset:x} "
                f"in phase {phase} {problem}"
            )

    for marker in MARKERS.values():
        if marker not in seen:
            failures.append(f"Marker {marker} not found in the trace")

    for dev in devices.values():
        if dev.role == "preserved":
            print(f"# {dev.bdf}: checked {dev.nr_writes} config write(s)")
        elif dev.endpoint and not dev.cleared_master:
            failures.append(
                f"{dev.bdf}: control device did not have Bus Master cleared "
                "during kexec (is the trace being checked correctly?)"
            )

    for failure in failures:
        print(f"# FAIL: {failure}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
