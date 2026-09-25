/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mount_minixfs - mount a MINIX file system image with FUSE.
 *
 *	mount_minixfs [-w [-u always|sync|seconds]] [-M SIZE:HEADS:SIDE]
 *	    [FUSE options] IMAGE MOUNTPOINT
 *
 * -M takes an image that holds the file system in the tracks of one side
 * only, as minixfs(1) does.
 *
 * The file system is served through the high-level FUSE API, which
 * libfuse (Linux, FreeBSD) and librefuse (NetBSD, MINIX 3) both provide.
 * FUSE_USE_VERSION selects the form of that API: 31 (FUSE 3, the
 * default) or 26 (FUSE 2, for older librefuse).
 *
 * The mount is read-only, and with -w read-write.  A file system that is
 * not marked clean, or has the flex directories of Minix-vmd, is mounted
 * read-only all the same, with a warning.  While it is mounted for
 * writing, the image is locked against other writers and its clean mark
 * is away; unmounting puts the mark back.  The kernel checks permissions
 * (default_permissions), and new files belong to the caller, or to the
 * group of their directory where the inode cannot hold that of the
 * caller.  -u says
 * when the bit maps go to the image: at fsync and unmount (sync, the
 * default, as fuse2fs does), as they change (always), or also when so
 * many seconds have gone since they last did (as update(8) of MINIX
 * does every 30).
 *
 * A file removed while it is open keeps its inode and zones, with no
 * name, until it is closed, as the system calls have it: writes to it
 * would otherwise go to a free inode, or to another file that took it.
 * libfuse tells when the last one closes.  librefuse (NetBSD, MINIX 3)
 * passes on only the first of the opens that overlap and calls release
 * at the first close, so there such a file is kept until the unmount;
 * and one opened twice, closed once and then removed is not kept.
 *
 * The mount is single-threaded, since the library keeps one set of block
 * buffers per image.  The image is opened before FUSE takes over, so a
 * relative path works even after FUSE has changed directory.
 */

#include "compat.h"

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mfs.h"

#ifdef PUFFS_SERVICE
#include <lib.h>
#endif

#define MAX_ARGS	64		/* arguments handed on to FUSE */

/*
 * Whether the FUSE library tells when the last user of a file closes it:
 * librefuse passes on only the first of the opens that overlap, and
 * calls release at the first close.
 */
#if defined(_REFUSE_VERSION_) || defined(PUFFS_SERVICE)
#define LAST_CLOSE_KNOWN 0
#else
#define LAST_CLOSE_KNOWN 1
#endif

/* Add one directory entry through the filler of readdir. */
#if FUSE_USE_VERSION >= 30
#define FILL(filler, buf, name, st)	(filler)((buf), (name), (st), 0, 0)
#else
#define FILL(filler, buf, name, st)	(filler)((buf), (name), (st), 0)
#endif

/* rename(2) flags of Linux, which FUSE 3 hands on. */
#define RENAME_NOREPLACE_	1
#define RENAME_EXCHANGE_	2

/* The options every mount gets; FUSE wants them writable. */
static char opt_single[] = "-s";
#ifndef PUFFS_SERVICE
static char opt_foreground[] = "-f";
#endif
static char opt_o[] = "-o";
#if FUSE_USE_VERSION >= 30
static char opt_ro[] = "ro";
static char opt_rw[] = "rw,default_permissions";
#else
static char opt_ro[] = "ro,use_ino,readdir_ino";
static char opt_rw[] = "rw,default_permissions,use_ino,readdir_ino";
#endif

/* A file opened for a mount for writing, by inode. */
struct handle {
	uint32_t	ino;		/* 0: a free slot */
	uint32_t	opens;		/* not closed yet */
	int		orphan;		/* removed, and kept for them */
};

/* The mounted file system. */
struct mount {
	struct mfs	fs;
	struct handle	*handles;	/* open addressing, by inode */
	size_t		nhandles;
	size_t		maxhandles;	/* a power of 2 */
	int		rw;		/* -w, and the image allows it */
	long		every;		/* -u seconds, or 0 */
	int64_t		flushed;	/* when the maps last went out */
	const char	*path;		/* of the image */
	int		closed;		/* by close_image() */
	int		failed;		/* close_image() could not write */
};

