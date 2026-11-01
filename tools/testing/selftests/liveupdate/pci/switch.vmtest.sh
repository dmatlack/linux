# SPDX-License-Identifier: GPL-2.0
#
# Preserve an endpoint below a switch, next to a control endpoint below another
# downstream port of the same switch.

source "$test_dir/pci/pci_vmtest_lib.sh"

pci_topology_switch
