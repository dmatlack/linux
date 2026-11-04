// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/*
 * Selftests for the Live Update Orchestrator.
 * This test suite verifies the functionality and behavior of the
 * /dev/liveupdate character device and its session management capabilities.
 *
 * Tests include:
 * - Device access: basic open/close, and enforcement of exclusive access.
 * - Session management: creation of unique sessions, and duplicate name detection.
 * - Resource preservation: successfully preserving individual and multiple memfds,
 *   verifying contents remain accessible.
 * - Complex multi-session scenarios involving mixed empty and populated files.
 * - Which tmpfs files can be preserved, and the frozen size of preserved files.
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/falloc.h>
#include <linux/fs.h>

#include <libliveupdate.h>
#include <linux/liveupdate.h>

#include "../kselftest.h"
#include "../kselftest_harness.h"

#define LIVEUPDATE_DEV "/dev/liveupdate"

FIXTURE(liveupdate_device) {
	int fd1;
	int fd2;
};

FIXTURE_SETUP(liveupdate_device)
{
	self->fd1 = -1;
	self->fd2 = -1;
}

FIXTURE_TEARDOWN(liveupdate_device)
{
	if (self->fd1 >= 0)
		close(self->fd1);
	if (self->fd2 >= 0)
		close(self->fd2);
}

/*
 * Test Case: Basic Open and Close
 *
 * Verifies that the /dev/liveupdate device can be opened and subsequently
 * closed without errors. Skips if the device does not exist.
 */
TEST_F(liveupdate_device, basic_open_close)
{
	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);

	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist.", LIVEUPDATE_DEV);

	ASSERT_GE(self->fd1, 0);
	ASSERT_EQ(close(self->fd1), 0);
	self->fd1 = -1;
}

/*
 * Test Case: Exclusive Open Enforcement
 *
 * Verifies that the /dev/liveupdate device can only be opened by one process
 * at a time. It checks that a second attempt to open the device fails with
 * the EBUSY error code.
 */
TEST_F(liveupdate_device, exclusive_open)
{
	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);

	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist.", LIVEUPDATE_DEV);

	ASSERT_GE(self->fd1, 0);
	self->fd2 = open(LIVEUPDATE_DEV, O_RDWR);
	EXPECT_LT(self->fd2, 0);
	EXPECT_EQ(errno, EBUSY);
}

/*
 * Test Case: Create Duplicate Session
 *
 * Verifies that attempting to create two sessions with the same name fails
 * on the second attempt with EEXIST.
 */
TEST_F(liveupdate_device, create_duplicate_session)
{
	int session_fd1, session_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);

	ASSERT_GE(self->fd1, 0);

	session_fd1 = luo_create_session(self->fd1, "duplicate-session-test");
	ASSERT_GE(session_fd1, 0);

	session_fd2 = luo_create_session(self->fd1, "duplicate-session-test");
	EXPECT_LT(session_fd2, 0);
	EXPECT_EQ(-session_fd2, EEXIST);

	ASSERT_EQ(close(session_fd1), 0);
}

/*
 * Test Case: Create Distinct Sessions
 *
 * Verifies that creating two sessions with different names succeeds.
 */
TEST_F(liveupdate_device, create_distinct_sessions)
{
	int session_fd1, session_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);

	ASSERT_GE(self->fd1, 0);

	session_fd1 = luo_create_session(self->fd1, "distinct-session-1");
	ASSERT_GE(session_fd1, 0);

	session_fd2 = luo_create_session(self->fd1, "distinct-session-2");
	ASSERT_GE(session_fd2, 0);

	ASSERT_EQ(close(session_fd1), 0);
	ASSERT_EQ(close(session_fd2), 0);
}

/*
 * Test Case: Preserve MemFD
 *
 * Verifies that a valid memfd can be successfully preserved in a session and
 * that its contents remain intact after the preservation call.
 */
TEST_F(liveupdate_device, preserve_memfd)
{
	const char *test_str = "hello liveupdate";
	char read_buf[64] = {};
	int session_fd, mem_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "preserve-memfd-test");
	ASSERT_GE(session_fd, 0);

	mem_fd = memfd_create("test-memfd", 0);
	ASSERT_GE(mem_fd, 0);

	ASSERT_EQ(write(mem_fd, test_str, strlen(test_str)), strlen(test_str));
	ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fd, 0x1234), 0);
	ASSERT_EQ(close(session_fd), 0);

	ASSERT_EQ(lseek(mem_fd, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd, read_buf, sizeof(read_buf)), strlen(test_str));
	ASSERT_STREQ(read_buf, test_str);
	ASSERT_EQ(close(mem_fd), 0);
}

