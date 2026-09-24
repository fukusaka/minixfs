/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * fsck_minixfs - check a MINIX file system image, and repair it.
 *
 *	fsck_minixfs [-y] image
 *
 * The check reads the super block, walks the tree from the root, and then
 * compares what it found with the inode table and the bit maps:
 *
 *	- the super block: a maximum file size that makes sense, an image
 *	  that holds the whole file system, and errors that Linux recorded
 *	  in the state word of V1 and V2;
 *	- directory entries: inode numbers in range, entries that name a
 *	  free inode or one of no valid type, empty names or names with
 *	  '/', "." and ".." first and pointing to the directory and its
 *	  parent, and directories that more than one directory lists;
 *	- inodes: a size the zones can reach, directories a whole number of
 *	  entries long, zone numbers inside the data area, zones that two
 *	  files share, device files with zone numbers besides the device
 *	  number, and symbolic links that are empty, longer than Linux
 *	  allows or cut short by a NUL byte;
 *	- link counts against the entries that name each inode, inodes in
 *	  use that no directory names, both bit maps, and bit 0 of each map,
 *	  which is never used but must be set.
 *
 * Without -y the image is only read.  With -y each problem is repaired
 * where it can be: bad entries are removed, "." and ".." are pointed
 * where they belong, and put back where they are missing (moving an entry
 * that holds their place, and giving the directory a zone if it has
 * none), zone numbers outside the data area and the second use of a zone
 * are cleared, sizes are cut to what the zones reach, to whole directory
 * entries and to the text of a symbolic link, link counts are set, inodes
 * that no directory names are freed, the bit maps are made to match, and
 * the super block gets a sensible maximum file size.  A root that is not
 * a directory and an image shorter than the file system are left.  The
 * image is then checked again, and marked clean if nothing is left: in
 * V1 and V2 as Linux marks it, in V3 with the clean flag of MINIX 3, which
 * otherwise mounts the file system read-only.  If problems remain, it is
 * marked as having errors instead.
 *
 * Each problem is printed on a line of its own, with "(repaired)" or
 * "(not repaired)" after it under -y.  A file system that is not marked
 * clean is noted, but is not a problem in itself.  Exit status: 0 if the
 * file system is consistent (after the repairs, under -y), 1 if problems
 * remain, 2 for a usage error, 3 if the image cannot be checked at all.
 */

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mfs.h"

#define EXIT_PROBLEMS	1
#define EXIT_USAGE	2
#define EXIT_CANNOT	3
#define V1_MAX_LINKS	255		/* the link count of V1 is 8 bits */
#define MAX_SYMLINK	4095		/* longest link text Linux writes */
#define MAX_SIZE	0x7fffffff	/* s_max_size is signed in MINIX */
#define NEED_DOT	1		/* "." is missing */
#define NEED_DOTDOT	2		/* ".." is missing */

/* What check_inode() found. */
enum kind {
	K_DIR,				/* a directory, to be read */
	K_OTHER,			/* any other file */
	K_FREE,				/* a free inode */
	K_BAD				/* no valid type, or unreadable */
};

/* A directory waiting to be read. */
struct pending {
	char		*path;
	uint32_t	ino;
	uint32_t	parent;
};

/* A directory whose "." or ".." is missing. */
struct dotfix {
	char		*path;
	uint32_t	ino;
	uint32_t	parent;
	int		need;			/* NEED_DOT, NEED_DOTDOT */
};

/* The state of one check. */
struct check {
	struct mfs		fs;
	struct pending		*queue;		/* directories to read */
	struct dotfix		*fixes;		/* dots to put back */
	struct mfs_inode	*cur;		/* whose zones are walked */
	const char		*image;
	const char		*path;		/* the name of *cur */
	uint32_t		*refs;		/* entries naming each inode */
	unsigned char		*reached;	/* bit per inode: named */
	unsigned char		*used;		/* bit per zone: in a file */
	unsigned char		*allocated;	/* bit per zone: by a repair */
	size_t			head;		/* first pending directory */
	size_t			nqueue;
	size_t			maxqueue;
	size_t			nfixes;
	size_t			maxfixes;
	unsigned long		problems;
	unsigned long		unrepaired;
	uint32_t		inodes_used;
	uint32_t		zones_used;
	int			repair;		/* -y */
	int			quiet;		/* report nothing but the sum */
	int			cur_dirty;	/* *cur needs writing back */
	char			why[64];	/* what makes an inode bad */
};

