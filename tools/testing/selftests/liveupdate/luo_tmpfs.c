// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 *
 * Test the preservation of tmpfs files across a kexec, and retrieving them into
 * a file on a tmpfs mount with struct liveupdate_shmem_retrieve_arg.
 *
 * Each test case preserves its source files in stage 1 and checks how they can
 * be retrieved in stage 2. The cases use separate tokens, so that a failed case
 * doesn't affect the others. In stage 2, each case mounts the tmpfs instances
 * it needs, so that their block usage can be checked exactly.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * Include this before sys/mman.h, which only defines the MFD_* flags that
 * aren't defined yet.
 */
#include <linux/memfd.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/falloc.h>
#include <linux/fs.h>

#ifdef NOLIBC
#include <asm/statfs.h>
#else
#include <sys/statfs.h>
#endif

#include <libliveupdate.h>

#ifndef MNT_DETACH
#define MNT_DETACH		2
#endif
#ifndef O_TMPFILE
#define O_TMPFILE		(020000000 | O_DIRECTORY)
#endif

#define STATE_SESSION_NAME	"tmpfs_state"
#define STATE_TOKEN		999
#define SESSION_NAME		"tmpfs"

/*
 * Everything happens on a tmpfs mounted at TEST_DIR. In stage 1, the source
 * files are created on the tmpfs mounted at TEST_DIR/src. In stage 2, each
 * case runs in the directory TEST_DIR/<name of the case>.
 */
#define TEST_DIR		"/luo_tmpfs"

#define SMALL_SIZE		8192
#define SPARSE_PAGES		8
#define NOBODY			65534
#define TEST_UMASK		022

/* The exec case runs a copy of this test with EXEC_ARG, see exec_main(). */
#define EXEC_ARG		"--exec"
#define EXEC_STATUS		200

/* TAINT_BAD_PAGE, TAINT_DIE and TAINT_WARN. */
#define BAD_TAINT		((1L << 5) | (1L << 7) | (1L << 9))

/*
 * Checks for the test cases. On failure, they print the failed check and
 * return KSFT_FAIL from the calling function.
 */
#define CHECK(cond) do {						\
	if (!(cond)) {							\
		ksft_print_msg("%s:%d: Check failed: %s (errno %d)\n",	\
			       __func__, __LINE__, #cond, errno);	\
		return KSFT_FAIL;					\
	}								\
} while (0)

#define CHECK_EQ(expr, expected) do {					\
	long long __val = (expr), __exp = (expected);			\
									\
	if (__val != __exp) {						\
		ksft_print_msg("%s:%d: %s is %lld, expected %lld\n",	\
			       __func__, __LINE__, #expr, __val, __exp); \
		return KSFT_FAIL;					\
	}								\
} while (0)

/* Check that a system call fails with @err. */
#define CHECK_ERRNO(expr, err) do {					\
	long __ret = (expr);						\
	int __err = errno, __exp = (err);				\
									\
	if (__ret != -1 || __err != __exp) {				\
		ksft_print_msg("%s:%d: %s: %ld, errno %d, expected %d\n", \
			       __func__, __LINE__, #expr,		\
			       __ret, __err, __exp);			\
		return KSFT_FAIL;					\
	}								\
} while (0)

/* Call a function that returns a KSFT_* code, and return if it fails. */
#define TRY(expr) do {							\
	int __ret = (expr);						\
									\
	if (__ret != KSFT_PASS) {					\
		ksft_print_msg("%s:%d: %s failed\n",			\
			       __func__, __LINE__, #expr);		\
		return __ret;						\
	}								\
} while (0)

struct test_case {
	const char *name;
	/* Size and file position of the source file. */
	size_t size;
	off_t pos;
	/* Stage 1: Preserve the source files. Defaults to preserve_file(). */
	int (*preserve)(struct test_case *tc);
	/* Stage 2: Retrieve the files and check them. */
	int (*verify)(struct test_case *tc);
	/* Stage 2: Optional checks after the session is finished. */
	int (*after_finish)(struct test_case *tc);
	/* The tokens of the case are token to token + 0xff. */
	__u64 token;
	/* State kept from verify() for after_finish(). */
	int result;
};

static unsigned long page_size;
static int dev_fd;
static int session_fd;

/* nolibc doesn't have these, so use syscall() where needed. */
static int do_fcntl(int fd, int cmd, long arg)
{
	return syscall(__NR_fcntl, fd, cmd, arg);
}

static int do_fallocate(int fd, int mode, off_t offset, off_t len)
{
	return syscall(__NR_fallocate, fd, mode, offset, len);
}

static int do_mount(const char *source, const char *target, const char *type,
		    unsigned long flags, const char *data)
{
	return syscall(__NR_mount, source, target, type, flags, data);
}

static int do_statfs(const char *path, struct statfs *buf)
{
#ifdef NOLIBC
	return syscall(__NR_statfs, path, buf);
#else
	return statfs(path, buf);
#endif
}

static unsigned long nr_pages(size_t size)
{
	return (size + page_size - 1) / page_size;
}

static long long file_size(int fd)
{
	struct stat st;

	if (fstat(fd, &st))
		return -1;

	return st.st_size;
}

/* Return the number of pages charged to the file. */
static long file_pages(int fd)
{
	struct stat st;

	if (fstat(fd, &st))
		return -1;

	return st.st_blocks * 512 / page_size;
}

/* Return the number of pages used on the tmpfs mounted at @path. */
static long mnt_used(const char *path)
{
	struct statfs buf;

	if (do_statfs(path, &buf))
		return -1;

	return buf.f_blocks - buf.f_bfree;
}

