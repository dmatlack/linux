#!/bin/bash

function main() {
	../../selftests/vfio/cleanup.sh

	echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
	echo 0 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages

	if [ -c /mnt/devtmpfs/liveupdate ]; then
		rm /dev/liveupdate
	fi
}

main "$@"
