/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mfs_ops.c - change the names of a MINIX file system: make files,
 * directories and links, remove them, and rename them, as the system
 * calls of those names do.  Everything that can be checked is checked
 * before anything is written; a failure after that leaves at most an
 * inode or a zone taken that fsck_minixfs frees.
 *
 * A directory has two names more than its entry in its parent: its own
 * "." and the ".." of each directory in it.  Renaming a directory into
 * another moves its ".." along.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "mfs.h"

#define MAX_LINKS_V1	255		/* the link count of V1 is a byte */
#define MAX_LINKS	65535
#define MAX_GID_V1	255		/* and so is its group */

/* The entry of a name in a directory. */
struct find {
	const char	*name;
	uint32_t	ino;
	uint32_t	off;
	int		others;		/* entries other than "." and ".." */
};

static int
find_fn(const struct mfs_dirent *de, void *arg)
{
	struct find *f;

	f = arg;
	if (f->name == NULL) {
		if (strcmp(de->name, ".") != 0 && strcmp(de->name, "..") != 0) {
			f->others = 1;
			return 1;
		}
		return 0;
	}
	if (strcmp(de->name, f->name) != 0)
		return 0;
	f->ino = de->ino;
	f->off = de->off;
	return 1;
}

/* The entry of name in *dp: 0 and its inode and offset, or -ENOENT. */
static int
find_entry(struct mfs *fs, const struct mfs_inode *dp, const char *name,
    uint32_t *ino, uint32_t *off)
{
	struct find f;
	int r;

	f.name = name;
	f.ino = f.off = 0;
	f.others = 0;
	if ((r = mfs_readdir(fs, dp, find_fn, &f)) < 0)
		return r;
	if (r == 0)
		return -ENOENT;
	*ino = f.ino;
	*off = f.off;
	return 0;
}

/* 1 if the directory *dp holds nothing but "." and "..", 0 if not. */
static int
is_empty(struct mfs *fs, const struct mfs_inode *dp)
{
	struct find f;
	int r;

	f.name = NULL;
	f.others = 0;
	if ((r = mfs_readdir(fs, dp, find_fn, &f)) < 0)
		return r;
	return !f.others;
}

/* A name that can go into a directory of fs. */
static int
check_name(const struct mfs *fs, const char *name)
{
	if (name[0] == '\0' || strcmp(name, ".") == 0 ||
	    strcmp(name, "..") == 0 || strchr(name, '/') != NULL)
		return -EINVAL;
	if (strlen(name) > fs->namelen)
		return -ENAMETOOLONG;
	if (fs->flex)
		return -ENOTSUP;
	return 0;
}

/* Read the directory dir into *dp. */
static int
get_dir(struct mfs *fs, uint32_t dir, struct mfs_inode *dp)
{
	int r;

	if ((r = mfs_read_inode(fs, dir, dp)) < 0)
		return r;
	return mfs_is_dir(dp) ? 0 : -ENOTDIR;
}

static uint32_t
max_links(const struct mfs *fs)
{
	return fs->version == 1 ? MAX_LINKS_V1 : MAX_LINKS;
}

/* Add delta to the link count of inode ino and set its ctime. */
static int
add_links(struct mfs *fs, uint32_t ino, int delta, uint32_t now)
{
	struct mfs_inode ip;
	int r;

	if ((r = mfs_read_inode(fs, ino, &ip)) < 0)
		return r;
	ip.nlinks = (uint16_t)(ip.nlinks + delta);
	ip.ctime = now;
	return mfs_put_inode(fs, &ip);
}

/* Set the mtime and ctime of the directory dir, whose entries changed. */
static int
touch(struct mfs *fs, uint32_t dir, uint32_t now)
{
	struct mfs_inode dp;
	int r;

	if ((r = mfs_read_inode(fs, dir, &dp)) < 0)
		return r;
	dp.mtime = dp.ctime = now;
	return mfs_put_inode(fs, &dp);
}

/* Free the inode *ip and all its zones. */
static int
release(struct mfs *fs, struct mfs_inode *ip)
{
	uint32_t ino;
	int r;

	ino = ip->num;
	if ((r = mfs_truncate(fs, ip)) < 0)
		return r;
	(void)memset(ip, 0, sizeof(*ip));
	ip->num = ino;
	if ((r = mfs_put_inode(fs, ip)) < 0)
		return r;
	return mfs_free_inode(fs, ino);
}