static void
usage(void)
{
	(void)fprintf(stderr, "usage: fsck_minixfs [-y] image\n");
	exit(EXIT_USAGE);
}

/*
 * Report one problem.  r is what the repair returned: 0 if it worked, a
 * negative errno value if it failed, or 1 if nothing can repair it; it
 * does not matter when not repairing.
 */
static void
problem(struct check *c, int r, const char *fmt, ...)
{
	va_list ap;

	c->problems++;
	if (c->repair && r != 0)
		c->unrepaired++;
	if (c->quiet)
		return;
	(void)printf("%s: ", c->image);
	va_start(ap, fmt);
	(void)vprintf(fmt, ap);
	va_end(ap);
	if (c->repair && r == 0)
		(void)printf(" (repaired)");
	else if (c->repair && r < 0)
		(void)printf(" (not repaired: %s)", strerror(-r));
	else if (c->repair)
		(void)printf(" (not repaired)");
	(void)putchar('\n');
}

static int
test_bit(const unsigned char *bits, uint32_t n)
{
	return (bits[n / 8] >> (n % 8)) & 1;
}

static void
set_bit(unsigned char *bits, uint32_t n, int v)
{
	if (v)
		bits[n / 8] |= (unsigned char)(1 << (n % 8));
	else
		bits[n / 8] &= (unsigned char)~(1 << (n % 8));
}

/* Memory for n bits, cleared; exit if there is none. */
static unsigned char *
new_bits(uint64_t n)
{
	unsigned char *bits;

	if ((bits = calloc((size_t)(n / 8 + 1), 1)) == NULL)
		err(EXIT_CANNOT, NULL);
	return bits;
}

static char *
join(const char *dir, const char *name)
{
	char *s;

	if ((s = malloc(strlen(dir) + strlen(name) + 2)) == NULL)
		err(EXIT_CANNOT, NULL);
	if (strcmp(dir, "/") == 0)
		(void)sprintf(s, "/%s", name);
	else
		(void)sprintf(s, "%s/%s", dir, name);
	return s;
}

/* Queue a directory; path becomes the queue's. */
static void
enqueue(struct check *c, uint32_t ino, uint32_t parent, char *path)
{
	struct pending *q;

	if (c->nqueue == c->maxqueue) {
		c->maxqueue = c->maxqueue == 0 ? 64 : c->maxqueue * 2;
		q = realloc(c->queue, c->maxqueue * sizeof(*q));
		if (q == NULL)
			err(EXIT_CANNOT, NULL);
		c->queue = q;
	}
	c->queue[c->nqueue].ino = ino;
	c->queue[c->nqueue].parent = parent;
	c->queue[c->nqueue].path = path;
	c->nqueue++;
}

/* Under -y, clear the zone number at ref; see problem() for the result. */
static int
clear_zone(struct check *c, const struct mfs_zref *ref)
{
	int r;

	if (!c->repair)
		return 0;
	if ((r = mfs_clear_zref(&c->fs, c->cur, ref)) == 0 && ref->block == 0)
		c->cur_dirty = 1;
	return r;
}

/* Note one zone of the file c->path. */
static int
zone_fn(uint32_t zone, int level, const struct mfs_zref *ref, void *arg)
{
	struct check *c;
	uint32_t n;

	(void)level;
	c = arg;
	if (zone < c->fs.firstdatazone || zone >= c->fs.nzones) {
		problem(c, clear_zone(c, ref), "%s: zone %" PRIu32
		    " is outside the data area", c->path, zone);
		return MFS_WALK_SKIP;
	}
	n = zone - c->fs.firstdatazone;
	if (test_bit(c->used, n)) {
		problem(c, clear_zone(c, ref), "%s: zone %" PRIu32
		    " is used by another file too", c->path, zone);
		/* What it lists belongs to the other file. */
		return MFS_WALK_SKIP;
	}
	set_bit(c->used, n, 1);
	c->zones_used++;
	return 0;
}

