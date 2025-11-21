#!/bin/bash

set -e

readonly DIR=$(dirname -- ${BASH_SOURCE[0]})

function add_hugepages() {
	echo ${1} > ${2}
	test ${1} = $(cat ${2})
}

function try_setup_devices() {
	local device_id=${1}

	if ../../selftests/vfio/run.sh echo > /dev/null; then
		echo "Skipping ${device_id}, devices are already set up."
		return
	fi

	for bdf in $(lspci -D -d ${device_id} | cut -d' ' -f1); do
		../../selftests/vfio/setup.sh ${bdf}
	done
}

function main() {
	try_setup_devices 8086:0B25 # Intel DSA
	try_setup_devices 8086:1457 # Diorite NVMe PF

	add_hugepages 1 /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
	add_hugepages 1 /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages

	if [ -c /mnt/devtmpfs/liveupdate ]; then
		ln -f -s /mnt/devtmpfs/liveupdate /dev/liveupdate
	fi
}

main "$@"