/*
 * Test Case: Preserve Multiple MemFDs
 *
 * Verifies that multiple memfds can be preserved in a single session,
 * each with a unique token, and that their contents remain distinct and
 * correct after preservation.
 */
TEST_F(liveupdate_device, preserve_multiple_memfds)
{
	const char *test_str1 = "data for memfd one";
	const char *test_str2 = "data for memfd two";
	char read_buf[64] = {};
	int session_fd, mem_fd1, mem_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "preserve-multi-memfd-test");
	ASSERT_GE(session_fd, 0);

	mem_fd1 = memfd_create("test-memfd-1", 0);
	ASSERT_GE(mem_fd1, 0);
	mem_fd2 = memfd_create("test-memfd-2", 0);
	ASSERT_GE(mem_fd2, 0);

	ASSERT_EQ(write(mem_fd1, test_str1, strlen(test_str1)), strlen(test_str1));
	ASSERT_EQ(write(mem_fd2, test_str2, strlen(test_str2)), strlen(test_str2));

	ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fd1, 0xAAAA), 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fd2, 0xBBBB), 0);

	memset(read_buf, 0, sizeof(read_buf));
	ASSERT_EQ(lseek(mem_fd1, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd1, read_buf, sizeof(read_buf)), strlen(test_str1));
	ASSERT_STREQ(read_buf, test_str1);

	memset(read_buf, 0, sizeof(read_buf));
	ASSERT_EQ(lseek(mem_fd2, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd2, read_buf, sizeof(read_buf)), strlen(test_str2));
	ASSERT_STREQ(read_buf, test_str2);

	ASSERT_EQ(close(mem_fd1), 0);
	ASSERT_EQ(close(mem_fd2), 0);
	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Preserve Complex Scenario
 *
 * Verifies a more complex scenario with multiple sessions and a mix of empty
 * and non-empty memfds distributed across them.
 */
TEST_F(liveupdate_device, preserve_complex_scenario)
{
	const char *data1 = "data for session 1";
	const char *data2 = "data for session 2";
	char read_buf[64] = {};
	int session_fd1, session_fd2;
	int mem_fd_data1, mem_fd_empty1, mem_fd_data2, mem_fd_empty2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd1 = luo_create_session(self->fd1, "complex-session-1");
	ASSERT_GE(session_fd1, 0);
	session_fd2 = luo_create_session(self->fd1, "complex-session-2");
	ASSERT_GE(session_fd2, 0);

	mem_fd_data1 = memfd_create("data1", 0);
	ASSERT_GE(mem_fd_data1, 0);
	ASSERT_EQ(write(mem_fd_data1, data1, strlen(data1)), strlen(data1));

	mem_fd_empty1 = memfd_create("empty1", 0);
	ASSERT_GE(mem_fd_empty1, 0);

	mem_fd_data2 = memfd_create("data2", 0);
	ASSERT_GE(mem_fd_data2, 0);
	ASSERT_EQ(write(mem_fd_data2, data2, strlen(data2)), strlen(data2));

	mem_fd_empty2 = memfd_create("empty2", 0);
	ASSERT_GE(mem_fd_empty2, 0);

	ASSERT_EQ(luo_session_preserve_fd(session_fd1, mem_fd_data1, 0x1111), 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd1, mem_fd_empty1, 0x2222), 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd2, mem_fd_data2, 0x3333), 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd2, mem_fd_empty2, 0x4444), 0);

	ASSERT_EQ(lseek(mem_fd_data1, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd_data1, read_buf, sizeof(read_buf)), strlen(data1));
	ASSERT_STREQ(read_buf, data1);

	memset(read_buf, 0, sizeof(read_buf));
	ASSERT_EQ(lseek(mem_fd_data2, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd_data2, read_buf, sizeof(read_buf)), strlen(data2));
	ASSERT_STREQ(read_buf, data2);

	ASSERT_EQ(lseek(mem_fd_empty1, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd_empty1, read_buf, sizeof(read_buf)), 0);

	ASSERT_EQ(lseek(mem_fd_empty2, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd_empty2, read_buf, sizeof(read_buf)), 0);

	ASSERT_EQ(close(mem_fd_data1), 0);
	ASSERT_EQ(close(mem_fd_empty1), 0);
	ASSERT_EQ(close(mem_fd_data2), 0);
	ASSERT_EQ(close(mem_fd_empty2), 0);
	ASSERT_EQ(close(session_fd1), 0);
	ASSERT_EQ(close(session_fd2), 0);
}

