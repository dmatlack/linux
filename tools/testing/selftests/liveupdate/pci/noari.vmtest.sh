# SPDX-License-Identifier: GPL-2.0
#
# Disable ARI in the first kernel only. IGB implements ARI, so the next kernel
# enables ARI Forwarding on the root port above the control endpoint, but must
# adopt it (i.e. leave it disabled) on the root port above the preserved
# endpoint.

source "$test_dir/pci/pci_vmtest_lib.sh"

pci_topology_root_ports

TEST_CMDLINE+=" lupci.control_changes=ari"
TEST_CMDLINE_STAGE1="pci=noari"