/*
 * The mount, for mfs_destroy(): librefuse of MINIX 3 gives that its own
 * struct fuse in place of the private data.
 */
static struct mount *the_mount;

/* In the background: the pipe on which the parent waits, or -1. */
static int ready_fd = -1;

static struct mount *
mount_of(void)
{
	return fuse_get_context()->private_data;
}

static struct mfs *
image(void)
{
	return &mount_of()->fs;
}

/*
 * The files open in a mount for writing, so that one removed while open
 * keeps its inode.
 */

/* The slot of inode ino in the table: its own, or a free one. */
static struct handle *
handle_slot(const struct mount *m, uint32_t ino)
{
	size_t i;

	for (i = ino & (m->maxhandles - 1); m->handles[i].ino != 0 &&
	    m->handles[i].ino != ino; i = (i + 1) & (m->maxhandles - 1))
		continue;
	return &m->handles[i];
}

/* The entry of inode ino, or NULL. */
static struct handle *
handle_find(const struct mount *m, uint32_t ino)
{
	struct handle *h;

	if (m->maxhandles == 0)
		return NULL;
	h = handle_slot(m, ino);
	return h->ino != 0 ? h : NULL;
}

/* Note that inode ino is opened once more.  Returns 0 or -ENOMEM. */
static int
handle_open(struct mount *m, uint32_t ino)
{
	struct handle *h, *old;
	size_t i, max;

	if ((m->nhandles + 1) * 2 > m->maxhandles) {
		old = m->handles;
		max = m->maxhandles;
		h = calloc(max == 0 ? 64 : max * 2, sizeof(*h));
		if (h == NULL)
			return -ENOMEM;
		m->handles = h;
		m->maxhandles = max == 0 ? 64 : max * 2;
		for (i = 0; i < max; i++)
			if (old[i].ino != 0)
				*handle_slot(m, old[i].ino) = old[i];
		free(old);
	}
	h = handle_slot(m, ino);
	if (h->ino == 0) {
		h->ino = ino;
		m->nhandles++;
	}
	h->opens++;
	return 0;
}

/* Take the entry *h out of the table, moving up those that follow it. */
static void
handle_drop(struct mount *m, struct handle *h)
{
	size_t home, i, j, mask;

	mask = m->maxhandles - 1;
	i = (size_t)(h - m->handles);
	for (j = (i + 1) & mask; m->handles[j].ino != 0; j = (j + 1) & mask) {
		home = m->handles[j].ino & mask;
		/* An entry whose home lies after the hole stays. */
		if (i <= j ? (i < home && home <= j) : (i < home || home <= j))
			continue;
		m->handles[i] = m->handles[j];
		i = j;
	}
	(void)memset(&m->handles[i], 0, sizeof(m->handles[i]));
	m->nhandles--;
}

/*
 * fs->keep: whether to keep inode ino when its last name goes, as it is
 * open; it is then noted as kept.
 */
static int
keep_open(void *arg, uint32_t ino)
{
	struct handle *h;

	if ((h = handle_find(arg, ino)) == NULL || h->opens == 0)
		return 0;
	h->orphan = 1;
	return 1;
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

	if ((fi->flags & O_ACCMODE) != O_RDONLY && !mount_of()->rw)
		return -EROFS;
	if ((r = mfs_namei(image(), path, &ino)) < 0)
		return r;
	if (mfs_is_dir(&ino))
		return -EISDIR;
	if (mount_of()->rw && (r = handle_open(mount_of(), ino.num)) < 0)
		return r;
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
	sv->f_flag = mount_of()->rw ? 0 : ST_RDONLY;
	return 0;
}

/*
 * Writing, with -w.
 */

#define MAX_DEV_PART	255		/* major and minor numbers of MINIX */
#define MAX_UID		65535
#define MAX_GID_V1	255		/* the gid of a V1 inode is a byte */
#define MAX_GID		65535

/*
 * The time now.  time(3) returns nonsense under the AddressSanitizer of
 * NetBSD/i386, and clock_gettime(2) does not.
 */
