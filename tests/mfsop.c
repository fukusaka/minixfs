/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mfsop - change a MINIX file system through the library, for the test
 * suite: what mount_minixfs -w does through FUSE, without FUSE.
 *
 *	mfsop [-a] IMAGE < SCRIPT
 *
 * Unlike mkimage, this is a driver of src/, not an independent writer:
 * the results are checked against the same commands run on a directory
 * of the host.  SCRIPT holds one command a line, with paths from the root
 * of the image:
 *
 *	mkdir PATH MODE
 *	mknod PATH f|p|s|c|b MODE [MAJOR MINOR]
 *	symlink PATH TARGET
 *	link EXISTING NEW
 *	unlink PATH
 *	rmdir PATH
 *	rename OLD NEW [noreplace]
 *	write PATH OFFSET HOSTFILE
 *	truncate PATH SIZE
 *	chmod PATH MODE
 *	chown PATH UID GID
 *	time SECONDS		the time of what follows (default 1000000000)
 *
 * Each command prints "N ok" or "N error: MESSAGE", N being its line.  New
 * files are owned by 0:0.  With -a, the bit maps are written as they
 * change, as mount_minixfs -u always does; else at the end.  The exit
 * status is 0 whatever the commands gave, 1 if the image cannot be
 * opened or written back, and 2 for a usage error.
 */

#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/mfs.h"

#define MAX_FIELDS	6
#define COPY_SIZE	4096
#define LINE_SIZE	4096		/* a symbolic link of a block */

static uint32_t now = 1000000000;

static void
usage(void)
{
	(void)fprintf(stderr, "usage: mfsop [-a] IMAGE < SCRIPT\n");
	exit(2);
}

/* Split a line into up to MAX_FIELDS blank-separated fields. */
static int
split(char *line, char **f)
{
	char *p;
	int n;

	n = 0;
	for (p = strtok(line, " \t\n"); p != NULL && n < MAX_FIELDS;
	    p = strtok(NULL, " \t\n"))
		f[n++] = p;
	return n;
}

static unsigned long
number(const char *s, int base)
{
	return strtoul(s, NULL, base);
}

/*
 * The directory that holds path and its last component: path is cut at
 * its last '/'.
 */
static int
parent(struct mfs *fs, char *path, uint32_t *dir, const char **name)
{
	struct mfs_inode dp;
	char *slash;
	int r;

	if ((slash = strrchr(path, '/')) == NULL)
		return -EINVAL;
	*slash = '\0';
	*name = slash + 1;
	if ((r = mfs_namei(fs, path[0] == '\0' ? "/" : path, &dp)) < 0)
		return r;
	*dir = dp.num;
	return 0;
}

static int
do_make(struct mfs *fs, char **f, int n)
{
	static const char types[] = "fpscb";
	static const uint16_t modes[] = {
		MFS_S_IFREG, MFS_S_IFIFO, MFS_S_IFSOCK, MFS_S_IFCHR,
		MFS_S_IFBLK
	};
	struct mfs_inode ip;
	struct mfs_new nw;
	const char *name, *t;
	uint32_t dir;
	int r;

	(void)memset(&nw, 0, sizeof(nw));
	nw.time = now;
	if (strcmp(f[0], "mkdir") == 0) {
		nw.mode = (uint16_t)(MFS_S_IFDIR | number(f[2], 8));
	} else {
		if (n < 4 || (t = strchr(types, f[2][0])) == NULL)
			return -EINVAL;
		nw.mode = (uint16_t)(modes[t - types] | number(f[3], 8));
		if (n == 6)
			nw.rdev = (uint32_t)(number(f[4], 10) << 8 |
			    number(f[5], 10));
	}
	if ((r = parent(fs, f[1], &dir, &name)) < 0)
		return r;
	return mfs_make(fs, dir, name, &nw, &ip);
}

static int
do_symlink(struct mfs *fs, char **f)
{
	struct mfs_inode ip;
	struct mfs_new nw;
	const char *name;
	uint32_t dir;
	int r;

	(void)memset(&nw, 0, sizeof(nw));
	nw.time = now;
	if ((r = parent(fs, f[1], &dir, &name)) < 0)
		return r;
	return mfs_symlink(fs, dir, name, f[2], &nw, &ip);
}

static int
do_link(struct mfs *fs, char **f)
{
	struct mfs_inode ip;
	const char *name;
	uint32_t dir;
	int r;

	if ((r = mfs_namei(fs, f[1], &ip)) < 0 ||
	    (r = parent(fs, f[2], &dir, &name)) < 0)
		return r;
	return mfs_link(fs, ip.num, dir, name, now);
}

static int
do_remove(struct mfs *fs, char **f)
{
	const char *name;
	uint32_t dir;
	int r;

	if ((r = parent(fs, f[1], &dir, &name)) < 0)
		return r;
	if (strcmp(f[0], "rmdir") == 0)
		return mfs_rmdir(fs, dir, name, now);
	return mfs_unlink(fs, dir, name, now);
}

