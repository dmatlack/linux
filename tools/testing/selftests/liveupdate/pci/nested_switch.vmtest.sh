# SPDX-License-Identifier: GPL-2.0
#
# Preserve an endpoint below two levels of switches.

source "$test_dir/pci/pci_vmtest_lib.sh"

pci_root_port rp1 0x2
pci_switch sw1 rp1 dsp1 dsp2
pci_switch sw2 dsp1 dsp3
pci_igb igb1 dsp3
pci_igb igb2 dsp2

pci_preserve 0000:00:02.0/00.0/00.0/00.0/00.0/00.0
pci_control 0000:00:02.0/00.0/01.0/00.0