static int64_t
now(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
		return 0;
	return (int64_t)ts.tv_sec;
}

/*
 * The end of a change: with -u seconds, write the maps out when that
 * long has gone since they last were.  Returns r, or the failure of the
 * write.
 */
static int
changed(int r)
{
	struct mount *m;
	int64_t t;
	int e;

	m = mount_of();
	if (m->every == 0)
		return r;
	t = now();
	if (t - m->flushed < m->every)
		return r;
	m->flushed = t;
	if ((e = mfs_sync(&m->fs)) < 0 && r >= 0)
		return e;
	return r;
}

/*
 * The directory that holds path, and the last component of path in name,
 * which holds MFS_MAX_NAME + 1 bytes.
 */
static int
parent_of(const char *path, uint32_t *dir, char *name)
{
	struct mfs_inode dp;
	char buf[PATH_MAX];
	char *slash;
	int r;

	if (strlen(path) >= sizeof(buf))
		return -ENAMETOOLONG;
	(void)strcpy(buf, path);
	if ((slash = strrchr(buf, '/')) == NULL)
		return -EINVAL;
	if (strlen(slash + 1) > MFS_MAX_NAME)
		return -ENAMETOOLONG;
	(void)strcpy(name, slash + 1);
	*slash = '\0';
	if ((r = mfs_namei(image(), buf[0] == '\0' ? "/" : buf, &dp)) < 0)
		return r;
	*dir = dp.num;
	return 0;
}

/*
 * The owner and mode of a new file of mode in directory dir, as System V
 * and Linux have them: the caller owns it, but in a set-group-ID
 * directory it takes the group of the directory, and a new directory
 * the set-group-ID bit too.  (Linux takes that bit away from a file
 * whose maker is not in the group before it asks for the file.)  A group
 * that the inode cannot hold, as most are in V1, where it is a byte,
 * gives way to that of the directory, as BSD gives every new file.
 */
static int
new_owner(uint32_t dir, mode_t mode, struct mfs_new *n)
{
	struct fuse_context *ctx;
	struct mfs_inode dp;
	int r;

	ctx = fuse_get_context();
	if (ctx->uid > MAX_UID)
		return -EINVAL;
	if ((r = mfs_read_inode(image(), dir, &dp)) < 0)
		return r;
	n->uid = (uint16_t)ctx->uid;
	n->mode = (uint16_t)mode;
	if (dp.mode & S_ISGID) {
		n->gid = dp.gid;
		if (S_ISDIR(mode))
			n->mode |= S_ISGID;
	} else if (ctx->gid <= (image()->version == 1 ? MAX_GID_V1 :
	    MAX_GID)) {
		n->gid = (uint16_t)ctx->gid;
	} else {
		n->gid = dp.gid;
	}
	return 0;
}

/* Make path, of mode and device number rdev, owned by the caller. */
static int
make(const char *path, mode_t mode, uint32_t rdev, struct mfs_inode *ip)
{
	char name[MFS_MAX_NAME + 1];
	struct mfs_new n;
	uint32_t dir;
	int r;

	(void)memset(&n, 0, sizeof(n));
	if ((r = parent_of(path, &dir, name)) < 0 ||
	    (r = new_owner(dir, mode, &n)) < 0)
		return r;
	n.rdev = rdev;
	n.time = (uint32_t)now();
	return changed(mfs_make(image(), dir, name, &n, ip));
}

static int
mfs_create_file(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	struct mfs_inode ip;
	int r;

	if ((r = make(path, (mode & 07777) | S_IFREG, 0, &ip)) < 0 ||
	    (r = handle_open(mount_of(), ip.num)) < 0)
		return r;
	fi->fh = ip.num;
	return 0;
}

/*
 * The close of a file opened for writing: once no one has it open, one
 * that was removed goes.  With librefuse another may still have it open,
 * so that is left to the unmount.
 */
