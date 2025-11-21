#!/bin/bash

set -e

if [ -f ../selftests/vfio/setup.sh ]; then
	../selftests/vfio/run.sh "$@"
else
	# TODO: Remove all this once setup.sh is upstream.

	# Intel DSA
	bdf=$(lspci -D -d 8086:0B25 | head -1 | cut -d' ' -f1)

	# Diorite NVMe PF
	if [ -z "${bdf}" ]; then
		bdf=$(lspci -D -d 8086:1457 | head -1 | cut -d' ' -f1)
	fi

	if [ "${bdf}" ]; then
		../selftests/vfio/run.sh -d ${bdf} -- "$@"
	else
		echo "No devices found."
		exit 4
	fi
fi