/* Mount a tmpfs with room for @pages pages at @dir. */
static int mount_tmpfs(const char *dir, unsigned long pages, const char *opts)
{
	char data[64];

	if (mkdir(dir, 0755) && errno != EEXIST)
		return -errno;

	snprintf(data, sizeof(data), "size=%lu%s%s", pages * page_size,
		 opts ? "," : "", opts ? opts : "");
	if (do_mount("tmpfs", dir, "tmpfs", 0, data))
		return -errno;

	return 0;
}

/* Read a number from the file at @path. Return -1 on failure. */
static long read_long(const char *path)
{
	char buf[32] = {};
	ssize_t len;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0)
		return -1;

	return strtol(buf, NULL, 10);
}

/* Retrieve with the argument @arg. Return the retrieved fd or -errno. */
static int retrieve_arg(__u64 token, struct liveupdate_shmem_retrieve_arg *arg)
{
	return luo_session_retrieve_fd_arg(session_fd, token,
					   (__u64)(uintptr_t)arg);
}

/*
 * Retrieve the file preserved with @token into the file @target. Return @target
 * or -errno.
 */
static int retrieve_into(__u64 token, int target)
{
	struct liveupdate_shmem_retrieve_arg arg = {
		.size = sizeof(arg),
		.fd = target,
	};

	return retrieve_arg(token, &arg);
}

/* Create an empty, unlinked file on the tmpfs mount of the directory @dir. */
static int new_target(const char *dir)
{
	return open(dir, O_TMPFILE | O_RDWR, 0644);
}

/*
 * Retrieve the file preserved with @token into a new file created with
 * new_target(@dir). Return the fd of the new file or -errno.
 */
static int retrieve_new(__u64 token, const char *dir)
{
	int target, ret;

	target = new_target(dir);
	if (target < 0)
		return -errno;

	ret = retrieve_into(token, target);
	if (ret < 0)
		close(target);
	return ret;
}

/* Link the unlinked file @fd at @path. */
static int link_fd(int fd, const char *path)
{
	char fd_path[64];

	snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
	return syscall(__NR_linkat, AT_FDCWD, fd_path, AT_FDCWD, path,
		       AT_SYMLINK_FOLLOW);
}

/* Open the file @fd again with @flags. */
static int reopen_fd(int fd, int flags)
{
	char fd_path[64];

	snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
	return open(fd_path, flags);
}

/* The contents of the files depend on their token. */
static unsigned char pattern(__u64 token, size_t offset)
{
	unsigned int word;

	word = (offset / 4 + 1) * 2654435761u ^ token * 2246822519u;
	return word >> (offset % 4 * 8);
}

static int write_pattern(int fd, size_t offset, size_t len, __u64 token)
{
	static unsigned char buf[65536];
	size_t i, n;

	if (lseek(fd, offset, SEEK_SET) != (off_t)offset)
		return -1;

	for (; len; offset += n, len -= n) {
		n = len < sizeof(buf) ? len : sizeof(buf);
		for (i = 0; i < n; i++)
			buf[i] = pattern(token, offset + i);
		if (write(fd, buf, n) != (ssize_t)n)
			return -1;
	}

	return 0;
}

/*
 * Check that [start, end) of @p has the contents of the file preserved with
 * @token, or zeroes if @token is 0.
 */
static int check_range(const unsigned char *p, size_t start, size_t end,
		       __u64 token)
{
	unsigned char expected;
	size_t i;

	for (i = start; i < end; i++) {
		expected = token ? pattern(token, i) : 0;
		if (p[i] != expected) {
			ksft_print_msg("Byte %zu is %#x, expected %#x\n",
				       i, p[i], expected);
			return KSFT_FAIL;
		}
	}

	return KSFT_PASS;
}

/* Check that the file has the first @size bytes of the pattern for @token. */
static int check_contents(int fd, size_t size, __u64 token)
{
	size_t len = nr_pages(size) * page_size;
	unsigned char *p;
	int ret;

	if (!size)
		return KSFT_PASS;

	p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED);

	ret = check_range(p, 0, size, token);
	/* The rest of the last page must be zero. */
	if (ret == KSFT_PASS)
		ret = check_range(p, size, len, 0);

	munmap(p, len);
	return ret;
}

/* Check that @fd has the size and contents of the source of @tc. */
static int check_restored(struct test_case *tc, int fd)
{
	CHECK_EQ(file_size(fd), tc->size);
	CHECK_EQ(file_pages(fd), nr_pages(tc->size));
	return check_contents(fd, tc->size, tc->token);
}

/* Create a file with the first @size bytes of the pattern for @token. */
static int create_source(const char *path, size_t size, __u64 token)
{
	int fd;

	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0644);
	if (fd < 0)
		return -1;

	if (write_pattern(fd, 0, size, token)) {
		close(fd);
		return -1;
	}

	return fd;
}

static int preserve_fd(int fd, off_t pos, __u64 token)
{
	CHECK_EQ(lseek(fd, pos, SEEK_SET), pos);
	CHECK_EQ(luo_session_preserve_fd(session_fd, fd, token), 0);
	return KSFT_PASS;
}

static int preserve_source(const char *path, size_t size, off_t pos,
			   __u64 token)
{
	int fd, ret;

	fd = create_source(path, size, token);
	CHECK(fd >= 0);
	ret = preserve_fd(fd, pos, token);
	close(fd);

	return ret;
}

/* The default for test_case.preserve(). */
static int preserve_file(struct test_case *tc)
{
	char path[128];

	snprintf(path, sizeof(path), "src/%s", tc->name);
	return preserve_source(path, tc->size, tc->pos, tc->token);
}