static int
mfs_release_file(const char *path, struct fuse_file_info *fi)
{
	struct handle *h;
	struct mount *m;
	int r;

	(void)path;
	m = mount_of();
	if (fi == NULL || fi->fh == 0 ||
	    (h = handle_find(m, (uint32_t)fi->fh)) == NULL)
		return 0;
	if (h->opens > 0)
		h->opens--;
	if (h->opens > 0 || (h->orphan && !LAST_CLOSE_KNOWN))
		return 0;
	r = h->orphan ? mfs_free_orphan(&m->fs, h->ino) : 0;
	handle_drop(m, h);
	return changed(r);
}

static int
mfs_make_node(const char *path, mode_t mode, dev_t rdev)
{
	struct mfs_inode ip;
	uint32_t dev;

	dev = 0;
	if (S_ISCHR(mode) || S_ISBLK(mode)) {
		if (major(rdev) > MAX_DEV_PART || minor(rdev) > MAX_DEV_PART)
			return -EINVAL;
		dev = (uint32_t)(major(rdev) << 8 | minor(rdev));
	}
	return make(path, mode, dev, &ip);
}

static int
mfs_make_dir(const char *path, mode_t mode)
{
	struct mfs_inode ip;

	return make(path, (mode & 07777) | S_IFDIR, 0, &ip);
}

static int
mfs_make_symlink(const char *target, const char *path)
{
	char name[MFS_MAX_NAME + 1];
	struct mfs_inode ip;
	struct mfs_new n;
	uint32_t dir;
	int r;

	(void)memset(&n, 0, sizeof(n));
	if ((r = parent_of(path, &dir, name)) < 0 ||
	    (r = new_owner(dir, S_IFLNK | 0777, &n)) < 0)
		return r;
	n.time = (uint32_t)now();
	return changed(mfs_symlink(image(), dir, name, target, &n, &ip));
}

static int
mfs_make_link(const char *from, const char *to)
{
	char name[MFS_MAX_NAME + 1];
	struct mfs_inode ip;
	uint32_t dir;
	int r;

	if ((r = mfs_namei(image(), from, &ip)) < 0 ||
	    (r = parent_of(to, &dir, name)) < 0)
		return r;
	return changed(mfs_link(image(), ip.num, dir, name,
	    (uint32_t)now()));
}

static int
mfs_remove(const char *path)
{
	char name[MFS_MAX_NAME + 1];
	uint32_t dir;
	int r;

	if ((r = parent_of(path, &dir, name)) < 0)
		return r;
	return changed(mfs_unlink(image(), dir, name, (uint32_t)now()));
}

static int
mfs_remove_dir(const char *path)
{
	char name[MFS_MAX_NAME + 1];
	uint32_t dir;
	int r;

	if ((r = parent_of(path, &dir, name)) < 0)
		return r;
	return changed(mfs_rmdir(image(), dir, name, (uint32_t)now()));
}

#if FUSE_USE_VERSION >= 30
static int
mfs_move(const char *from, const char *to, unsigned int flags)
#else
static int
mfs_move(const char *from, const char *to)
#endif
{
	char oname[MFS_MAX_NAME + 1], nname[MFS_MAX_NAME + 1];
	uint32_t odir, ndir;
	int noreplace, r;

	noreplace = 0;
#if FUSE_USE_VERSION >= 30
	if (flags & ~RENAME_NOREPLACE_)
		return -EINVAL;
	noreplace = (flags & RENAME_NOREPLACE_) != 0;
#endif
	if ((r = parent_of(from, &odir, oname)) < 0 ||
	    (r = parent_of(to, &ndir, nname)) < 0)
		return r;
	return changed(mfs_rename(image(), odir, oname, ndir, nname,
	    noreplace, (uint32_t)now()));
}

/*
 * The inode of path, or of the file open as fi: an open file that was
 * renamed or removed is still found.
 */
static int
inode_of(const char *path, struct fuse_file_info *fi, struct mfs_inode *ip)
{
	if (fi != NULL && fi->fh != 0)
		return mfs_read_inode(image(), (uint32_t)fi->fh, ip);
	return mfs_namei(image(), path, ip);
}

/* Write the inode back with its ctime set. */
static int
put_changed(struct mfs_inode *ip)
{
	ip->ctime = (uint32_t)now();
	return changed(mfs_put_inode(image(), ip));
}

