/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * fdops - work on a file through its descriptor once its name is gone,
 * for the test suite: what mount_minixfs has to keep a file removed while
 * open for.
 *
 *	fdops [-r | -R EMPTY | -L LINK] FILE [OTHER]
 *
 * FILE holds "AAAA".  It is opened and removed, or with OTHER, OTHER is
 * renamed over it.  With -r the directory of FILE is then removed, and
 * with -R the empty directory EMPTY renamed over it, which succeeds only
 * where nothing hides FILE there.  With -L, LINK, another name of FILE,
 * is opened and removed as well, both are stat, and LINK is closed.
 * Then, through the descriptor of FILE, "XXXX" is written after the four
 * bytes, the file is read, stat, cut to 6 bytes, given a time, a mode
 * and its owner again, synced, stat and read again, and closed.  The
 * second stat leaves out the link count: libfuse takes its hidden
 * name off the count that getattr gives, but not off the one that a
 * change of the attributes gives back, which the kernel then keeps.
 * Each step prints a line: what it gave, or the error.  "hidden N" counts
 * the names of libfuse for such files (".fuse_hidden...") in the
 * directory of FILE.  The exit status is 0 whatever the steps gave, 1 if
 * FILE cannot be opened, and 2 for a usage error.
 */

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HIDDEN_PREFIX	".fuse_hidden"
#define TIME		1000000000	/* the time given */

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: fdops [-r | -R EMPTY | -L LINK] FILE [OTHER]\n");
	exit(2);
}

/* Print "what ok", or what failed with errno. */
static void
step(const char *what, int failed)
{
	if (failed)
		(void)printf("%s: %s\n", what, strerror(errno));
	else
		(void)printf("%s ok\n", what);
}

/* The directory of path, into dir of PATH_MAX bytes. */
static void
dir_of(const char *path, char *dir)
{
	const char *slash;

	if ((slash = strrchr(path, '/')) == NULL)
		(void)strcpy(dir, ".");
	else if (slash == path)
		(void)strcpy(dir, "/");
	else
		(void)snprintf(dir, PATH_MAX, "%.*s", (int)(slash - path),
		    path);
}

/* The names of libfuse for hidden files in the directory of path. */
static void
hidden(const char *path)
{
	char dir[PATH_MAX];
	struct dirent *de;
	unsigned long n;
	DIR *dp;

	dir_of(path, dir);
	if ((dp = opendir(dir)) == NULL) {
		step("opendir", 1);
		return;
	}
	n = 0;
	while ((de = readdir(dp)) != NULL)
		if (strncmp(de->d_name, HIDDEN_PREFIX,
		    sizeof(HIDDEN_PREFIX) - 1) == 0)
			n++;
	(void)closedir(dp);
	(void)printf("hidden %lu\n", n);
}

static void
show_stat(int fd, int full)
{
	struct stat st;

	if (fstat(fd, &st) == -1) {
		step("fstat", 1);
		return;
	}
	if (full)
		(void)printf("fstat size %lld mode %o mtime %lld\n",
		    (long long)st.st_size, (unsigned)st.st_mode & 07777,
		    (long long)st.st_mtime);
	else
		(void)printf("fstat nlink %lu size %lld\n",
		    (unsigned long)st.st_nlink, (long long)st.st_size);
}

static void
show_data(int fd)
{
	char buf[16];
	ssize_t n;

	if ((n = pread(fd, buf, sizeof(buf), 0)) == -1) {
		step("pread", 1);
		return;
	}
	(void)printf("pread %.*s\n", (int)n, buf);
}

int
main(int argc, char **argv)
{
	char dir[PATH_MAX];
	struct timespec ts[2];
	const char *empty, *link;
	int ch, fd, lfd, rmparent;

	empty = link = NULL;
	lfd = -1;
	rmparent = 0;
	while ((ch = getopt(argc, argv, "L:rR:")) != -1) {
		switch (ch) {
		case 'L':
			link = optarg;
			break;
		case 'r':
			rmparent = 1;
			break;
		case 'R':
			empty = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if ((argc != 1 && argc != 2) ||
	    rmparent + (empty != NULL) + (link != NULL) > 1)
		usage();
	if ((fd = open(argv[0], O_RDWR)) == -1) {
		perror(argv[0]);
		return 1;
	}
	if (link != NULL && (lfd = open(link, O_RDWR)) == -1) {
		perror(link);
		return 1;
	}
	if (argc == 1)
		step("unlink", unlink(argv[0]) == -1);
	else
		step("rename", rename(argv[1], argv[0]) == -1);
	if (link != NULL)
		step("unlink link", unlink(link) == -1);
	hidden(argv[0]);
	if (link != NULL) {
		show_stat(lfd, 0);
		step("close link", close(lfd) == -1);
	}
	dir_of(argv[0], dir);
	if (rmparent)
		step("rmdir parent", rmdir(dir) == -1);
	if (empty != NULL)
		step("replace parent", rename(empty, dir) == -1);
	step("pwrite", pwrite(fd, "XXXX", 4, 4) != 4);
	show_data(fd);
	show_stat(fd, 0);
	step("ftruncate", ftruncate(fd, 6) == -1);
	ts[0].tv_sec = ts[1].tv_sec = TIME;
	ts[0].tv_nsec = ts[1].tv_nsec = 0;
	step("futimens", futimens(fd, ts) == -1);
	step("fchmod", fchmod(fd, 0600) == -1);
	step("fchown", fchown(fd, getuid(), getgid()) == -1);
	step("fsync", fsync(fd) == -1);
	show_stat(fd, 1);
	show_data(fd);
	step("close", close(fd) == -1);
	return 0;
}