/* Preserve a memfd with the contents of the source of @tc. */
static int preserve_memfd(struct test_case *tc, unsigned int flags,
			  unsigned int seals)
{
	int fd, ret;

	fd = memfd_create(tc->name, flags);
	CHECK(fd >= 0);
	CHECK_EQ(write_pattern(fd, 0, tc->size, tc->token), 0);
	if (seals)
		CHECK_EQ(do_fcntl(fd, F_ADD_SEALS, seals), 0);
	ret = preserve_fd(fd, tc->pos, tc->token);
	close(fd);

	return ret;
}

static int drop_privileges(void)
{
	CHECK_EQ(syscall(__NR_setgroups, 0, NULL), 0);
	CHECK_EQ(syscall(__NR_setresgid, NOBODY, NOBODY, NOBODY), 0);
	CHECK_EQ(syscall(__NR_setresuid, NOBODY, NOBODY, NOBODY), 0);
	return KSFT_PASS;
}

/* Run @fn(@tc) in a child process, and return its result. */
static int run_child(int (*fn)(struct test_case *tc), struct test_case *tc)
{
	int status, ret;
	pid_t pid;

	fflush(stdout);
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		ret = fn(tc);
		fflush(stdout);
		_exit(ret);
	}

	CHECK_EQ(waitpid(pid, &status, 0), pid);
	CHECK(WIFEXITED(status));
	return WEXITSTATUS(status);
}

/*
 * A copy of this test that is executed with EXEC_ARG writes a 0 byte to stdout
 * once it runs, and exits with EXEC_STATUS once stdin is closed.
 */
static int exec_main(void)
{
	char c;

	if (write(STDOUT_FILENO, "", 1) != 1)
		return 1;

	while (read(STDIN_FILENO, &c, 1) > 0)
		continue;

	return EXEC_STATUS;
}

/* Copy the binary of this test to a new file at @path. */
static int copy_self(const char *path)
{
	static char buf[65536];
	int in_fd, out_fd;
	ssize_t len;

	in_fd = open("/proc/self/exe", O_RDONLY);
	CHECK(in_fd >= 0);
	out_fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0755);
	CHECK(out_fd >= 0);

	while ((len = read(in_fd, buf, sizeof(buf))) > 0)
		CHECK_EQ(write(out_fd, buf, len), len);
	CHECK_EQ(len, 0);

	close(out_fd);
	close(in_fd);
	return KSFT_PASS;
}

/* Return the number of pages of the binary of this test. */
static long self_pages(void)
{
	struct stat st;

	if (stat("/proc/self/exe", &st))
		return -1;

	return nr_pages(st.st_size);
}

/* Check that @fd has the contents of the binary of this test. */
static int check_self(int fd)
{
	long long size = file_size(fd);
	void *p, *self;
	int self_fd;

	self_fd = open("/proc/self/exe", O_RDONLY);
	CHECK(self_fd >= 0);
	CHECK(size > 0);
	CHECK_EQ(file_size(self_fd), size);

	p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED);
	self = mmap(NULL, size, PROT_READ, MAP_SHARED, self_fd, 0);
	CHECK(self != MAP_FAILED);
	CHECK(!memcmp(p, self, size));

	munmap(self, size);
	munmap(p, size);
	close(self_fd);
	return KSFT_PASS;
}

/*
 * Execute the copy of this test at @path and wait until it runs. Return the
 * write end of its stdin, which ends it when closed, or -errno if it can't be
 * executed.
 */
static int start_exe(const char *path, pid_t *pid)
{
	char *const argv[] = { (char *)path, EXEC_ARG, NULL };
	char *const envp[] = { NULL };
	unsigned char err = EIO;
	int in[2], out[2], status;

	if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC))
		return -errno;

	fflush(stdout);
	*pid = fork();
	if (!*pid) {
		if (dup2(in[0], STDIN_FILENO) >= 0 &&
		    dup2(out[1], STDOUT_FILENO) >= 0)
			execve(path, argv, envp);

		/* Report the error instead of the 0 byte from exec_main(). */
		err = errno;
		if (write(out[1], &err, 1) != 1)
			_exit(2);
		_exit(1);
	}

	close(in[0]);
	close(out[1]);
	if (*pid > 0 && read(out[0], &err, 1) != 1)
		err = EIO;
	close(out[0]);
	if (!err)
		return in[1];

	close(in[1]);
	if (*pid > 0)
		waitpid(*pid, &status, 0);
	return -err;
}

/* End a program started by start_exe() and check its exit status. */
static int stop_exe(pid_t pid, int stdin_fd)
{
	int status;

	close(stdin_fd);
	CHECK_EQ(waitpid(pid, &status, 0), pid);
	CHECK(WIFEXITED(status));
	CHECK_EQ(WEXITSTATUS(status), EXEC_STATUS);
	return KSFT_PASS;
}

/* Execute the copy of this test at @path. Return 0 or -errno. */
static int run_exe(const char *path)
{
	pid_t pid = -1;
	int stdin_fd;

	stdin_fd = start_exe(path, &pid);
	if (stdin_fd < 0)
		return stdin_fd;

	return stop_exe(pid, stdin_fd) == KSFT_PASS ? 0 : -ECHILD;
}

/*
 * Preserve a copy of the binary of this test while it runs, through a
 * read-only fd, since it can't be opened for writing.
 */
static int preserve_running_exe(struct test_case *tc)
{
	int fd, stdin_fd;
	char path[128];
	pid_t pid;

	snprintf(path, sizeof(path), "src/%s", tc->name);
	TRY(copy_self(path));

	stdin_fd = start_exe(path, &pid);
	CHECK(stdin_fd >= 0);
	CHECK_ERRNO(open(path, O_RDWR), ETXTBSY);

	fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	CHECK_EQ(luo_session_preserve_fd(session_fd, fd, tc->token), 0);
	close(fd);

	return stop_exe(pid, stdin_fd);
}