/*
 * Test Case: Preserve Unsupported File Descriptor
 *
 * Verifies that attempting to preserve a file descriptor that does not have
 * a registered Live Update handler fails gracefully.
 * Uses /dev/null as a representative of a file type (character device)
 * that is not supported by the orchestrator.
 */
TEST_F(liveupdate_device, preserve_unsupported_fd)
{
	int session_fd, unsupported_fd;
	int ret;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "unsupported-fd-test");
	ASSERT_GE(session_fd, 0);

	unsupported_fd = open("/dev/null", O_RDWR);
	ASSERT_GE(unsupported_fd, 0);

	ret = luo_session_preserve_fd(session_fd, unsupported_fd, 0xDEAD);
	EXPECT_EQ(ret, -ENOENT);

	ASSERT_EQ(close(unsupported_fd), 0);
	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Prevent Double Preservation
 *
 * Verifies that a file (memfd) can only be preserved once across all active
 * sessions. Attempting to preserve it a second time, whether in the same or
 * a different session, should fail with EBUSY.
 */
TEST_F(liveupdate_device, prevent_double_preservation)
{
	int session_fd1, session_fd2, mem_fd;
	int ret;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd1 = luo_create_session(self->fd1, "double-preserve-session-1");
	ASSERT_GE(session_fd1, 0);
	session_fd2 = luo_create_session(self->fd1, "double-preserve-session-2");
	ASSERT_GE(session_fd2, 0);

	mem_fd = memfd_create("test-memfd", 0);
	ASSERT_GE(mem_fd, 0);

	/* First preservation should succeed */
	ASSERT_EQ(luo_session_preserve_fd(session_fd1, mem_fd, 0x1111), 0);

	/* Second preservation in a different session should fail with EBUSY */
	ret = luo_session_preserve_fd(session_fd2, mem_fd, 0x2222);
	EXPECT_EQ(ret, -EBUSY);

	/* Second preservation in the same session (different token) should fail with EBUSY */
	ret = luo_session_preserve_fd(session_fd1, mem_fd, 0x3333);
	EXPECT_EQ(ret, -EBUSY);

	ASSERT_EQ(close(mem_fd), 0);
	ASSERT_EQ(close(session_fd1), 0);
	ASSERT_EQ(close(session_fd2), 0);
}

/*
 * Test Case: Create Session with No Null Termination
 *
 * Verifies that filling the entire 64-byte name field with non-null characters
 * (no '\0' terminator) is rejected by the kernel with EINVAL.
 */
TEST_F(liveupdate_device, create_session_no_null_termination)
{
	struct liveupdate_ioctl_create_session args = {};

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	/* Fill entire name field with 'X', no null terminator */
	args.size = sizeof(args);
	memset(args.name, 'X', sizeof(args.name));

	EXPECT_LT(ioctl(self->fd1, LIVEUPDATE_IOCTL_CREATE_SESSION, &args), 0);
	EXPECT_EQ(errno, EINVAL);
}

/*
 * Test Case: Create Session with Empty Name
 *
 * Verifies that creating a session with an empty string name fails
 * with EINVAL.
 */
TEST_F(liveupdate_device, create_session_empty_name)
{
	int session_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "");
	EXPECT_EQ(session_fd, -EINVAL);
}

/*
 * Test Case: Get Session Name
 *
 * Verifies that the full session name can be retrieved from a session file
 * descriptor via ioctl.
 */
TEST_F(liveupdate_device, get_session_name)
{
	char name_buf[LIVEUPDATE_SESSION_NAME_LENGTH] = {};
	const char *session_name = "get-name-test-session";
	int session_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, session_name);
	ASSERT_GE(session_fd, 0);

	ASSERT_EQ(luo_get_session_name(session_fd, name_buf, sizeof(name_buf)), 0);
	ASSERT_STREQ(name_buf, session_name);

	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Get Session Name at Maximum Length
 *
 * Verifies that a session name using the full LIVEUPDATE_SESSION_NAME_LENGTH
 * (minus the null terminator) can be correctly retrieved.
 */