static int
valid_type(uint16_t mode)
{
	switch (mode & MFS_S_IFMT) {
	case MFS_S_IFREG:
	case MFS_S_IFDIR:
	case MFS_S_IFLNK:
	case MFS_S_IFCHR:
	case MFS_S_IFBLK:
	case MFS_S_IFIFO:
	case MFS_S_IFSOCK:
		return 1;
	default:
		return 0;
	}
}

/* Under -y, write *ip back; see problem() for the result. */
static int
put_inode(struct check *c, const struct mfs_inode *ip)
{
	if (!c->repair)
		return 0;
	return mfs_put_inode(&c->fs, ip);
}

/* Sizes that cannot be right. */
static void
check_size(struct check *c, struct mfs_inode *ip, const char *path)
{
	uint32_t size;

	if (ip->size > c->fs.max_file) {
		size = ip->size;
		ip->size = (uint32_t)c->fs.max_file;
		problem(c, put_inode(c, ip), "%s: size %" PRIu32 " is more "
		    "than the zones reach", path, size);
	}
	if (mfs_is_dir(ip) && ip->size % c->fs.dirent_size != 0) {
		size = ip->size;
		ip->size -= ip->size % c->fs.dirent_size;
		problem(c, put_inode(c, ip), "%s: directory size %" PRIu32
		    " is not a whole number of entries", path, size);
	}
}

/* A device file keeps its device number in zone 0 and nothing else. */
static void
check_dev(struct check *c, struct mfs_inode *ip, const char *path)
{
	int i, found;

	found = 0;
	for (i = 1; i < MFS_NR_ZONES; i++) {
		if (ip->zone[i] != 0) {
			found = 1;
			ip->zone[i] = 0;
		}
	}
	if (found)
		problem(c, put_inode(c, ip), "%s: device file has zone "
		    "numbers besides its device number", path);
}

/*
 * The text of a symbolic link: not empty, not longer than Linux writes,
 * and without a NUL byte.  Returns 0 if the link can stay; otherwise
 * c->why says what is wrong with it.
 */
static int
check_symlink(struct check *c, struct mfs_inode *ip, const char *path)
{
	char text[MAX_SYMLINK];
	uint32_t size;
	ssize_t n;
	size_t len;

	if (ip->size == 0 || ip->size > MAX_SYMLINK) {
		(void)snprintf(c->why, sizeof(c->why), "a symbolic link of %"
		    PRIu32 " bytes", ip->size);
		return -1;
	}
	/* A link that cannot be read is left to the walk of its zones. */
	if ((n = mfs_pread(&c->fs, ip, text, ip->size, 0)) !=
	    (ssize_t)ip->size)
		return 0;
	if ((len = strnlen(text, (size_t)n)) == 0) {
		(void)snprintf(c->why, sizeof(c->why), "a symbolic link that "
		    "starts with a NUL byte");
		return -1;
	}
	if (len < ip->size) {
		size = ip->size;
		ip->size = (uint32_t)len;
		problem(c, put_inode(c, ip), "%s: symbolic link of %" PRIu32
		    " bytes ends at a NUL byte after %zu", path, size, len);
	}
	return 0;
}

/*
 * Check the inode that path names, the first time the walk reaches it,
 * and repair it.  For K_BAD, c->why says what is wrong.
 */
static enum kind
check_inode(struct check *c, uint32_t ino, const char *path)
{
	struct mfs_inode ip;
	int r;

	if ((r = mfs_get_inode(&c->fs, ino, &ip)) < 0) {
		(void)snprintf(c->why, sizeof(c->why), "unreadable");
		return K_BAD;
	}
	if (ip.mode == 0)
		return K_FREE;
	if (!valid_type(ip.mode)) {
		(void)snprintf(c->why, sizeof(c->why), "of no valid type");
		return K_BAD;
	}
	check_size(c, &ip, path);
	if (mfs_is_lnk(&ip) && check_symlink(c, &ip, path) < 0)
		return K_BAD;
	c->inodes_used++;
	if (mfs_is_dev(&ip)) {
		check_dev(c, &ip, path);
	} else {
		c->cur = &ip;
		c->cur_dirty = 0;
		c->path = path;
		if ((r = mfs_walk_zones(&c->fs, &ip, zone_fn, c)) < 0)
			problem(c, 1, "%s: %s", path, strerror(-r));
		if (c->cur_dirty && (r = mfs_put_inode(&c->fs, &ip)) < 0)
			problem(c, r, "%s: inode %" PRIu32 " cannot be written",
			    path, ino);
		c->cur = NULL;
	}
	return mfs_is_dir(&ip) ? K_DIR : K_OTHER;
}

