# SPDX-License-Identifier: GPL-2.0
#
# Common setup for the PCI Live Update selftests.
#
# Sourced by pci/*.vmtest.sh, which are themselves sourced by ../vmtest.sh
# after $arch, $target, $test_dir and $workspace_dir are set. See vmtest.sh for
# the TEST_* variables.
#
# Each test builds a PCIe topology out of the helpers below, using the QEMU
# "igb" device as the endpoint, and names the endpoints to preserve and the
# control endpoints (bound to the test driver but not preserved) by their path
# from the root bus. See luo_pci_test.c for the path format.

PCI_CHASSIS=0

TEST_BINARY=pci/luo_pci_test
TEST_MODULES=(liveupdate_pci_test)
TEST_KCONFIGS=(pci/config)
if [ -f "$test_dir/pci/config.$target" ]; then
	TEST_KCONFIGS+=("pci/config.$target")
fi
TEST_BUILD_NAME="pci"
TEST_TIMEOUT="180s"

TEST_QEMU_ARGS=()

# The default x86 machine (pc) does not support PCIe.
if [[ "$arch" == "x86" ]]; then
	TEST_QEMU_ARGS+=(-M q35)
fi

# pci_root_port <id> <addr>
#   Add a root port at device <addr> on the root bus.
function pci_root_port() {
	PCI_CHASSIS=$((PCI_CHASSIS + 1))
	TEST_QEMU_ARGS+=(-device "pcie-root-port,id=$1,bus=pcie.0,addr=$2,chassis=$PCI_CHASSIS")
}

# pci_igb <id> <bus> [<addr>]
#   Add an IGB endpoint at device <addr> (default 0) on <bus>.
function pci_igb() {
	TEST_QEMU_ARGS+=(-device "igb,id=$1,bus=$2,addr=${3:-0}")
}

# pci_preserve <path>[,<path>...]
#   Preserve the endpoints at <path>.
function pci_preserve() {
	TEST_CMDLINE+=" lupci.preserve=$1"
}

# pci_control <path>[,<path>...]
#   Bind the endpoints at <path> to the test driver, but don't preserve them.
#   Bus Master must be cleared on them during kexec.
function pci_control() {
	TEST_CMDLINE+=" lupci.control=$1"
}

# pci_topology_root_ports
#   Two root ports, with an endpoint to preserve below one and a control
#   endpoint below the other.
function pci_topology_root_ports() {
	pci_root_port rp1 0x2
	pci_igb igb1 rp1
	pci_root_port rp2 0x3
	pci_igb igb2 rp2

	pci_preserve 0000:00:02.0/00.0
	pci_control 0000:00:03.0/00.0
}
