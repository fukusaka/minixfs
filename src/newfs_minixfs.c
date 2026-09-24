/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * newfs_minixfs - make an empty MINIX file system.
 *
 *	newfs_minixfs -V version [-N] [-B le|be] [-b block-size]
 *	    [-i inodes] [-l name-length] [-s blocks] [-t time]
 *	    [-z log-zone-size] image
 *
 * The version has to be given.  The size is taken from -s, or else from
 * the size of an existing image file; with -s, an image file is created
 * or cut to that size.  -N prints the layout and writes nothing.
 */

#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mfs.h"

#define STATIC_BLOCK	1024		/* block size of V1 and V2 */
#define V3_BLOCK	4096		/* default block size of V3 */

/* What the command line asks for. */
struct options {
	struct mfs_params	params;
	const char		*image;
	int			dry_run;	/* -N */
	int			sized;		/* -s given */
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: newfs_minixfs -V version [-N] [-B le|be] [-b block-size]\n"
	    "           [-i inodes] [-l name-length] [-s blocks] [-t time]\n"
	    "           [-z log-zone-size] image\n");
	exit(2);
}

/* A number from the command line, or exit with a message. */
static uint32_t
number(const char *what, const char *s)
{
	unsigned long v;
	char *end;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || *end != '\0' || end == s || s[0] == '-' ||
	    v > UINT32_MAX)
		errx(2, "%s: bad %s", s, what);
	return (uint32_t)v;
}

static void
parse(int argc, char **argv, struct options *o)
{
	struct mfs_params *p;
	int ch;

	(void)memset(o, 0, sizeof(*o));
	p = &o->params;
	p->order = MFS_LITTLE_ENDIAN;
	p->time = (uint32_t)time(NULL);
	while ((ch = getopt(argc, argv, "B:b:i:l:Ns:t:V:z:")) != -1) {
		switch (ch) {
		case 'B':
			if (strcmp(optarg, "le") == 0)
				p->order = MFS_LITTLE_ENDIAN;
			else if (strcmp(optarg, "be") == 0)
				p->order = MFS_BIG_ENDIAN;
			else
				usage();
			break;
		case 'b':
			p->block_size = number("block size", optarg);
			break;
		case 'i':
			p->ninodes = number("number of inodes", optarg);
			if (p->ninodes == 0)
				errx(2, "the number of inodes must be > 0");
			break;
		case 'l':
			p->namelen = number("name length", optarg);
			break;
		case 'N':
			o->dry_run = 1;
			break;
		case 's':
			p->nblocks = number("size", optarg);
			o->sized = 1;
			break;
		case 't':
			p->time = number("time", optarg);
			break;
		case 'V':
			p->version = (int)number("version", optarg);
			break;
		case 'z':
			p->log_zone_size = number("zone size", optarg);
			break;
		default:
			usage();
		}
	}
	if (argc - optind != 1 || p->version == 0)
		usage();
	o->image = argv[optind];
}

/* Check the options against the version, with messages that say why. */
static void
check_options(const struct mfs_params *p)
{
	if (p->version < 1 || p->version > 3)
		errx(2, "version %d: only 1, 2 and 3 exist", p->version);
	if (p->version == 3) {
		if (p->namelen != 0 && p->namelen != 60)
			errx(2, "V3 names are 60 characters");
		if (p->block_size != 0 && (p->block_size < 1024 ||
		    p->block_size > 32768 || p->block_size % 1024 != 0))
			errx(2, "block size %" PRIu32 ": give a multiple of "
			    "1024 up to 32768", p->block_size);
	} else {
		if (p->namelen != 0 && p->namelen != 14 && p->namelen != 30)
			errx(2, "names are 14 or 30 characters in V%d",
			    p->version);
		if (p->block_size != 0 && p->block_size != STATIC_BLOCK)
			errx(2, "V%d blocks are always 1024 bytes",
			    p->version);
	}
}

/*
 * Open the image for writing, creating it if -s was given, and find its
 * size in blocks when -s was not.  Returns -1 for a dry run on an image
 * that does not exist.
 */
static int
open_image(struct options *o, uint32_t block_size)
{
	struct stat st;
	int fd, flags;

	flags = o->dry_run ? O_RDONLY : O_RDWR;
	if (o->sized && !o->dry_run)
		flags |= O_CREAT;
	if ((fd = open(o->image, flags, 0666)) == -1) {
		if (errno == ENOENT && o->sized)
			return -1;
		if (errno == ENOENT)
			errx(1, "%s: does not exist; give the size with -s",
			    o->image);
		err(1, "%s", o->image);
	}
	if (fstat(fd, &st) == -1)
		err(1, "%s", o->image);
	if (o->sized)
		return fd;
	if (!S_ISREG(st.st_mode))
		errx(1, "%s: not a file; give the size with -s", o->image);
	if ((uintmax_t)st.st_size / block_size > UINT32_MAX)
		errx(1, "%s: too large", o->image);
	o->params.nblocks = (uint32_t)(st.st_size / block_size);
	return fd;
}

static void
print_layout(const struct mfs_params *p, const struct mfs_layout *l)
{
	(void)printf("version: %d\n", p->version);
	(void)printf("byte order: %s\n",
	    p->order == MFS_BIG_ENDIAN ? "big-endian" : "little-endian");
	(void)printf("magic: 0x%04" PRIx16 "\n", l->magic);
	(void)printf("name length: %" PRIu32 "\n", l->namelen);
	(void)printf("block size: %" PRIu32 "\n", l->block_size);
	(void)printf("blocks: %" PRIu32 "\n", l->nblocks);
	(void)printf("inodes: %" PRIu32 "\n", l->ninodes);
	(void)printf("zones: %" PRIu32 "\n", l->nzones);
	(void)printf("inode map blocks: %" PRIu32 "\n", l->imap_blocks);
	(void)printf("zone map blocks: %" PRIu32 "\n", l->zmap_blocks);
	(void)printf("inode table blocks: %" PRIu32 "\n", l->itable_blocks);
	(void)printf("first data zone: %" PRIu32 "\n", l->firstdatazone);
	(void)printf("log zone size: %" PRIu32 "\n", p->log_zone_size);
}

static void
plan(const struct options *o, struct mfs_layout *l)
{
	int r;

	switch (r = mfs_plan(&o->params, l)) {
	case 0:
		return;
	case -ENOSPC:
		errx(1, "%s: %" PRIu32 " blocks are too few", o->image,
		    o->params.nblocks);
	case -EFBIG:
		errx(1, "%s: too many blocks or inodes for V%d", o->image,
		    o->params.version);
	default:
		errx(1, "%s: %s", o->image, strerror(-r));
	}
}

int
main(int argc, char **argv)
{
	struct mfs_layout l;
	struct options o;
	uint32_t block_size;
	int fd, r;

	parse(argc, argv, &o);
	check_options(&o.params);
	block_size = o.params.block_size;
	if (block_size == 0)
		block_size = o.params.version == 3 ? V3_BLOCK : STATIC_BLOCK;

	fd = open_image(&o, block_size);
	plan(&o, &l);
	if (o.dry_run) {
		print_layout(&o.params, &l);
		return 0;
	}
	if (o.sized && ftruncate(fd, (off_t)l.nblocks * l.block_size) == -1) {
		/* A device cannot be cut to size; that is fine. */
		if (errno != EINVAL)
			err(1, "%s", o.image);
	}
	if ((r = mfs_format(fd, &o.params, &l)) < 0)
		errx(1, "%s: %s", o.image, strerror(-r));
	if (close(fd) == -1)
		err(1, "%s", o.image);
	return 0;
}
