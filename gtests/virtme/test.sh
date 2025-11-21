#!/bin/bash

set -x -e
tools/testing/selftests/vfio/scripts/setup.sh 0000:01:00.0
tools/testing/selftests/vfio/vfio_pci_liveupdate_kexec_test "$@" 0000:01:00.0
