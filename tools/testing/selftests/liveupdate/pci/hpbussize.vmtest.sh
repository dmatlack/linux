# SPDX-License-Identifier: GPL-2.0
#
# Ask the next kernel to reserve extra bus numbers below hotplug bridges, which
# would change the bus numbers of bridges above a preserved endpoint if they
# were reassigned.

source "$test_dir/pci/pci_vmtest_lib.sh"

pci_topology_switch

# arm64 always assigns bus numbers, but x86 must be asked to.
if [[ "$arch" == "x86" ]]; then
	TEST_CMDLINE_STAGE2="pci=assign-busses,hpbussize=16"
else
	TEST_CMDLINE_STAGE2="pci=hpbussize=16"
fi