static int
set_mode(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	struct mfs_inode ip;
	int r;

	if ((r = inode_of(path, fi, &ip)) < 0)
		return r;
	ip.mode = (uint16_t)((ip.mode & MFS_S_IFMT) | (mode & 07777));
	return put_changed(&ip);
}

static int
set_owner(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	struct mfs_inode ip;
	int r;

	if ((r = inode_of(path, fi, &ip)) < 0)
		return r;
	if (uid != (uid_t)-1 && uid > MAX_UID)
		return -EINVAL;
	if (gid != (gid_t)-1 &&
	    gid > (image()->version == 1 ? MAX_GID_V1 : MAX_GID))
		return -EINVAL;
	if (uid != (uid_t)-1)
		ip.uid = (uint16_t)uid;
	if (gid != (gid_t)-1)
		ip.gid = (uint16_t)gid;
	return put_changed(&ip);
}

static int
set_size(const char *path, off_t size, struct fuse_file_info *fi)
{
	struct mfs_inode ip;
	int r;

	if (size < 0)
		return -EINVAL;
	if ((uint64_t)size > UINT32_MAX)
		return -EFBIG;
	if ((r = inode_of(path, fi, &ip)) < 0)
		return r;
	if (mfs_is_dir(&ip))
		return -EISDIR;
	if ((r = mfs_resize(image(), &ip, (uint32_t)size)) < 0)
		return r;
	ip.mtime = (uint32_t)now();
	return put_changed(&ip);
}

/* The time of a timespec of utimensat(2): given, now, or unchanged. */
static uint32_t
new_time(const struct timespec *ts, uint32_t old)
{
	if (ts->tv_nsec == UTIME_OMIT)
		return old;
	if (ts->tv_nsec == UTIME_NOW)
		return (uint32_t)now();
	return (uint32_t)ts->tv_sec;
}

static int
set_times(const char *path, const struct timespec ts[2],
    struct fuse_file_info *fi)
{
	struct mfs_inode ip;
	int r;

	if ((r = inode_of(path, fi, &ip)) < 0)
		return r;
	ip.atime = new_time(&ts[0], ip.atime);
	ip.mtime = new_time(&ts[1], ip.mtime);
	return put_changed(&ip);
}

#if FUSE_USE_VERSION >= 30
static int
op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	return set_mode(path, mode, fi);
}

static int
op_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	return set_owner(path, uid, gid, fi);
}

static int
op_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	return set_size(path, size, fi);
}

static int
op_utimens(const char *path, const struct timespec ts[2],
    struct fuse_file_info *fi)
{
	return set_times(path, ts, fi);
}
#else
static int
op_chmod(const char *path, mode_t mode)
{
	return set_mode(path, mode, NULL);
}

static int
op_chown(const char *path, uid_t uid, gid_t gid)
{
	return set_owner(path, uid, gid, NULL);
}

static int
op_truncate(const char *path, off_t size)
{
	return set_size(path, size, NULL);
}

static int
op_utimens(const char *path, const struct timespec ts[2])
{
	return set_times(path, ts, NULL);
}
#endif

static int
mfs_write_file(const char *path, const char *buf, size_t size, off_t off,
    struct fuse_file_info *fi)
{
	struct mfs_inode ip;
	int e, r;

	if (off < 0)
		return -EINVAL;
	if ((uint64_t)off + size > UINT32_MAX)
		return -EFBIG;
	if ((r = inode_of(path, fi, &ip)) < 0)
		return r;
	r = mfs_pwrite(image(), &ip, buf, size, (uint32_t)off);
	/* What was written before a failure stays, with its zones. */
	ip.mtime = (uint32_t)now();
	if ((e = put_changed(&ip)) < 0 && r >= 0)
		r = e;
	/* FUSE asks for a few pages at a time, which fits an int. */
	return r < 0 ? r : (int)size;
}

static int
mfs_sync_file(const char *path, int datasync, struct fuse_file_info *fi)
{
	struct mount *m;
	int r;

	(void)path;
	(void)datasync;
	(void)fi;
	m = mount_of();
	if ((r = mfs_sync(&m->fs)) < 0)
		return r;
	m->flushed = now();
	return fsync(m->fs.fd) == -1 ? -errno : 0;
}

