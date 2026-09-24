/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * tunefs_minixfs - change the settings of a MINIX file system.
 *
 *	tunefs_minixfs [-fN] [-B le|be] [-c clean|dirty] [-e 0|1]
 *	    [-l 14|30] [-m minix|linux|bytes] [-s blocks]
 *	    [-T SIZE:HEADS:SIDE] image
 *
 * Without options, or with -N, the settings are printed and nothing is
 * written; -N shows what the other options would change.
 *
 *	-B	store every number in the other byte order: little-endian,
 *		as on the PC, or big-endian, as on the 68000 machines; not
 *		for Minix-vmd
 *	-c	mark the file system clean or dirty: in V1 and V2 in the
 *		state word that Linux keeps, in V3 in the flags of MINIX 3,
 *		which mounts a file system that is not clean read-only
 *	-f	change a file system that is not marked clean with -B, -l
 *		and -s all the same
 *	-e	set the bits of each map past the last inode or zone to 0,
 *		as the mkfs of MINIX leaves them, or 1, as that of Linux
 *	-l	names of 14 or 30 characters, in V1 and V2 but not
 *		Minix-vmd: every directory is written anew with entries
 *		of the new size.  Names too long for 14 are listed, and
 *		nothing is changed.
 *	-m	the maximum file size in the super block: what MINIX works
 *		out, what Linux and newfs_minixfs write, or a number
 *	-s	grow or shrink the file system and the image file to so
 *		many blocks.  To grow, if the zone map needs more blocks,
 *		the inode table and the data zones move up to make room;
 *		to shrink, the zones in use past the new end move to free
 *		zones before it, and the zone map keeps its blocks
 *	-T	an image that holds one side of a disk, as for minixfs(1);
 *		it cannot change size
 *
 * Each change is printed as the old and the new value.  The byte order
 * is changed first, then the name length, then the size.  Changes that
 * rewrite more than the super block and the maps work on a file system
 * that fsck_minixfs passes and that is not mounted, and one cut short
 * leaves it half changed: keep a copy.  Linux and MINIX 3 take the clean
 * mark away while they have a file system mounted for writing, and would
 * write their own idea of it back over the change, so -B, -l and -s
 * refuse a file system that is not marked clean, unless -f is given.
 * fsck_minixfs -y marks a file system clean that it finds consistent.
 * Exit status: 0 on success, 1 if anything failed, 2 for a usage error.
 */

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mfs.h"

#define MAX_SIZE	0x7fffffff	/* s_max_size is signed in MINIX */

/* What the command line asks for. */
struct options {
	struct mfs_tracks tracks;	/* -T */
	const char	*image;
	const char	*max;		/* -m, or NULL */
	int		order;		/* -B: an mfs_order, or -1 as is */
	uint32_t	namelen;	/* -l: 14 or 30, or 0 as is */
	uint32_t	nblocks;	/* -s, or 0 as is */
	int		clean;		/* -c: 1 clean, 0 dirty, -1 as is */
	int		end;		/* -e: 0 or 1, or -1 as is */
	int		dry_run;	/* -N */
	int		force;		/* -f */
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: tunefs_minixfs [-fN] [-B le|be] [-c clean|dirty] [-e 0|1]\n"
	    "           [-l 14|30] [-m minix|linux|bytes] [-s blocks]\n"
	    "           [-T SIZE:HEADS:SIDE] image\n");
	exit(2);
}

/* The size that -s asks for. */
static uint32_t
blocks(const char *s)
{
	unsigned long v;
	char *end;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || *end != '\0' || end == s || s[0] == '-' || v == 0 ||
	    v > UINT32_MAX)
		errx(2, "%s: bad size", s);
	return (uint32_t)v;
}

