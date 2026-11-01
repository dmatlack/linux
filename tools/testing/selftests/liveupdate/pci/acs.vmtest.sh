# SPDX-License-Identifier: GPL-2.0
#
# Configure different ACS controls in each kernel. The next kernel must adopt
# the ACS controls of bridges above a preserved endpoint, and apply its own to
# the root port above the control endpoint.

source "$test_dir/pci/pci_vmtest_lib.sh"

pci_topology_root_ports

# Bits, from most to least significant: DT EC UF CR RR TB SV
TEST_CMDLINE+=" lupci.control_changes=acs"
TEST_CMDLINE_STAGE1="pci=config_acs=xx11101@pci:0:0"
TEST_CMDLINE_STAGE2="pci=config_acs=xx00000@pci:0:0"