/* Retrieve the file of @tc into a new file on a new mount at dst. */
static int restore_verify(struct test_case *tc)
{
	int fd;

	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	fd = retrieve_new(tc->token, "dst");
	CHECK(fd >= 0);
	TRY(check_restored(tc, fd));

	close(fd);
	return KSFT_PASS;
}

static int basic_verify(struct test_case *tc)
{
	struct stat target_st, st, path_st;
	int target, fd, path_fd;

	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = open("dst", O_TMPFILE | O_RDWR, 0640);
	CHECK(target >= 0);
	CHECK(!fstat(target, &target_st));
	CHECK_EQ(lseek(target, 77, SEEK_SET), 77);

	/* The retrieve returns the fd of the target instead of a new one. */
	fd = retrieve_into(tc->token, target);
	CHECK_EQ(fd, target);
	TRY(check_restored(tc, fd));
	CHECK_EQ(mnt_used("dst"), nr_pages(tc->size));

	/*
	 * Only the contents and size are restored. Everything else is like
	 * userspace created the file, including its position.
	 */
	CHECK(!fstat(fd, &st));
	CHECK_EQ(st.st_ino, target_st.st_ino);
	CHECK_EQ(st.st_mode & 07777, 0640);
	CHECK(S_ISREG(st.st_mode));
	CHECK_EQ(st.st_nlink, 0);
	CHECK_EQ(st.st_uid, 0);
	CHECK_EQ(st.st_gid, 0);
	CHECK_EQ(do_fcntl(fd, F_GETFL, 0) & (O_ACCMODE | O_TMPFILE),
		 O_RDWR | O_TMPFILE);
	CHECK_EQ(lseek(fd, 0, SEEK_CUR), 77);

	/* Like all tmpfs files, the file only has F_SEAL_SEAL. */
	CHECK_EQ(do_fcntl(fd, F_GET_SEALS, 0), F_SEAL_SEAL);

	/* The file can be linked into place, and opened by its path. */
	CHECK_EQ(link_fd(fd, "dst/f"), 0);
	CHECK(!stat("dst/f", &path_st));
	CHECK_EQ(path_st.st_ino, st.st_ino);
	CHECK_EQ(path_st.st_nlink, 1);
	path_fd = open("dst/f", O_RDONLY);
	CHECK(path_fd >= 0);
	TRY(check_contents(path_fd, tc->size, tc->token));
	CHECK_EQ(mnt_used("dst"), nr_pages(tc->size));

	close(path_fd);
	close(fd);
	return KSFT_PASS;
}

static int empty_verify(struct test_case *tc)
{
	int fd;

	CHECK_EQ(mount_tmpfs("dst", 1, NULL), 0);
	fd = retrieve_new(tc->token, "dst");
	CHECK(fd >= 0);
	TRY(check_restored(tc, fd));
	CHECK_EQ(mnt_used("dst"), 0);

	close(fd);
	return KSFT_PASS;
}

static int sparse_preserve(struct test_case *tc)
{
	size_t size = SPARSE_PAGES * page_size;
	int fd;

	/* Only the first and the last page have data. */
	fd = open("src/sparse", O_CREAT | O_EXCL | O_RDWR, 0644);
	CHECK(fd >= 0);
	CHECK_EQ(ftruncate(fd, size), 0);
	CHECK_EQ(write_pattern(fd, 0, page_size, tc->token), 0);
	CHECK_EQ(write_pattern(fd, size - page_size, page_size, tc->token), 0);
	CHECK_EQ(file_pages(fd), 2);

	CHECK_EQ(lseek(fd, 0, SEEK_SET), 0);
	CHECK_EQ(luo_session_preserve_fd(session_fd, fd, tc->token), 0);

	/* Preserving the file fills the holes. */
	CHECK_EQ(file_pages(fd), SPARSE_PAGES);

	close(fd);
	return KSFT_PASS;
}

static int sparse_verify(struct test_case *tc)
{
	size_t size = SPARSE_PAGES * page_size;
	unsigned char *p;
	bool data;
	int fd, i;

	CHECK_EQ(mount_tmpfs("dst", SPARSE_PAGES, NULL), 0);
	fd = retrieve_new(tc->token, "dst");
	CHECK(fd >= 0);
	CHECK_EQ(file_size(fd), size);
	CHECK_EQ(file_pages(fd), SPARSE_PAGES);
	CHECK_EQ(mnt_used("dst"), SPARSE_PAGES);

	/* The holes read as zeroes. */
	p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED);
	for (i = 0; i < SPARSE_PAGES; i++) {
		data = i == 0 || i == SPARSE_PAGES - 1;
		TRY(check_range(p, i * page_size, (i + 1) * page_size,
				data ? tc->token : 0));
	}

	munmap(p, size);
	close(fd);
	return KSFT_PASS;
}

static int tmpfile_source_preserve(struct test_case *tc)
{
	int fd, ret;

	/* An unlinked tmpfs file is still a tmpfs file. */
	fd = open("src", O_TMPFILE | O_RDWR, 0644);
	CHECK(fd >= 0);
	CHECK_EQ(write_pattern(fd, 0, tc->size, tc->token), 0);
	ret = preserve_fd(fd, tc->pos, tc->token);
	close(fd);

	return ret;
}

static int linked_target_verify(struct test_case *tc)
{
	int target, path_fd;

	/* The target can also be created with a path. */
	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = open("dst/f", O_CREAT | O_EXCL | O_RDWR, 0644);
	CHECK(target >= 0);
	CHECK_EQ(retrieve_into(tc->token, target), target);
	TRY(check_restored(tc, target));

	path_fd = open("dst/f", O_RDONLY);
	CHECK(path_fd >= 0);
	TRY(check_contents(path_fd, tc->size, tc->token));

	close(path_fd);
	close(target);
	return KSFT_PASS;
}

