CFLAGS = $(KHDR_INCLUDES)
TEST_GEN_PROGS_EXTENDED += vfio_pci_device_test
include ../lib.mk
include lib/libvfio.mk

CFLAGS += -I$(top_srcdir)/tools/include
CFLAGS += -MD

$(TEST_GEN_PROGS_EXTENDED): %: %.o $(LIBVFIO_O)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $< $(LIBVFIO_O) $(LDLIBS) -o $@

TEST_GEN_PROGS_EXTENDED_O = $(patsubst %, %.o, $(TEST_GEN_PROGS_EXTENDED))
TEST_DEP_FILES = $(patsubst %.o, %.d, $(TEST_GEN_PROGS_EXTENDED_O) $(LIBVFIO_O))
-include $(TEST_DEP_FILES)

EXTRA_CLEAN += $(TEST_GEN_PROGS_EXTENDED_O) $(TEST_DEP_FILES)
