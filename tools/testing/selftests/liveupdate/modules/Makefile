# SPDX-License-Identifier: GPL-2.0
#
# Kernel modules used by the Live Update selftests.

ifneq ($(KERNELRELEASE),)

# Only build modules whose kernel APIs exist in the kernel being built against.
ifneq ($(wildcard $(srctree)/include/linux/pci_liveupdate.h),)
obj-m += liveupdate_pci_test.o
endif

else

KDIR ?= /lib/modules/$(shell uname -r)/build
TESTMODS_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# The kselftest Makefile passes its own output directory as O=, which must not
# be passed on to the kernel build.
MAKEOVERRIDES := $(filter-out O=%,$(MAKEOVERRIDES))

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