static void
parse(int argc, char **argv, struct options *o)
{
	int ch;

	(void)memset(o, 0, sizeof(*o));
	o->clean = -1;
	o->end = -1;
	o->order = -1;
	while ((ch = getopt(argc, argv, "B:c:e:fl:m:Ns:T:")) != -1) {
		switch (ch) {
		case 'B':
			if (strcmp(optarg, "le") == 0)
				o->order = MFS_LITTLE_ENDIAN;
			else if (strcmp(optarg, "be") == 0)
				o->order = MFS_BIG_ENDIAN;
			else
				usage();
			break;
		case 'c':
			if (strcmp(optarg, "clean") == 0)
				o->clean = 1;
			else if (strcmp(optarg, "dirty") == 0)
				o->clean = 0;
			else
				usage();
			break;
		case 'e':
			if (strcmp(optarg, "0") == 0)
				o->end = 0;
			else if (strcmp(optarg, "1") == 0)
				o->end = 1;
			else
				usage();
			break;
		case 'f':
			o->force = 1;
			break;
		case 'l':
			if (strcmp(optarg, "14") == 0)
				o->namelen = 14;
			else if (strcmp(optarg, "30") == 0)
				o->namelen = 30;
			else
				usage();
			break;
		case 'm':
			o->max = optarg;
			break;
		case 'N':
			o->dry_run = 1;
			break;
		case 's':
			o->nblocks = blocks(optarg);
			break;
		case 'T':
			if (mfs_parse_tracks(optarg, &o->tracks) < 0)
				usage();
			break;
		default:
			usage();
		}
	}
	if (argc - optind != 1)
		usage();
	o->image = argv[optind];
}

/* Whether any option asks for a change. */
static int
changes(const struct options *o)
{
	return o->order != -1 || o->namelen != 0 || o->nblocks != 0 ||
	    o->clean != -1 || o->end != -1 || o->max != NULL;
}

/* The maximum file size that -m asks for. */
static uint32_t
max_size(const struct mfs *fs, const char *s)
{
	unsigned long v;
	char *end;

	if (strcmp(s, "minix") == 0)
		return mfs_minix_max_size(fs);
	if (strcmp(s, "linux") == 0)
		return mfs_max_size(fs->version, fs->log_zone_size);
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || *end != '\0' || end == s || s[0] == '-' || v == 0 ||
	    v > MAX_SIZE)
		errx(2, "%s: give minix, linux or a size from 1 to %d", s,
		    MAX_SIZE);
	return (uint32_t)v;
}

/* The state word with the file system marked clean or dirty. */
static uint16_t
clean_state(const struct mfs *fs, int clean)
{
	uint16_t s;

	s = fs->state;
	if (fs->vmd)
		return clean ? s | MFS_VMD_CLEAN :
		    s & (uint16_t)~MFS_VMD_CLEAN;
	if (fs->version == 3)
		return clean ? s | MFS_FLAG_CLEAN :
		    s & (uint16_t)~MFS_FLAG_CLEAN;
	if (clean)
		return (s | MFS_STATE_VALID) & (uint16_t)~MFS_STATE_ERROR;
	return s & (uint16_t)~MFS_STATE_VALID;
}

/* The bits of a map from bit first to the end of its blocks. */
struct map_end {
	unsigned char	*map;
	enum mfs_map	which;
	uint64_t	first;
	uint64_t	nbits;
};

static int
load_end(struct mfs *fs, enum mfs_map which, struct map_end *m)
{
	uint32_t blocks;
	int r;

	m->which = which;
	if (which == MFS_IMAP) {
		blocks = fs->imap_blocks;
		m->first = (uint64_t)fs->ninodes + 1;
	} else {
		blocks = fs->zmap_blocks;
		m->first = (uint64_t)fs->nzones - fs->firstdatazone + 1;
	}
	m->nbits = (uint64_t)blocks * fs->block_size * 8;
	if ((r = mfs_load_map(fs, which, &m->map)) < 0)
		return r;
	return 0;
}

/* "0", "1" or "mixed" for the bits past the end of both maps. */
static const char *
end_value(const struct mfs *fs, const struct map_end *m, int n)
{
	uint64_t bit, ones, zeros;
	int i;

	ones = zeros = 0;
	for (i = 0; i < n; i++) {
		for (bit = m[i].first; bit < m[i].nbits; bit++) {
			if (mfs_map_bit(fs, m[i].map, (uint32_t)bit))
				ones++;
			else
				zeros++;
		}
	}
	if (ones == 0 && zeros == 0)
		return "none";
	if (zeros == 0)
		return "1";
	if (ones == 0)
		return "0";
	return "mixed";
}

/* A directory waiting to be searched for long names. */
struct pending {
	uint32_t	ino;
	char		*path;
};

struct long_names {
	struct pending	*queue;
	size_t		n;
	size_t		max;
	const char	*dir;		/* the path of the one searched */
	uint32_t	namelen;
	unsigned long	found;
	struct mfs	*fs;
	unsigned char	*seen;		/* bit per inode: queued */
};