/* Set the inode flag @flag, e.g. FS_IMMUTABLE_FL, on the file at @path. */
static int set_inode_flag(const char *path, int flag)
{
	int fd, flags, ret = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	if (ioctl(fd, FS_IOC_GETFLAGS, &flags)) {
		ret = -errno;
	} else {
		flags |= flag;
		if (ioctl(fd, FS_IOC_SETFLAGS, &flags))
			ret = -errno;
	}

	close(fd);
	return ret;
}

static int target_rdonly_verify(struct test_case *tc)
{
	int target, rd_fd;

	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = new_target("dst");
	CHECK(target >= 0);
	rd_fd = reopen_fd(target, O_RDONLY);
	CHECK(rd_fd >= 0);

	/* The target must be opened for writing. */
	CHECK_EQ(retrieve_into(tc->token, rd_fd), -EBADF);
	CHECK_EQ(file_size(target), 0);

	/* The failure didn't consume the preserved file. */
	CHECK_EQ(retrieve_into(tc->token, target), target);
	TRY(check_restored(tc, target));

	close(rd_fd);
	close(target);
	return KSFT_PASS;
}

static int target_wronly_verify(struct test_case *tc)
{
	int target, rd_fd;

	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = open("dst", O_TMPFILE | O_WRONLY | O_APPEND, 0644);
	CHECK(target >= 0);
	CHECK_EQ(retrieve_into(tc->token, target), target);

	/* The target keeps its flags. */
	CHECK_EQ(do_fcntl(target, F_GETFL, 0) & (O_ACCMODE | O_APPEND),
		 O_WRONLY | O_APPEND);
	CHECK_EQ(file_size(target), tc->size);

	rd_fd = reopen_fd(target, O_RDONLY);
	CHECK(rd_fd >= 0);
	TRY(check_contents(rd_fd, tc->size, tc->token));

	close(rd_fd);
	close(target);
	return KSFT_PASS;
}

static int target_not_empty_verify(struct test_case *tc)
{
	int target, fd;

	/* The target must be empty: It can't have data... */
	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = new_target("dst");
	CHECK(target >= 0);
	CHECK_EQ(write(target, "x", 1), 1);
	CHECK_EQ(retrieve_into(tc->token, target), -EINVAL);
	close(target);

	/* ... a size... */
	target = new_target("dst");
	CHECK(target >= 0);
	CHECK_EQ(ftruncate(target, page_size), 0);
	CHECK_EQ(retrieve_into(tc->token, target), -EINVAL);
	close(target);

	/* ... or blocks past the end of the file. */
	target = new_target("dst");
	CHECK(target >= 0);
	CHECK_EQ(do_fallocate(target, FALLOC_FL_KEEP_SIZE, 0, page_size), 0);
	CHECK_EQ(file_size(target), 0);
	CHECK_EQ(retrieve_into(tc->token, target), -EINVAL);
	close(target);

	/* The failures didn't consume the preserved file. */
	fd = retrieve_new(tc->token, "dst");
	CHECK(fd >= 0);
	TRY(check_restored(tc, fd));

	close(fd);
	return KSFT_PASS;
}

static int target_not_tmpfs_verify(struct test_case *tc)
{
	int target, pipe_fds[2], fd;

	/* Like the preserved file, the target must be on a tmpfs mount. */
	target = memfd_create(tc->name, 0);
	CHECK(target >= 0);
	CHECK_EQ(retrieve_into(tc->token, target), -EINVAL);
	close(target);

	CHECK_EQ(pipe2(pipe_fds, O_CLOEXEC), 0);
	CHECK_EQ(retrieve_into(tc->token, pipe_fds[1]), -EINVAL);
	close(pipe_fds[0]);
	close(pipe_fds[1]);

	/* It must also be a regular file. */
	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = open("dst", O_RDONLY | O_DIRECTORY);
	CHECK(target >= 0);
	CHECK_EQ(retrieve_into(tc->token, target), -EINVAL);
	close(target);

	/* The failures didn't consume the preserved file. */
	fd = retrieve_new(tc->token, "dst");
	CHECK(fd >= 0);
	TRY(check_restored(tc, fd));

	close(fd);
	return KSFT_PASS;
}

static int target_immutable_verify(struct test_case *tc)
{
	int target, fd, ret;

	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = open("dst/f", O_CREAT | O_EXCL | O_RDWR, 0644);
	CHECK(target >= 0);
	ret = set_inode_flag("dst/f", FS_IMMUTABLE_FL);
	if (ret == -ENOTTY || ret == -EOPNOTSUPP) {
		ksft_print_msg("%s: FS_IOC_SETFLAGS is not supported\n",
			       tc->name);
		close(target);
		return KSFT_SKIP;
	}
	CHECK_EQ(ret, 0);

	/* An immutable file can't be the target, even if opened before. */
	CHECK_EQ(retrieve_into(tc->token, target), -EPERM);
	CHECK_EQ(file_size(target), 0);
	close(target);

	/* The failure didn't consume the preserved file. */
	fd = retrieve_new(tc->token, "dst");
	CHECK(fd >= 0);
	TRY(check_restored(tc, fd));

	close(fd);
	return KSFT_PASS;
}