/* The entries of a directory, in order. */
struct entries {
	struct mfs_dirent	*ent;
	size_t			n;
	size_t			max;
};

static int
collect_fn(const struct mfs_dirent *de, void *arg)
{
	struct mfs_dirent *p;
	struct entries *e;

	e = arg;
	if (e->n == e->max) {
		e->max = e->max == 0 ? 32 : e->max * 2;
		if ((p = realloc(e->ent, e->max * sizeof(*p))) == NULL)
			return -ENOMEM;
		e->ent = p;
	}
	e->ent[e->n++] = *de;
	return 0;
}

/* Under -y, set the inode number of an entry; 0 removes it. */
static int
set_entry(struct check *c, const struct mfs_inode *dp,
    const struct mfs_dirent *de, uint32_t ino)
{
	if (!c->repair)
		return 0;
	return mfs_set_entry(&c->fs, dp, de->off, ino);
}

/* Check that "." or ".." names ino, and count the link it makes. */
static void
check_dot(struct check *c, const struct pending *d,
    const struct mfs_inode *dp, const struct mfs_dirent *de, uint32_t ino)
{
	if (de->ino != ino) {
		problem(c, set_entry(c, dp, de, ino), "%s: \"%s\" names inode %"
		    PRIu32 ", not %" PRIu32, d->path, de->name, de->ino, ino);
		if (!c->repair)
			return;
	}
	c->refs[ino]++;
}

/* Remember a directory whose "." or ".." is missing. */
static void
need_dots(struct check *c, const struct pending *d, int need)
{
	struct dotfix *f;

	if (c->nfixes == c->maxfixes) {
		c->maxfixes = c->maxfixes == 0 ? 16 : c->maxfixes * 2;
		f = realloc(c->fixes, c->maxfixes * sizeof(*f));
		if (f == NULL)
			err(EXIT_CANNOT, NULL);
		c->fixes = f;
	}
	f = &c->fixes[c->nfixes++];
	if ((f->path = strdup(d->path)) == NULL)
		err(EXIT_CANNOT, NULL);
	f->ino = d->ino;
	f->parent = d->parent;
	f->need = need;
}

/* Check one entry of a directory, other than its "." and "..". */
static void
check_entry(struct check *c, const struct pending *d,
    const struct mfs_inode *dp, const struct mfs_dirent *de)
{
	struct mfs_inode ip;
	enum kind kind;
	char *path;

	if (de->ino > c->fs.ninodes) {
		problem(c, set_entry(c, dp, de, 0), "%s: entry \"%s\" names "
		    "inode %" PRIu32 ", past the last", d->path, de->name,
		    de->ino);
		return;
	}
	if (de->name[0] == '\0' || strchr(de->name, '/') != NULL ||
	    strcmp(de->name, ".") == 0 || strcmp(de->name, "..") == 0) {
		problem(c, set_entry(c, dp, de, 0), "%s: bad name \"%s\"",
		    d->path, de->name);
		if (c->repair)
			return;
	}
	path = join(d->path, de->name);
	if (test_bit(c->reached, de->ino)) {
		if (mfs_get_inode(&c->fs, de->ino, &ip) == 0 &&
		    mfs_is_dir(&ip)) {
			problem(c, set_entry(c, dp, de, 0), "%s: directory "
			    "inode %" PRIu32 " is listed in more than one "
			    "directory", path, de->ino);
			if (c->repair) {
				free(path);
				return;
			}
		}
		c->refs[de->ino]++;
		free(path);
		return;
	}

	kind = check_inode(c, de->ino, path);
	if (kind == K_FREE || kind == K_BAD) {
		problem(c, set_entry(c, dp, de, 0), "%s: names inode %" PRIu32
		    ", which is %s", path, de->ino,
		    kind == K_FREE ? "free" : c->why);
		if (c->repair) {
			free(path);
			return;
		}
	}
	set_bit(c->reached, de->ino, 1);
	c->refs[de->ino]++;
	if (kind == K_DIR)
		enqueue(c, de->ino, d->ino, path);
	else
		free(path);
}