static char *
join(const char *dir, const char *name)
{
	char *s;

	if ((s = malloc(strlen(dir) + strlen(name) + 2)) == NULL)
		err(1, NULL);
	(void)sprintf(s, "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/",
	    name);
	return s;
}

static int
long_fn(const struct mfs_dirent *de, void *arg)
{
	struct long_names *l;
	struct mfs_inode ip;
	struct pending *q;

	l = arg;
	if (strcmp(de->name, ".") == 0 || strcmp(de->name, "..") == 0)
		return 0;
	if (strlen(de->name) > l->namelen) {
		(void)printf("name longer than %" PRIu32
		    " characters: %s%s%s\n", l->namelen, l->dir,
		    strcmp(l->dir, "/") == 0 ? "" : "/", de->name);
		l->found++;
	}
	if (de->ino == 0 || de->ino > l->fs->ninodes ||
	    (l->seen[de->ino / 8] >> (de->ino % 8) & 1) != 0 ||
	    mfs_get_inode(l->fs, de->ino, &ip) < 0 || !mfs_is_dir(&ip))
		return 0;
	l->seen[de->ino / 8] |= (unsigned char)(1 << (de->ino % 8));
	if (l->n == l->max) {
		l->max = l->max == 0 ? 64 : l->max * 2;
		if ((q = realloc(l->queue, l->max * sizeof(*q))) == NULL)
			err(1, NULL);
		l->queue = q;
	}
	l->queue[l->n].ino = de->ino;
	l->queue[l->n++].path = join(l->dir, de->name);
	return 0;
}

/* Print every name longer than namelen, and return how many. */
static unsigned long
long_names(struct mfs *fs, uint32_t namelen)
{
	struct long_names l;
	struct mfs_inode ip;
	size_t i;

	(void)memset(&l, 0, sizeof(l));
	l.fs = fs;
	l.namelen = namelen;
	if ((l.seen = calloc((size_t)fs->ninodes / 8 + 1, 1)) == NULL)
		err(1, NULL);
	l.seen[MFS_ROOT_INO / 8] |= 1 << MFS_ROOT_INO % 8;
	if ((l.queue = malloc(sizeof(*l.queue))) == NULL)
		err(1, NULL);
	l.max = 1;
	l.queue[l.n].ino = MFS_ROOT_INO;
	if ((l.queue[l.n++].path = strdup("/")) == NULL)
		err(1, NULL);
	for (i = 0; i < l.n; i++) {
		l.dir = l.queue[i].path;
		if (mfs_get_inode(fs, l.queue[i].ino, &ip) == 0)
			(void)mfs_readdir(fs, &ip, long_fn, &l);
	}
	for (i = 0; i < l.n; i++)
		free(l.queue[i].path);
	free(l.queue);
	free(l.seen);
	return l.found;
}

static const char *
order_name(int order)
{
	return order == MFS_BIG_ENDIAN ? "big-endian" : "little-endian";
}

static const char *
clean_value(const struct mfs *fs, uint16_t state)
{
	struct mfs t;

	t = *fs;
	t.state = state;
	return mfs_is_clean(&t) ? "clean" : "dirty";
}

/* -s: grow or shrink the file system, or say why not. */
static void
resize(struct mfs *fs, const struct options *o, int write)
{
	uint32_t newz;
	int r, shrink;

	newz = o->nblocks >> fs->log_zone_size;
	if (o->tracks.size != 0)
		errx(1, "%s: an image of one side of a disk cannot change "
		    "size", o->image);
	shrink = newz < fs->nzones;
	(void)printf("blocks: %" PRIu32 " -> %" PRIu32 "\n", fs->nblocks,
	    newz << fs->log_zone_size);
	if (!write)
		return;
	r = shrink ? mfs_shrink(fs, o->nblocks) : mfs_grow(fs, o->nblocks);
	switch (r) {
	case 0:
		return;
	case -EFBIG:
		errx(1, "%s: too many zones for V%d", o->image, fs->version);
	case -ENOSPC:
		if (shrink)
			errx(1, "%s: what is in use does not fit in %" PRIu32
			    " blocks; nothing changed", o->image,
			    o->nblocks);
		errx(1, "%s: the zone map needs more blocks, and the zones "
		    "that move up to make room need a larger size", o->image);
	default:
		errx(1, "%s: size: %s", o->image, strerror(-r));
	}
}

