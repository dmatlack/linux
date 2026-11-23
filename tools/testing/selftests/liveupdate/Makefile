# SPDX-License-Identifier: GPL-2.0-only

# With NOLIBC=1, build the tests that run across kexec, and the init process
# that runs them, as static binaries against nolibc. vmtest.sh uses this to
# build the initrds for its VMs.
ifeq ($(NOLIBC),)
TEST_GEN_PROGS += liveupdate
else
TEST_GEN_FILES += init
endif

TEST_GEN_PROGS_EXTENDED += luo_kexec_simple
TEST_GEN_PROGS_EXTENDED += luo_multi_session
TEST_GEN_PROGS_EXTENDED += luo_stress_sessions
TEST_GEN_PROGS_EXTENDED += luo_stress_files
TEST_GEN_PROGS_EXTENDED += pci/luo_pci_test

TEST_FILES += do_kexec.sh

TEST_GEN_MODS_DIR := modules

include ../lib.mk
include lib/libliveupdate.mk

ifneq ($(NOLIBC),)
CFLAGS += -static -nostdlib -nostdinc -I$(top_srcdir)/tools/include/nolibc
endif
CFLAGS += $(KHDR_INCLUDES)
CFLAGS += -Wall -O2 -Wno-unused-function
CFLAGS += -MD

TEST_O := $(patsubst %, %.o, $(TEST_GEN_PROGS))
TEST_O += $(patsubst %, %.o, $(TEST_GEN_PROGS_EXTENDED))

TEST_DEP_FILES := $(patsubst %.o, %.d, $(LIBLIVEUPDATE_O))
TEST_DEP_FILES += $(patsubst %.o, %.d, $(TEST_O))
-include $(TEST_DEP_FILES)

$(TEST_O): $(OUTPUT)/%.o: %.c
	$(Q)mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c $< -o $@

$(TEST_GEN_PROGS) $(TEST_GEN_PROGS_EXTENDED): $(OUTPUT)/%: $(OUTPUT)/%.o $(LIBLIVEUPDATE_O)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH) $< $(LIBLIVEUPDATE_O) $(LDLIBS) -o $@

EXTRA_CLEAN += $(TEST_O)
EXTRA_CLEAN += $(TEST_DEP_FILES)