/*
 * Read and check one pending directory.  "." must be the first entry and
 * ".." the second; whatever else holds their places is an entry like any
 * other.
 */
static void
check_dir(struct check *c, const struct pending *d)
{
	struct mfs_inode ip;
	struct mfs_dirent *de;
	struct entries e;
	size_t k;
	int have, r;

	(void)memset(&e, 0, sizeof(e));
	r = mfs_get_inode(&c->fs, d->ino, &ip);
	if (r == 0)
		r = mfs_readdir(&c->fs, &ip, collect_fn, &e);
	if (r < 0) {
		problem(c, 1, "%s: %s", d->path, strerror(-r));
		free(e.ent);
		return;
	}
	have = 0;
	for (k = 0; k < e.n; k++) {
		de = &e.ent[k];
		if (de->off == 0 && strcmp(de->name, ".") == 0) {
			check_dot(c, d, &ip, de, d->ino);
			have |= NEED_DOT;
		} else if (de->off == c->fs.dirent_size &&
		    strcmp(de->name, "..") == 0) {
			check_dot(c, d, &ip, de, d->parent);
			have |= NEED_DOTDOT;
		} else {
			check_entry(c, d, &ip, de);
		}
	}
	if (have != (NEED_DOT | NEED_DOTDOT))
		need_dots(c, d, ~have & (NEED_DOT | NEED_DOTDOT));
	free(e.ent);
}

/*
 * Take a zone that no file uses, fill it with zeros and store it in
 * *zonep.  Only after the walk is it known which zones are free.
 */
static int
alloc_zone(struct check *c, uint32_t *zonep)
{
	unsigned char *zero;
	uint32_t block, i, j, n;
	int r;

	n = c->fs.nzones - c->fs.firstdatazone;
	for (i = 0; i < n && test_bit(c->used, i); i++)
		continue;
	if (i == n)
		return -ENOSPC;
	if ((zero = calloc(1, c->fs.block_size)) == NULL)
		return -ENOMEM;
	block = (c->fs.firstdatazone + i) << c->fs.log_zone_size;
	r = 0;
	for (j = 0; j < 1U << c->fs.log_zone_size && r == 0; j++)
		r = mfs_write_block(&c->fs, block + j, zero);
	free(zero);
	if (r < 0)
		return r;
	set_bit(c->used, i, 1);
	set_bit(c->allocated, i, 1);
	c->zones_used++;
	*zonep = c->fs.firstdatazone + i;
	return 0;
}

/*
 * Make the directory *dp size bytes long, giving it direct zones where
 * the new bytes fall into a hole, and write it back.
 */
static int
grow_dir(struct check *c, struct mfs_inode *dp, uint32_t size)
{
	uint32_t block, fblock, slot;
	int r;

	if (size <= dp->size)
		return 0;
	for (fblock = dp->size / c->fs.block_size;
	    fblock <= (size - 1) / c->fs.block_size; fblock++) {
		if ((r = mfs_bmap(&c->fs, dp, fblock, &block)) < 0)
			return r;
		if (block != 0)
			continue;
		slot = fblock >> c->fs.log_zone_size;
		if (slot >= c->fs.ndzones)
			return -EFBIG;
		if ((r = alloc_zone(c, &dp->zone[slot])) < 0)
			return r;
	}
	dp->size = size;
	return mfs_put_inode(&c->fs, dp);
}

/*
 * Put name, naming ino, into entry k (0 or 1) of the directory f->ino.
 * An entry with another name in that place moves to a free entry, or to
 * a new one at the end.
 */