/* Add an entry name for ino to directory dir, read anew. */
static int
enter(struct mfs *fs, uint32_t dir, const char *name, uint32_t ino)
{
	struct mfs_inode dp;
	int r;

	if ((r = mfs_read_inode(fs, dir, &dp)) < 0 ||
	    (r = mfs_add_entry(fs, &dp, name, ino)) < 0)
		return r;
	return mfs_put_inode(fs, &dp);
}

/* Check that a new name can go into dir; *dp gets the directory. */
static int
check_new(struct mfs *fs, uint32_t dir, const char *name,
    struct mfs_inode *dp)
{
	uint32_t ino, off;
	int r;

	if ((r = check_name(fs, name)) < 0 || (r = get_dir(fs, dir, dp)) < 0)
		return r;
	if (dp->nlinks == 0)
		return -ENOENT;		/* removed, but still open */
	r = find_entry(fs, dp, name, &ino, &off);
	return r == 0 ? -EEXIST : r == -ENOENT ? 0 : r;
}

/* Fill in a new inode, and the contents of a directory or a link. */
static int
fill_new(struct mfs *fs, struct mfs_inode *ip, uint32_t dir,
    const struct mfs_new *n, const char *target)
{
	int r;

	ip->mode = n->mode;
	ip->uid = n->uid;
	ip->gid = n->gid;
	ip->atime = ip->mtime = ip->ctime = n->time;
	ip->nlinks = 1;
	if (mfs_is_dev(ip))
		ip->zone[0] = n->rdev;
	if (mfs_is_dir(ip)) {
		ip->nlinks = 2;
		if ((r = mfs_add_entry(fs, ip, ".", ip->num)) < 0 ||
		    (r = mfs_add_entry(fs, ip, "..", dir)) < 0)
			return r;
	}
	if (target != NULL &&
	    (r = mfs_pwrite(fs, ip, target, strlen(target), 0)) < 0)
		return r;
	return mfs_put_inode(fs, ip);
}

/* The common part of mfs_make() and mfs_symlink(). */
static int
make_node(struct mfs *fs, uint32_t dir, const char *name,
    const struct mfs_new *n, const char *target, struct mfs_inode *ip)
{
	struct mfs_inode dp;
	uint32_t ino;
	int dirs, r;

	if ((r = check_new(fs, dir, name, &dp)) < 0)
		return r;
	if (fs->version == 1 && n->gid > MAX_GID_V1)
		return -EINVAL;
	dirs = (n->mode & MFS_S_IFMT) == MFS_S_IFDIR;
	if (dirs && dp.nlinks >= max_links(fs))
		return -EMLINK;
	if ((r = mfs_alloc_inode(fs, &ino)) < 0)
		return r;
	(void)memset(ip, 0, sizeof(*ip));
	ip->num = ino;
	if ((r = fill_new(fs, ip, dir, n, target)) < 0 ||
	    (r = enter(fs, dir, name, ino)) < 0) {
		/* Give back what was taken; the name is not there. */
		(void)release(fs, ip);
		return r;
	}
	if (dirs && (r = add_links(fs, dir, 1, n->time)) < 0)
		return r;
	return touch(fs, dir, n->time);
}

int
mfs_make(struct mfs *fs, uint32_t dir, const char *name,
    const struct mfs_new *n, struct mfs_inode *ip)
{
	/* Symbolic links take mfs_symlink(); no other type is made. */
	switch (n->mode & MFS_S_IFMT) {
	case MFS_S_IFIFO:
	case MFS_S_IFCHR:
	case MFS_S_IFDIR:
	case MFS_S_IFBLK:
	case MFS_S_IFREG:
	case MFS_S_IFSOCK:
		return make_node(fs, dir, name, n, NULL, ip);
	default:
		return -EINVAL;
	}
}

int
mfs_symlink(struct mfs *fs, uint32_t dir, const char *name,
    const char *target, const struct mfs_new *n, struct mfs_inode *ip)
{
	struct mfs_new ln;
	size_t len;

	/* MINIX reads a link from its first block, with a NUL after it. */
	len = strlen(target);
	if (len == 0)
		return -ENOENT;
	if (len >= fs->block_size)
		return -ENAMETOOLONG;
	ln = *n;
	ln.mode = MFS_S_IFLNK | 0777;
	return make_node(fs, dir, name, &ln, target, ip);
}

