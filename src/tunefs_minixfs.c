/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * tunefs_minixfs - change the settings of a MINIX file system.
 *
 *	tunefs_minixfs [-N] [-B le|be] [-c clean|dirty] [-e 0|1]
 *	    [-m minix|linux|bytes] [-T SIZE:HEADS:SIDE] image
 *
 * Without options, or with -N, the settings are printed and nothing is
 * written; -N shows what the other options would change.
 *
 *	-B	store every number in the other byte order: little-endian,
 *		as on the PC, or big-endian, as on the 68000 machines
 *	-c	mark the file system clean or dirty: in V1 and V2 in the
 *		state word that Linux keeps, in V3 in the flags of MINIX 3,
 *		which mounts a file system that is not clean read-only
 *	-e	set the bits of each map past the last inode or zone to 0,
 *		as the mkfs of MINIX leaves them, or 1, as that of Linux
 *	-m	the maximum file size in the super block: what MINIX works
 *		out, what Linux and newfs_minixfs write, or a number
 *	-T	an image that holds one side of a disk, as for minixfs(1)
 *
 * Each change is printed as the old and the new value.  The byte order
 * is changed first.  Changes that rewrite more than the super block and
 * the maps work on a file system that fsck_minixfs passes, and one cut
 * short leaves it half changed: keep a copy.  Exit status: 0 on success,
 * 1 if anything failed, 2 for a usage error.
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
	int		clean;		/* -c: 1 clean, 0 dirty, -1 as is */
	int		end;		/* -e: 0 or 1, or -1 as is */
	int		dry_run;	/* -N */
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: tunefs_minixfs [-N] [-B le|be] [-c clean|dirty] [-e 0|1]\n"
	    "           [-m minix|linux|bytes] [-T SIZE:HEADS:SIDE] image\n");
	exit(2);
}

static void
parse(int argc, char **argv, struct options *o)
{
	int ch;

	(void)memset(o, 0, sizeof(*o));
	o->clean = -1;
	o->end = -1;
	o->order = -1;
	while ((ch = getopt(argc, argv, "B:c:e:m:NT:")) != -1) {
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
		case 'm':
			o->max = optarg;
			break;
		case 'N':
			o->dry_run = 1;
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
	return o->order != -1 || o->clean != -1 || o->end != -1 ||
	    o->max != NULL;
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
	if (o.order != -1) {
		(void)printf("byte order: %s -> %s\n", order_name(fs.order),
		    order_name(o.order));
		if (write && (r = mfs_convert_order(&fs, o.order)) < 0)
			errx(1, "%s: byte order: %s", o.image, strerror(-r));
	}
	for (i = 0; i < 2; i++) {
		if ((r = load_end(&fs, i == 0 ? MFS_IMAP : MFS_ZMAP,
		    &maps[i])) < 0)
			errx(1, "%s: bit maps: %s", o.image, strerror(-r));
	}

	if (!changes(&o)) {
		(void)printf("byte order: %s\n", order_name(fs.order));
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