static int
put_dot(struct check *c, const struct dotfix *f, uint32_t k,
    const char *name, uint32_t ino)
{
	struct mfs_inode dp;
	struct mfs_dirent *old;
	struct entries e;
	uint32_t off, to;
	size_t i;
	int r;

	(void)memset(&e, 0, sizeof(e));
	off = k * c->fs.dirent_size;
	if ((r = mfs_get_inode(&c->fs, f->ino, &dp)) < 0 ||
	    (r = grow_dir(c, &dp, off + c->fs.dirent_size)) < 0 ||
	    (r = mfs_readdir(&c->fs, &dp, collect_fn, &e)) < 0)
		goto out;

	/* e is in the order of the offsets. */
	old = NULL;
	to = 2 * c->fs.dirent_size;
	for (i = 0; i < e.n; i++) {
		if (e.ent[i].off == off)
			old = &e.ent[i];
		if (e.ent[i].off == to)
			to += c->fs.dirent_size;
	}
	if (old != NULL && strcmp(old->name, name) != 0) {
		if (to >= dp.size &&
		    (r = grow_dir(c, &dp, to + c->fs.dirent_size)) < 0)
			goto out;
		r = mfs_put_entry(&c->fs, &dp, to, old->ino, old->name);
		if (r < 0)
			goto out;
	}
	r = mfs_put_entry(&c->fs, &dp, off, ino, name);
out:
	free(e.ent);
	return r;
}

/* Report the missing dots of one directory, and put them back. */
static void
fix_dots(struct check *c, const struct dotfix *f)
{
	static const struct {
		const char	*name;
		int		need;
	} dots[] = {
		{ ".",  NEED_DOT },
		{ "..", NEED_DOTDOT }
	};
	uint32_t ino, k;
	int r;

	for (k = 0; k < 2; k++) {
		if ((f->need & dots[k].need) == 0)
			continue;
		ino = k == 0 ? f->ino : f->parent;
		r = c->repair ? put_dot(c, f, k, dots[k].name, ino) : 0;
		problem(c, r, "%s: \"%s\" is not entry %" PRIu32, f->path,
		    dots[k].name, k + 1);
		if (c->repair && r == 0)
			c->refs[ino]++;
	}
}

/* Walk the tree from the root. */
static void
walk_tree(struct check *c)
{
	char *root;
	size_t i;

	if ((root = strdup("/")) == NULL)
		err(EXIT_CANNOT, NULL);
	set_bit(c->reached, MFS_ROOT_INO, 1);
	if (check_inode(c, MFS_ROOT_INO, root) != K_DIR) {
		problem(c, 1, "/: the root is not a directory");
		free(root);
		return;
	}
	enqueue(c, MFS_ROOT_INO, MFS_ROOT_INO, root);
	for (; c->head < c->nqueue; c->head++) {
		check_dir(c, &c->queue[c->head]);
		free(c->queue[c->head].path);
	}
	for (i = 0; i < c->nfixes; i++) {
		fix_dots(c, &c->fixes[i]);
		free(c->fixes[i].path);
	}
}

/* Under -y, set a bit of a map; returns 0 as problem() wants. */
static int
fix_bit(struct check *c, unsigned char *map, uint32_t n, int v, int *dirty)
{
	if (!c->repair)
		return 0;
	mfs_set_map_bit(&c->fs, map, n, v);
	*dirty = 1;
	return 0;
}

/* Link counts and inodes that no directory names. */
static void
check_links(struct check *c, uint32_t ino, struct mfs_inode *ip)
{
	uint32_t nlinks;
	uint16_t mode;
	int r;

	if (!test_bit(c->reached, ino)) {
		if (ip->mode == 0)
			return;
		mode = ip->mode;
		if (c->repair) {
			(void)memset(ip, 0, sizeof(*ip));
			ip->num = ino;
		}
		problem(c, put_inode(c, ip), "inode %" PRIu32 " is in use "
		    "(mode %o) but no directory names it", ino, (unsigned)mode);
		return;
	}
	if (ip->nlinks == c->refs[ino])
		return;
	nlinks = ip->nlinks;
	if (c->fs.version == 1 && c->refs[ino] > V1_MAX_LINKS) {
		problem(c, 1, "inode %" PRIu32 ": link count %" PRIu32 ", but %"
		    PRIu32 " entries", ino, nlinks, c->refs[ino]);
		return;
	}
	ip->nlinks = (uint16_t)c->refs[ino];
	r = put_inode(c, ip);
	problem(c, r, "inode %" PRIu32 ": link count %" PRIu32 ", but %"
	    PRIu32 " entries", ino, nlinks, c->refs[ino]);
}