static int
do_rename(struct mfs *fs, char **f, int n)
{
	const char *oname, *nname;
	uint32_t odir, ndir;
	int r;

	if ((r = parent(fs, f[1], &odir, &oname)) < 0 ||
	    (r = parent(fs, f[2], &ndir, &nname)) < 0)
		return r;
	return mfs_rename(fs, odir, oname, ndir, nname,
	    n > 3 && strcmp(f[3], "noreplace") == 0, now);
}

/* Write the contents of a host file into the file at an offset. */
static int
do_write(struct mfs *fs, char **f)
{
	unsigned char buf[COPY_SIZE];
	struct mfs_inode ip;
	uint32_t off;
	size_t len;
	FILE *in;
	int e, r;

	if ((r = mfs_namei(fs, f[1], &ip)) < 0)
		return r;
	if ((in = fopen(f[3], "rb")) == NULL)
		return -errno;
	r = 0;
	off = (uint32_t)number(f[2], 10);
	while (r == 0 && (len = fread(buf, 1, sizeof(buf), in)) > 0) {
		r = mfs_pwrite(fs, &ip, buf, len, off);
		off += (uint32_t)len;
	}
	(void)fclose(in);
	/* What was written before a failure stays, with its zones. */
	ip.mtime = ip.ctime = now;
	if ((e = mfs_put_inode(fs, &ip)) < 0 && r == 0)
		r = e;
	return r;
}

/* truncate, chmod and chown: change an inode in place. */
static int
do_change(struct mfs *fs, char **f, int n)
{
	struct mfs_inode ip;
	int r;

	if ((r = mfs_namei(fs, f[1], &ip)) < 0)
		return r;
	if (strcmp(f[0], "truncate") == 0) {
		if ((r = mfs_resize(fs, &ip, (uint32_t)number(f[2], 10))) < 0)
			return r;
		ip.mtime = now;
	} else if (strcmp(f[0], "chmod") == 0) {
		ip.mode = (uint16_t)((ip.mode & MFS_S_IFMT) |
		    (number(f[2], 8) & 07777));
	} else if (n == 4) {
		ip.uid = (uint16_t)number(f[2], 10);
		ip.gid = (uint16_t)number(f[3], 10);
	} else {
		return -EINVAL;
	}
	ip.ctime = now;
	return mfs_put_inode(fs, &ip);
}

static int
run(struct mfs *fs, char **f, int n)
{
	if (n >= 3 && (strcmp(f[0], "mkdir") == 0 ||
	    strcmp(f[0], "mknod") == 0))
		return do_make(fs, f, n);
	if (n == 3 && strcmp(f[0], "symlink") == 0)
		return do_symlink(fs, f);
	if (n == 3 && strcmp(f[0], "link") == 0)
		return do_link(fs, f);
	if (n == 2 && (strcmp(f[0], "unlink") == 0 ||
	    strcmp(f[0], "rmdir") == 0))
		return do_remove(fs, f);
	if (n >= 3 && strcmp(f[0], "rename") == 0)
		return do_rename(fs, f, n);
	if (n == 4 && strcmp(f[0], "write") == 0)
		return do_write(fs, f);
	if (n >= 3 && (strcmp(f[0], "truncate") == 0 ||
	    strcmp(f[0], "chmod") == 0 || strcmp(f[0], "chown") == 0))
		return do_change(fs, f, n);
	if (n == 2 && strcmp(f[0], "time") == 0) {
		now = (uint32_t)number(f[1], 10);
		return 0;
	}
	return -EINVAL;
}

int
main(int argc, char **argv)
{
	char line[LINE_SIZE], *f[MAX_FIELDS];
	struct mfs fs;
	int ch, lineno, n, r, through;

	through = 0;
	while ((ch = getopt(argc, argv, "a")) != -1) {
		if (ch != 'a')
			usage();
		through = 1;
	}
	if (argc - optind != 1)
		usage();
	if ((r = mfs_open_rw(&fs, argv[optind])) < 0)
		errx(1, "%s: %s", argv[optind], strerror(-r));
	mfs_maps_through(&fs, through);
	for (lineno = 1; fgets(line, sizeof(line), stdin) != NULL; lineno++) {
		if ((n = split(line, f)) == 0)
			continue;
		if ((r = run(&fs, f, n)) < 0)
			(void)printf("%d error: %s\n", lineno, strerror(-r));
		else
			(void)printf("%d ok\n", lineno);
		/* A test may read how far it got, and kill it there. */
		(void)fflush(stdout);
	}
	r = mfs_sync(&fs);
	mfs_close(&fs);
	if (r < 0)
		errx(1, "%s: %s", argv[optind], strerror(-r));
	return 0;
}
