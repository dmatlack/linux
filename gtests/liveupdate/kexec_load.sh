#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e

# Determine the boot command line we need to pass to the kexec kernel.  Note
# that the kernel will append to it its builtin command line, so make sure we
# subtract the builtin command to avoid accumulating kernel parameters and
# eventually overflowing the command line.
full_cmdline=$(cat /proc/cmdline)
builtin_cmdline=$(zcat /proc/config.gz|grep CONFIG_CMDLINE=|cut -f2 -d\")
cmdline=${full_cmdline/$builtin_cmdline /}

append_cmdline=""
remove_cmdline=""

while getopts 'a:r:' flag; do
  case "${flag}" in
    a) append_cmdline=" ${OPTARG}" ;;
    r) remove_cmdline=" ${OPTARG}" ;;
    *) exit 1 ;;
  esac
done

# Replace this with check_cmd when b/427483588 is fixed.
if [ -n "$append_cmdline" ] && ! grep -q nd_e820 /proc/cmdline; then
  cmdline+=$append_cmdline
elif [ -n "$remove_cmdline" ] && grep -q "$remove_cmdline" /proc/cmdline; then
  cmdline=${cmdline/$remove_cmdline/}
fi

if zgrep -q "CONFIG_DEBUG_VM=y" /proc/config.gz; then
	if ! grep -q "earlyprintk=" <<< "$cmdline"; then
		echo "INFO: Debug kernel, appending early printk."
		cmdline="$cmdline earlyprintk=ttyS0,115200 loglevel=8"
	fi
fi

set -x

kexec -l -a --command-line="${cmdline}" /boot/vmlinuz-$(uname -r)

# Tell /etc/init.d/kexec that it should perform a kexec during shutdown.
touch /var/google/session/initscripts/reboot_skip_bios

# Tell /etc/init.d/kexec that the kexec kernel is already loaded.
touch /var/google/session/initscripts/kexec_kernel_loaded