int
mfs_link(struct mfs *fs, uint32_t ino, uint32_t dir, const char *name,
    uint32_t now)
{
	struct mfs_inode dp, ip;
	int r;

	if ((r = check_new(fs, dir, name, &dp)) < 0 ||
	    (r = mfs_read_inode(fs, ino, &ip)) < 0)
		return r;
	if (mfs_is_dir(&ip))
		return -EPERM;
	if (ip.nlinks >= max_links(fs))
		return -EMLINK;
	if ((r = enter(fs, dir, name, ino)) < 0 ||
	    (r = add_links(fs, ino, 1, now)) < 0)
		return r;
	return touch(fs, dir, now);
}

/* Drop one name of the file *ip, which is not a directory. */
static int
drop_name(struct mfs *fs, struct mfs_inode *ip, uint32_t now)
{
	if (ip->nlinks <= 1)
		return release(fs, ip);
	ip->nlinks--;
	ip->ctime = now;
	return mfs_put_inode(fs, ip);
}

int
mfs_unlink(struct mfs *fs, uint32_t dir, const char *name, uint32_t now)
{
	struct mfs_inode dp, ip;
	uint32_t ino, off;
	int r;

	if ((r = get_dir(fs, dir, &dp)) < 0 ||
	    (r = find_entry(fs, &dp, name, &ino, &off)) < 0 ||
	    (r = mfs_read_inode(fs, ino, &ip)) < 0)
		return r;
	if (mfs_is_dir(&ip))
		return -EISDIR;
	if ((r = mfs_set_entry(fs, &dp, off, 0)) < 0 ||
	    (r = drop_name(fs, &ip, now)) < 0)
		return r;
	return touch(fs, dir, now);
}

/* Check that directory *ip can go: it is empty and not the root. */
static int
check_rmdir(struct mfs *fs, const struct mfs_inode *ip)
{
	int r;

	if (ip->num == MFS_ROOT_INO)
		return -EBUSY;
	if ((r = is_empty(fs, ip)) < 0)
		return r;
	return r ? 0 : -ENOTEMPTY;
}

/* Free the empty directory *ip, whose parent is dir. */
static int
drop_dir(struct mfs *fs, struct mfs_inode *ip, uint32_t dir, uint32_t now)
{
	int r;

	if ((r = release(fs, ip)) < 0)
		return r;
	return add_links(fs, dir, -1, now);
}

int
mfs_rmdir(struct mfs *fs, uint32_t dir, const char *name, uint32_t now)
{
	struct mfs_inode dp, ip;
	uint32_t ino, off;
	int r;

	if (strcmp(name, ".") == 0)
		return -EINVAL;
	if (strcmp(name, "..") == 0)
		return -ENOTEMPTY;
	if ((r = get_dir(fs, dir, &dp)) < 0 ||
	    (r = find_entry(fs, &dp, name, &ino, &off)) < 0 ||
	    (r = mfs_read_inode(fs, ino, &ip)) < 0)
		return r;
	if (!mfs_is_dir(&ip))
		return -ENOTDIR;
	if ((r = check_rmdir(fs, &ip)) < 0 ||
	    (r = mfs_set_entry(fs, &dp, off, 0)) < 0 ||
	    (r = drop_dir(fs, &ip, dir, now)) < 0)
		return r;
	return touch(fs, dir, now);
}

/*
 * Whether directory dir is src or below it, which a rename of src into
 * dir would make a loop of: follow ".." up to the root.
 */
static int
is_below(struct mfs *fs, uint32_t dir, uint32_t src)
{
	struct mfs_inode dp;
	uint32_t i, off, up;
	int r;

	for (i = 0; i < fs->ninodes; i++) {
		if (dir == src)
			return 1;
		if (dir == MFS_ROOT_INO)
			return 0;
		if ((r = get_dir(fs, dir, &dp)) < 0 ||
		    (r = find_entry(fs, &dp, "..", &up, &off)) < 0)
			return r;
		dir = up;
	}
	return -EIO;			/* ".." goes round in a loop */
}

/* A rename: the source, and the target if a file has that name. */
struct rename {
	struct mfs_inode src;
	struct mfs_inode dst;
	uint32_t	src_off;
	uint32_t	dst_off;
	int		replace;	/* a file has the new name */
};

