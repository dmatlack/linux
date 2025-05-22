# SPDX-License-Identifier: GPL-2.0-or-later

readonly VFIO_PCI_DRIVER=/sys/bus/pci/drivers/vfio-pci

function bind() {
	echo "Binding ${1} to ${2}"
	echo "${1}" > "${2}/bind"
}

function unbind() {
	echo "Unbinding ${1} from ${2}"
	echo "${1}" > "${2}/unbind"
}

function set_sriov_numvfs() {
	echo "Setting ${1} sriov_numvfs to ${2}"
	echo ${2} > /sys/bus/pci/devices/${1}/sriov_numvfs
}

function add_id() {
	if echo $(echo ${1} | tr : ' ') > ${2}/new_id 2> /dev/null; then
		echo "Added ${1} to ${2}"
		return 0
	fi

	return 1
}

function remove_id() {
	echo "Removing ${1} from ${2}"
	echo $(echo ${1} | tr : ' ') > ${2}/remove_id
}

function cleanup() {
	if [ "${new_driver}" ]; then unbind ${bdf} ${new_driver} ; fi
	if [ "${new_id}"     ]; then remove_id ${device_id} ${VFIO_PCI_DRIVER} ; fi
	if [ "${old_driver}" ]; then bind ${bdf} ${old_driver} ; fi
	if [ "${old_numvfs}" ]; then set_sriov_numvfs ${bdf} ${old_numvfs} ; fi
}

function usage() {
	echo "usage: $0 [-d segment:bus:device.function] [-s] [-h] [cmd ...]" >&2
	echo >&2
	echo "  -d: The BDF of the device to use for the test (required)" >&2
	echo "  -h: Show this help message" >&2
	echo "  -s: Drop into a shell rather than running a command" >&2
	echo >&2
	echo "   cmd: The command to run and arguments to pass to it." >&2
	echo "        Required when not using -s. The SBDF will be " >&2
	echo "        appended to the argument list." >&2
	exit 1
}

function main() {
	while getopts "d:hs" opt; do
		case $opt in
			d) bdf="$OPTARG" ;;
			s) shell=true ;;
			*) usage ;;
		esac
	done

	# Shift past all optional arguments.
	shift $((OPTIND - 1))

	# Check that the user passed in the command to run.
	[ ! "${shell}" ] && [ $# = 0 ] && usage

	# Check that the user passed in a BDF.
	[ "${bdf}" ] || usage

	trap cleanup EXIT
	set -e

	test -d /sys/bus/pci/devices/${bdf}

	device_id=$(lspci -s ${bdf} -n | cut -d' ' -f3)

	if [ -f /sys/bus/pci/devices/${bdf}/sriov_numvfs ]; then
		old_numvfs=$(cat /sys/bus/pci/devices/${bdf}/sriov_numvfs)
		set_sriov_numvfs ${bdf} 0
	fi

	if [ -L /sys/bus/pci/devices/${bdf}/driver ]; then
		old_driver=$(readlink -m /sys/bus/pci/devices/${bdf}/driver)
		unbind ${bdf} ${old_driver}
	fi

	# Add the device ID to vfio-pci. If it hasn't already been added, this will
	# succeed and bind the device to vfio-pci. If it has already been added, this
	# will fail and we have to manually bind the device.
	if add_id ${device_id} ${VFIO_PCI_DRIVER}; then
		new_id=true
	else
		bind ${bdf} ${VFIO_PCI_DRIVER}
	fi

	new_driver=${VFIO_PCI_DRIVER}

	echo
	if [ "${shell}" ]; then
		echo "Dropping into ${SHELL} with BDF=${bdf}"
		BDF=${bdf} ${SHELL}
	else
		"$@" ${bdf}
	fi
	echo
}

main "$@"
