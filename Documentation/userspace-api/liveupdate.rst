.. SPDX-License-Identifier: GPL-2.0

================
Live Update uAPI
================
:Author: Pasha Tatashin <pasha.tatashin@soleen.com>

ioctl interface
===============
.. kernel-doc:: kernel/liveupdate/luo_core.c
   :doc: LUO ioctl Interface

ioctl uAPI
===========
.. kernel-doc:: include/uapi/linux/liveupdate.h

File Preservation
=================

Files can be preserved across Live Update in sessions. Since only one process
can open /dev/liveupdate, sessions must be created by a centralized process
(e.g. "luod") and then passed via UDS to lower privilege processes (e.g. VMMs)
for them to preserve their own files.

luod::

  luo_fd = open("/dev/liveupdate", ...);

  ...

  // Create a new session with the given name.
  struct liveupdate_ioctl_create_session arg = {
          .size = sizeof(arg),
	  .name = SESSION_NAME,
  };
  ioctl(luo_fd, LIVEUPDATE_IOCTL_CREATE_SESSION, &arg);

  // Send session_fd to the VMM over UDS.
  send_session_fd(..., arg.fd);

VMM::

  // Receive the newly created session from luod over UDS
  session_fd = create_session(SESSION_NAME);

  ...

  // Preserve a file with a unique token value in the session.
  struct liveupdate_session_preserve_fd arg = {
          .size = sizeof(arg),
          .fd = fd,
          .token = TOKEN,
  }
  ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &arg);

Files can be unpreserved with the LIVEUPDATE_SESSION_UNPRESERVE_FD ioctl. They
are also unpreserved once the last reference to the session is dropped.  To
carry preserved files across a Live Update, references must be kept on the
session files through the reboot(LINUX_REBOOT_CMD_KEXEC) syscall.

While a file is preserved in a session, the kernel holds an extra reference
to it to prevent it from being destroyed.

Only the following types of files support LIVEUPDATE_SESSION_PRESERVE_FD. More
types of files are expected to be added in the future.

 - memfd
 - VFIO character device files (vfio-pci only)

File Retrieval
==============

Files that are preserved in a session retrieved after
reboot(LINUX_REBOOT_CMD_KEXEC).

luod::

  luo_fd = open("/dev/liveupdate", ...);

  ...

  struct liveupdate_ioctl_retrieve_session arg = {
          .size = sizeof(arg),
	  .name = SESSION_NAME,
  };
  ioctl(luo_fd, LIVEUPDATE_IOCTL_RETRIEVE_SESSION, &arg);

  // Send session_fd to VMM over UDS.
  send_session_fd(..., arg.fd);

VMM::

  // Receive the retrieved session from luod over UDS
  session_fd = retrieve_session(SESSION_NAME);

  ...

  // Retrieve the file associated with the token from the session.
  struct liveupdate_session_retrieve_fd arg = {
          .size = sizeof(arg),
          .token = TOKEN,
  };
  ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg);

  ...

  ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, ...);

A session can only be finished once all of the files within it have been
retrieved, and are fully restored from the kernel's perspective. The exact
requirements will vary by file type.

VFIO Character Device (cdev) Files
==================================

The kernel supports preserving VFIO character device files across Live Update
within a session::

  device_fd = open("/dev/vfio/devices/X");

  ...

  ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, { ..., device_fd, ...});

Attempting to preserve files acquired via VFIO_GROUP_GET_DEVICE_FD will fail.

Since the kernel holds an extra reference to files preserved in sessions, there
is no way for the underlying PCI device to be unbound from vfio-pci while it
is being preserved.

When a VFIO device file is preserved in a session, interrupts must be disabled
on the device or reboot(LINUX_REBOOT_CMD_KEXEC) will fail.

Preserved VFIO device files can be retrieved after a Live Update just like any
other preserved file::

  ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg);
  device_fd = arg.fd;

  ...

  ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, ...);

Attempting to open the device through /dev/vfio/devices/ or
VFIO_GROUP_GET_DEVICE_FD before LIVEUPDATE_SESSION_FINISH will fail.

VFIO devices files must be retrieved from the session and bound to an iommufd
for LIVEUPDATE_SESSION_FINISH to succeed.

The eventual goal of these support is to preserve devices running uninterrupted
across a Live Update. However there are many steps still needed to achieve this
(see Future Work below). So for now, VFIO will reset and restore the device
back into an idle state during reboot(LINUX_REBOOT_CMD_KEXEC).

Future work:

 - Preservation of iommufd files
 - Preservation of IOMMU driver state
 - Preservation of PCI state (BAR resources, device state, bridge state, ...)
 - Preservation of vfio-pci driver state

See Also
========

- :doc:`Live Update Orchestrator </core-api/liveupdate>`
