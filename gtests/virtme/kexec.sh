#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -e

full_cmdline=$(cat /proc/cmdline)
builtin_cmdline=$(zcat /proc/config.gz|grep CONFIG_CMDLINE=|cut -f2 -d\")
cmdline=${full_cmdline/$builtin_cmdline /}

set -x
kexec -l -s --command-line="${cmdline}" --initrd=./.virtme_initramfs arch/x86/boot/bzImage
kexec -e