TEST_F(liveupdate_device, get_session_name_max_length)
{
	char name_buf[LIVEUPDATE_SESSION_NAME_LENGTH] = {};
	char long_name[LIVEUPDATE_SESSION_NAME_LENGTH];
	int session_fd;

	memset(long_name, 'A', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, long_name);
	ASSERT_GE(session_fd, 0);

	ASSERT_EQ(luo_get_session_name(session_fd, name_buf, sizeof(name_buf)), 0);
	ASSERT_STREQ(name_buf, long_name);

	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Manage Many Sessions
 *
 * Verifies that a large number of sessions can be created and then
 * destroyed during normal system operation. This specifically tests the
 * dynamic block allocation and reuse logic for session metadata management
 * without preserving any files.
 */
TEST_F(liveupdate_device, preserve_many_sessions)
{
#define MANY_SESSIONS 2000
	int session_fds[MANY_SESSIONS];
	int ret, i;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	ret = luo_ensure_nofile_limit(MANY_SESSIONS);
	if (ret == -EPERM)
		SKIP(return, "Insufficient privileges to set RLIMIT_NOFILE");
	ASSERT_EQ(ret, 0);

	for (i = 0; i < MANY_SESSIONS; i++) {
		char name[64];

		snprintf(name, sizeof(name), "many-session-%d", i);
		session_fds[i] = luo_create_session(self->fd1, name);
		ASSERT_GE(session_fds[i], 0);
	}

	for (i = 0; i < MANY_SESSIONS; i++)
		ASSERT_EQ(close(session_fds[i]), 0);
}

/*
 * Test Case: Preserve Many Files
 *
 * Verifies that a large number of files can be preserved in a single session
 * and then destroyed during normal system operation. This tests the dynamic
 * block allocation and management for outgoing files.
 */
TEST_F(liveupdate_device, preserve_many_files)
{
#define MANY_FILES 500
	int mem_fds[MANY_FILES];
	int session_fd, ret, i;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "many-files-test");
	ASSERT_GE(session_fd, 0);

	ret = luo_ensure_nofile_limit(MANY_FILES + 10);
	if (ret == -EPERM)
		SKIP(return, "Insufficient privileges to set RLIMIT_NOFILE");
	ASSERT_EQ(ret, 0);

	for (i = 0; i < MANY_FILES; i++) {
		mem_fds[i] = memfd_create("test-memfd", 0);
		ASSERT_GE(mem_fds[i], 0);
		ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fds[i], i), 0);
	}

	for (i = 0; i < MANY_FILES; i++)
		ASSERT_EQ(close(mem_fds[i]), 0);

	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Preserve MemFD with User Argument
 *
 * Verifies that passing a non-zero user argument to memfd preservation fails
 * with -EOPNOTSUPP, while passing a zero argument or using the minimum struct
 * size without the arg field succeeds.
 */
TEST_F(liveupdate_device, preserve_memfd_with_arg)
{
	struct liveupdate_session_preserve_fd min_arg = {};
	int session_fd, mem_fd1, mem_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "preserve-arg-test");
	ASSERT_GE(session_fd, 0);

	mem_fd1 = memfd_create("arg-memfd-1", 0);
	ASSERT_GE(mem_fd1, 0);

	/* Non-zero arg is not supported by memfd handler */
	EXPECT_EQ(luo_session_preserve_fd_arg(session_fd, mem_fd1, 0x1111, 1),
		  -EOPNOTSUPP);

	/* Legacy struct size (up to token) should zero-fill arg and succeed */
	min_arg.size = offsetof(struct liveupdate_session_preserve_fd, arg);
	min_arg.fd = mem_fd1;
	min_arg.token = 0x1111;
	ASSERT_EQ(ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &min_arg), 0);

	/* Explicit zero arg with full struct size should also succeed */
	mem_fd2 = memfd_create("arg-memfd-2", 0);
	ASSERT_GE(mem_fd2, 0);
	ASSERT_EQ(luo_session_preserve_fd_arg(session_fd, mem_fd2, 0x2222, 0), 0);

	ASSERT_EQ(close(session_fd), 0);
	ASSERT_EQ(close(mem_fd1), 0);
	ASSERT_EQ(close(mem_fd2), 0);
}

#define NOBODY 65534

FIXTURE(preserve_tmpfs) {
	char dir[32];
	int luo_fd;
	int session_fd;
	bool mounted;
};