/* Bit 0 of a map stands for no inode or zone, and must be set. */
static void
check_bit0(struct check *c, unsigned char *map, const char *name,
    int *dirty)
{
	if (!mfs_map_bit(&c->fs, map, 0))
		problem(c, fix_bit(c, map, 0, 1, dirty), "bit 0 of the %s map "
		    "is clear", name);
}

/* Link counts, inodes that no directory names, and the inode map. */
static void
check_inodes(struct check *c, unsigned char *imap, int *dirty)
{
	struct mfs_inode ip;
	uint32_t ino;
	int inmap, inuse;

	check_bit0(c, imap, "inode", dirty);
	for (ino = 1; ino <= c->fs.ninodes; ino++) {
		if (mfs_get_inode(&c->fs, ino, &ip) < 0) {
			problem(c, 1, "inode %" PRIu32 " cannot be read", ino);
			continue;
		}
		check_links(c, ino, &ip);
		inuse = ip.mode != 0;
		inmap = mfs_map_bit(&c->fs, imap, ino);
		if (inuse && !inmap)
			problem(c, fix_bit(c, imap, ino, 1, dirty), "inode %"
			    PRIu32 " is in use but free in the inode map", ino);
		else if (!inuse && inmap)
			problem(c, fix_bit(c, imap, ino, 0, dirty), "inode %"
			    PRIu32 " is free but marked in the inode map", ino);
	}
}

/* Zones in use against the zone map. */
static void
check_zones(struct check *c, unsigned char *zmap, int *dirty)
{
	uint32_t i, n, zone;
	int inmap, used;

	check_bit0(c, zmap, "zone", dirty);
	n = c->fs.nzones - c->fs.firstdatazone;
	for (i = 0; i < n; i++) {
		used = test_bit(c->used, i);
		inmap = mfs_map_bit(&c->fs, zmap, i + 1);
		zone = c->fs.firstdatazone + i;
		/* A zone that a repair took is not a problem of its own. */
		if (test_bit(c->allocated, i))
			(void)fix_bit(c, zmap, i + 1, 1, dirty);
		else if (used && !inmap)
			problem(c, fix_bit(c, zmap, i + 1, 1, dirty), "zone %"
			    PRIu32 " is in use but free in the zone map", zone);
		else if (!used && inmap)
			problem(c, fix_bit(c, zmap, i + 1, 0, dirty), "zone %"
			    PRIu32 " is free but marked in the zone map", zone);
	}
}

/* Check both maps, and write back a map that was repaired. */
static void
check_maps(struct check *c)
{
	unsigned char *imap, *zmap;
	int dirty, r;

	if ((r = mfs_load_map(&c->fs, MFS_IMAP, &imap)) < 0 ||
	    (r = mfs_load_map(&c->fs, MFS_ZMAP, &zmap)) < 0)
		errx(EXIT_CANNOT, "%s: bit maps: %s", c->image, strerror(-r));
	dirty = 0;
	check_inodes(c, imap, &dirty);
	if (dirty && (r = mfs_store_map(&c->fs, MFS_IMAP, imap)) < 0)
		problem(c, r, "the inode map cannot be written");
	dirty = 0;
	check_zones(c, zmap, &dirty);
	if (dirty && (r = mfs_store_map(&c->fs, MFS_ZMAP, zmap)) < 0)
		problem(c, r, "the zone map cannot be written");
	free(imap);
	free(zmap);
}

/* Under -y, write the super block back; see problem() for the result. */
static int
put_super(struct check *c)
{
	if (!c->repair)
		return 0;
	return mfs_put_super(&c->fs);
}

/*
 * What the super block says beyond its layout, which mfs_open() has
 * checked.  MINIX leaves the state word of V1 and V2 zero, so only the
 * error bit that Linux sets is a problem.
 */