static int unprivileged_child(struct test_case *tc)
{
	struct stat st;
	int target;

	TRY(drop_privileges());

	/* The file keeps the owner and mode it was created with. */
	target = open("dst/nobody", O_TMPFILE | O_RDWR, 0600);
	CHECK(target >= 0);
	CHECK_EQ(retrieve_into(tc->token, target), target);
	TRY(check_restored(tc, target));
	CHECK(!fstat(target, &st));
	CHECK_EQ(st.st_uid, NOBODY);
	CHECK_EQ(st.st_gid, NOBODY);
	CHECK_EQ(st.st_mode & 07777, 0600);

	close(target);
	return KSFT_PASS;
}

static int unprivileged_verify(struct test_case *tc)
{
	CHECK_EQ(mount_tmpfs("dst", 16, "mode=0755"), 0);
	CHECK_EQ(mkdir("dst/nobody", 0755), 0);
	CHECK_EQ(chown("dst/nobody", NOBODY, NOBODY), 0);

	return run_child(unprivileged_child, tc);
}

static int exec_verify(struct test_case *tc)
{
	long pages = self_pages();
	int target;

	CHECK(pages > 0);
	CHECK_EQ(mount_tmpfs("dst", pages, NULL), 0);
	target = open("dst", O_TMPFILE | O_RDWR, 0755);
	CHECK(target >= 0);
	CHECK_EQ(retrieve_into(tc->token, target), target);
	TRY(check_self(target));
	CHECK_EQ(mnt_used("dst"), pages);
	CHECK_EQ(link_fd(target, "dst/exe"), 0);
	close(target);

	/*
	 * LUO keeps the file open for writing until the session is finished,
	 * so the binary can't be executed before that.
	 */
	CHECK_EQ(run_exe("dst/exe"), -ETXTBSY);
	return KSFT_PASS;
}

static int exec_after_finish(struct test_case *tc)
{
	CHECK_EQ(run_exe("dst/exe"), 0);
	return KSFT_PASS;
}

static int memfd_preserve(struct test_case *tc)
{
	return preserve_memfd(tc, 0, 0);
}

static int memfd_verify(struct test_case *tc)
{
	struct stat st;
	int fd;

	/* memfds are only retrieved as memfds. */
	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	CHECK_EQ(retrieve_new(tc->token, "dst"), -EINVAL);
	CHECK_EQ(mnt_used("dst"), 0);

	fd = luo_session_retrieve_fd(session_fd, tc->token);
	CHECK(fd >= 0);
	TRY(check_restored(tc, fd));
	CHECK(!fstat(fd, &st));
	CHECK_EQ(st.st_nlink, 0);
	CHECK_EQ(do_fcntl(fd, F_GETFL, 0) & O_ACCMODE, O_RDWR);

	/* Unlike for tmpfs files, the file position is restored. */
	CHECK_EQ(lseek(fd, 0, SEEK_CUR), tc->pos);

	close(fd);
	return KSFT_PASS;
}

#define MEMFD_SEALS		(F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)

static int sealed_memfd_preserve(struct test_case *tc)
{
	return preserve_memfd(tc, MFD_ALLOW_SEALING, MEMFD_SEALS);
}

static int sealed_memfd_verify(struct test_case *tc)
{
	void *p;
	int fd;

	fd = luo_session_retrieve_fd(session_fd, tc->token);
	CHECK(fd >= 0);
	TRY(check_restored(tc, fd));
	CHECK_EQ(do_fcntl(fd, F_GET_SEALS, 0), MEMFD_SEALS);
	CHECK_ERRNO(write(fd, "x", 1), EPERM);
	CHECK_ERRNO(ftruncate(fd, 0), EPERM);
	p = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(p == MAP_FAILED && errno == EPERM);

	close(fd);
	return KSFT_PASS;
}

static int bad_args_verify(struct test_case *tc)
{
	struct {
		struct liveupdate_shmem_retrieve_arg arg;
		__u64 extra;
	} big = {};
	struct liveupdate_shmem_retrieve_arg arg;
	int target, path_fd;

	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = new_target("dst");
	CHECK(target >= 0);

	/* Files from tmpfs mounts are only retrieved into files on tmpfs. */
	CHECK_EQ(luo_session_retrieve_fd(session_fd, tc->token), -EINVAL);

	memset(&arg, 0, sizeof(arg));
	arg.size = sizeof(arg);
	arg.fd = target;

	/* Problems with the argument itself. */
	arg.size = offsetof(struct liveupdate_shmem_retrieve_arg, fd);
	CHECK_EQ(retrieve_arg(tc->token, &arg), -EINVAL);
	arg.size = 2 * page_size;
	CHECK_EQ(retrieve_arg(tc->token, &arg), -E2BIG);
	arg.size = sizeof(arg);

	big.arg = arg;
	big.arg.size = sizeof(big);
	big.extra = 1;
	CHECK_EQ(retrieve_arg(tc->token, &big.arg), -E2BIG);

	/* Problems with the file descriptor. */
	arg.fd = -1;
	CHECK_EQ(retrieve_arg(tc->token, &arg), -EBADF);
	path_fd = reopen_fd(target, O_PATH);
	CHECK(path_fd >= 0);
	arg.fd = path_fd;
	CHECK_EQ(retrieve_arg(tc->token, &arg), -EBADF);
	close(path_fd);
	arg.fd = target;

	CHECK_EQ(retrieve_arg(tc->token + 1, &arg), -ENOENT);
	CHECK_EQ(file_size(target), 0);
	CHECK_EQ(mnt_used("dst"), 0);

	/* The failures didn't consume the preserved file. */
	CHECK_EQ(retrieve_arg(tc->token, &arg), target);
	TRY(check_restored(tc, target));

	close(target);
	return KSFT_PASS;
}

