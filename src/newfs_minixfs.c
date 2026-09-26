/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * newfs_minixfs - make a MINIX file system, empty or from a directory.
 *
 *	newfs_minixfs -V version [-N] [-B le|be] [-b block-size]
 *	    [-d directory [-F specfile [-P dbdir] [-x]] [-o uid:gid]]
 *	    [-e 0|1] [-i inodes] [-l name-length|flex] [-m minix|linux|bytes]
 *	    [-s blocks] [-t time] [-z log-zone-size] image
 *
 * The version has to be given.  The size is taken from -s, or else from
 * the size of an existing image file; with -s, an image file is created
 * or cut to that size.  -N prints the layout and writes nothing.  The
 * maximum file size is what MINIX works out, all that it reads, unless
 * -m gives that of Linux or a number.  The bits of the maps past the
 * last inode and zone are clear, as the mkfs of MINIX leaves them, but
 * set for names of 30 characters, which only Linux reads, as mkfs.minix
 * of Linux sets them, unless -e gives them.  -l flex makes a V1 or V2
 * file system of Minix-vmd, whose flex directories hold names of up to
 * 60 characters.
 *
 * With -d, the file system holds a copy of the directory: its files,
 * directories, symbolic links, devices and pipes, with their modes,
 * owners and times, and hard links as links; -o gives every file the
 * owner uid and group gid instead, since the group of a V1 inode is one
 * byte.  -F reads an mtree(8) specification as makefs -F does: it sets
 * the type, mode, owner, group, time, link target and device number of
 * what it names, and adds what the directory does not have; user and
 * group names come from the passwd and group files of -P, or of the
 * system.  With -x, only what the specification names goes in.  The
 * root directory takes the mode, owner and times of the directory
 * itself.  Everything is checked before anything is written: names too
 * long, owners and device numbers too large, and whether it fits,
 * counting holes as data.  Without -s and an image file, the image is
 * made large enough by that count, and the inodes are raised to what the
 * tree needs unless -i gives them.
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
#include "tree.h"

#define STATIC_BLOCK	1024		/* block size of V1 and V2 */
#define V3_BLOCK	4096		/* default block size of V3 */