static void
check_super(struct check *c)
{
	struct mfs *fs;
	uint64_t need;
	uint32_t size;

	fs = &c->fs;
	if (fs->max_size == 0 || fs->max_size > MAX_SIZE) {
		size = fs->max_size;
		fs->max_size = mfs_max_size(fs->version, fs->log_zone_size);
		problem(c, put_super(c), "the super block gives a maximum file "
		    "size of %" PRIu32, size);
	}
	need = (uint64_t)fs->nblocks * fs->block_size;
	if ((uint64_t)fs->image_size < need)
		problem(c, 1, "the image holds %jd bytes of the %" PRIu64
		    " that the file system needs", (intmax_t)fs->image_size,
		    need);
	if (fs->version != 3 && (fs->state & MFS_STATE_ERROR) != 0) {
		/* Whether it stays is decided at the end. */
		if (c->repair)
			fs->state &= (uint16_t)~MFS_STATE_ERROR;
		problem(c, put_super(c), "the super block records errors");
	}
	if (!c->quiet && !mfs_is_clean(fs) &&
	    (fs->version == 3 || (fs->state & MFS_STATE_ERROR) == 0))
		(void)printf("%s: the file system is not marked clean\n",
		    c->image);
}

/* Check the image once; exit with status 3 if it cannot be. */
static void
check(struct check *c, const char *image, int repair, int quiet)
{
	int r;

	(void)memset(c, 0, sizeof(*c));
	c->image = image;
	c->repair = repair;
	c->quiet = quiet;
	if (repair)
		r = mfs_open_rw(&c->fs, image);
	else
		r = mfs_open(&c->fs, image);
	if (r == -EINVAL)
		errx(EXIT_CANNOT, "%s: not a MINIX file system", image);
	if (r == -ENOTSUP)
		errx(EXIT_CANNOT, "%s: needs features that fsck_minixfs does "
		    "not know", image);
	if (r < 0)
		errx(EXIT_CANNOT, "%s: %s", image, strerror(-r));
	c->refs = calloc((size_t)c->fs.ninodes + 1, sizeof(*c->refs));
	if (c->refs == NULL)
		err(EXIT_CANNOT, NULL);
	c->reached = new_bits((uint64_t)c->fs.ninodes + 1);
	c->used = new_bits(c->fs.nzones);
	c->allocated = new_bits(c->fs.nzones);

	check_super(c);
	walk_tree(c);
	check_maps(c);

	free(c->queue);
	free(c->fixes);
	free(c->allocated);
	free(c->refs);
	free(c->reached);
	free(c->used);
	mfs_close(&c->fs);
}

static void
summary(const struct check *c, const char *when)
{
	(void)printf("%s: %" PRIu32 " of %" PRIu32 " inodes and %" PRIu32
	    " of %" PRIu32 " zones in use, %lu problems%s\n", c->image,
	    c->inodes_used, c->fs.ninodes, c->zones_used,
	    c->fs.nzones - c->fs.firstdatazone, c->problems, when);
}

/* Under -y, mark the file system clean, or as having errors. */
static void
mark(const char *image, int clean)
{
	struct mfs fs;
	uint16_t state;
	int r;

	if ((r = mfs_open_rw(&fs, image)) < 0)
		errx(EXIT_CANNOT, "%s: %s", image, strerror(-r));
	state = fs.state;
	if ((r = mfs_mark_clean(&fs, clean)) < 0)
		errx(EXIT_CANNOT, "%s: super block: %s", image, strerror(-r));
	if (fs.state != state)
		(void)printf("%s: marked %s\n", image,
		    clean ? "clean" : "as having errors");
	mfs_close(&fs);
}

int
main(int argc, char **argv)
{
	struct check c;
	int ch, repair;

	repair = 0;
	while ((ch = getopt(argc, argv, "y")) != -1) {
		switch (ch) {
		case 'y':
			repair = 1;
			break;
		default:
			usage();
		}
	}
	if (argc - optind != 1)
		usage();

	check(&c, argv[optind], repair, 0);
	summary(&c, "");
	if (!repair)
		return c.problems == 0 ? 0 : EXIT_PROBLEMS;

	/* Check again what the repairs left. */
	if (c.problems != 0) {
		check(&c, argv[optind], 0, 1);
		summary(&c, " after the repairs");
	}
	mark(argv[optind], c.problems == 0);
	return c.problems == 0 ? 0 : EXIT_PROBLEMS;
}