/* Check a rename of odir/oname to ndir/nname before anything changes. */
static int
check_rename(struct mfs *fs, uint32_t odir, const char *oname,
    uint32_t ndir, const char *nname, int noreplace, struct rename *rn)
{
	struct mfs_inode dp;
	uint32_t ino;
	int r;

	if (strcmp(oname, ".") == 0 || strcmp(oname, "..") == 0)
		return -EINVAL;
	if ((r = check_name(fs, nname)) < 0 ||
	    (r = get_dir(fs, odir, &dp)) < 0 ||
	    (r = find_entry(fs, &dp, oname, &ino, &rn->src_off)) < 0 ||
	    (r = mfs_read_inode(fs, ino, &rn->src)) < 0 ||
	    (r = get_dir(fs, ndir, &dp)) < 0)
		return r;
	r = find_entry(fs, &dp, nname, &ino, &rn->dst_off);
	if (r < 0 && r != -ENOENT)
		return r;
	rn->replace = r == 0;
	if (rn->replace && (noreplace ||
	    (r = mfs_read_inode(fs, ino, &rn->dst)) < 0))
		return noreplace ? -EEXIST : r;
	if (rn->replace && rn->dst.num == rn->src.num)
		return 0;
	if (rn->replace && mfs_is_dir(&rn->dst) != mfs_is_dir(&rn->src))
		return mfs_is_dir(&rn->dst) ? -EISDIR : -ENOTDIR;
	if (rn->replace && mfs_is_dir(&rn->dst) &&
	    (r = check_rmdir(fs, &rn->dst)) < 0)
		return r;
	if (!mfs_is_dir(&rn->src) || odir == ndir)
		return 0;
	if ((r = is_below(fs, ndir, rn->src.num)) != 0)
		return r < 0 ? r : -EINVAL;
	if (!rn->replace && dp.nlinks >= max_links(fs))
		return -EMLINK;
	return 0;
}

/* Point the ".." of the directory *ip at dir. */
static int
move_dotdot(struct mfs *fs, const struct mfs_inode *ip, uint32_t dir)
{
	uint32_t ino, off;
	int r;

	if ((r = find_entry(fs, ip, "..", &ino, &off)) < 0)
		return r;
	return mfs_set_entry(fs, ip, off, dir);
}

/* Give the new name to the source: over the file there, or a new entry. */
static int
rename_to(struct mfs *fs, uint32_t ndir, const char *nname,
    struct rename *rn, uint32_t now)
{
	struct mfs_inode dp;
	int r;

	if (!rn->replace)
		return enter(fs, ndir, nname, rn->src.num);
	if ((r = mfs_read_inode(fs, ndir, &dp)) < 0 ||
	    (r = mfs_set_entry(fs, &dp, rn->dst_off, rn->src.num)) < 0)
		return r;
	if (mfs_is_dir(&rn->dst))
		return drop_dir(fs, &rn->dst, ndir, now);
	return drop_name(fs, &rn->dst, now);
}

int
mfs_rename(struct mfs *fs, uint32_t odir, const char *oname, uint32_t ndir,
    const char *nname, int noreplace, uint32_t now)
{
	struct mfs_inode dp;
	struct rename rn;
	int r;

	if ((r = check_rename(fs, odir, oname, ndir, nname, noreplace,
	    &rn)) < 0)
		return r;
	if (rn.replace && rn.dst.num == rn.src.num)
		return 0;
	if ((r = rename_to(fs, ndir, nname, &rn, now)) < 0 ||
	    (r = mfs_read_inode(fs, odir, &dp)) < 0 ||
	    (r = mfs_set_entry(fs, &dp, rn.src_off, 0)) < 0)
		return r;
	if (mfs_is_dir(&rn.src) && odir != ndir &&
	    ((r = move_dotdot(fs, &rn.src, ndir)) < 0 ||
	    (r = add_links(fs, odir, -1, now)) < 0 ||
	    (r = add_links(fs, ndir, 1, now)) < 0))
		return r;
	if ((r = add_links(fs, rn.src.num, 0, now)) < 0 ||
	    (r = touch(fs, odir, now)) < 0)
		return r;
	return odir == ndir ? 0 : touch(fs, ndir, now);
}
