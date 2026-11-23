# SPDX-License-Identifier: GPL-2.0
#
# Preserve an endpoint on the root bus, i.e. with no bridges above it.

source "$test_dir/pci/pci_vmtest_lib.sh"

# QEMU reserves the routing IDs of IGB's VFs (offset 0x80, stride 2) on the same
# bus, i.e. devices 18-19 for the IGB at device 2 and 20-21 for device 4.
pci_igb igb1 pcie.0 0x2
pci_igb igb2 pcie.0 0x4

pci_preserve 0000:00:02.0
pci_control 0000:00:04.0