/* What the command line asks for. */
struct options {
	struct mfs_params	params;
	const char		*image;
	const char		*dir;		/* -d, or NULL */
	const char		*owner;		/* -o, or NULL */
	const char		*specfile;	/* -F, or NULL */
	const char		*dbdir;		/* -P, or NULL */
	const char		*max;		/* -m, or NULL */
	int			exclude;	/* -x */
	int			dry_run;	/* -N */
	int			sized;		/* -s given */
	int			inodes;		/* -i given */
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: newfs_minixfs -V version [-N] [-B le|be] [-b block-size]\n"
	    "           [-d directory [-F specfile [-P dbdir] [-x]]\n"
	    "           [-o uid:gid]] [-e 0|1] [-i inodes]\n"
	    "           [-l name-length|flex]\n"
	    "           [-m minix|linux|bytes] [-s blocks] [-t time]\n"
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
	p->map_end = -1;
	while ((ch = getopt(argc, argv, "B:b:d:e:F:i:l:m:No:P:s:t:V:xz:")) !=
	    -1) {
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
		case 'd':
			o->dir = optarg;
			break;
		case 'e':
			if (strcmp(optarg, "0") == 0)
				p->map_end = 0;
			else if (strcmp(optarg, "1") == 0)
				p->map_end = 1;
			else
				usage();
			break;
		case 'F':
			o->specfile = optarg;
			break;
		case 'P':
			o->dbdir = optarg;
			break;
		case 'x':
			o->exclude = 1;
			break;
		case 'm':
			o->max = optarg;
			break;
		case 'i':
			p->ninodes = number("number of inodes", optarg);
			if (p->ninodes == 0)
				errx(2, "the number of inodes must be > 0");
			o->inodes = 1;
			break;
		case 'l':
			if (strcmp(optarg, "flex") == 0) {
				p->flex = 1;
				p->namelen = 0;
			} else {
				p->flex = 0;
				p->namelen = number("name length", optarg);
			}
			break;
		case 'N':
			o->dry_run = 1;
			break;
		case 'o':
			o->owner = optarg;
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
	if (argc - optind != 1 || p->version == 0 ||
	    (o->dir == NULL && (o->owner != NULL || o->specfile != NULL)) ||
	    (o->specfile == NULL && (o->dbdir != NULL || o->exclude)))
		usage();
	o->image = argv[optind];
}

/* Check the options against the version, with messages that say why. */
static void
check_options(const struct mfs_params *p)
{
	if (p->version < 1 || p->version > 3)
		errx(2, "version %d: only 1, 2 and 3 exist", p->version);
	if (p->version == 3 && p->flex)
		errx(2, "flex directories are of Minix-vmd, which is V1 or V2");
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
	int fd, flags, r;

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
	if (!o->dry_run && (r = mfs_lock(fd)) < 0)
		errx(1, "%s: %s", o->image, strerror(-r));
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
	(void)printf("name length: %" PRIu32 "%s\n", l->namelen,
	    p->flex ? ", flex" : "");
	(void)printf("block size: %" PRIu32 "\n", l->block_size);
	(void)printf("blocks: %" PRIu32 "\n", l->nblocks);
	(void)printf("inodes: %" PRIu32 "\n", l->ninodes);
	(void)printf("zones: %" PRIu32 "\n", l->nzones);
	(void)printf("inode map blocks: %" PRIu32 "\n", l->imap_blocks);
	(void)printf("zone map blocks: %" PRIu32 "\n", l->zmap_blocks);
	(void)printf("inode table blocks: %" PRIu32 "\n", l->itable_blocks);
	(void)printf("first data zone: %" PRIu32 "\n", l->firstdatazone);
	(void)printf("log zone size: %" PRIu32 "\n", p->log_zone_size);
	(void)printf("max file size: %" PRIu32 "\n", l->max_size);
	(void)printf("bits past the end of the maps: %d\n", p->map_end);
}

/* The maximum file size that -m asks for, or what MINIX works out. */
static uint32_t
max_size(const struct options *o, uint32_t block_size)
{
	const struct mfs_params *p;
	unsigned long v;
	char *end;

	p = &o->params;
	if (o->max == NULL || strcmp(o->max, "minix") == 0)
		return mfs_minix_size(p->version, block_size,
		    p->log_zone_size);
	if (strcmp(o->max, "linux") == 0)
		return mfs_linux_max_size(p->version, p->log_zone_size);
	errno = 0;
	v = strtoul(o->max, &end, 10);
	if (errno != 0 || *end != '\0' || end == o->max || o->max[0] == '-' ||
	    v == 0 || v > INT32_MAX)
		errx(2, "%s: give minix, linux or a size from 1 to %d", o->max,
		    INT32_MAX);
	return (uint32_t)v;
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

/* What the tree is copied into, and with which owners. */
static void
tree_shape(const struct options *o, uint32_t block_size, struct tree_fs *f)
{
	unsigned long v;
	const char *gid;
	char *end;

	(void)memset(f, 0, sizeof(*f));
	f->exclude = o->exclude;
	if (o->specfile != NULL) {
		if ((f->spec = malloc(sizeof(*f->spec))) == NULL)
			err(1, NULL);
		if (spec_read(o->specfile, o->dbdir, f->spec) == -1)
			errx(1, "%s: nothing written", o->specfile);
	}
	f->version = o->params.version;
	f->block_size = block_size;
	f->log_zone_size = o->params.log_zone_size;
	f->flex = o->params.flex;
	f->namelen = o->params.namelen != 0 ? o->params.namelen :
	    o->params.version == 3 || o->params.flex ? 60 : 14;
	f->max_size = o->params.max_size;
	if (o->owner == NULL)
		return;
	if ((gid = strchr(o->owner, ':')) == NULL)
		errx(2, "%s: give the owner as uid:gid", o->owner);
	f->owned = 1;
	f->gid = number("group", gid + 1);
	/* The number before the colon, checked as number() checks one. */
	errno = 0;
	v = strtoul(o->owner, &end, 10);
	if (errno != 0 || end != gid || end == o->owner ||
	    o->owner[0] < '0' || o->owner[0] > '9' || v > UINT32_MAX)
		errx(2, "%s: bad owner", o->owner);
	f->uid = (uint32_t)v;
}

/* Walk the directory of -d, and stop if any of it cannot be copied. */
static void
scan(const struct options *o, const struct tree_fs *f,
    struct tree_need *need)
{
	if (tree_scan(o->dir, f, need) == -1)
		exit(1);
	if (need->owners > 0)
		warnx("%s: owners that do not fit can be set with -o uid:gid",
		    o->dir);
	if (need->problems > 0)
		errx(1, "%s: %lu things above cannot be copied; nothing "
		    "written", o->dir, need->problems);
}

/*
 * Make sure that the layout holds the tree: raise the inodes if -i did
 * not give them, or stop.
 */
static void
fit(struct options *o, struct mfs_layout *l, const struct tree_need *need)
{
	uint32_t avail;

	if (l->ninodes < need->inodes) {
		if (o->inodes)
			errx(1, "%s: the tree needs %ju inodes, more than -i "
			    "gives", o->dir, (uintmax_t)need->inodes);
		if (need->inodes > UINT32_MAX)
			errx(1, "%s: too many files", o->dir);
		o->params.ninodes = (uint32_t)need->inodes;
		plan(o, l);
	}
	avail = l->nzones - l->firstdatazone;
	if (need->zones > avail)
		errx(1, "%s: the tree needs up to %ju zones, and %" PRIu32
		    " blocks leave %" PRIu32 "; give more with -s", o->dir,
		    (uintmax_t)need->zones, l->nblocks, avail);
}

/* The size of an image just large enough for the tree. */
static void
size_for(struct options *o, const struct tree_need *need)
{
	struct mfs_layout l;
	uint64_t avail, nblocks, zone;
	int r;

	zone = (uint64_t)1 << o->params.log_zone_size;
	nblocks = (need->zones + 1) * zone;
	for (;;) {
		if (nblocks > UINT32_MAX)
			errx(1, "%s: too large a tree", o->dir);
		o->params.nblocks = (uint32_t)nblocks;
		if (!o->inodes)
			o->params.ninodes = 0;
		if ((r = mfs_plan(&o->params, &l)) == -ENOSPC) {
			nblocks += zone;
			continue;
		}
		if (r < 0)
			plan(o, &l);		/* says why, and exits */
		if (!o->inodes && l.ninodes < need->inodes) {
			/* The inode table can leave too few blocks. */
			o->params.ninodes = (uint32_t)need->inodes;
			if ((r = mfs_plan(&o->params, &l)) == -ENOSPC) {
				nblocks += zone;
				continue;
			}
			if (r < 0)
				plan(o, &l);	/* says why, and exits */
		}
		if (l.ninodes < need->inodes)
			errx(1, "%s: the tree needs %ju inodes, more than -i "
			    "gives", o->dir, (uintmax_t)need->inodes);
		avail = l.nzones - l.firstdatazone;
		if (avail >= need->zones)
			break;
		nblocks += (need->zones - avail) * zone;
	}
	o->sized = 1;
}

int
main(int argc, char **argv)
{
	struct tree_need need;
	struct tree_fs f;
	struct mfs_layout l;
	struct options o;
	struct stat st;
	struct mfs fs;
	uint32_t block_size;
	int fd, r;

	parse(argc, argv, &o);
	check_options(&o.params);
	block_size = o.params.block_size;
	if (block_size == 0)
		block_size = o.params.version == 3 ? V3_BLOCK : STATIC_BLOCK;
	o.params.max_size = max_size(&o, block_size);
	if (o.params.map_end == -1)
		o.params.map_end = mfs_default_map_end(o.params.version,
		    o.params.namelen);
	if (o.dir != NULL) {
		tree_shape(&o, block_size, &f);
		scan(&o, &f, &need);
		if (!o.sized && stat(o.image, &st) == -1 && errno == ENOENT)
			size_for(&o, &need);
		/* With the size known, check before the image is made. */
		if (o.sized) {
			plan(&o, &l);
			fit(&o, &l, &need);
		}
	}

	fd = open_image(&o, block_size);
	plan(&o, &l);
	if (o.dir != NULL)
		fit(&o, &l, &need);
	if (o.dry_run) {
		print_layout(&o.params, &l);
		if (o.dir != NULL)
			(void)printf("the tree needs: %ju inodes, up to %ju "
			    "zones\n", (uintmax_t)need.inodes,
			    (uintmax_t)need.zones);
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
	if (o.dir == NULL)
		return 0;
	if ((r = mfs_open_rw(&fs, o.image)) < 0)
		errx(1, "%s: %s", o.image, strerror(-r));
	r = tree_copy(&fs, o.dir, &f);
	mfs_close(&fs);
	if (f.spec != NULL) {
		spec_free(f.spec);
		free(f.spec);
	}
	return r == 0 ? 0 : 1;
}
