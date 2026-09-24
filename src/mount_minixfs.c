/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mount_minixfs - mount a MINIX file system image, read-only, with FUSE.
 *
 *	mount_minixfs [-M SIZE:HEADS:SIDE] [FUSE options] IMAGE MOUNTPOINT
 *
 * -M reads an image that holds the file system in the tracks of one side
 * only, as minixfs(1) does.
 *
 * The file system is served through the high-level FUSE API, which
 * libfuse (Linux, FreeBSD) and librefuse (NetBSD, MINIX 3) both provide.
 * FUSE_USE_VERSION selects the form of that API: 31 (FUSE 3, the
 * default) or 26 (FUSE 2, for older librefuse).
 *
 * The mount is always read-only and single-threaded, since the library
 * keeps one set of block buffers per image.  The image is opened before
 * FUSE takes over, so a relative path works even after FUSE has changed
 * directory.
 */

#include "compat.h"

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfs.h"

#define MAX_ARGS	64		/* arguments handed on to FUSE */

/* Add one directory entry through the filler of readdir. */
#if FUSE_USE_VERSION >= 30
#define FILL(filler, buf, name, st)	(filler)((buf), (name), (st), 0, 0)
#else
#define FILL(filler, buf, name, st)	(filler)((buf), (name), (st), 0)
#endif

/* The options every mount gets; FUSE wants them writable. */
static char opt_single[] = "-s";
static char opt_o[] = "-o";
#if FUSE_USE_VERSION >= 30
static char opt_ro[] = "ro";
#else
static char opt_ro[] = "ro,use_ino,readdir_ino";
#endif

static struct mfs *
image(void)
{
	return fuse_get_context()->private_data;
}

/* Fill *st from an inode. */
static void
fill_stat(const struct mfs *fs, const struct mfs_inode *ip, struct stat *st)
{
	(void)memset(st, 0, sizeof(*st));
	st->st_ino = ip->num;
	st->st_mode = ip->mode;
	st->st_nlink = ip->nlinks;
	st->st_uid = ip->uid;
	st->st_gid = ip->gid;
	st->st_size = ip->size;
	st->st_atime = (time_t)ip->atime;
	st->st_mtime = (time_t)ip->mtime;
	st->st_ctime = (time_t)ip->ctime;
	st->st_blksize = (blksize_t)fs->block_size;
	st->st_blocks = ((blkcnt_t)ip->size + 511) / 512;
	if (mfs_is_dev(ip)) {
		st->st_size = 0;
		st->st_rdev = makedev(mfs_rdev(ip) >> 8 & 0xff,
		    mfs_rdev(ip) & 0xff);
	}
}

#if FUSE_USE_VERSION >= 30
static int
mfs_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
#else
static int
mfs_getattr(const char *path, struct stat *st)
#endif
{
	struct mfs_inode ino;
	int r;

#if FUSE_USE_VERSION >= 30
	(void)fi;
#endif
	if ((r = mfs_namei(image(), path, &ino)) < 0)
		return r;
	fill_stat(image(), &ino, st);
	return 0;
}

static int
mfs_readlink(const char *path, char *buf, size_t size)
{
	struct mfs_inode ino;
	ssize_t n;
	int r;

	if (size == 0)
		return -EINVAL;
	if ((r = mfs_namei(image(), path, &ino)) < 0)
		return r;
	if (!mfs_is_lnk(&ino))
		return -EINVAL;
	if ((n = mfs_pread(image(), &ino, buf, size - 1, 0)) < 0)
		return (int)n;
	buf[n] = '\0';
	return 0;
}

static int
mfs_open_file(const char *path, struct fuse_file_info *fi)
{
	struct mfs_inode ino;
	int r;

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EROFS;
	if ((r = mfs_namei(image(), path, &ino)) < 0)
		return r;
	if (mfs_is_dir(&ino))
		return -EISDIR;
	/* Remember the inode, so that read does not look the path up. */
	fi->fh = ino.num;
	return 0;
}

static int
mfs_read_file(const char *path, char *buf, size_t size, off_t off,
    struct fuse_file_info *fi)
{
	struct mfs_inode ino;
	ssize_t n;
	int r;

	if (fi != NULL && fi->fh != 0)
		r = mfs_read_inode(image(), (uint32_t)fi->fh, &ino);
	else
		r = mfs_namei(image(), path, &ino);
	if (r < 0)
		return r;
	if (off < 0)
		return -EINVAL;
	if ((uint64_t)off >= ino.size)
		return 0;
	/* FUSE asks for a few pages at a time, which fits an int. */
	n = mfs_pread(image(), &ino, buf, size, (uint32_t)off);
	return (int)n;
}

struct fill {
	void		*buf;
	fuse_fill_dir_t	filler;
	int		full;
};

/*
 * Hand one entry to FUSE with the full attributes of its inode:
 * librefuse keeps what readdir gives as the attributes of the node, so an
 * inode number alone is not enough.  An inode that cannot be read is
 * listed with its number only; looking it up then fails.
 */