static int retrieve_twice_verify(struct test_case *tc)
{
	struct stat st, new_st;
	int target, fd, new_fd;

	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	fd = retrieve_new(tc->token, "dst");
	CHECK(fd >= 0);
	TRY(check_restored(tc, fd));

	/* The argument only applies to the retrieve that restores the file. */
	target = new_target("dst");
	CHECK(target >= 0);
	CHECK_EQ(retrieve_into(tc->token, target), -EBUSY);
	CHECK_EQ(file_size(target), 0);
	close(target);

	/* A plain retrieve returns the same open file. */
	new_fd = luo_session_retrieve_fd(session_fd, tc->token);
	CHECK(new_fd >= 0);
	CHECK(!fstat(fd, &st));
	CHECK(!fstat(new_fd, &new_st));
	CHECK_EQ(new_st.st_dev, st.st_dev);
	CHECK_EQ(new_st.st_ino, st.st_ino);
	CHECK_EQ(lseek(new_fd, 77, SEEK_SET), 77);
	CHECK_EQ(lseek(fd, 0, SEEK_CUR), 77);
	CHECK_EQ(mnt_used("dst"), nr_pages(tc->size));

	close(new_fd);
	close(fd);
	return KSFT_PASS;
}

static int preserve_nothing(struct test_case *tc)
{
	return KSFT_PASS;
}

static int outgoing_session_verify(struct test_case *tc)
{
	struct liveupdate_shmem_retrieve_arg arg = {
		.size = sizeof(arg),
	};
	int out_fd, source_fd;

	out_fd = luo_create_session(dev_fd, SESSION_NAME "_outgoing_session");
	CHECK(out_fd >= 0);
	CHECK_EQ(mount_tmpfs("src", 16, NULL), 0);
	source_fd = new_target("src");
	CHECK(source_fd >= 0);
	CHECK_EQ(luo_session_preserve_fd(out_fd, source_fd, tc->token), 0);

	/* Files can only be retrieved from incoming sessions. */
	arg.fd = new_target("src");
	CHECK(arg.fd >= 0);
	CHECK_EQ(luo_session_retrieve_fd_arg(out_fd, tc->token,
					     (__u64)(uintptr_t)&arg), -EINVAL);

	close(arg.fd);
	close(source_fd);
	close(out_fd);
	return KSFT_PASS;
}

static int target_preserved_verify(struct test_case *tc)
{
	int out_fd, target;

	/* A file preserved in an outgoing session can't be the target. */
	out_fd = luo_create_session(dev_fd, SESSION_NAME "_target_preserved");
	CHECK(out_fd >= 0);
	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = new_target("dst");
	CHECK(target >= 0);
	CHECK_EQ(luo_session_preserve_fd(out_fd, target, tc->token), 0);
	CHECK_EQ(retrieve_into(tc->token, target), -EBUSY);
	CHECK_EQ(file_size(target), 0);

	/* Once the session is closed, the file is no longer preserved. */
	close(out_fd);
	CHECK_EQ(retrieve_into(tc->token, target), target);
	TRY(check_restored(tc, target));

	close(target);
	return KSFT_PASS;
}

static int target_retrieved_preserve(struct test_case *tc)
{
	TRY(preserve_source("src/target_retrieved_empty", 0, 0, tc->token));
	return preserve_source("src/target_retrieved", tc->size, 0,
			       tc->token + 1);
}

static int target_retrieved_verify(struct test_case *tc)
{
	int target, other_fd;

	/* Retrieving an empty file leaves the target empty... */
	CHECK_EQ(mount_tmpfs("dst", 16, NULL), 0);
	target = new_target("dst");
	CHECK(target >= 0);
	CHECK_EQ(retrieve_into(tc->token, target), target);
	CHECK_EQ(file_size(target), 0);

	/* ... but LUO manages the file now, so it can't be a target again. */
	CHECK_EQ(retrieve_into(tc->token + 1, target), -EBUSY);
	CHECK_EQ(file_size(target), 0);

	/* The failure didn't consume the preserved file. */
	other_fd = retrieve_new(tc->token + 1, "dst");
	CHECK(other_fd >= 0);
	CHECK_EQ(file_size(other_fd), tc->size);
	TRY(check_contents(other_fd, tc->size, tc->token + 1));

	close(other_fd);
	close(target);
	return KSFT_PASS;
}

static struct test_case test_cases[] = {
	/* The restored file. */
	{
		.name = "basic",
		.size = 10000,
		.pos = 1234,
		.verify = basic_verify,
	},
	{
		.name = "empty",
		.verify = empty_verify,
	},
	{
		.name = "sparse",
		.preserve = sparse_preserve,
		.verify = sparse_verify,
	},
	{
		.name = "tmpfile_source",
		.size = SMALL_SIZE,
		.preserve = tmpfile_source_preserve,
		.verify = restore_verify,
	},
	{
		.name = "linked_target",
		.size = SMALL_SIZE,
		.verify = linked_target_verify,
	},
	/* The target file. */
	{
		.name = "target_rdonly",
		.size = SMALL_SIZE,
		.verify = target_rdonly_verify,
	},
	{
		.name = "target_wronly",
		.size = SMALL_SIZE,
		.verify = target_wronly_verify,
	},
	{
		.name = "target_not_empty",
		.size = SMALL_SIZE,
		.verify = target_not_empty_verify,
	},
	{
		.name = "target_not_tmpfs",
		.size = SMALL_SIZE,
		.verify = target_not_tmpfs_verify,
	},
	{
		.name = "target_immutable",
		.size = SMALL_SIZE,
		.verify = target_immutable_verify,
	},
	{
		.name = "unprivileged",
		.size = SMALL_SIZE,
		.verify = unprivileged_verify,
	},
	{
		.name = "exec",
		.preserve = preserve_running_exe,
		.verify = exec_verify,
		.after_finish = exec_after_finish,
	},
	/* memfds. */
	{
		.name = "memfd",
		.size = SMALL_SIZE,
		.pos = 700,
		.preserve = memfd_preserve,
		.verify = memfd_verify,
	},
	{
		.name = "sealed_memfd",
		.size = SMALL_SIZE,
		.preserve = sealed_memfd_preserve,
		.verify = sealed_memfd_verify,
	},
	/* Interactions with LUO. */
	{
		.name = "bad_args",
		.size = SMALL_SIZE,
		.verify = bad_args_verify,
	},
	{
		.name = "retrieve_twice",
		.size = SMALL_SIZE,
		.verify = retrieve_twice_verify,
	},
	{
		.name = "outgoing_session",
		.preserve = preserve_nothing,
		.verify = outgoing_session_verify,
	},
	{
		.name = "target_preserved",
		.size = SMALL_SIZE,
		.verify = target_preserved_verify,
	},
	{
		.name = "target_retrieved",
		.size = SMALL_SIZE,
		.preserve = target_retrieved_preserve,
		.verify = target_retrieved_verify,
	},
};