int
main(int argc, char **argv)
{
	struct map_end maps[2];
	struct options o;
	struct mfs fs;
	uint32_t max;
	uint16_t state;
	uint64_t bit;
	const char *old;
	int i, r, status, write;

	parse(argc, argv, &o);
	write = changes(&o) && !o.dry_run;
	if ((r = mfs_open_tracks(&fs, o.image, write, &o.tracks)) < 0) {
		if (r == -EINVAL)
			errx(1, "%s: not a MINIX file system", o.image);
		errx(1, "%s: %s", o.image, strerror(-r));
	}
	status = 0;
	if (fs.vmd && o.order != -1)
		errx(1, "%s: -B does not know the super block and flex "
		    "directories of Minix-vmd", o.image);
	if (fs.vmd && o.namelen != 0)
		errx(1, "%s: -l does not apply to Minix-vmd, whose flex "
		    "directories take names of up to 60 characters", o.image);
	/* What rewrites more than the super block wants it unmounted. */
	if (write && !o.force && !mfs_is_clean(&fs) &&
	    (o.order != -1 || o.namelen != 0 || o.nblocks != 0))
		errx(1, "%s: not marked clean, so it may be mounted; unmount "
		    "it and run fsck_minixfs -y, or give -f", o.image);
	if (o.order != -1) {
		(void)printf("byte order: %s -> %s\n", order_name(fs.order),
		    order_name(o.order));
		if (write && (r = mfs_convert_order(&fs, o.order)) < 0)
			errx(1, "%s: byte order: %s", o.image, strerror(-r));
	}
	if (o.namelen != 0) {
		if (fs.version == 3)
			errx(1, "%s: V3 names are always 60 characters",
			    o.image);
		(void)printf("name length: %" PRIu32 " -> %" PRIu32 "\n",
		    fs.namelen, o.namelen);
		if (long_names(&fs, o.namelen) > 0)
			errx(1, "%s: names too long; nothing changed", o.image);
		if (write && (r = mfs_change_namelen(&fs, o.namelen)) < 0)
			errx(1, "%s: name length: %s", o.image, strerror(-r));
	}
	if (o.nblocks != 0)
		resize(&fs, &o, write);
	for (i = 0; i < 2; i++) {
		if ((r = load_end(&fs, i == 0 ? MFS_IMAP : MFS_ZMAP,
		    &maps[i])) < 0)
			errx(1, "%s: bit maps: %s", o.image, strerror(-r));
	}

	if (!changes(&o)) {
		(void)printf("byte order: %s\n", order_name(fs.order));
		(void)printf("name length: %" PRIu32 "\n", fs.namelen);
		(void)printf("blocks: %" PRIu32 "\n", fs.nblocks);
		(void)printf("state: %s\n", clean_value(&fs, fs.state));
		(void)printf("max file size: %" PRIu32 " (MINIX works out %"
		    PRIu32 ")\n", fs.max_size, mfs_minix_max_size(&fs));
		(void)printf("bits past the end of the maps: %s\n",
		    end_value(&fs, maps, 2));
	}

	if (o.clean != -1) {
		state = clean_state(&fs, o.clean);
		(void)printf("state: %s -> %s\n", clean_value(&fs, fs.state),
		    clean_value(&fs, state));
		fs.state = state;
	}
	if (o.max != NULL) {
		max = max_size(&fs, o.max);
		(void)printf("max file size: %" PRIu32 " -> %" PRIu32 "\n",
		    fs.max_size, max);
		fs.max_size = max;
	}
	if (o.end != -1) {
		old = end_value(&fs, maps, 2);
		for (i = 0; i < 2; i++)
			for (bit = maps[i].first; bit < maps[i].nbits; bit++)
				mfs_set_map_bit(&fs, maps[i].map,
				    (uint32_t)bit, o.end);
		(void)printf("bits past the end of the maps: %s -> %s\n", old,
		    end_value(&fs, maps, 2));
	}

	if (write) {
		if ((o.clean != -1 || o.max != NULL) &&
		    (r = mfs_put_super(&fs)) < 0) {
			warnx("%s: super block: %s", o.image, strerror(-r));
			status = 1;
		}
		for (i = 0; i < 2 && o.end != -1; i++) {
			if ((r = mfs_store_map(&fs, maps[i].which,
			    maps[i].map)) < 0) {
				warnx("%s: bit maps: %s", o.image,
				    strerror(-r));
				status = 1;
			}
		}
	}
	for (i = 0; i < 2; i++)
		free(maps[i].map);
	mfs_close(&fs);
	return status;
}
