# SPDX-License-Identifier: GPL-2.0
#
# Ask the next kernel to assign all bus numbers itself, which it must not do for
# bridges above a preserved endpoint.
#
# arm64 always assigns bus numbers, so every arm64 test already covers this.

source "$test_dir/pci/pci_vmtest_lib.sh"

if [[ "$arch" != "x86" ]]; then
	skip "pci=assign-busses is only supported on x86"
fi

pci_topology_switch
TEST_CMDLINE_STAGE2="pci=assign-busses"