static long read_taint(void)
{
	return read_long("/proc/sys/kernel/tainted");
}

static int check_taint(long old_taint)
{
	long taint = read_taint();

	if (old_taint < 0 || taint < 0) {
		ksft_print_msg("Failed to read the taint flags\n");
		return KSFT_SKIP;
	}

	if (old_taint & BAD_TAINT) {
		ksft_print_msg("The kernel was already tainted\n");
		return KSFT_SKIP;
	}

	CHECK_EQ(taint & BAD_TAINT, 0);
	return KSFT_PASS;
}

static void setup(int luo_fd)
{
	unsigned int i;

	dev_fd = luo_fd;
	page_size = getpagesize();
	umask(TEST_UMASK);

	for (i = 0; i < ARRAY_SIZE(test_cases); i++)
		test_cases[i].token = (i + 1) * 0x100;

	if (mount_tmpfs(TEST_DIR, 16, "mode=0755"))
		fail_exit("Failed to mount %s", TEST_DIR);
}

static void enter_case_dir(struct test_case *tc)
{
	char dir[128];

	snprintf(dir, sizeof(dir), TEST_DIR "/%s", tc->name);
	if ((mkdir(dir, 0755) && errno != EEXIST) || chdir(dir))
		fail_exit("Failed to create %s", dir);
}

static void run_stage_1(int luo_fd)
{
	long taint = read_taint();
	struct test_case *tc;
	unsigned int i;
	int ret;

	ksft_print_msg("[STAGE 1] Creating state file for next stage (2)...\n");
	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_TOKEN, 2);

	setup(luo_fd);
	if (chdir(TEST_DIR) || mount_tmpfs("src", 4096, "huge=never"))
		fail_exit("Failed to mount the source tmpfs");

	session_fd = luo_create_session(luo_fd, SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_NAME);

	ksft_print_msg("[STAGE 1] Preserving the source files...\n");
	for (i = 0; i < ARRAY_SIZE(test_cases); i++) {
		tc = &test_cases[i];
		ret = tc->preserve ? tc->preserve(tc) : preserve_file(tc);
		if (ret == KSFT_SKIP)
			ksft_print_msg("[STAGE 1] %s: Skipped\n", tc->name);
		else if (ret != KSFT_PASS)
			ksft_exit_fail_msg("%s: Failed to preserve\n",
					   tc->name);
	}

	if (check_taint(taint) == KSFT_FAIL)
		ksft_exit_fail_msg("The kernel was tainted in stage 1\n");

	close(luo_fd);
	daemonize_and_wait();
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	long taint = read_taint();
	unsigned int i, nr_tests;
	struct test_case *tc;
	int stage, ret;

	ksft_print_msg("[STAGE 2] Starting post-kexec verification...\n");
	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	if (stage != 2)
		fail_exit("Expected stage 2, but state file contains %d",
			  stage);

	setup(luo_fd);
	session_fd = luo_retrieve_session(luo_fd, SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_NAME);

	nr_tests = ARRAY_SIZE(test_cases) + 1;
	for (i = 0; i < ARRAY_SIZE(test_cases); i++)
		nr_tests += !!test_cases[i].after_finish;

	ksft_print_header();
	ksft_set_plan(nr_tests);

	for (i = 0; i < ARRAY_SIZE(test_cases); i++) {
		tc = &test_cases[i];
		enter_case_dir(tc);
		tc->result = tc->verify(tc);
		ksft_test_result_report(tc->result, "%s\n", tc->name);
	}

	if (luo_session_finish(session_fd))
		fail_exit("luo_session_finish for '%s'", SESSION_NAME);
	close(session_fd);

	for (i = 0; i < ARRAY_SIZE(test_cases); i++) {
		tc = &test_cases[i];
		if (!tc->after_finish)
			continue;

		enter_case_dir(tc);
		ret = KSFT_SKIP;
		if (tc->result == KSFT_PASS)
			ret = tc->after_finish(tc);
		ksft_test_result_report(ret, "%s_after_finish\n", tc->name);
	}

	if (chdir("/") || syscall(__NR_umount2, TEST_DIR, MNT_DETACH) ||
	    rmdir(TEST_DIR))
		fail_exit("Failed to clean up %s", TEST_DIR);

	ksft_test_result_report(check_taint(taint), "no_kernel_warnings\n");

	if (luo_session_finish(state_session_fd))
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	if (!ksft_get_fail_cnt())
		ksft_print_msg("--- TMPFS TEST PASSED ---\n");

	ksft_finished();
}

int main(int argc, char *argv[])
{
	if (argc == 2 && !strcmp(argv[1], EXEC_ARG))
		return exec_main();

	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