FIXTURE_SETUP(preserve_tmpfs)
{
	self->luo_fd = -1;
	self->session_fd = -1;
	self->mounted = false;

	self->luo_fd = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->luo_fd < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->luo_fd, 0);

	self->session_fd = luo_create_session(self->luo_fd, "preserve-tmpfs");
	ASSERT_GE(self->session_fd, 0);

	strcpy(self->dir, "/tmp/luo-tmpfs-XXXXXX");
	ASSERT_NE(mkdtemp(self->dir), NULL);
	if (mount("tmpfs", self->dir, "tmpfs", 0, "size=1M"))
		SKIP(return, "Failed to mount tmpfs: %s", strerror(errno));
	self->mounted = true;
	ASSERT_EQ(chdir(self->dir), 0);
}

FIXTURE_TEARDOWN(preserve_tmpfs)
{
	if (self->session_fd >= 0)
		close(self->session_fd);
	if (self->luo_fd >= 0)
		close(self->luo_fd);
	if (chdir("/"))
		return;
	if (self->mounted)
		umount2(self->dir, MNT_DETACH);
	rmdir(self->dir);
}

/* Create the file @path with some contents and the mode @mode. */
static int create_file(const char *path, mode_t mode)
{
	int fd;

	fd = open(path, O_CREAT | O_EXCL | O_RDWR, mode);
	if (fd < 0)
		return -1;

	if (write(fd, "data", 4) != 4 || fchmod(fd, mode)) {
		close(fd);
		return -1;
	}

	return fd;
}

/* Open @path with @flags and preserve it. Return 0 or -errno. */
static int preserve_path(int session_fd, const char *path, int flags,
			 __u64 token)
{
	int fd, ret;

	fd = open(path, flags);
	if (fd < 0)
		return -errno;

	ret = luo_session_preserve_fd(session_fd, fd, token);
	close(fd);
	return ret;
}

/* Like preserve_path(), but as the user nobody. */
static int preserve_as_nobody(int session_fd, const char *path, __u64 token)
{
	int status;
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -errno;

	if (!pid) {
		if (setgroups(0, NULL) || setresgid(NOBODY, NOBODY, NOBODY) ||
		    setresuid(NOBODY, NOBODY, NOBODY))
			_exit(255);
		_exit(-preserve_path(session_fd, path, O_RDONLY, token));
	}

	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return -ECHILD;

	return -WEXITSTATUS(status);
}

/* Add the inode flag @flag, e.g. FS_IMMUTABLE_FL, to the file @fd. */
static int add_inode_flag(int fd, int flag)
{
	int flags;

	if (ioctl(fd, FS_IOC_GETFLAGS, &flags))
		return -errno;

	flags |= flag;
	if (ioctl(fd, FS_IOC_SETFLAGS, &flags))
		return -errno;

	return 0;
}

/*
 * Test Case: Preserve Through Read-Only and Write-Only Fds
 *
 * A file can be preserved through a read-only fd, but not through a
 * write-only fd, since the retrieved file can be read.
 */
TEST_F(preserve_tmpfs, access_mode)
{
	int fd;

	fd = create_file("file", 0644);
	ASSERT_GE(fd, 0);
	close(fd);

	EXPECT_EQ(preserve_path(self->session_fd, "file", O_WRONLY, 1), -EBADF);
	EXPECT_EQ(preserve_path(self->session_fd, "file", O_RDONLY, 2), 0);
}

/*
 * Test Case: Preserve Without Write Permission
 *
 * Preserving a file freezes its size for all of its users, so it requires
 * permission to write to the file.
 */
TEST_F(preserve_tmpfs, unwritable)
{
	int fd;

	fd = create_file("file", 0644);
	ASSERT_GE(fd, 0);
	close(fd);

	EXPECT_EQ(preserve_as_nobody(self->session_fd, "file", 1), -EACCES);
	EXPECT_EQ(preserve_path(self->session_fd, "file", O_RDONLY, 2), 0);
}

/*
 * Test Case: Preserve an Append-Only File
 *
 * Freezing the size of an append-only file would stop all writes to it, so
 * append-only files can't be preserved.
 */
TEST_F(preserve_tmpfs, append_only)
{
	int fd, ret;

	fd = create_file("file", 0644);
	ASSERT_GE(fd, 0);
	ret = add_inode_flag(fd, FS_APPEND_FL);
	close(fd);
	if (ret == -ENOTTY || ret == -EOPNOTSUPP)
		SKIP(return, "FS_IOC_SETFLAGS is not supported");
	ASSERT_EQ(ret, 0);

	EXPECT_EQ(preserve_path(self->session_fd, "file", O_RDONLY, 1), -EPERM);
}

