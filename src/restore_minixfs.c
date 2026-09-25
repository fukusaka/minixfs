/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * restore_minixfs - restore a dump into a MINIX file system image.
 *
 *	restore_minixfs -t [-c] -f file
 *	restore_minixfs -r [-cNv] [-M SIZE:HEADS:SIDE] [-o uid:gid]
 *	    [-s symtable] -f file image
 *
 * The dump is one in the format of BSD dump (see dumpfmt.h): that of
 * dump_minixfs, of NetBSD and FreeBSD for UFS1 and UFS2, of Linux dump
 * for ext2, 3 and 4, and of 4.2BSD and 4.3BSD for FFS and the file
 * systems before it, in either byte order.
 *
 *	-t	list the names of the files in the dump, with their inode
 *		numbers in the dump
 *	-r	restore the dump into the root directory of image: a full
 *		dump into an empty file system, then each incremental one
 *		in the order they were made
 *	-c	read directories of V7, of 16-byte entries, as the restore
 *		of NetBSD does with -c; those of a dump with the header of
 *		OFS_MAGIC, of a file system older than 4.2BSD, are always
 *		read so
 *	-f	the dump; "-" is standard input
 *	-M	an image that holds one side of a disk, as for minixfs(1)
 *	-N	check everything and write nothing
 *	-o	make every file owned by uid and gid, numbers both
 *	-s	where -r keeps which inode of the image each inode of the
 *		dump went to, for the next incremental dump; by default
 *		"restoresymtable", as that of NetBSD
 *	-v	name each file as it is restored
 *
 * -r reads the directories first, which a dump holds before the other
 * files, and checks the names and the number of inodes against the file
 * system before it writes anything; from a file, it also reads through
 * the rest and checks owners, device numbers and sizes.  Each directory
 * of the dump is made to hold what the dump says it holds: names that
 * are gone are removed, new ones made, and a file whose last name went
 * away is freed.  An incremental dump must follow the dump restored
 * last: its date of the dump before it is checked against the table of
 * -s.  While it writes, the image is not marked clean; the mark comes
 * back at the end, unless an operation on the image failed other than
 * for a lack of room or a limit of the file system: a dump that ends
 * too soon, or a file that does not fit, leaves what was written in
 * order, but an error of the device may not.  Extended attributes, file
 * flags and sockets have no place in a MINIX file system and are left
 * out with a warning, as are names of files that a dump of part of a
 * file system does not hold.  Exit status: 0 on success, 1 if anything
 * failed, 2 for a usage error.
 */

#include "compat.h"

#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dumpfmt.h"
#include "mfs.h"

#define SYMTAB		"restoresymtable"	/* default of -s */
#define SYMTAB_MAGIC	"minixfs-restoresymtable 1"
#define MAX_DIR		(64 * 1024 * 1024)	/* bytes of a directory */
#define MAX_UID		65535
#define MAX_GID_V1	255		/* the gid of a V1 inode is a byte */
#define MAX_GID		65535
#define MAX_LINKS_V1	255		/* and so is its link count */
#define MAX_LINKS	65535
#define MAX_DEV		0xffff		/* major * 256 + minor */
#define MAX_TIME	UINT32_MAX
#define MAX_DEPTH	1024		/* deepest directory followed */

/* What restore_files() waits for in an inode it made. */
#define PENDING		1		/* the file, from later in the dump */
#define LEFT_OUT	2		/* nothing: its names are to go */

/* What the command line asks for. */
struct options {
	struct mfs_tracks tracks;	/* -M */
	const char	*file;		/* -f */
	const char	*image;
	const char	*symtab;	/* -s */
	uint32_t	uid;		/* -o */
	uint32_t	gid;
	int		owned;		/* -o given */
	int		list;		/* -t */
	int		restore;	/* -r */
	int		v7;		/* -c */
	int		dry_run;	/* -N */
	int		verbose;	/* -v */
};

/* A pair of the inode number in the dump and that in the image. */
struct ino_pair {
	uint32_t	t;		/* 0: a free slot */
	uint32_t	m;
};

/* Inode numbers of the dump to those of the image, by open addressing. */
struct ino_map {
	struct ino_pair	*tab;
	size_t		n;
	size_t		max;		/* a power of 2 */
};

/* A name in a directory of the dump. */
struct dent {
	char		*name;
	uint32_t	t;
};

/* A directory of the dump. */
struct ddir {
	struct dent	*ent;
	size_t		n;
	size_t		max;
	char		*path;		/* from the root, "." for it */
	uint32_t	parent;		/* the directory that names it */
	struct dump_header h;		/* its inode */
	int		seen;		/* reached from the root */
};

/* The directories of the dump, and an index of them by inode number. */
struct ddirs {
	struct ddir	*d;
	size_t		n;
	size_t		max;
	struct ino_pair	*idx;		/* t: inode, m: index + 1 */
	size_t		idxmax;		/* a power of 2 */
};

/* One run of the command. */
struct restore {
	struct options	*o;
	struct mfs	fs;
	FILE		*in;
	int		seekable;	/* the dump can be read twice */
	int		cut;		/* the dump ends too soon */
	int		status;
	int		broken;		/* an operation on the image failed */
	struct dump_header tape;	/* TS_TAPE */
	struct dump_header next;	/* a header read ahead */
	int		have_next;
	enum dump_dirfmt dirfmt;
	unsigned char	*used;		/* TS_CLRI: in use at the dump */
	uint64_t	nused;		/* bits of it */
	unsigned char	*dumped;	/* TS_BITS: in the dump */
	uint64_t	ndumped;
	struct ddirs	dirs;
	struct ino_map	map;		/* dump to image */
	struct ino_map	paths;		/* m: index into pathv + 1 */
	char		**pathv;
	size_t		npaths;
	size_t		maxpaths;
	uint32_t	*rev;		/* image to dump, 0 for none */
	unsigned char	*ours;		/* image inodes restores made */
	unsigned char	*pending;	/* made, waiting for their data */
	int64_t		prev_date;	/* of the dump restored last */
	off_t		files_at;	/* where the other files start */

	/* Counts for the summary. */
	unsigned long	ndirs;
	unsigned long	nfiles;
	unsigned long	nsymlinks;
	unsigned long	ndevs;
	unsigned long	npipes;
	unsigned long	nfreed;
	unsigned long	sockets;	/* left out */
	unsigned long	attrs;		/* files with attributes left out */
	unsigned long	unknown;	/* names of files not in the dump */
	unsigned long	unnamed;	/* files in the dump without names */
	unsigned long	skipped;	/* records that were no headers */
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: restore_minixfs -t [-c] -f file\n"
	    "       restore_minixfs -r [-cNv] [-M SIZE:HEADS:SIDE] "
	    "[-o uid:gid]\n"
	    "           [-s symtable] -f file image\n");
	exit(2);
}

/* Report a problem and remember that the command failed. */
static void
problem(struct restore *r, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarnx(fmt, ap);
	va_end(ap);
	r->status = 1;
}

static void *
xcalloc(size_t n, size_t size)
{
	void *p;

	if ((p = calloc(n == 0 ? 1 : n, size)) == NULL)
		err(1, NULL);
	return p;
}

static char *
xstrdup(const char *s)
{
	char *p;

	if ((p = strdup(s)) == NULL)
		err(1, NULL);
	return p;
}

/* "uid:gid" of -o, both numbers. */
static void
owner(const char *s, struct options *o)
{
	unsigned long u, g;
	char *end;

	errno = 0;
	u = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != ':' || s[0] == '-' ||
	    u > UINT32_MAX)
		usage();
	s = end + 1;
	g = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || s[0] == '-' ||
	    g > UINT32_MAX)
		usage();
	o->uid = (uint32_t)u;
	o->gid = (uint32_t)g;
	o->owned = 1;
}

