/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * fsck_minixfs - check a MINIX file system image.
 *
 *	fsck_minixfs image
 *
 * The image is only read.  The check walks the tree from the root and
 * then compares what it found with the inode table and the bit maps:
 *
 *	- directory entries: inode numbers in range, entries that name a
 *	  free inode, empty names or names with '/', "." and ".." first and
 *	  pointing to the directory and its parent, and directories that
 *	  more than one directory lists;
 *	- inodes: the file type, a size the zones can reach, directories a
 *	  whole number of entries long, zone numbers inside the data area,
 *	  and zones that two files share;
 *	- link counts against the entries that name each inode, inodes in
 *	  use that no directory names, and both bit maps.
 *
 * Each problem is printed on a line of its own.  Exit status: 0 if the
 * file system is consistent, 1 if problems were found, 2 for a usage
 * error, 3 if the image cannot be checked at all.
 */

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfs.h"

#define EXIT_PROBLEMS	1
#define EXIT_USAGE	2
#define EXIT_CANNOT	3

/* A directory waiting to be read. */
struct pending {
	char		*path;
	uint32_t	ino;
	uint32_t	parent;
};

/* The state of one check. */
struct check {
	struct mfs	fs;
	struct pending	*queue;		/* directories to read */
	const char	*image;
	uint32_t	*refs;		/* entries naming each inode */
	unsigned char	*reached;	/* bit per inode: named from the root */
	unsigned char	*used;		/* bit per zone: in some file */
	const char	*path;		/* the file whose zones are walked */
	size_t		head;		/* first pending directory */
	size_t		nqueue;
	size_t		maxqueue;
	unsigned long	problems;
	uint32_t	inodes_used;
	uint32_t	zones_used;
};

static void
usage(void)
{
	(void)fprintf(stderr, "usage: fsck_minixfs image\n");
	exit(EXIT_USAGE);
}

/* Report one problem. */
static void
problem(struct check *c, const char *fmt, ...)
{
	va_list ap;

	(void)printf("%s: ", c->image);
	va_start(ap, fmt);
	(void)vprintf(fmt, ap);
	va_end(ap);
	(void)putchar('\n');
	c->problems++;
}

static int
test_bit(const unsigned char *bits, uint32_t n)
{
	return (bits[n / 8] >> (n % 8)) & 1;
}