static int
fill_fn(const struct mfs_dirent *de, void *arg)
{
	struct mfs_inode ino;
	struct fill *f;
	struct stat st;

	f = arg;
	if (mfs_read_inode(image(), de->ino, &ino) == 0) {
		fill_stat(image(), &ino, &st);
	} else {
		(void)memset(&st, 0, sizeof(st));
		st.st_ino = de->ino;
	}
	if (FILL(f->filler, f->buf, de->name, &st) != 0) {
		f->full = 1;
		return 1;
	}
	return 0;
}

#if FUSE_USE_VERSION >= 30
static int
mfs_read_dir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
    struct fuse_file_info *fi, enum fuse_readdir_flags flags)
#else
static int
mfs_read_dir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
    struct fuse_file_info *fi)
#endif
{
	struct mfs_inode ino;
	struct fill f;
	int r;

	(void)off;
	(void)fi;
#if FUSE_USE_VERSION >= 30
	(void)flags;
#endif
	if ((r = mfs_namei(image(), path, &ino)) < 0)
		return r;
	f.buf = buf;
	f.filler = filler;
	f.full = 0;
	if ((r = mfs_readdir(image(), &ino, fill_fn, &f)) < 0)
		return r;
	return f.full ? -ENOMEM : 0;
}

static int
mfs_statfs(const char *path, struct statvfs *sv)
{
	struct mfs *fs;
	uint32_t inodes, zones;
	int r;

	(void)path;
	fs = image();
	if ((r = mfs_count_free(fs, &inodes, &zones)) < 0)
		return r;
	(void)memset(sv, 0, sizeof(*sv));
	sv->f_bsize = fs->block_size;
	sv->f_frsize = fs->block_size;
	sv->f_blocks = fs->nblocks;
	sv->f_bfree = (fsblkcnt_t)zones << fs->log_zone_size;
	sv->f_bavail = sv->f_bfree;
	sv->f_files = fs->ninodes;
	sv->f_ffree = inodes;
	sv->f_favail = inodes;
	sv->f_namemax = fs->namelen;
	sv->f_flag = ST_RDONLY;
	return 0;
}

#if FUSE_USE_VERSION >= 30
static void *
mfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	(void)conn;
	cfg->use_ino = 1;
	cfg->readdir_ino = 1;
	return image();
}
#else
static void *
mfs_init(struct fuse_conn_info *conn)
{
	(void)conn;
	return image();
}
#endif

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: mount_minixfs [-M SIZE:HEADS:SIDE] [FUSE options] IMAGE "
	    "MOUNTPOINT\n");
	exit(2);
}

/*
 * Take IMAGE, the first argument that is not an option, and -M out of
 * argv, and build the arguments for FUSE in fargv: the rest, with a
 * read-only, single-threaded mount added.  FUSE options that take a value
 * ("-o x") are passed on as they are.
 */
static const char *
split_args(int argc, char **argv, char **fargv, int *fargc,
    struct mfs_tracks *tracks)
{
	const char *img;
	int i, n;

	img = NULL;
	n = 0;
	fargv[n++] = argv[0];
	fargv[n++] = opt_single;
	fargv[n++] = opt_o;
	fargv[n++] = opt_ro;
	for (i = 1; i < argc; i++) {
		if (n >= MAX_ARGS - 1)
			usage();
		if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) {
			if (mfs_parse_tracks(argv[++i], tracks) < 0)
				usage();
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			fargv[n++] = argv[i++];
			fargv[n++] = argv[i];
		} else if (argv[i][0] == '-' || img != NULL) {
			fargv[n++] = argv[i];
		} else {
			img = argv[i];
		}
	}
	fargv[n] = NULL;
	*fargc = n;
	return img;
}

int
main(int argc, char **argv)
{
	static struct fuse_operations ops;
	struct mfs_tracks tracks;
	char *fargv[MAX_ARGS];
	struct mfs fs;
	const char *img;
	int fargc, r;

	if (argc < 3)
		usage();
	(void)memset(&tracks, 0, sizeof(tracks));
	if ((img = split_args(argc, argv, fargv, &fargc, &tracks)) == NULL)
		usage();
	if ((r = mfs_open_tracks(&fs, img, 0, &tracks)) < 0) {
		if (r == -EINVAL)
			errx(1, "%s: not a MINIX file system", img);
		errx(1, "%s: %s", img, strerror(-r));
	}

	ops.getattr = mfs_getattr;
	ops.readlink = mfs_readlink;
	ops.open = mfs_open_file;
	ops.read = mfs_read_file;
	ops.readdir = mfs_read_dir;
	ops.statfs = mfs_statfs;
	ops.init = mfs_init;

	r = fuse_main(fargc, fargv, &ops, &fs);
	mfs_close(&fs);
	return r == 0 ? 0 : 1;
}