/*
 * Mounted, in the background: let go of the terminal and the directory,
 * and tell the parent, which then exits.
 */
static void
mounted(void)
{
	int fd;

	if (ready_fd == -1)
		return;
	if (chdir("/") == -1)
		warn("/");
	if (write(ready_fd, "", 1) == -1)
		warn("cannot tell the parent that the mount is made");
	(void)close(ready_fd);
	if ((fd = open("/dev/null", O_RDWR)) != -1) {
		(void)dup2(fd, STDIN_FILENO);
		(void)dup2(fd, STDOUT_FILENO);
		(void)dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			(void)close(fd);
	}
	ready_fd = -1;
}

#if FUSE_USE_VERSION >= 30
static void *
mfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	(void)conn;
	cfg->use_ino = 1;
	cfg->readdir_ino = 1;
	mounted();
	return mount_of();
}
#else
static void *
mfs_init(struct fuse_conn_info *conn)
{
	(void)conn;
	mounted();
	return mount_of();
}
#endif

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: mount_minixfs [-w [-u always|sync|seconds]] "
	    "[-M SIZE:HEADS:SIDE]\n"
	    "           [FUSE options] IMAGE MOUNTPOINT\n");
	exit(2);
}

/* What the command line asks of the mount itself. */
struct options {
	struct mfs_tracks tracks;	/* -M */
	const char	*image;
	int		write;		/* -w */
	int		always;		/* -u always */
	long		every;		/* -u seconds */
	int		foreground;	/* -f or -d, or -o debug, for FUSE */
};

/* -u: always, sync, or a number of seconds. */
static void
flush_option(const char *s, struct options *o)
{
	char *end;
	long n;

	if (strcmp(s, "always") == 0) {
		o->always = 1;
	} else if (strcmp(s, "sync") != 0) {
		n = strtol(s, &end, 10);
		if (*end != '\0' || end == s || n <= 0)
			usage();
		o->every = n;
	}
}

/*
 * Take the options of mount_minixfs out of the list of -o, as mount(8)
 * gives them, on MINIX 3 the only way: rw for -w, ro, update=X for -u X
 * and tracks=X for -M X.  The rest stays in list, for FUSE.  Returns the
 * value of update=, or NULL.
 */
static const char *
mount_opts(char *list, struct options *o)
{
	const char *u;
	char *next, *p, *w;

	u = NULL;
	w = list;
	for (p = list; p != NULL; p = next) {
		if ((next = strchr(p, ',')) != NULL)
			*next++ = '\0';
		if (strcmp(p, "rw") == 0) {
			o->write = 1;
		} else if (strcmp(p, "ro") == 0) {
			o->write = 0;
		} else if (strncmp(p, "update=", 7) == 0) {
			u = p + 7;
		} else if (strncmp(p, "tracks=", 7) == 0) {
			if (mfs_parse_tracks(p + 7, &o->tracks) < 0)
				usage();
		} else if (*p != '\0') {
			if (strcmp(p, "debug") == 0)
				o->foreground = 1;
			if (w != list)
				*w++ = ',';
			(void)memmove(w, p, strlen(p) + 1);
			w += strlen(w);
		}
	}
	*w = '\0';
	return u;
}

/*
 * Take IMAGE, the first argument that is not an option, and -M, -w, -u
 * and the options of -o that mount_opts() knows out of argv, and build
 * the arguments for FUSE in fargv: the rest, with a single-threaded
 * mount, read-only unless -w says otherwise, in place of the "ro" left
 * for main() to change.
 */
static void
split_args(int argc, char **argv, char **fargv, int *fargc,
    struct options *o)
{
	const char *ou, *u;
	int i, n;

