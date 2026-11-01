# SPDX-License-Identifier: GPL-2.0
#
# Kernel modules used by the Live Update selftests.

ifneq ($(KERNELRELEASE),)

obj-m += liveupdate_pci_test.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build
TESTMODS_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# Skip the build if KDIR does not exist
modules:
ifneq ("$(wildcard $(KDIR))", "")
	$(Q)$(MAKE) -C $(KDIR) modules M=$(TESTMODS_DIR)
endif

clean:
ifneq ("$(wildcard $(KDIR))", "")
	$(Q)$(MAKE) -C $(KDIR) clean M=$(TESTMODS_DIR)
endif

endif