static void
parse(int argc, char **argv, struct options *o)
{
	int ch;

	(void)memset(o, 0, sizeof(*o));
	o->symtab = SYMTAB;
	while ((ch = getopt(argc, argv, "cf:M:No:rs:tv")) != -1) {
		switch (ch) {
		case 'c':
			o->v7 = 1;
			break;
		case 'f':
			o->file = optarg;
			break;
		case 'M':
			if (mfs_parse_tracks(optarg, &o->tracks) < 0)
				usage();
			break;
		case 'N':
			o->dry_run = 1;
			break;
		case 'o':
			owner(optarg, o);
			break;
		case 'r':
			o->restore = 1;
			break;
		case 's':
			o->symtab = optarg;
			break;
		case 't':
			o->list = 1;
			break;
		case 'v':
			o->verbose = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (o->file == NULL || o->list + o->restore != 1)
		usage();
	if (o->list && argc != 0)
		usage();
	if (o->restore && argc != 1)
		usage();
	o->image = o->restore ? argv[0] : NULL;
}

/*
 * Reading the dump.
 */

/* Read one record.  Returns 1, 0 at the end of the dump, or -1. */
static int
read_record(struct restore *r, unsigned char *rec)
{
	if (r->cut)
		return 0;
	size_t n;

	n = fread(rec, 1, DUMP_BSIZE, r->in);
	if (n == DUMP_BSIZE)
		return 1;
	if (ferror(r->in)) {
		problem(r, "%s: %s", r->o->file, strerror(errno));
		return -1;
	}
	if (n != 0) {
		problem(r, "%s: ends inside a record", r->o->file);
		r->cut = 1;
		return -1;
	}
	return 0;
}

/* Read a record of data, which the dump must have. */
static int
read_data(struct restore *r, unsigned char *rec)
{
	int n;

	if ((n = read_record(r, rec)) == 0 && !r->cut) {
		problem(r, "%s: ends before TS_END", r->o->file);
		r->cut = 1;
	}
	return n == 1 ? 1 : -1;
}

/*
 * Read the next header, skipping records that are not headers, as the
 * restore of BSD does to find its way again.  Returns 1, 0 at the end of
 * the dump, or -1.
 */
static int
read_header(struct restore *r, struct dump_header *h)
{
	unsigned char rec[DUMP_BSIZE];
	int n;

	if (r->have_next) {
		*h = r->next;
		r->have_next = 0;
		return 1;
	}
	for (;;) {
		if ((n = read_record(r, rec)) <= 0)
			return n;
		if (dump_get_header(rec, h) == 0)
			return 1;
		r->skipped++;
	}
}

/* Put h back, to be read again next. */
static void
unread_header(struct restore *r, const struct dump_header *h)
{
	r->next = *h;
	r->have_next = 1;
}

/* Skip the data records of the header *h.  Returns 0 or -1. */
static int
skip_data(struct restore *r, const struct dump_header *h)
{
	unsigned char rec[DUMP_BSIZE];
	int64_t i, n;

	if ((n = dump_data_records(h)) < 0) {
		problem(r, "%s: a damaged header of inode %" PRIu32,
		    r->o->file, h->inumber);
		return 0;
	}
	for (i = 0; i < n; i++)
		if (read_data(r, rec) != 1)
			return -1;
	return 0;
}

/* Where the contents of a file go: a directory in memory, or a file. */
struct sink {
	unsigned char	*buf;		/* a directory, or NULL */
	struct mfs_inode *ip;		/* a file, or NULL: skip */
	const char	*path;
	int		failed;
};

/* Take one data record at pos of a file of size bytes. */
static void
take_record(struct restore *r, struct sink *s, const unsigned char *rec,
    uint64_t pos, uint64_t size)
{
	size_t len;
	int e;

	if (pos >= size || s->failed)
		return;
	len = size - pos < DUMP_BSIZE ? (size_t)(size - pos) : DUMP_BSIZE;
	if (s->buf != NULL) {
		(void)memcpy(s->buf + pos, rec, len);
	} else if (s->ip != NULL &&
	    (e = mfs_pwrite(&r->fs, s->ip, rec, len, (uint32_t)pos)) < 0) {
		problem(r, "%s: %s", s->path, strerror(-e));
		if (mfs_failure_breaks(e))
			r->broken = 1;
		s->failed = 1;
	}
}

/*
 * Read the data of the file whose TS_INODE header is *h into s, going on
 * with its TS_ADDR headers, as the getfile() of the restore of BSD does.
 * Records past the size, extended attributes of UFS2, are read and
 * dropped.  Returns 0, or -1 if the dump cannot be read on.
 */
static int
read_contents(struct restore *r, const struct dump_header *h,
    struct sink *s)
{
	struct dump_run runs[DUMP_NADDR];
	unsigned char rec[DUMP_BSIZE];
	struct dump_header cur;
	uint64_t pos;
	uint32_t k;
	int i, n;

	cur = *h;
	for (pos = 0;;) {
		if ((n = dump_addr_runs(&cur, runs)) < 0) {
			problem(r, "%s: a damaged header", s->path);
			return 0;
		}
		for (i = 0; i < n; i++) {
			for (k = 0; k < runs[i].len; k++, pos += DUMP_BSIZE) {
				if (!runs[i].data)
					continue;
				if (read_data(r, rec) != 1)
					return -1;
				take_record(r, s, rec, pos, h->size);
			}
		}
		if (pos >= h->size)
			return 0;
		if ((n = read_header(r, &cur)) != 1)
			return n == 0 ? 0 : -1;
		if (cur.type != TS_ADDR || cur.inumber != h->inumber) {
			problem(r, "%s: the dump holds only %ju of %ju bytes",
			    s->path, (uintmax_t)pos, (uintmax_t)h->size);
			unread_header(r, &cur);
			return 0;
		}
	}
}

/* Test bit t - 1 of a map of the dump, of nbits bits. */
static int
map_has(const unsigned char *map, uint64_t nbits, uint32_t t)
{
	if (map == NULL || t == 0 || t > nbits)
		return 0;
	return (map[(t - 1) / 8] >> ((t - 1) % 8)) & 1;
}

/* Read the map that follows the TS_CLRI or TS_BITS header *h. */
static int
read_map(struct restore *r, const struct dump_header *h,
    unsigned char **map, uint64_t *nbits)
{
	int32_t i;

	if (h->count < 0 || h->count > INT32_MAX / DUMP_BSIZE) {
		problem(r, "%s: a damaged map", r->o->file);
		return -1;
	}
	free(*map);
	*map = xcalloc((size_t)h->count, DUMP_BSIZE);
	*nbits = (uint64_t)h->count * DUMP_BSIZE * 8;
	for (i = 0; i < h->count; i++)
		if (read_data(r, *map + (size_t)i * DUMP_BSIZE) != 1)
			return -1;
	return 0;
}

/*
 * The inode maps.
 */

static struct ino_pair *
map_slot(const struct ino_map *mp, uint32_t t)
{
	size_t i;

	for (i = t & (mp->max - 1); mp->tab[i].t != 0 && mp->tab[i].t != t;
	    i = (i + 1) & (mp->max - 1))
		continue;
	return &mp->tab[i];
}

/* The value for t, or 0. */
static uint32_t
map_get(const struct ino_map *mp, uint32_t t)
{
	if (mp->max == 0)
		return 0;
	return map_slot(mp, t)->m;
}

static void
map_set(struct ino_map *mp, uint32_t t, uint32_t m)
{
	struct ino_pair *old, *p;
	size_t i, max;

	if (mp->n * 2 >= mp->max) {
		old = mp->tab;
		max = mp->max;
		mp->max = max == 0 ? 64 : max * 2;
		mp->tab = xcalloc(mp->max, sizeof(*mp->tab));
		for (i = 0; i < max; i++)
			if (old[i].t != 0)
				*map_slot(mp, old[i].t) = old[i];
		free(old);
	}
	p = map_slot(mp, t);
	if (p->t == 0) {
		p->t = t;
		mp->n++;
	}
	p->m = m;
}

/* The directory of the dump with inode t, or NULL. */
static struct ddir *
ddir_find(const struct ddirs *ds, uint32_t t)
{
	size_t i;

	if (ds->idxmax == 0)
		return NULL;
	for (i = t & (ds->idxmax - 1); ds->idx[i].t != 0;
	    i = (i + 1) & (ds->idxmax - 1))
		if (ds->idx[i].t == t)
			return &ds->d[ds->idx[i].m - 1];
	return NULL;
}

/* Index directory number n + 1 under inode t. */
static void
ddir_index(struct ddirs *ds, uint32_t t, uint32_t n)
{
	struct ino_pair *old;
	size_t i, j, max;

	if (ds->n * 2 >= ds->idxmax) {
		old = ds->idx;
		max = ds->idxmax;
		ds->idxmax = max == 0 ? 64 : max * 2;
		ds->idx = xcalloc(ds->idxmax, sizeof(*ds->idx));
		for (i = 0; i < max; i++) {
			if (old[i].t == 0)
				continue;
			for (j = old[i].t & (ds->idxmax - 1);
			    ds->idx[j].t != 0; j = (j + 1) & (ds->idxmax - 1))
				continue;
			ds->idx[j] = old[i];
		}
		free(old);
	}
	for (i = t & (ds->idxmax - 1); ds->idx[i].t != 0;
	    i = (i + 1) & (ds->idxmax - 1))
		continue;
	ds->idx[i].t = t;
	ds->idx[i].m = n;
}

/* The path of dump inode t, as first named, or a stand-in. */
static const char *
path_of(struct restore *r, uint32_t t, char *buf, size_t len)
{
	uint32_t i;

	if ((i = map_get(&r->paths, t)) != 0)
		return r->pathv[i - 1];
	(void)snprintf(buf, len, "inode %" PRIu32, t);
	return buf;
}

static void
path_note(struct restore *r, uint32_t t, const char *path)
{
	char **p;

	if (map_get(&r->paths, t) != 0)
		return;
	if (r->npaths == r->maxpaths) {
		r->maxpaths = r->maxpaths == 0 ? 64 : r->maxpaths * 2;
		p = realloc(r->pathv, r->maxpaths * sizeof(*p));
		if (p == NULL)
			err(1, NULL);
		r->pathv = p;
	}
	r->pathv[r->npaths++] = xstrdup(path);
	map_set(&r->paths, t, (uint32_t)r->npaths);
}

/*
 * The directories of the dump.
 */

static void
ddir_add_entry(struct ddir *d, const struct dump_dirent *de)
{
	struct dent *p;

	if (d->n == d->max) {
		d->max = d->max == 0 ? 16 : d->max * 2;
		if ((p = realloc(d->ent, d->max * sizeof(*p))) == NULL)
			err(1, NULL);
		d->ent = p;
	}
	d->ent[d->n].name = xstrdup(de->name);
	d->ent[d->n].t = de->ino;
	d->n++;
}

static void
ddir_free(struct ddir *d)
{
	size_t i;

	for (i = 0; i < d->n; i++)
		free(d->ent[i].name);
	free(d->ent);
	free(d->path);
}

/* Parse the entries of a directory of size bytes at buf into *d. */
static void
parse_dir(struct restore *r, struct ddir *d, const unsigned char *buf,
    size_t size)
{
	struct dump_dirent de;
	size_t avail, chunk, loc;
	int n;

	for (loc = 0; loc < size; loc += (size_t)n) {
		chunk = DUMP_DIRBLK - loc % DUMP_DIRBLK;
		avail = size - loc < chunk ? size - loc : chunk;
		n = dump_get_dirent(buf + loc, avail, r->dirfmt, r->tape.order,
		    &de);
		if (n < 0) {
			problem(r, "%s: directory inode %" PRIu32 ": a "
			    "damaged entry at %zu", r->o->file, d->h.inumber,
			    loc);
			n = (int)avail;
			continue;
		}
		if (de.ino != 0)
			ddir_add_entry(d, &de);
	}
}

/* Read the directory whose TS_INODE header is *h. */
static int
read_dir(struct restore *r, const struct dump_header *h)
{
	struct sink s;
	struct ddir *d, *p;

	if (r->dirs.n == r->dirs.max) {
		r->dirs.max = r->dirs.max == 0 ? 64 : r->dirs.max * 2;
		p = realloc(r->dirs.d, r->dirs.max * sizeof(*p));
		if (p == NULL)
			err(1, NULL);
		r->dirs.d = p;
	}
	d = &r->dirs.d[r->dirs.n];
	(void)memset(d, 0, sizeof(*d));
	d->h = *h;
	(void)memset(&s, 0, sizeof(s));
	s.path = "a directory";
	if (h->size > MAX_DIR) {
		problem(r, "%s: directory inode %" PRIu32 " of %ju bytes",
		    r->o->file, h->inumber, (uintmax_t)h->size);
		d->h.size = 0;
	}
	s.buf = xcalloc((size_t)d->h.size, 1);
	if (read_contents(r, &d->h, &s) == -1) {
		free(s.buf);
		return -1;
	}
	parse_dir(r, d, s.buf, (size_t)d->h.size);
	free(s.buf);
	if (ddir_find(&r->dirs, h->inumber) != NULL) {
		problem(r, "%s: directory inode %" PRIu32 " twice",
		    r->o->file, h->inumber);
		ddir_free(d);
		return 0;
	}
	ddir_index(&r->dirs, h->inumber, (uint32_t)r->dirs.n + 1);
	r->dirs.n++;
	return 0;
}

/* Whether a header is the extended attributes that Linux dump adds. */
static int
is_attrs(const struct dump_header *h)
{
	return h->type == TS_INODE && (h->flags & DR_EXTATTRIBUTES) != 0;
}

/*
 * Read the directories, up to the first other file, which is put back.
 * Returns 0 or -1.
 */
static int
read_dirs(struct restore *r)
{
	struct dump_header h;
	int n;

	while ((n = read_header(r, &h)) == 1) {
		if (is_attrs(&h) || h.type == TS_ADDR) {
			if (skip_data(r, &h) == -1)
				return -1;
			continue;
		}
		if (h.type != TS_INODE ||
		    (h.mode & MFS_S_IFMT) != MFS_S_IFDIR) {
			unread_header(r, &h);
			return 0;
		}
		if (read_dir(r, &h) == -1)
			return -1;
	}
	return n == 0 ? 0 : -1;
}

/* What the TS_TAPE header says, as the restore of BSD prints it. */
static void
print_info(const struct dump_header *h)
{
	time_t t;

	t = (time_t)h->date;
	(void)printf("Dump   date: %s", ctime(&t));
	t = (time_t)h->ddate;
	(void)printf("Dumped from: %s", h->ddate == 0 ? "the epoch\n" :
	    ctime(&t));
	(void)fflush(stdout);
	(void)fprintf(stderr, "Level %d dump of %s on %s:%s\n", (int)h->level,
	    h->filesys, h->host[0] != '\0' ? h->host : "[unknown]", h->dev);
	(void)fprintf(stderr, "Label: %s\n", h->label);
}

/* Read the TS_TAPE header and the maps. */
static int
read_start(struct restore *r)
{
	unsigned char rec[DUMP_BSIZE];
	struct dump_header h;
	int n;

	if (read_record(r, rec) != 1 || dump_get_header(rec, &r->tape) != 0 ||
	    r->tape.type != TS_TAPE) {
		problem(r, "%s: not a dump", r->o->file);
		return -1;
	}
	if (r->o->list || r->o->verbose)
		print_info(&r->tape);
	if (r->tape.volume != 1) {
		problem(r, "%s: not the first volume of a dump", r->o->file);
		return -1;
	}
	if (r->tape.flags & DR_COMPRESSED) {
		problem(r, "%s: a compressed dump of Linux, which this does "
		    "not read", r->o->file);
		return -1;
	}
	/* OFS_MAGIC is the dump of a file system older than 4.2BSD. */
	if (r->o->v7 || r->tape.kind == DUMP_OFS)
		r->dirfmt = DUMP_DIR_V7;
	else if (r->tape.flags & DR_NEWINODEFMT)
		r->dirfmt = DUMP_DIR_NEW;
	else
		r->dirfmt = DUMP_DIR_OLD;
	while ((n = read_header(r, &h)) == 1 &&
	    (h.type == TS_CLRI || h.type == TS_BITS)) {
		if (read_map(r, &h, h.type == TS_CLRI ? &r->used : &r->dumped,
		    h.type == TS_CLRI ? &r->nused : &r->ndumped) == -1)
			return -1;
	}
	if (n == 1)
		unread_header(r, &h);
	if (r->dumped == NULL) {
		problem(r, "%s: no map of the inodes in the dump", r->o->file);
		return -1;
	}
	return n == -1 ? -1 : 0;
}

/* Name each directory from the root down, and note its path. */
static void
name_dirs(struct restore *r, struct ddir *d, uint32_t parent,
    const char *path, int depth)
{
	struct ddir *c;
	size_t i, len;
	char *p;

	d->seen = 1;
	d->parent = parent;
	d->path = xstrdup(path);
	path_note(r, d->h.inumber, path);
	for (i = 0; i < d->n; i++) {
		if (strcmp(d->ent[i].name, ".") == 0 ||
		    strcmp(d->ent[i].name, "..") == 0)
			continue;
		len = strlen(path) + strlen(d->ent[i].name) + 2;
		if ((p = malloc(len)) == NULL)
			err(1, NULL);
		(void)snprintf(p, len, "%s/%s", path, d->ent[i].name);
		path_note(r, d->ent[i].t, p);
		c = ddir_find(&r->dirs, d->ent[i].t);
		if (c != NULL && !c->seen && depth < MAX_DEPTH)
			name_dirs(r, c, d->h.inumber, p, depth + 1);
		free(p);
	}
}

/*
 * -t: list the files in the dump.
 */

static void
list_dir(struct restore *r, const struct ddir *d, const char *path,
    int depth)
{
	const struct ddir *c;
	size_t i, len;
	char *p;

	for (i = 0; i < d->n; i++) {
		if (strcmp(d->ent[i].name, ".") == 0 ||
		    strcmp(d->ent[i].name, "..") == 0 ||
		    !map_has(r->dumped, r->ndumped, d->ent[i].t))
			continue;
		len = strlen(path) + strlen(d->ent[i].name) + 2;
		if ((p = malloc(len)) == NULL)
			err(1, NULL);
		(void)snprintf(p, len, "%s/%s", path, d->ent[i].name);
		(void)printf("%10" PRIu32 "\t%s\n", d->ent[i].t, p);
		c = ddir_find(&r->dirs, d->ent[i].t);
		if (c != NULL && c->path != NULL && strcmp(c->path, p) == 0 &&
		    depth < MAX_DEPTH)
			list_dir(r, c, p, depth + 1);
		free(p);
	}
}

static void
list(struct restore *r)
{
	struct ddir *root;

	if ((root = ddir_find(&r->dirs, DUMP_ROOTINO)) == NULL) {
		problem(r, "%s: the dump holds no root directory",
		    r->o->file);
		return;
	}
	name_dirs(r, root, DUMP_ROOTINO, ".", 0);
	(void)printf("%10d\t.\n", DUMP_ROOTINO);
	list_dir(r, root, ".", 0);
}

/*
 * The table that -s keeps between restores: a line with SYMTAB_MAGIC, a
 * line "date N" with the date of the dump restored last, and a line
 * "T M" for each inode T of the dumps that is inode M of the image.
 */

static void
note_pair(struct restore *r, uint32_t t, uint32_t m)
{
	map_set(&r->map, t, m);
	r->rev[m] = t;
	r->ours[m] = 1;
}

static int
load_symtab(struct restore *r)
{
	char line[128];
	long long date;
	uint32_t m, t;
	FILE *fp;
	int bad;

	if ((fp = fopen(r->o->symtab, "r")) == NULL) {
		problem(r, "%s: %s: an incremental dump needs the table of "
		    "the restores before it", r->o->symtab, strerror(errno));
		return -1;
	}
	bad = fgets(line, sizeof(line), fp) == NULL ||
	    strcmp(line, SYMTAB_MAGIC "\n") != 0 ||
	    fgets(line, sizeof(line), fp) == NULL ||
	    sscanf(line, "date %lld", &date) != 1;
	while (!bad && fgets(line, sizeof(line), fp) != NULL) {
		if (sscanf(line, "%" SCNu32 " %" SCNu32, &t, &m) != 2 ||
		    t == 0 || m == 0 || m > r->fs.ninodes)
			bad = 1;
		else
			note_pair(r, t, m);
	}
	(void)fclose(fp);
	if (bad) {
		problem(r, "%s: not a table of restore_minixfs", r->o->symtab);
		return -1;
	}
	r->prev_date = date;
	return 0;
}

/* Write the table of -s: the inodes, and the date the next dump follows. */
static int
save_symtab(struct restore *r, int64_t date)
{
	char tmp[PATH_MAX];
	uint32_t m;
	FILE *fp;

	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", r->o->symtab) >=
	    sizeof(tmp)) {
		problem(r, "%s: name too long", r->o->symtab);
		return -1;
	}
	if ((fp = fopen(tmp, "w")) == NULL) {
		problem(r, "%s: %s", tmp, strerror(errno));
		return -1;
	}
	(void)fprintf(fp, "%s\ndate %lld\n", SYMTAB_MAGIC, (long long)date);
	for (m = 1; m <= r->fs.ninodes; m++)
		if (r->rev[m] != 0)
			(void)fprintf(fp, "%" PRIu32 " %" PRIu32 "\n",
			    r->rev[m], m);
	if (fflush(fp) == EOF || ferror(fp)) {
		problem(r, "%s: %s", tmp, strerror(errno));
		(void)fclose(fp);
		return -1;
	}
	(void)fclose(fp);
	if (rename(tmp, r->o->symtab) == -1) {
		problem(r, "%s: %s", r->o->symtab, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Checking the dump against the file system.
 */

/* Whether the name of an entry stands for the directory or its parent. */
static int
is_dot(const char *name)
{
	return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* Whether inode t can be restored: it is in the dump or already here. */
static int
known(const struct restore *r, uint32_t t)
{
	return map_get(&r->map, t) != 0 ||
	    map_has(r->dumped, r->ndumped, t);
}

/*
 * Check the owner and times of a file whose header is *h.  Returns the
 * number of problems it reported.
 */
static int
check_attrs(struct restore *r, const struct dump_header *h,
    const char *path)
{
	int n;

	n = 0;
	if (!r->o->owned && h->uid > MAX_UID) {
		problem(r, "%s: owner %" PRIu32 " does not fit; see -o",
		    path, h->uid);
		n++;
	}
	if (!r->o->owned &&
	    h->gid > (r->fs.version == 1 ? MAX_GID_V1 : MAX_GID)) {
		problem(r, "%s: group %" PRIu32 " does not fit in V%d; "
		    "see -o", path, h->gid, r->fs.version);
		n++;
	}
	if (h->mtime < 0 || h->mtime > MAX_TIME || h->atime < 0 ||
	    h->atime > MAX_TIME || h->ctime < 0 || h->ctime > MAX_TIME) {
		problem(r, "%s: a time before 1970 or after 2106", path);
		n++;
	}
	return n;
}

/* Check a file other than a directory; returns 0 if it can be made. */
static int
check_file(struct restore *r, const struct dump_header *h,
    const char *path)
{
	int n;

	n = check_attrs(r, h, path);
	switch (h->mode & MFS_S_IFMT) {
	case MFS_S_IFREG:
		if (h->size > r->fs.max_file || h->size > UINT32_MAX) {
			problem(r, "%s: %ju bytes are more than a file of "
			    "V%d holds", path, (uintmax_t)h->size,
			    r->fs.version);
			n++;
		}
		break;
	case MFS_S_IFLNK:
		/* MINIX reads a link from its first block, with a NUL. */
		if (h->size == 0 || h->size >= r->fs.block_size) {
			problem(r, "%s: a symbolic link of %ju bytes, where "
			    "MINIX takes 1 to %" PRIu32, path,
			    (uintmax_t)h->size, r->fs.block_size - 1);
			n++;
		}
		break;
	case MFS_S_IFCHR:
	case MFS_S_IFBLK:
		if (h->rdev > MAX_DEV) {
			problem(r, "%s: device number %#" PRIx32 " does not "
			    "fit", path, h->rdev);
			n++;
		}
		break;
	case MFS_S_IFIFO:
	case MFS_S_IFSOCK:
		break;
	default:
		problem(r, "%s: an unknown kind of file, mode %#o", path,
		    (unsigned)h->mode);
		n++;
		break;
	}
	return n == 0 ? 0 : -1;
}

/* Check the names of a directory, and count what it needs. */
static void
check_dir(struct restore *r, const struct ddir *d, struct ino_map *seen,
    uint64_t *need)
{
	struct mfs_inode ip;
	uint32_t m;
	size_t i;

	for (i = 0; i < d->n; i++) {
		if (is_dot(d->ent[i].name) || d->ent[i].t == DUMP_WINO)
			continue;
		if (strlen(d->ent[i].name) > r->fs.namelen)
			problem(r, "%s/%s: name longer than %" PRIu32
			    " characters", d->path, d->ent[i].name,
			    r->fs.namelen);
		if (strchr(d->ent[i].name, '/') != NULL ||
		    d->ent[i].name[0] == '\0')
			problem(r, "%s: a bad name \"%s\"", d->path,
			    d->ent[i].name);
		if (!known(r, d->ent[i].t)) {
			r->unknown++;
			continue;
		}
		if (map_get(seen, d->ent[i].t) != 0)
			continue;
		map_set(seen, d->ent[i].t, 1);
		m = map_get(&r->map, d->ent[i].t);
		if (m == 0 || (map_has(r->dumped, r->ndumped, d->ent[i].t) &&
		    mfs_get_inode(&r->fs, m, &ip) == 0 && mfs_is_dir(&ip) !=
		    (ddir_find(&r->dirs, d->ent[i].t) != NULL)))
			(*need)++;
	}
	(void)check_attrs(r, &d->h, d->path);
}

/*
 * Read through the other files of a dump that can be read twice, check
 * each, and go back to the first of them.
 */
static int
check_files(struct restore *r)
{
	struct dump_header h;
	char buf[32];
	int n;

	while ((n = read_header(r, &h)) == 1 && h.type != TS_END) {
		if (h.type == TS_INODE && !is_attrs(&h) &&
		    map_get(&r->paths, h.inumber) != 0 &&
		    (h.mode & MFS_S_IFMT) != MFS_S_IFDIR)
			(void)check_file(r, &h,
			    path_of(r, h.inumber, buf, sizeof(buf)));
		if (skip_data(r, &h) == -1)
			return -1;
	}
	if (n == -1)
		return -1;
	r->have_next = 0;
	if (fseeko(r->in, r->files_at, SEEK_SET) == -1) {
		problem(r, "%s: %s", r->o->file, strerror(errno));
		return -1;
	}
	return 0;
}

/* Collect the entries of a directory of the image. */
struct entries {
	struct mfs_dirent *e;
	size_t		n;
	size_t		max;
};

static int
collect_fn(const struct mfs_dirent *de, void *arg)
{
	struct mfs_dirent *p;
	struct entries *es;

	es = arg;
	if (es->n == es->max) {
		es->max = es->max == 0 ? 32 : es->max * 2;
		if ((p = realloc(es->e, es->max * sizeof(*p))) == NULL)
			return -ENOMEM;
		es->e = p;
	}
	es->e[es->n++] = *de;
	return 0;
}

static int
read_entries(struct restore *r, uint32_t m, struct entries *es,
    struct mfs_inode *dp)
{
	int e;

	(void)memset(es, 0, sizeof(*es));
	if ((e = mfs_get_inode(&r->fs, m, dp)) < 0 ||
	    (e = mfs_readdir(&r->fs, dp, collect_fn, es)) < 0) {
		problem(r, "%s: inode %" PRIu32 ": %s", r->o->image, m,
		    strerror(-e));
		r->broken = 1;
		free(es->e);
		es->e = NULL;
		return -1;
	}
	return 0;
}

/* Whether the image is ready for a full dump: an empty root. */
static int
check_empty(struct restore *r)
{
	struct mfs_inode dp;
	struct entries es;
	size_t i;

	if (read_entries(r, MFS_ROOT_INO, &es, &dp) == -1)
		return -1;
	for (i = 0; i < es.n; i++)
		if (!is_dot(es.e[i].name))
			break;
	free(es.e);
	if (i < es.n) {
		problem(r, "%s: a full dump goes into an empty file system",
		    r->o->image);
		return -1;
	}
	return 0;
}

/* Check the whole dump, before anything is written. */
static int
check(struct restore *r)
{
	struct ino_map seen;
	uint64_t need, gone;
	uint32_t finodes, fzones;
	size_t i;
	int e;

	if (r->o->owned && (r->o->uid > MAX_UID ||
	    r->o->gid > (r->fs.version == 1 ? MAX_GID_V1 : MAX_GID)))
		problem(r, "%s: -o %" PRIu32 ":%" PRIu32 " does not fit in "
		    "V%d", r->o->image, r->o->uid, r->o->gid, r->fs.version);
	(void)memset(&seen, 0, sizeof(seen));
	need = 0;
	for (i = 0; i < r->dirs.n; i++)
		if (r->dirs.d[i].seen)
			check_dir(r, &r->dirs.d[i], &seen, &need);
	free(seen.tab);
	gone = 0;
	for (i = 0; i < r->map.max; i++)
		if (r->map.tab[i].t != 0 && r->map.tab[i].m != 0 &&
		    r->used != NULL &&
		    !map_has(r->used, r->nused, r->map.tab[i].t))
			gone++;
	if ((e = mfs_count_free(&r->fs, &finodes, &fzones)) < 0) {
		problem(r, "%s: %s", r->o->image, strerror(-e));
		return -1;
	}
	if (need > finodes + gone)
		problem(r, "%s: the dump needs %ju inodes and %" PRIu32
		    " are free", r->o->image, (uintmax_t)need, finodes);
	if (r->seekable && check_files(r) == -1)
		return -1;
	return r->status == 0 ? 0 : -1;
}

/*
 * Writing into the image.
 */

/*
 * Report the failure e of an operation on the image, a negative errno
 * value, and return -1.
 */
static int
failed(struct restore *r, const char *path, int e)
{
	problem(r, "%s: %s", path, strerror(-e));
	if (mfs_failure_breaks(e))
		r->broken = 1;
	return -1;
}

/* Give *ip the mode, owner and times of the header *h. */
static void
set_attrs(struct restore *r, struct mfs_inode *ip,
    const struct dump_header *h)
{
	ip->mode = h->mode;
	/* check_attrs() and check() have seen that these fit. */
	ip->uid = (uint16_t)(r->o->owned ? r->o->uid : h->uid);
	ip->gid = (uint16_t)(r->o->owned ? r->o->gid : h->gid);
	ip->atime = (uint32_t)h->atime;
	ip->mtime = (uint32_t)h->mtime;
	/* UFS2 keeps no ctime in the dump: that of the data is closest. */
	ip->ctime = (uint32_t)(h->kind == DUMP_UFS2 ? h->mtime : h->ctime);
}

/*
 * Whether dump inode t, which is inode m here, has become a directory or
 * stopped being one; then it needs an inode of its own.
 */
static int
type_changed(struct restore *r, uint32_t t, uint32_t m)
{
	struct mfs_inode ip;

	if (!map_has(r->dumped, r->ndumped, t) ||
	    mfs_get_inode(&r->fs, m, &ip) < 0)
		return 0;
	return mfs_is_dir(&ip) != (ddir_find(&r->dirs, t) != NULL);
}

/*
 * Take an inode of the image for dump inode t: a directory as the dump
 * has it, or a stand-in for a file that comes later in the dump.
 */
static int
new_inode(struct restore *r, uint32_t t)
{
	struct mfs_inode ip;
	struct ddir *d;
	uint32_t m, old;
	char buf[32];
	int e;

	if ((e = mfs_alloc_inode(&r->fs, &m)) < 0)
		return failed(r, path_of(r, t, buf, sizeof(buf)), e);
	(void)memset(&ip, 0, sizeof(ip));
	ip.num = m;
	ip.nlinks = 1;
	if ((d = ddir_find(&r->dirs, t)) != NULL) {
		set_attrs(r, &ip, &d->h);
	} else {
		ip.mode = MFS_S_IFREG;
		r->pending[m] = PENDING;
	}
	if ((e = mfs_put_inode(&r->fs, &ip)) < 0)
		return failed(r, path_of(r, t, buf, sizeof(buf)), e);
	if ((old = map_get(&r->map, t)) != 0)
		r->rev[old] = 0;
	note_pair(r, t, m);
	return 0;
}

/* Take the inodes of the files that the directories name anew. */
static int
make_inodes(struct restore *r)
{
	const struct ddir *d;
	uint32_t m, t;
	size_t i, j;

	for (i = 0; i < r->dirs.n; i++) {
		d = &r->dirs.d[i];
		for (j = 0; d->seen && j < d->n; j++) {
			t = d->ent[j].t;
			if (is_dot(d->ent[j].name) || t == DUMP_WINO ||
			    !known(r, t))
				continue;
			m = map_get(&r->map, t);
			if ((m == 0 || type_changed(r, t, m)) &&
			    new_inode(r, t) == -1)
				return -1;
		}
	}
	return 0;
}

/* A name that a directory is to hold. */
struct want {
	const char	*name;
	uint32_t	m;
	int		used;		/* found there, or put there */
};

static int
want_cmp(const void *a, const void *b)
{
	const struct want *const *x = a;
	const struct want *const *y = b;

	return strcmp((*x)->name, (*y)->name);
}

static struct want *
want_find(struct want **sorted, size_t n, const char *name)
{
	struct want key, *kp, **p;

	key.name = name;
	kp = &key;
	p = bsearch(&kp, sorted, n, sizeof(*sorted), want_cmp);
	return p == NULL ? NULL : *p;
}

/*
 * The names the dump directory *d holds, as inodes of the image: "." and
 * ".." first, whatever the dump has for them, then the others.
 */
static size_t
wants(struct restore *r, const struct ddir *d, uint32_t m,
    struct want *w)
{
	uint32_t target;
	size_t i, n;

	w[0].name = ".";
	w[0].m = m;
	w[1].name = "..";
	w[1].m = d->h.inumber == DUMP_ROOTINO ? MFS_ROOT_INO :
	    map_get(&r->map, d->parent);
	n = 2;
	for (i = 0; i < d->n; i++) {
		if (is_dot(d->ent[i].name) || d->ent[i].t == DUMP_WINO ||
		    (target = map_get(&r->map, d->ent[i].t)) == 0)
			continue;
		w[n].name = d->ent[i].name;
		w[n].m = target;
		n++;
	}
	for (i = 0; i < n; i++)
		w[i].used = 0;
	return n;
}

/* Sort the names, and take a name that comes twice as there once. */
static void
sort_wants(struct want *w, size_t n, struct want **sorted)
{
	size_t i;

	for (i = 0; i < n; i++)
		sorted[i] = &w[i];
	qsort(sorted, n, sizeof(*sorted), want_cmp);
	for (i = 1; i < n; i++)
		if (strcmp(sorted[i - 1]->name, sorted[i]->name) == 0)
			sorted[i]->used = 1;
}

/*
 * Make the entries of directory m of the image those of *w: keep what is
 * there already, point "." and ".." anew if they moved, remove the rest.
 */
static int
match_entries(struct restore *r, const char *path, uint32_t m,
    struct want **sorted, size_t n)
{
	struct mfs_inode dp;
	struct entries es;
	struct want *w;
	size_t i;
	int e;

	if (read_entries(r, m, &es, &dp) == -1)
		return -1;
	for (i = 0, e = 0; i < es.n && e == 0; i++) {
		w = want_find(sorted, n, es.e[i].name);
		if (w != NULL && !w->used && w->m == es.e[i].ino) {
			w->used = 1;
		} else if (is_dot(es.e[i].name)) {
			if (w != NULL && !w->used)
				e = mfs_set_entry(&r->fs, &dp, es.e[i].off,
				    w->m);
			if (w != NULL)
				w->used = 1;
		} else {
			e = mfs_set_entry(&r->fs, &dp, es.e[i].off, 0);
		}
	}
	free(es.e);
	return e < 0 ? failed(r, path, e) : 0;
}

/* Add the names of *w not yet in directory m, in their order. */
static int
add_entries(struct restore *r, const char *path, uint32_t m,
    struct want *w, size_t n)
{
	struct mfs_inode dp;
	size_t i;
	int e;

	for (i = 0; i < n; i++) {
		if (w[i].used)
			continue;
		/* The directory changes as entries go in: read it anew. */
		if ((e = mfs_get_inode(&r->fs, m, &dp)) < 0 ||
		    (e = mfs_add_entry(&r->fs, &dp, w[i].name, w[i].m)) < 0 ||
		    (e = mfs_put_inode(&r->fs, &dp)) < 0)
			return failed(r, path, e);
	}
	return 0;
}

/* Make directory *d of the dump hold what the dump says, and its mode. */
static int
update_dir(struct restore *r, const struct ddir *d)
{
	struct mfs_inode dp;
	struct want **sorted, *w;
	uint32_t m;
	size_t n;
	int e, rv;

	if ((m = map_get(&r->map, d->h.inumber)) == 0)
		return 0;
	w = xcalloc(d->n + 2, sizeof(*w));
	sorted = xcalloc(d->n + 2, sizeof(*sorted));
	n = wants(r, d, m, w);
	sort_wants(w, n, sorted);
	rv = match_entries(r, d->path, m, sorted, n);
	if (rv == 0)
		rv = add_entries(r, d->path, m, w, n);
	free(sorted);
	free(w);
	if (rv == 0 && (e = mfs_get_inode(&r->fs, m, &dp)) == 0) {
		set_attrs(r, &dp, &d->h);
		e = mfs_put_inode(&r->fs, &dp);
	}
	if (rv == 0 && e < 0)
		rv = failed(r, d->path, e);
	if (rv == 0) {
		r->ndirs++;
		if (r->o->verbose)
			(void)printf("%s\n", d->path);
	}
	return rv;
}

/* A file that restore_files() cannot make: a stand-in loses its names. */
static int
leave_out(struct restore *r, const struct dump_header *h, uint32_t m)
{
	if (r->pending[m] == PENDING)
		r->pending[m] = LEFT_OUT;
	return skip_data(r, h);
}

static void
count_file(struct restore *r, uint16_t mode)
{
	switch (mode & MFS_S_IFMT) {
	case MFS_S_IFLNK:
		r->nsymlinks++;
		break;
	case MFS_S_IFCHR:
	case MFS_S_IFBLK:
		r->ndevs++;
		break;
	case MFS_S_IFIFO:
		r->npipes++;
		break;
	default:
		r->nfiles++;
		break;
	}
}

/*
 * After a file failed: the zones it took go back, and the inode on the
 * disk stops naming the ones it gave up first, so that collect() finds
 * the file as it is.
 */
static void
give_back(struct restore *r, struct mfs_inode *ip, const char *path)
{
	int e;

	if ((e = mfs_truncate(&r->fs, ip)) < 0 ||
	    (e = mfs_put_inode(&r->fs, ip)) < 0)
		(void)failed(r, path, e);
}

/* Restore the file whose TS_INODE header is *h into inode m. */
static int
restore_file(struct restore *r, const struct dump_header *h, uint32_t m)
{
	struct mfs_inode ip;
	struct sink s;
	const char *path;
	char buf[32];
	int e;

	path = path_of(r, h->inumber, buf, sizeof(buf));
	if ((h->mode & MFS_S_IFMT) == MFS_S_IFSOCK) {
		r->sockets++;
		return leave_out(r, h, m);
	}
	if (check_file(r, h, path) == -1)
		return leave_out(r, h, m);
	if ((e = mfs_get_inode(&r->fs, m, &ip)) < 0 ||
	    (e = mfs_truncate(&r->fs, &ip)) < 0) {
		(void)failed(r, path, e);
		return leave_out(r, h, m);
	}
	set_attrs(r, &ip, h);
	(void)memset(&s, 0, sizeof(s));
	s.path = path;
	if (mfs_is_reg(&ip) || mfs_is_lnk(&ip)) {
		s.ip = &ip;
		if (read_contents(r, h, &s) == -1) {
			give_back(r, &ip, path);
			return -1;
		}
		/* A hole at the end is not written; the size says it. */
		ip.size = (uint32_t)h->size;
	} else {
		if (mfs_is_dev(&ip))
			ip.zone[0] = h->rdev;
		if (skip_data(r, h) == -1)
			return -1;
	}
	if (!s.failed && (e = mfs_put_inode(&r->fs, &ip)) < 0)
		s.failed = failed(r, path, e);
	if (s.failed) {
		give_back(r, &ip, path);
		if (r->pending[m] == PENDING)
			r->pending[m] = LEFT_OUT;
		return 0;
	}
	r->pending[m] = 0;
	count_file(r, h->mode);
	if (r->o->verbose)
		(void)printf("%s\n", path);
	return 0;
}

/* Restore the files after the directories, up to TS_END. */
static int
restore_files(struct restore *r)
{
	struct dump_header h;
	uint32_t m;
	int n;

	while ((n = read_header(r, &h)) == 1 && h.type != TS_END) {
		if (is_attrs(&h)) {
			r->attrs++;
		} else if (h.type == TS_INODE &&
		    (h.mode & MFS_S_IFMT) != MFS_S_IFDIR &&
		    (m = map_get(&r->map, h.inumber)) != 0) {
			if (h.extsize > 0)
				r->attrs++;
			if (restore_file(r, &h, m) == -1)
				return -1;
			continue;
		} else if (h.type == TS_INODE) {
			r->unnamed++;
		} else if (h.type == TS_TAPE) {
			problem(r, "%s: more than one volume, which this does "
			    "not read", r->o->file);
			return -1;
		}
		if (skip_data(r, &h) == -1)
			return -1;
	}
	if (n == 0 && !r->cut) {
		problem(r, "%s: ends before TS_END", r->o->file);
		r->cut = 1;
	}
	return n == -1 ? -1 : 0;
}

/*
 * Afterwards: count the names of each inode from the root down, for its
 * link count; remove the names of stand-ins that no file came for; free
 * what restores made that has no name left.
 */
struct walk {
	uint32_t	*count;		/* names of each inode */
	uint32_t	*queue;		/* directories to read */
	size_t		head;
	size_t		tail;
	unsigned char	*reached;
};

static void
walk_entry(struct restore *r, struct walk *wk, const struct mfs_inode *dp,
    const struct mfs_dirent *de)
{
	struct mfs_inode ip;
	char buf[32];
	int e;

	if (de->ino == 0 || de->ino > r->fs.ninodes)
		return;
	if (r->pending[de->ino] != 0) {
		if (r->pending[de->ino] == PENDING && !r->cut)
			problem(r, "%s: not in the dump", path_of(r,
			    r->rev[de->ino], buf, sizeof(buf)));
		if ((e = mfs_set_entry(&r->fs, dp, de->off, 0)) < 0)
			(void)failed(r, r->o->image, e);
		return;
	}
	wk->count[de->ino]++;
	if (wk->reached[de->ino] || is_dot(de->name))
		return;
	wk->reached[de->ino] = 1;
	if (mfs_get_inode(&r->fs, de->ino, &ip) == 0 && mfs_is_dir(&ip))
		wk->queue[wk->tail++] = de->ino;
}

/* Free inode *ip and its zones, and forget the dump inode it was. */
static void
drop(struct restore *r, struct mfs_inode *ip)
{
	uint32_t m;
	int e;

	m = ip->num;
	if (ip->mode == 0)
		return;
	if (r->pending[m] == 0)
		r->nfreed++;
	if ((e = mfs_truncate(&r->fs, ip)) < 0)
		(void)failed(r, r->o->image, e);
	(void)memset(ip, 0, sizeof(*ip));
	ip->num = m;
	if ((e = mfs_put_inode(&r->fs, ip)) < 0 ||
	    (e = mfs_free_inode(&r->fs, m)) < 0)
		(void)failed(r, r->o->image, e);
	if (r->rev[m] != 0)
		map_set(&r->map, r->rev[m], 0);
	r->rev[m] = 0;
	r->ours[m] = 0;
	r->pending[m] = 0;
}

/* Set the link count of inode m, or free it if nothing names it. */
static void
settle(struct restore *r, struct walk *wk, uint32_t m)
{
	struct mfs_inode ip;
	uint32_t max;
	int e;

	if (!wk->reached[m] && !r->ours[m])
		return;
	if ((e = mfs_get_inode(&r->fs, m, &ip)) < 0) {
		(void)failed(r, r->o->image, e);
		return;
	}
	if (wk->reached[m]) {
		max = r->fs.version == 1 ? MAX_LINKS_V1 : MAX_LINKS;
		if (wk->count[m] > max)
			problem(r, "%s: inode %" PRIu32 " has %" PRIu32
			    " names, more than V%d counts", r->o->image, m,
			    wk->count[m], r->fs.version);
		else if (ip.nlinks != wk->count[m]) {
			ip.nlinks = (uint16_t)wk->count[m];
			if ((e = mfs_put_inode(&r->fs, &ip)) < 0)
				(void)failed(r, r->o->image, e);
		}
		return;
	}
	drop(r, &ip);
}

static void
collect(struct restore *r)
{
	struct mfs_inode dp;
	struct entries es;
	struct walk wk;
	uint32_t m;
	size_t i;

	wk.count = xcalloc((size_t)r->fs.ninodes + 1, sizeof(*wk.count));
	wk.queue = xcalloc((size_t)r->fs.ninodes + 1, sizeof(*wk.queue));
	wk.reached = xcalloc((size_t)r->fs.ninodes + 1, 1);
	wk.head = wk.tail = 0;
	wk.queue[wk.tail++] = MFS_ROOT_INO;
	wk.reached[MFS_ROOT_INO] = 1;
	while (wk.head < wk.tail) {
		m = wk.queue[wk.head++];
		if (read_entries(r, m, &es, &dp) == -1)
			continue;
		for (i = 0; i < es.n; i++)
			walk_entry(r, &wk, &dp, &es.e[i]);
		free(es.e);
	}
	for (m = 1; m <= r->fs.ninodes; m++)
		settle(r, &wk, m);
	free(wk.count);
	free(wk.queue);
	free(wk.reached);
}

/*
 * The command.
 */

static void
open_input(struct restore *r)
{
	if (strcmp(r->o->file, "-") == 0) {
		r->in = stdin;
		r->o->file = "standard input";
	} else if ((r->in = fopen(r->o->file, "rb")) == NULL) {
		err(1, "%s", r->o->file);
	}
}

/* Note where the other files start, if the dump can be read again. */
static void
note_files_at(struct restore *r)
{
	struct stat st;

	r->files_at = ftello(r->in);
	if (r->files_at == -1 || fstat(fileno(r->in), &st) == -1 ||
	    !S_ISREG(st.st_mode))
		return;
	if (r->have_next)
		r->files_at -= DUMP_BSIZE;
	r->seekable = 1;
}

static void
open_fs(struct restore *r)
{
	int e;

	e = mfs_open_tracks(&r->fs, r->o->image, !r->o->dry_run,
	    &r->o->tracks);
	if (e == -EINVAL)
		errx(1, "%s: not a MINIX file system", r->o->image);
	if (e < 0)
		errx(1, "%s: %s", r->o->image, strerror(-e));
	if (r->fs.flex)
		errx(1, "%s: the flex directories of Minix-vmd cannot be "
		    "written", r->o->image);
	if (!mfs_is_clean(&r->fs))
		errx(1, "%s: not marked clean; check it with fsck_minixfs",
		    r->o->image);
	r->rev = xcalloc((size_t)r->fs.ninodes + 1, sizeof(*r->rev));
	r->ours = xcalloc((size_t)r->fs.ninodes + 1, 1);
	r->pending = xcalloc((size_t)r->fs.ninodes + 1, 1);
}

/* What the image holds already: the restores before, or nothing. */
static int
start_from(struct restore *r)
{
	char was[32], is[32];
	time_t t;

	if (r->tape.ddate == 0) {
		if (check_empty(r) == -1)
			return -1;
		note_pair(r, DUMP_ROOTINO, MFS_ROOT_INO);
		return 0;
	}
	if (load_symtab(r) == -1)
		return -1;
	if (r->prev_date == r->tape.ddate)
		return 0;
	t = (time_t)r->tape.ddate;
	(void)strftime(was, sizeof(was), "%Y-%m-%d %H:%M:%S", gmtime(&t));
	t = (time_t)r->prev_date;
	(void)strftime(is, sizeof(is), "%Y-%m-%d %H:%M:%S", gmtime(&t));
	problem(r, "%s: follows a dump of %s UTC, but the dump restored "
	    "last is of %s UTC", r->o->file, was, is);
	return -1;
}

static void
summary(struct restore *r)
{
	(void)fprintf(stderr, "%lu directories, %lu files, %lu symbolic "
	    "links, %lu devices, %lu pipes restored; %lu files removed\n",
	    r->ndirs, r->nfiles, r->nsymlinks, r->ndevs, r->npipes,
	    r->nfreed);
	if (r->sockets > 0)
		warnx("warning: %lu sockets left out", r->sockets);
	if (r->attrs > 0)
		warnx("warning: the extended attributes of %lu files left "
		    "out", r->attrs);
	if (r->unknown > 0)
		warnx("warning: %lu names of files that the dump does not "
		    "hold left out", r->unknown);
	if (r->unnamed > 0)
		warnx("warning: %lu files of the dump without a name left "
		    "out", r->unnamed);
}

/*
 * Free the inodes of the files that the dump says are gone before new
 * ones are taken, as check() counts them free.  Their names go when the
 * directories are made to hold what the dump says.  The map may grow as
 * it changes, so the inodes are listed first.
 */
static void
free_gone(struct restore *r)
{
	struct mfs_inode ip;
	uint32_t *gone;
	size_t i, n;
	int e;

	if (r->used == NULL)
		return;
	gone = xcalloc(r->map.max + 1, sizeof(*gone));
	for (i = n = 0; i < r->map.max; i++)
		if (r->map.tab[i].t != 0 && r->map.tab[i].m != 0 &&
		    !map_has(r->used, r->nused, r->map.tab[i].t))
			gone[n++] = r->map.tab[i].m;
	for (i = 0; i < n; i++) {
		if ((e = mfs_get_inode(&r->fs, gone[i], &ip)) < 0)
			(void)failed(r, r->o->image, e);
		else
			drop(r, &ip);
	}
	free(gone);
}

/* Everything -r does after the dump has been checked. */
static void
write_all(struct restore *r)
{
	size_t i;
	int e, synced;

	free_gone(r);
	/* What was written is put in order whatever failed. */
	if (make_inodes(r) == 0) {
		for (i = 0; i < r->dirs.n; i++)
			if (r->dirs.d[i].seen &&
			    update_dir(r, &r->dirs.d[i]) == -1)
				break;
		if (i == r->dirs.n && restore_files(r) == -1)
			r->cut = 1;
	}
	collect(r);
	/*
	 * The clean mark goes back once the rest is on the disk, unless an
	 * operation on the image failed as mfs_failure_breaks() tells: what
	 * the dump
	 * lacked or the room refused is put in order, but an error of the
	 * device may leave anything.
	 */
	if ((synced = mfs_sync(&r->fs)) < 0)
		(void)failed(r, r->o->image, synced);
	else if (fsync(r->fs.fd) == -1)
		(void)failed(r, r->o->image, -errno);
	else if (!r->broken && (e = mfs_mark_clean(&r->fs, 1)) < 0)
		(void)failed(r, r->o->image, e);
	else if (!r->broken && fsync(r->fs.fd) == -1)
		(void)failed(r, r->o->image, -errno);
	if (r->broken)
		warnx("warning: %s is left marked not clean; check it with "
		    "fsck_minixfs -y", r->o->image);
	if (synced < 0)
		return;
	/*
	 * The table follows the image whatever failed, but the next
	 * incremental dump follows only a whole restore: after a failure it
	 * is this dump that comes next again.
	 */
	if (r->cut || r->status != 0) {
		warnx("warning: not all of %s was restored; restore it again "
		    "once what failed is put right", r->o->file);
		(void)save_symtab(r, r->tape.ddate);
	} else {
		(void)save_symtab(r, r->tape.date);
	}
}

static void
run_restore(struct restore *r)
{
	struct ddir *root;
	int e;

	open_fs(r);
	if (read_start(r) == -1 || start_from(r) == -1 || read_dirs(r) == -1)
		return;
	if ((root = ddir_find(&r->dirs, DUMP_ROOTINO)) == NULL) {
		problem(r, "%s: the dump holds no root directory",
		    r->o->file);
		return;
	}
	name_dirs(r, root, DUMP_ROOTINO, ".", 0);
	note_files_at(r);
	if (check(r) == -1)
		return;
	if (r->o->dry_run) {
		(void)fprintf(stderr, "%s: %zu directories and the files "
		    "they name can be restored\n", r->o->image, r->dirs.n);
		return;
	}
	/* The clean mark is away while the image is written. */
	if ((e = mfs_mark_in_use(&r->fs)) < 0) {
		problem(r, "%s: %s", r->o->image, strerror(-e));
		return;
	}
	write_all(r);
	summary(r);
}

static void
restore_free(struct restore *r)
{
	size_t i;

	for (i = 0; i < r->dirs.n; i++)
		ddir_free(&r->dirs.d[i]);
	free(r->dirs.d);
	free(r->dirs.idx);
	for (i = 0; i < r->npaths; i++)
		free(r->pathv[i]);
	free(r->pathv);
	free(r->paths.tab);
	free(r->map.tab);
	free(r->used);
	free(r->dumped);
	free(r->rev);
	free(r->ours);
	free(r->pending);
	if (r->in != NULL && r->in != stdin)
		(void)fclose(r->in);
}

int
main(int argc, char **argv)
{
	struct options o;
	struct restore r;

	parse(argc, argv, &o);
	(void)memset(&r, 0, sizeof(r));
	r.o = &o;
	open_input(&r);
	if (o.list) {
		if (read_start(&r) == 0 && read_dirs(&r) == 0)
			list(&r);
	} else {
		run_restore(&r);
		mfs_close(&r.fs);
	}
	if (r.skipped > 0)
		problem(&r, "%s: %lu records that were not headers skipped",
		    o.file, r.skipped);
	restore_free(&r);
	return r.status;
}