	u = NULL;
	n = 0;
	fargv[n++] = argv[0];
	fargv[n++] = opt_single;
	fargv[n++] = opt_o;
	fargv[n++] = opt_ro;
#ifndef PUFFS_SERVICE
	/* FUSE stays where it is; see background(). */
	fargv[n++] = opt_foreground;
#endif
	for (i = 1; i < argc; i++) {
		if (n >= MAX_ARGS - 1)
			usage();
		if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) {
			if (mfs_parse_tracks(argv[++i], &o->tracks) < 0)
				usage();
		} else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
			u = argv[++i];
		} else if (strcmp(argv[i], "-w") == 0) {
			o->write = 1;
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			if ((ou = mount_opts(argv[++i], o)) != NULL)
				u = ou;
			if (argv[i][0] != '\0') {
				fargv[n++] = argv[i - 1];
				fargv[n++] = argv[i];
			}
		} else if (argv[i][0] == '-' || o->image != NULL) {
			if (strcmp(argv[i], "-f") == 0 ||
			    strcmp(argv[i], "-d") == 0)
				o->foreground = 1;
			fargv[n++] = argv[i];
		} else {
			o->image = argv[i];
		}
	}
	if (u != NULL && !o->write)
		usage();
	if (u != NULL)
		flush_option(u, o);
	fargv[n] = NULL;
	*fargc = n;
}

#ifndef PUFFS_SERVICE
/*
 * Go into the background, before the image is opened and locked, and not
 * after, as FUSE would: a lock of fcntl(2) belongs to the process, and
 * would go with the parent that FUSE lets exit.  The parent waits until
 * the child has mounted, and exits with 0, or has failed, and exits as it
 * did.
 */
static void
background(void)
{
	ssize_t n;
	pid_t pid;
	int fd[2], status;
	char c;

	if (pipe(fd) == -1)
		err(1, "pipe");
	if ((pid = fork()) == -1)
		err(1, "fork");
	if (pid == 0) {
		(void)close(fd[0]);
		ready_fd = fd[1];
		if (setsid() == -1)
			err(1, "setsid");
		return;
	}
	(void)close(fd[1]);
	while ((n = read(fd[0], &c, 1)) == -1 && errno == EINTR)
		continue;
	if (n == 1)
		_exit(0);
	if (waitpid(pid, &status, 0) == -1)
		err(1, "waitpid");
	exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}
#endif

/* Open the image, for writing if -w asks and the image allows. */
static void
open_image(struct mount *m, const struct options *o)
{
	int r;

	r = mfs_open_tracks(&m->fs, o->image, o->write, &o->tracks);
	if (r == -EINVAL)
		errx(1, "%s: not a MINIX file system", o->image);
	if (r < 0)
		errx(1, "%s: %s", o->image, strerror(-r));
	m->rw = o->write;
	if (m->rw && (m->fs.flex || !mfs_is_clean(&m->fs))) {
		warnx("warning: %s: %s; mounted read-only", o->image,
		    m->fs.flex ? "the flex directories of Minix-vmd cannot "
		    "be written" : "not marked clean; check it with "
		    "fsck_minixfs");
		mfs_close(&m->fs);
		if ((r = mfs_open_tracks(&m->fs, o->image, 0,
		    &o->tracks)) < 0)
			errx(1, "%s: %s", o->image, strerror(-r));
		m->rw = 0;
	}
	if (!m->rw)
		return;
	mfs_maps_through(&m->fs, o->always);
	m->fs.keep = keep_open;
	m->fs.keep_arg = m;
	if ((r = mfs_mark_in_use(&m->fs)) < 0)
		errx(1, "%s: %s", o->image, strerror(-r));
	m->every = o->every;
	m->flushed = now();
}

/* Free the files that were removed while open.  Returns 0 or -errno. */
static int
free_orphans(struct mount *m)
{
	size_t i;
	int e, r;

	r = 0;
	for (i = 0; i < m->maxhandles; i++)
		if (m->handles[i].ino != 0 && m->handles[i].orphan &&
		    (e = mfs_free_orphan(&m->fs, m->handles[i].ino)) < 0 &&
		    r == 0)
			r = e;
	free(m->handles);
	m->handles = NULL;
	m->nhandles = m->maxhandles = 0;
	return r;
}

/*
 * After the unmount: the files removed while open freed, the maps out,
 * the clean mark back, and the image closed, once.  Returns 1 if they
 * could not be written.
 */
