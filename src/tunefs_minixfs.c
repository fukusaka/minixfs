/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * tunefs_minixfs - change the settings of a MINIX file system.
 *
 *	tunefs_minixfs [-fN] [-B le|be] [-c clean|dirty] [-e 0|1]
 *	    [-l 14|30] [-m minix|linux|bytes]
 *	    [-M SIZE:HEADS:SIDE] [-s blocks [-k]] [-W [8|16|32|64:]8|16|32|64]
 *	    image
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
 *		as the mkfs of MINIX 2 and later leaves them, or 1, as
 *		those of MINIX 1 and Linux set them
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
 *	-k	with -s, keep the size of the image file, which then has to
 *		hold the new size; a device always keeps its size
 *	-M	an image that holds one side of a disk, as for minixfs(1);
 *		it cannot change size
 *	-W	the bits in a word of the bit maps of a big-endian file
 *		system: read them as words of the first number, or as the
 *		version has them, and write them anew as words of the
 *		second; with -B be, write them so
 *
 * Each change is printed as the old and the new value.  The byte order
 * is changed first, then the words of the bit maps, then the name length,
 * then the size.  Changes that
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
	struct mfs_tracks tracks;	/* -M */
	const char	*image;
	const char	*max;		/* -m, or NULL */
	int		order;		/* -B: an mfs_order, or -1 as is */
	uint32_t	namelen;	/* -l: 14 or 30, or 0 as is */
	uint32_t	nblocks;	/* -s, or 0 as is */
	int		clean;		/* -c: 1 clean, 0 dirty, -1 as is */
	int		end;		/* -e: 0 or 1, or -1 as is */
	int		dry_run;	/* -N */
	int		force;		/* -f */
	int		keep;		/* -k */
	uint32_t	word_old;	/* -W: bytes of a map word as read */
	uint32_t	word_new;	/* and as written; 0 as is */
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: tunefs_minixfs [-fN] [-B le|be] [-c clean|dirty] [-e 0|1]\n"
	    "           [-l 14|30] [-m minix|linux|bytes]\n"
	    "           [-M SIZE:HEADS:SIDE] [-s blocks [-k]]\n"
	    "           [-W [8|16|32|64:]8|16|32|64] image\n");
	exit(2);
}

/* -W: "new", or "old:new". */
static void
map_words(char *s, struct options *o)
{
	char *colon;

	if ((colon = strchr(s, ':')) != NULL) {
		*colon = '\0';
		if (mfs_parse_map_word(s, &o->word_old) < 0)
			usage();
		s = colon + 1;
	}
	if (mfs_parse_map_word(s, &o->word_new) < 0)
		usage();
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
	while ((ch = getopt(argc, argv, "B:c:e:fkl:m:M:Ns:W:")) != -1) {
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
		case 'k':
			o->keep = 1;
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
		case 'M':
			if (mfs_parse_tracks(optarg, &o->tracks) < 0)
				usage();
			break;
		case 'N':
			o->dry_run = 1;
			break;
		case 's':
			o->nblocks = blocks(optarg);
			break;
		case 'W':
			map_words(optarg, o);
			break;
		default:
			usage();
		}
	}
	if (argc - optind != 1 || (o->keep && o->nblocks == 0))
		usage();
	o->image = argv[optind];
}

