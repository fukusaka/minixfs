/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * fsck_minixfs - check a MINIX file system image, and repair it.
 *
 *	fsck_minixfs [-y] image
 *
 * The check walks the tree from the root and then compares what it found
 * with the inode table and the bit maps:
 *
 *	- directory entries: inode numbers in range, entries that name a
 *	  free inode or one of no valid type, empty names or names with
 *	  '/', "." and ".." first and pointing to the directory and its
 *	  parent, and directories that more than one directory lists;
 *	- inodes: a size the zones can reach, directories a whole number of
 *	  entries long, zone numbers inside the data area, and zones that
 *	  two files share;
 *	- link counts against the entries that name each inode, inodes in
 *	  use that no directory names, and both bit maps.
 *
 * Without -y the image is only read.  With -y each problem is repaired
 * where it can be: bad entries are removed, "." and ".." are pointed
 * where they belong, zone numbers outside the data area and the second
 * use of a zone are cleared, sizes are cut to what the zones reach and
 * to whole directory entries, link counts are set, inodes that no
 * directory names are freed, and the bit maps are made to match.  A
 * missing "." or ".." and a root that is not a directory are left.  The
 * image is then checked again.
 *
 * Each problem is printed on a line of its own, with "(repaired)" or
 * "(not repaired)" after it under -y.  Exit status: 0 if the file system
 * is consistent (after the repairs, under -y), 1 if problems remain, 2
 * for a usage error, 3 if the image cannot be checked at all.
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

/* The state of one check. */
struct check {
	struct mfs		fs;
	struct pending		*queue;		/* directories to read */
	struct mfs_inode	*cur;		/* whose zones are walked */
	const char		*image;
	const char		*path;		/* the name of *cur */
	uint32_t		*refs;		/* entries naming each inode */
	unsigned char		*reached;	/* bit per inode: named */
	unsigned char		*used;		/* bit per zone: in a file */
	size_t			head;		/* first pending directory */
	size_t			nqueue;
	size_t			maxqueue;
	unsigned long		problems;
	unsigned long		unrepaired;
	uint32_t		inodes_used;
	uint32_t		zones_used;
	int			repair;		/* -y */
	int			quiet;		/* report nothing but the sum */
	int			cur_dirty;	/* *cur needs writing back */
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

/*
 * Check the inode that path names, the first time the walk reaches it,
 * and repair its size and zones.
 */
static enum kind
check_inode(struct check *c, uint32_t ino, const char *path)
{
	struct mfs_inode ip;
	int r;

	if ((r = mfs_get_inode(&c->fs, ino, &ip)) < 0)
		return K_BAD;
	if (ip.mode == 0)
		return K_FREE;
	if (!valid_type(ip.mode))
		return K_BAD;
	c->inodes_used++;
	check_size(c, &ip, path);
	if (!mfs_is_dev(&ip)) {
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

/*
 * Check that entry k of a directory is name and names ino, and count the
 * link it makes.
 */
static void
check_dot(struct check *c, const struct pending *d,
    const struct mfs_inode *dp, const struct entries *e, size_t k,
    const char *name, uint32_t ino)
{
	if (e->n <= k || strcmp(e->ent[k].name, name) != 0) {
		problem(c, 1, "%s: \"%s\" is not entry %zu", d->path, name,
		    k + 1);
		return;
	}
	if (e->ent[k].ino != ino) {
		problem(c, set_entry(c, dp, &e->ent[k], ino), "%s: \"%s\" "
		    "names inode %" PRIu32 ", not %" PRIu32, d->path, name,
		    e->ent[k].ino, ino);
		if (!c->repair)
			return;
	}
	c->refs[ino]++;
}

/* Check one entry, after "." and "..", of a directory. */
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
		    kind == K_FREE ? "free" : "of no valid type");
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

/* Read and check one pending directory. */
static void
check_dir(struct check *c, const struct pending *d)
{
	struct mfs_inode ip;
	struct entries e;
	size_t k;
	int r;

	(void)memset(&e, 0, sizeof(e));
	r = mfs_get_inode(&c->fs, d->ino, &ip);
	if (r == 0)
		r = mfs_readdir(&c->fs, &ip, collect_fn, &e);
	if (r < 0) {
		problem(c, 1, "%s: %s", d->path, strerror(-r));
		free(e.ent);
		return;
	}
	check_dot(c, d, &ip, &e, 0, ".", d->ino);
	check_dot(c, d, &ip, &e, 1, "..", d->parent);
	for (k = 2; k < e.n; k++)
		check_entry(c, d, &ip, &e.ent[k]);
	free(e.ent);
}

/* Walk the tree from the root. */
static void
walk_tree(struct check *c)
{
	char *root;

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

/* Link counts, inodes that no directory names, and the inode map. */
static void
check_inodes(struct check *c, unsigned char *imap, int *dirty)
{
	struct mfs_inode ip;
	uint32_t ino;
	int inmap, inuse;

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

	n = c->fs.nzones - c->fs.firstdatazone;
	for (i = 0; i < n; i++) {
		used = test_bit(c->used, i);
		inmap = mfs_map_bit(&c->fs, zmap, i + 1);
		zone = c->fs.firstdatazone + i;
		if (used && !inmap)
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
	if (r < 0)
		errx(EXIT_CANNOT, "%s: %s", image, strerror(-r));
	c->refs = calloc((size_t)c->fs.ninodes + 1, sizeof(*c->refs));
	if (c->refs == NULL)
		err(EXIT_CANNOT, NULL);
	c->reached = new_bits((uint64_t)c->fs.ninodes + 1);
	c->used = new_bits(c->fs.nzones);

	walk_tree(c);
	check_maps(c);

	free(c->queue);
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
	if (!repair || c.problems == 0)
		return c.problems == 0 ? 0 : EXIT_PROBLEMS;

	/* Check again what the repairs left. */
	check(&c, argv[optind], 0, 1);
	summary(&c, " after the repairs");
	return c.problems == 0 ? 0 : EXIT_PROBLEMS;
}