static int
close_image(struct mount *m)
{
	int r;

	if (m->closed)
		return m->failed;
	m->closed = 1;
	r = 0;
	if (m->rw)
		r = free_orphans(m);
	if (m->rw && r == 0 && (r = mfs_sync(&m->fs)) == 0 &&
	    (r = mfs_mark_clean(&m->fs, 1)) == 0 && fsync(m->fs.fd) == -1)
		r = -errno;
	mfs_close(&m->fs);
	if (r < 0) {
		warnx("%s: %s", m->path, strerror(-r));
		m->failed = 1;
	}
	return m->failed;
}

/*
 * The unmount.  On MINIX 3 this is the last the service hears before
 * mount(8) is told the file system is gone, and it lives on until it is
 * stopped; so the image is closed here, and not after fuse_main().
 */
static void
mfs_destroy(void *data)
{
	(void)data;
	(void)close_image(the_mount);
}

static void
write_ops(struct fuse_operations *ops)
{
	ops->create = mfs_create_file;
	ops->mknod = mfs_make_node;
	ops->mkdir = mfs_make_dir;
	ops->symlink = mfs_make_symlink;
	ops->link = mfs_make_link;
	ops->unlink = mfs_remove;
	ops->rmdir = mfs_remove_dir;
	ops->rename = mfs_move;
	ops->chmod = op_chmod;
	ops->chown = op_chown;
	ops->truncate = op_truncate;
	ops->utimens = op_utimens;
	ops->write = mfs_write_file;
	ops->fsync = mfs_sync_file;
	ops->release = mfs_release_file;
}

#ifdef PUFFS_SERVICE
/*
 * Built as a service of MINIX 3 (make fuse-minix): libpuffs has the
 * main() of the program, __wrap_main(), which starts the service and
 * calls __real_main() with the arguments that mount(8) gave.  Started
 * from a shell, the service cannot start, and hangs without a word.  One
 * that the reincarnation server started has none of the standard
 * descriptors open, so an open one says it was not, and how to mount is
 * shown instead.  The exit is asked of the process manager, as _exit()
 * of libc does, since libsys puts in its place one that only a service
 * can use.
 */
int __wrap_main(int, char **);
int __real_main(int, char **);

int
main(int argc, char **argv)
{
	message m;

	if (fcntl(STDERR_FILENO, F_GETFD) != -1) {
		(void)fprintf(stderr,
		    "usage: mount -t minixfs [-o OPTIONS] SPECIAL NODE\n"
		    "       OPTIONS: rw, ro, update=always|sync|SECONDS, "
		    "tracks=SIZE:HEADS:SIDE\n"
		    "mount_minixfs is a service that mount(8) starts; "
		    "see mount_minixfs(8).\n");
		(void)memset(&m, 0, sizeof(m));
		m.m_lc_pm_exit.status = 2;
		(void)_syscall(PM_PROC_NR, PM_EXIT, &m);
		abort();
	}
	return __wrap_main(argc, argv);
}

#define main	__real_main
#endif

int
main(int argc, char **argv)
{
	static struct fuse_operations ops;
	static struct mount m;
	char *fargv[MAX_ARGS];
	struct options o;
	int fargc, r;

	if (argc < 3)
		usage();
	(void)memset(&o, 0, sizeof(o));
	split_args(argc, argv, fargv, &fargc, &o);
	if (o.image == NULL)
		usage();
#ifndef PUFFS_SERVICE
	if (!o.foreground)
		background();
#endif
	open_image(&m, &o);
	m.path = o.image;
	the_mount = &m;
	if (m.rw) {
		fargv[3] = opt_rw;
		write_ops(&ops);
	}
	ops.getattr = mfs_getattr;
	ops.readlink = mfs_readlink;
	ops.open = mfs_open_file;
	ops.read = mfs_read_file;
	ops.readdir = mfs_read_dir;
	ops.statfs = mfs_statfs;
	ops.init = mfs_init;
	ops.destroy = mfs_destroy;

	r = fuse_main(fargc, fargv, &ops, &m);
	/* Where FUSE did not mount, or did not say it unmounted. */
	if (close_image(&m) != 0)
		r = 1;
	return r == 0 ? 0 : 1;
}