/*
 * Test Case: Preserve an Immutable File
 *
 * The size of an immutable file can't change anyway, so its owner may preserve
 * it. Others still need permission to write to it.
 */
TEST_F(preserve_tmpfs, immutable)
{
	int fd, ret;

	fd = create_file("own", 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(fchown(fd, NOBODY, NOBODY), 0);
	ret = add_inode_flag(fd, FS_IMMUTABLE_FL);
	close(fd);
	if (ret == -ENOTTY || ret == -EOPNOTSUPP)
		SKIP(return, "FS_IOC_SETFLAGS is not supported");
	ASSERT_EQ(ret, 0);

	fd = create_file("other", 0666);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(add_inode_flag(fd, FS_IMMUTABLE_FL), 0);
	close(fd);

	EXPECT_EQ(preserve_as_nobody(self->session_fd, "own", 1), 0);
	EXPECT_EQ(preserve_as_nobody(self->session_fd, "other", 2), -EPERM);
}

/*
 * Test Case: Preserve Through a Read-Only Mount
 *
 * Preserving a file only needs permission to write to the file, not a writable
 * mount, so it works through a read-only mount.
 */
TEST_F(preserve_tmpfs, rdonly_mount)
{
	int fd;

	ASSERT_EQ(mkdir("src", 0755), 0);
	fd = create_file("src/file", 0644);
	ASSERT_GE(fd, 0);
	close(fd);

	ASSERT_EQ(mkdir("ro", 0755), 0);
	ASSERT_EQ(mount("src", "ro", NULL, MS_BIND, NULL), 0);
	ASSERT_EQ(mount(NULL, "ro", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY,
			NULL), 0);

	EXPECT_EQ(preserve_path(self->session_fd, "ro/file", O_RDONLY, 1), 0);
	umount2("ro", MNT_DETACH);
}

/*
 * Test Case: Preserve on a Read-Only Superblock
 *
 * The size of a file on a read-only superblock can't change anyway, so its
 * owner may preserve it. Others still need permission to write to it.
 */
TEST_F(preserve_tmpfs, rdonly_sb)
{
	int fd;

	fd = create_file("own", 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(fchown(fd, NOBODY, NOBODY), 0);
	close(fd);

	fd = create_file("other", 0666);
	ASSERT_GE(fd, 0);
	close(fd);

	ASSERT_EQ(mount(NULL, self->dir, NULL, MS_REMOUNT | MS_RDONLY, NULL),
		  0);

	EXPECT_EQ(preserve_as_nobody(self->session_fd, "own", 1), 0);
	EXPECT_EQ(preserve_as_nobody(self->session_fd, "other", 2), -EROFS);
}

/*
 * Test Case: Frozen Size of a Preserved File
 *
 * While a file is preserved, its size can't change and no holes can be punched
 * into it. Writes within the file still work. Closing the session unpreserves
 * the file, after which its size can change again.
 */
TEST_F(preserve_tmpfs, frozen_size)
{
	long page_size = getpagesize();
	struct stat st;
	char c;
	int fd;

	fd = create_file("file", 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(ftruncate(fd, 2 * page_size), 0);
	ASSERT_EQ(luo_session_preserve_fd(self->session_fd, fd, 1), 0);

	EXPECT_EQ(ftruncate(fd, page_size), -1);
	EXPECT_EQ(errno, EPERM);
	EXPECT_EQ(ftruncate(fd, 4 * page_size), -1);
	EXPECT_EQ(errno, EPERM);
	EXPECT_EQ(fallocate(fd, 0, 0, 4 * page_size), -1);
	EXPECT_EQ(errno, EPERM);
	EXPECT_EQ(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0,
			    page_size), -1);
	EXPECT_EQ(errno, EPERM);
	EXPECT_EQ(pwrite(fd, "x", 1, 2 * page_size), -1);
	EXPECT_EQ(errno, EPERM);

	/* The size didn't change, but writes within the file work. */
	ASSERT_EQ(fstat(fd, &st), 0);
	EXPECT_EQ(st.st_size, 2 * page_size);
	EXPECT_EQ(pwrite(fd, "x", 1, page_size), 1);
	EXPECT_EQ(pread(fd, &c, 1, page_size), 1);
	EXPECT_EQ(c, 'x');

	ASSERT_EQ(close(self->session_fd), 0);
	self->session_fd = -1;
	EXPECT_EQ(ftruncate(fd, page_size), 0);

	close(fd);
}

TEST_HARNESS_MAIN