static void
set_bit(unsigned char *bits, uint32_t n)
{
	bits[n / 8] |= (unsigned char)(1 << (n % 8));
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

/* Note one zone of the file c->path. */
static int
zone_fn(uint32_t zone, int level, void *arg)
{
	struct check *c;
	uint32_t n;

	(void)level;
	c = arg;
	if (zone < c->fs.firstdatazone || zone >= c->fs.nzones) {
		problem(c, "%s: zone %" PRIu32 " is outside the data area",
		    c->path, zone);
		return 0;
	}
	n = zone - c->fs.firstdatazone;
	if (test_bit(c->used, n)) {
		problem(c, "%s: zone %" PRIu32 " is used by another file too",
		    c->path, zone);
		return 0;
	}
	set_bit(c->used, n);
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

/*
 * Check the inode that path names, the first time the walk reaches it.
 * Returns 1 if it is a directory to read, 0 otherwise.
 */
static int
check_inode(struct check *c, uint32_t ino, const char *path)
{
	struct mfs_inode ip;
	int r;

	if ((r = mfs_get_inode(&c->fs, ino, &ip)) < 0) {
		problem(c, "%s: inode %" PRIu32 ": %s", path, ino,
		    strerror(-r));
		return 0;
	}
	if (ip.mode == 0) {
		problem(c, "%s: names inode %" PRIu32 ", which is free", path,
		    ino);
		return 0;
	}
	c->inodes_used++;
	if (!valid_type(ip.mode)) {
		problem(c, "%s: inode %" PRIu32 " has no valid type (mode %o)",
		    path, ino, (unsigned)ip.mode);
		return 0;
	}
	if (ip.size > c->fs.max_file)
		problem(c, "%s: size %" PRIu32 " is more than the zones reach",
		    path, ip.size);
	if (mfs_is_dir(&ip) && ip.size % c->fs.dirent_size != 0)
		problem(c, "%s: directory size %" PRIu32 " is not a whole "
		    "number of entries", path, ip.size);
	if (!mfs_is_dev(&ip)) {
		c->path = path;
		if ((r = mfs_walk_zones(&c->fs, &ip, zone_fn, c)) < 0)
			problem(c, "%s: %s", path, strerror(-r));
	}
	return mfs_is_dir(&ip);
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

/* Check that entry k of a directory is name and names ino. */
static void
check_dot(struct check *c, const struct pending *d,
    const struct entries *e, size_t k, const char *name, uint32_t ino)
{
	if (e->n <= k || strcmp(e->ent[k].name, name) != 0)
		problem(c, "%s: \"%s\" is not entry %zu", d->path, name,
		    k + 1);
	else if (e->ent[k].ino != ino)
		problem(c, "%s: \"%s\" names inode %" PRIu32 ", not %" PRIu32,
		    d->path, name, e->ent[k].ino, ino);
}

/* Check one entry, after "." and "..", of a directory. */
static void
check_entry(struct check *c, const struct pending *d,
    const struct mfs_dirent *de)
{
	struct mfs_inode ip;
	char *path;

	path = join(d->path, de->name);
	if (de->name[0] == '\0' || strchr(de->name, '/') != NULL ||
	    strcmp(de->name, ".") == 0 || strcmp(de->name, "..") == 0)
		problem(c, "%s: bad name \"%s\"", d->path, de->name);
	if (!test_bit(c->reached, de->ino)) {
		set_bit(c->reached, de->ino);
		if (check_inode(c, de->ino, path)) {
			enqueue(c, de->ino, d->ino, path);
			return;
		}
	} else if (mfs_get_inode(&c->fs, de->ino, &ip) == 0 &&
	    mfs_is_dir(&ip)) {
		problem(c, "%s: directory inode %" PRIu32 " is listed in more "
		    "than one directory", path, de->ino);
	}
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
		problem(c, "%s: %s", d->path, strerror(-r));
		free(e.ent);
		return;
	}
	check_dot(c, d, &e, 0, ".", d->ino);
	check_dot(c, d, &e, 1, "..", d->parent);
	for (k = 0; k < e.n; k++) {
		if (e.ent[k].ino > c->fs.ninodes) {
			problem(c, "%s: entry \"%s\" names inode %" PRIu32
			    ", past the last", d->path, e.ent[k].name,
			    e.ent[k].ino);
			continue;
		}
		if (c->refs[e.ent[k].ino] < UINT32_MAX)
			c->refs[e.ent[k].ino]++;
		if (k >= 2)
			check_entry(c, d, &e.ent[k]);
	}
	free(e.ent);
}

/* Walk the tree from the root. */
static void
walk_tree(struct check *c)
{
	char *root;

	if ((root = strdup("/")) == NULL)
		err(EXIT_CANNOT, NULL);
	set_bit(c->reached, MFS_ROOT_INO);
	if (!check_inode(c, MFS_ROOT_INO, root)) {
		problem(c, "/: the root is not a directory");
		free(root);
		return;
	}
	enqueue(c, MFS_ROOT_INO, MFS_ROOT_INO, root);
	for (; c->head < c->nqueue; c->head++) {
		check_dir(c, &c->queue[c->head]);
		free(c->queue[c->head].path);
	}
}

/* Link counts, inodes that no directory names, and the inode map. */
static void
check_inodes(struct check *c, const unsigned char *imap)
{
	struct mfs_inode ip;
	uint32_t ino;
	int inmap, reached;

	for (ino = 1; ino <= c->fs.ninodes; ino++) {
		if (mfs_get_inode(&c->fs, ino, &ip) < 0) {
			problem(c, "inode %" PRIu32 " cannot be read", ino);
			continue;
		}
		reached = test_bit(c->reached, ino) && ip.mode != 0;
		inmap = mfs_map_bit(&c->fs, imap, ino);
		if (reached && ip.nlinks != c->refs[ino])
			problem(c, "inode %" PRIu32 ": link count %" PRIu16
			    ", but %" PRIu32 " entries", ino, ip.nlinks,
			    c->refs[ino]);
		if (!reached && ip.mode != 0)
			problem(c, "inode %" PRIu32 " is in use (mode %o) but "
			    "no directory names it", ino, (unsigned)ip.mode);
		if ((reached || ip.mode != 0) && !inmap)
			problem(c, "inode %" PRIu32 " is in use but free in "
			    "the inode map", ino);
		if (!reached && ip.mode == 0 && inmap)
			problem(c, "inode %" PRIu32 " is free but marked in "
			    "the inode map", ino);
	}
}

/* Zones in use against the zone map. */
static void
check_zones(struct check *c, const unsigned char *zmap)
{
	uint32_t i, n;
	int inmap, used;

	n = c->fs.nzones - c->fs.firstdatazone;
	for (i = 0; i < n; i++) {
		used = test_bit(c->used, i);
		inmap = mfs_map_bit(&c->fs, zmap, i + 1);
		if (used && !inmap)
			problem(c, "zone %" PRIu32 " is in use but free in the "
			    "zone map", c->fs.firstdatazone + i);
		else if (!used && inmap)
			problem(c, "zone %" PRIu32 " is free but marked in the "
			    "zone map", c->fs.firstdatazone + i);
	}
}

/* Open the image and allocate the state, or exit with status 3. */
static void
start(struct check *c, const char *image)
{
	int r;

	(void)memset(c, 0, sizeof(*c));
	c->image = image;
	if ((r = mfs_open(&c->fs, image)) < 0) {
		if (r == -EINVAL)
			errx(EXIT_CANNOT, "%s: not a MINIX file system", image);
		errx(EXIT_CANNOT, "%s: %s", image, strerror(-r));
	}
	c->refs = calloc((size_t)c->fs.ninodes + 1, sizeof(*c->refs));
	if (c->refs == NULL)
		err(EXIT_CANNOT, NULL);
	c->reached = new_bits((uint64_t)c->fs.ninodes + 1);
	c->used = new_bits(c->fs.nzones);
}

int
main(int argc, char **argv)
{
	unsigned char *imap, *zmap;
	struct check c;
	int r;

	if (argc != 2)
		usage();
	start(&c, argv[1]);
	walk_tree(&c);
	if ((r = mfs_load_map(&c.fs, MFS_IMAP, &imap)) < 0 ||
	    (r = mfs_load_map(&c.fs, MFS_ZMAP, &zmap)) < 0)
		errx(EXIT_CANNOT, "%s: bit maps: %s", c.image, strerror(-r));
	check_inodes(&c, imap);
	check_zones(&c, zmap);
	(void)printf("%s: %" PRIu32 " of %" PRIu32 " inodes and %" PRIu32
	    " of %" PRIu32 " zones in use, %lu problems\n", c.image,
	    c.inodes_used, c.fs.ninodes, c.zones_used,
	    c.fs.nzones - c.fs.firstdatazone, c.problems);
	free(imap);
	free(zmap);
	free(c.queue);
	free(c.refs);
	free(c.reached);
	free(c.used);
	mfs_close(&c.fs);
	return c.problems == 0 ? 0 : EXIT_PROBLEMS;
}