/* Whether any option asks for a change. */
static int
changes(const struct options *o)
{
	return o->order != -1 || o->namelen != 0 || o->nblocks != 0 ||
	    o->clean != -1 || o->end != -1 || o->max != NULL ||
	    o->word_new != 0;
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
		return mfs_linux_max_size(fs->version, fs->log_zone_size);
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

/*
 * Why -s cannot be done, from what mfs_grow() or mfs_shrink() returned,
 * and what was changed before it; exits.
 */
static void
resize_error(const struct mfs *fs, const struct options *o, int r,
    const char *done)
{
	uint64_t size;
	uint32_t newz;

	newz = o->nblocks >> fs->log_zone_size;
	size = ((uint64_t)newz << fs->log_zone_size) * fs->block_size;
	switch (r) {
	case -EFBIG:
		errx(1, "%s: too many zones for V%d; %s", o->image,
		    fs->version, done);
	case -ENOSPC:
		if (newz < fs->nzones)
			errx(1, "%s: what is in use does not fit in %" PRIu32
			    " blocks; %s", o->image, o->nblocks, done);
		errx(1, "%s: the zone map needs more blocks, and the zones "
		    "that move up to make room need a larger size; %s",
		    o->image, done);
	case -ENXIO:
		if (fs->image_size < 0)
			errx(1, "%s: the size of the device is not known; %s",
			    o->image, done);
		errx(1, "%s: the image holds %jd bytes, fewer than the %"
		    PRIu64 " asked for; %s", o->image,
		    (intmax_t)fs->image_size, size, done);
	default:
		errx(1, "%s: size: %s; %s", o->image, strerror(-r), done);
	}
}

/* -s: grow or shrink the file system, or only find out whether it can. */
static void
resize(struct mfs *fs, const struct options *o, int flags, const char *done)
{
	uint32_t newz;
	int r;

	newz = o->nblocks >> fs->log_zone_size;
	if (o->keep)
		flags |= MFS_RESIZE_KEEP;
	r = newz < fs->nzones ? mfs_shrink(fs, o->nblocks, flags) :
	    mfs_grow(fs, o->nblocks, flags);
	if (r < 0)
		resize_error(fs, o, r, done);
}

/*
 * The largest file of the file system, for -m, which may not go below
 * it; its inode goes to *ino.  Free inodes, of mode 0, and devices,
 * whose size is no file, are passed over.
 */
static uint32_t
largest_file(struct mfs *fs, const struct options *o, uint32_t *ino)
{
	struct mfs_inode ip;
	uint32_t i, max;
	int r;

	max = 0;
	*ino = 0;
	for (i = 1; i <= fs->ninodes; i++) {
		if ((r = mfs_get_inode(fs, i, &ip)) < 0)
			errx(1, "%s: inode %" PRIu32 ": %s; nothing changed",
			    o->image, i, strerror(-r));
		if (ip.mode == 0 || mfs_is_dev(&ip) || ip.size <= max)
			continue;
		max = ip.size;
		*ino = i;
	}
	return max;
}

/*
 * Whatever of the options can be refused is refused before anything is
 * written, so that nothing changes then.  Only what an earlier change
 * alters escapes: -s after -l, whose directories take other zones.
 */
static void
check_all(struct mfs *fs, const struct options *o, int write)
{
	uint32_t big, ino, max;
	int r;

	if (fs->vmd && o->order != -1)
		errx(1, "%s: -B does not know the super block and flex "
		    "directories of Minix-vmd", o->image);
	if (fs->vmd && o->namelen != 0)
		errx(1, "%s: -l does not apply to Minix-vmd, whose flex "
		    "directories take names of up to 60 characters", o->image);
	/* What rewrites more than the super block wants it unmounted. */
	if (write && !o->force && !mfs_is_clean(fs) &&
	    (o->order != -1 || o->namelen != 0 || o->nblocks != 0))
		errx(1, "%s: not marked clean, so it may be mounted; unmount "
		    "it and run fsck_minixfs -y, or give -f", o->image);
	if (o->namelen != 0) {
		if (fs->version == 3)
			errx(1, "%s: V3 names are always 60 characters",
			    o->image);
		if (long_names(fs, o->namelen) > 0)
			errx(1, "%s: names too long; nothing changed", o->image);
		if ((r = mfs_change_namelen(fs, o->namelen, 1)) < 0)
			errx(1, "%s: name length: %s; nothing changed",
			    o->image, strerror(-r));
	}
	if (o->max != NULL) {
		max = max_size(fs, o->max);
		if ((big = largest_file(fs, o, &ino)) > max)
			errx(1, "%s: inode %" PRIu32 " holds %" PRIu32 " bytes, "
			    "more than %" PRIu32 "; nothing changed", o->image,
			    ino, big, max);
	}
	if (o->nblocks != 0) {
		if (o->tracks.size != 0)
			errx(1, "%s: an image of one side of a disk cannot "
			    "change size", o->image);
		resize(fs, o, MFS_RESIZE_CHECK, "nothing changed");
	}
}

int
main(int argc, char **argv)
{
	struct map_end maps[2];
	struct options o;
	struct mfs fs;
	uint32_t len, max, words;
	uint16_t state;
	uint64_t bit;
	const char *done, *from, *old;
	int i, r, status, write;

	parse(argc, argv, &o);
	write = changes(&o) && !o.dry_run;
	if ((r = mfs_open_tracks(&fs, o.image, write, &o.tracks)) < 0) {
		if (r == -EINVAL)
			errx(1, "%s: not a MINIX file system", o.image);
		errx(1, "%s: %s", o.image, strerror(-r));
	}
	if (o.word_old != 0)
		mfs_set_map_word(&fs, o.word_old);
	status = 0;
	check_all(&fs, &o, write);
	done = "nothing changed";
	words = 0;
	if (o.order != -1) {
		from = order_name(fs.order);
		/* Big-endian, the maps are written as the words they get. */
		if (o.order == MFS_BIG_ENDIAN && fs.order != MFS_BIG_ENDIAN)
			mfs_set_map_word(&fs, o.word_new != 0 ? o.word_new :
			    mfs_default_map_word(fs.version, fs.namelen));
		if (write && (r = mfs_convert_order(&fs, o.order)) < 0)
			errx(1, "%s: byte order: %s", o.image, strerror(-r));
		(void)printf("byte order: %s -> %s\n", from,
		    order_name(o.order));
		done = "the byte order was changed, nothing else";
	}
	if (o.word_new != 0 && fs.order == MFS_BIG_ENDIAN &&
	    o.word_new != fs.map_word) {
		words = fs.map_word;
		if (write && (r = mfs_convert_map_word(&fs, o.word_new)) < 0)
			errx(1, "%s: bit maps: %s", o.image, strerror(-r));
		(void)printf("bit map words: %" PRIu32 " -> %" PRIu32
		    " bits\n", words * 8, o.word_new * 8);
		done = o.order != -1 ? "the byte order and the bit map words "
		    "were changed, nothing else" :
		    "the bit map words were changed, nothing else";
	}
	if (o.namelen != 0) {
		len = fs.namelen;
		if (write && (r = mfs_change_namelen(&fs, o.namelen, 0)) < 0)
			errx(1, "%s: name length: %s; %s", o.image,
			    strerror(-r), done);
		(void)printf("name length: %" PRIu32 " -> %" PRIu32 "\n",
		    len, o.namelen);
		done = o.order != -1 ? "the byte order and the name length "
		    "were changed, nothing else" : words != 0 ?
		    "the bit map words and the name length were changed, "
		    "nothing else" :
		    "the name length was changed, nothing else";
	}
	if (o.nblocks != 0) {
		len = fs.nblocks;
		if (write)
			resize(&fs, &o, 0, done);
		(void)printf("blocks: %" PRIu32 " -> %" PRIu32 "\n", len,
		    o.nblocks >> fs.log_zone_size << fs.log_zone_size);
	}
	for (i = 0; i < 2; i++) {
		if ((r = load_end(&fs, i == 0 ? MFS_IMAP : MFS_ZMAP,
		    &maps[i])) < 0)
			errx(1, "%s: bit maps: %s", o.image, strerror(-r));
	}

	if (!changes(&o)) {
		(void)printf("byte order: %s\n", order_name(fs.order));
		if (fs.order == MFS_BIG_ENDIAN)
			(void)printf("bit map words: %" PRIu32 " bits\n",
			    fs.map_word * 8);
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
