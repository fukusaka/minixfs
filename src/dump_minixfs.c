/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * dump_minixfs - dump a MINIX file system image in the format of BSD dump.
 *
 *	dump_minixfs [-0123456789u] [-D dumpdates] [-L label]
 *	    [-M SIZE:HEADS:SIDE] [-T date] [-W 8|16|32|64] -f file image
 *	    [path]
 *
 * The dump is the one NetBSD dump writes for UFS1 (see dumpfmt.h), in the
 * byte order of the image, so that the restore of NetBSD, FreeBSD and
 * Linux read it as well as restore_minixfs.  Inode n of the image is inode
 * n + 1 in the dump, since the restore of BSD takes 2 for the root, and
 * directories go in as those of 4.4BSD.  The image is only read.
 *
 *	-0 .. -9 the level: a dump of level n holds what changed since the
 *		last dump of a lower level, as the dumpdates file tells;
 *		level 0, the default, holds everything
 *	-u	note the dump in the dumpdates file when it is done
 *	-D	the dumpdates file, /etc/dumpdates by default; its names
 *		are those of the images
 *	-L	the label of the dump
 *	-M	an image that holds one side of a disk, as for minixfs(1)
 *	-T	the date to dump what changed since, as ctime(3) prints it,
 *		instead of one from the dumpdates file
 *	-W	the bits in a word of the bit maps of a big-endian image
 *	-f	where the dump goes; "-" is standard output
 *
 * With path, only that directory and what is below it go in, with the
 * directories above it, at level 0 and without -u, as NetBSD dump does
 * for a subdirectory.  A file changed if its mtime or ctime is not
 * before the date; V1 keeps no ctime, so a change of mode, owner or name
 * alone escapes a dump of a higher level.  As in BSD dump, the directories
 * that hold a file of the dump go in, and those that have directories in
 * them.  Exit status: 0 on success, 1 if anything failed, 2 for a usage
 * error.
 */

#include "compat.h"

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

#define DUMPDATES	"/etc/dumpdates"
#define DATES_NAME	511		/* longest name in dumpdates */
#define DATES_LINE	(DATES_NAME + 64)
#define NO_LABEL	"none"		/* as NetBSD dump labels */
#define MAX_DEPTH	1024		/* deepest directory followed */

/* What the command line asks for. */
struct options {
	struct mfs_tracks tracks;	/* -M */
	uint32_t	map_word;	/* -W, or 0 */
	const char	*file;		/* -f */
	const char	*image;
	const char	*path;		/* only this directory */
	const char	*dates;		/* -D */
	const char	*label;		/* -L */
	int64_t		since;		/* -T, or -1 */
	int		level;
	int		update;		/* -u */
};

/* One dump. */
struct dump {
	struct options	*o;
	struct mfs	fs;
	FILE		*out;
	int		status;
	struct dump_header h;		/* the header being written */
	unsigned char	*used;		/* TS_CLRI: inodes in use */
	unsigned char	*dirs;		/* directories that go in */
	unsigned char	*files;		/* TS_BITS: inodes that go in */
	size_t		mapsize;	/* bytes of a map */
	uint32_t	maxino;		/* one past the last inode */
	unsigned long	ndirs;
	unsigned long	nfiles;
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: dump_minixfs [-0123456789u] [-D dumpdates] [-L label]\n"
	    "           [-M SIZE:HEADS:SIDE] [-T date] [-W 8|16|32|64] "
	    "-f file image [path]\n");
	exit(2);
}

static void
problem(struct dump *d, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarnx(fmt, ap);
	va_end(ap);
	d->status = 1;
}

/* The time now; a dump cannot be dated without it. */
static int64_t
now(void)
{
	int64_t t;

	if ((t = compat_now()) < 0)
		err(1, "clock_gettime");
	return t;
}

/* Seconds from the epoch to a time of the Gregorian calendar, in UTC. */
static int64_t
utc_seconds(const struct tm *tm)
{
	int64_t days, era, y, yoe, doy, doe, m;

	/* days_from_civil() of Howard Hinnant, with March first. */
	m = tm->tm_mon + 1;
	y = (int64_t)tm->tm_year + 1900 - (m <= 2);
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + tm->tm_mday - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	days = era * 146097 + doe - 719468;
	return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 +
	    tm->tm_sec;
}

/*
 * The time that *tm gives in local time.  It works mktime(3) out from
 * localtime(3), since mktime(3) returns nonsense under the
 * AddressSanitizer of NetBSD/i386.  A time that a change of the clock
 * skips or repeats comes out as a time next to it.
 */
static int64_t
local_seconds(const struct tm *tm)
{
	const struct tm *lt;
	int64_t t, u;
	time_t tt;
	int i;

	u = utc_seconds(tm);
	t = u;
	for (i = 0; i < 2; i++) {
		tt = (time_t)t;
		if ((lt = localtime(&tt)) == NULL)
			return -1;
		t = u - (utc_seconds(lt) - t);
	}
	return t;
}

/*
 * A date as ctime(3) prints it, "Sat Jan  1 00:00:00 2000", in local
 * time, or -1, also for a date before the epoch.
 */
static int64_t
parse_date(const char *s)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char day[4], mon[4], rest[2];
	const char *m;
	struct tm tm;

	(void)memset(&tm, 0, sizeof(tm));
	if (sscanf(s, "%3s %3s %d %d:%d:%d %d %1s", day, mon, &tm.tm_mday,
	    &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &tm.tm_year, rest) != 7 ||
	    strlen(mon) != 3 || (m = strstr(months, mon)) == NULL ||
	    (m - months) % 3 != 0 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
	    tm.tm_hour < 0 || tm.tm_hour > 23 || tm.tm_min < 0 ||
	    tm.tm_min > 59 || tm.tm_sec < 0 || tm.tm_sec > 60 ||
	    tm.tm_year < 1970 || tm.tm_year > 9999)
		return -1;
	tm.tm_mon = (int)((m - months) / 3);
	tm.tm_year -= 1900;
	return local_seconds(&tm);
}

static void
parse(int argc, char **argv, struct options *o)
{
	int ch;

	(void)memset(o, 0, sizeof(*o));
	o->dates = DUMPDATES;
	o->label = NO_LABEL;
	o->since = -1;
	while ((ch = getopt(argc, argv, "0123456789D:f:L:M:T:uW:")) != -1) {
		switch (ch) {
		case 'D':
			o->dates = optarg;
			break;
		case 'f':
			o->file = optarg;
			break;
		case 'L':
			o->label = optarg;
			break;
		case 'M':
			if (mfs_parse_tracks(optarg, &o->tracks) < 0)
				usage();
			break;
		case 'W':
			if (mfs_parse_map_word(optarg, &o->map_word) < 0)
				usage();
			break;
		case 'T':
			if ((o->since = parse_date(optarg)) < 0)
				errx(2, "%s: not a date as ctime(3) prints "
				    "it", optarg);
			break;
		case 'u':
			o->update = 1;
			break;
		default:
			if (ch < '0' || ch > '9')
				usage();
			o->level = ch - '0';
			break;
		}
	}
	argc -= optind;
	argv += optind;
	if (o->file == NULL || argc < 1 || argc > 2)
		usage();
	o->image = argv[0];
	o->path = argc == 2 ? argv[1] : NULL;
}

/*
 * The dumpdates file, in the form of BSD dump: a line for each image and
 * level, the name padded to DATES_NAME characters, the level and the date
 * as ctime(3) prints it.  The name is the first word of the line, as BSD
 * reads it, so that the blanks, newlines and backslashes of a path are
 * written as a backslash and three octal digits.
 */

/*
 * The name of image as the dumpdates file holds it, into buf of
 * DATES_NAME + 1 bytes.  Returns 0, or -1 if it does not fit.
 */
static int
dates_name(const char *image, char *buf)
{
	const unsigned char *p;
	size_t n;

	n = 0;
	for (p = (const unsigned char *)image; *p != '\0'; p++) {
		if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\\') {
			if (n + 4 > DATES_NAME)
				return -1;
			(void)snprintf(buf + n, 5, "\\%03o", *p);
			n += 4;
		} else {
			if (n + 1 > DATES_NAME)
				return -1;
			buf[n++] = (char)*p;
		}
	}
	buf[n] = '\0';
	return 0;
}

/*
 * Take a line of the dumpdates file apart: the name, into name of
 * DATES_NAME + 1 bytes, the level and the date.  Returns 0, or -1 for a
 * line of another form.
 */
static int
dates_line(const char *line, char *name, int *level, int64_t *t)
{
	char date[64], lv;

	/* 511 is DATES_NAME. */
	if (sscanf(line, "%511s %c %63[^\n]", name, &lv, date) != 3 ||
	    lv < '0' || lv > '9' || (*t = parse_date(date)) < 0)
		return -1;
	*level = lv - '0';
	return 0;
}

/* The date of the last dump of image of a lower level, or 0. */
static int64_t
last_date(struct dump *d, int *lastlevel)
{
	char line[DATES_LINE], name[DATES_NAME + 1], want[DATES_NAME + 1];
	int64_t best, t;
	FILE *fp;
	int lv;

	best = 0;
	*lastlevel = -1;
	if (dates_name(d->o->image, want) == -1)
		return 0;
	if ((fp = fopen(d->o->dates, "r")) == NULL) {
		if (errno != ENOENT || d->o->level > 0)
			warn("%s", d->o->dates);
		return 0;
	}
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (dates_line(line, name, &lv, &t) < 0 ||
		    strcmp(name, want) != 0 || lv >= d->o->level || t <= best)
			continue;
		best = t;
		*lastlevel = lv;
	}
	(void)fclose(fp);
	return best;
}

/* Write the dumpdates file anew with this dump in it. */
static void
note_date(struct dump *d)
{
	char line[DATES_LINE], name[DATES_NAME + 1], want[DATES_NAME + 1];
	char tmp[PATH_MAX];
	FILE *in, *out;
	int64_t when;
	time_t t;
	int lv;

	if (dates_name(d->o->image, want) == -1) {
		problem(d, "%s: the name is too long for %s", d->o->image,
		    d->o->dates);
		return;
	}
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", d->o->dates) >=
	    sizeof(tmp) || (out = fopen(tmp, "w")) == NULL) {
		problem(d, "%s: cannot be written", d->o->dates);
		return;
	}
	if ((in = fopen(d->o->dates, "r")) != NULL) {
		while (fgets(line, sizeof(line), in) != NULL) {
			if (dates_line(line, name, &lv, &when) == 0 &&
			    strcmp(name, want) == 0 && lv == d->o->level)
				continue;
			(void)fputs(line, out);
		}
		(void)fclose(in);
	}
	t = (time_t)d->h.date;
	(void)fprintf(out, "%-*s %c %s", DATES_NAME, want, '0' + d->o->level,
	    ctime(&t));
	if (fflush(out) == EOF || ferror(out) || fclose(out) == EOF ||
	    rename(tmp, d->o->dates) == -1)
		problem(d, "%s: %s", d->o->dates, strerror(errno));
}

/*
 * The maps, by inode of the dump: bit t - 1 for inode t, which is inode
 * t - 1 of the image.
 */

static void
set_bit(unsigned char *map, uint32_t t)
{
	map[(t - 1) / 8] |= (unsigned char)(1 << ((t - 1) % 8));
}

static void
clear_bit(unsigned char *map, uint32_t t)
{
	map[(t - 1) / 8] &= (unsigned char)~(1 << ((t - 1) % 8));
}

static int
has_bit(const unsigned char *map, uint32_t t)
{
	return (map[(t - 1) / 8] >> ((t - 1) % 8)) & 1;
}

/* Whether the inode changed since the last dump of a lower level. */
static int
changed(const struct dump *d, const struct mfs_inode *ip)
{
	return (int64_t)ip->mtime >= d->h.ddate ||
	    (int64_t)ip->ctime >= d->h.ddate;
}

/* Put inode m of the image in the maps: in use, and in the dump if so. */
static void
mark(struct dump *d, const struct mfs_inode *ip)
{
	uint32_t t;

	t = ip->num + 1;
	set_bit(d->used, t);
	if (mfs_is_dir(ip))
		set_bit(d->dirs, t);
	if (changed(d, ip))
		set_bit(d->files, t);
}

/* Pass I over the whole file system: every inode in use. */
static void
mark_all(struct dump *d)
{
	struct mfs_inode ip;
	unsigned char *imap;
	uint32_t m;
	int e;

	if ((e = mfs_load_map(&d->fs, MFS_IMAP, &imap)) < 0)
		errx(1, "%s: %s", d->o->image, strerror(-e));
	for (m = 1; m <= d->fs.ninodes; m++) {
		if (!mfs_map_bit(&d->fs, imap, m) ||
		    mfs_get_inode(&d->fs, m, &ip) < 0 || ip.mode == 0)
			continue;
		mark(d, &ip);
	}
	free(imap);
}

/* The entries of a directory of the image. */
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
read_entries(struct dump *d, const struct mfs_inode *dp, struct entries *es)
{
	int e;

	(void)memset(es, 0, sizeof(*es));
	if ((e = mfs_readdir(&d->fs, dp, collect_fn, es)) < 0) {
		problem(d, "%s: inode %" PRIu32 ": %s", d->o->image, dp->num,
		    strerror(-e));
		free(es->e);
		es->e = NULL;
		es->n = 0;
		return -1;
	}
	return 0;
}

static int
is_dot(const char *name)
{
	return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* Mark directory *dp and everything below it, for a dump of a subtree. */
static void
mark_below(struct dump *d, const struct mfs_inode *dp, int depth)
{
	struct mfs_inode ip;
	struct entries es;
	size_t i;

	mark(d, dp);
	if (depth == MAX_DEPTH || read_entries(d, dp, &es) == -1)
		return;
	for (i = 0; i < es.n; i++) {
		if (is_dot(es.e[i].name) || es.e[i].ino == 0 ||
		    es.e[i].ino > d->fs.ninodes ||
		    has_bit(d->used, es.e[i].ino + 1) ||
		    mfs_get_inode(&d->fs, es.e[i].ino, &ip) < 0 ||
		    ip.mode == 0)
			continue;
		if (mfs_is_dir(&ip))
			mark_below(d, &ip, depth + 1);
		else
			mark(d, &ip);
	}
	free(es.e);
}

/* Pass I for a subtree: it, and the directories on the way to it. */
static void
mark_subtree(struct dump *d)
{
	struct mfs_inode ip;
	const char *p;
	char *part;
	size_t len;
	int e;

	if ((e = mfs_namei(&d->fs, d->o->path, &ip)) < 0)
		errx(1, "%s:%s: %s", d->o->image, d->o->path, strerror(-e));
	if (!mfs_is_dir(&ip))
		errx(1, "%s:%s: not a directory", d->o->image, d->o->path);
	mark_below(d, &ip, 0);
	for (p = d->o->path; *p != '\0'; p++) {
		if (*p != '/' || p == d->o->path)
			continue;
		len = (size_t)(p - d->o->path);
		if ((part = strndup(d->o->path, len)) == NULL)
			err(1, NULL);
		if (mfs_namei(&d->fs, part, &ip) == 0)
			mark(d, &ip);
		free(part);
	}
	if (mfs_get_inode(&d->fs, MFS_ROOT_INO, &ip) == 0)
		mark(d, &ip);
}

/*
 * Pass II, as mapdirs() of BSD dump: a directory that holds a file of the
 * dump goes in; one that has no directories left in it and did not
 * change stays out.  Repeat until nothing changes.
 */
static int
prune_once(struct dump *d)
{
	struct mfs_inode dp;
	struct entries es;
	uint32_t m, t;
	size_t i;
	int changes, has_file, has_dir;

	changes = 0;
	for (m = 1; m <= d->fs.ninodes; m++) {
		t = m + 1;
		if (!has_bit(d->dirs, t) || has_bit(d->files, t) ||
		    mfs_get_inode(&d->fs, m, &dp) < 0 ||
		    read_entries(d, &dp, &es) == -1)
			continue;
		has_file = has_dir = 0;
		for (i = 0; i < es.n; i++) {
			if (is_dot(es.e[i].name) || es.e[i].ino == 0 ||
			    es.e[i].ino > d->fs.ninodes)
				continue;
			has_file |= has_bit(d->files, es.e[i].ino + 1);
			has_dir |= has_bit(d->dirs, es.e[i].ino + 1);
		}
		free(es.e);
		if (has_file) {
			set_bit(d->files, t);
			changes = 1;
		} else if (!has_dir) {
			clear_bit(d->dirs, t);
			changes = 1;
		}
	}
	return changes;
}

/*
 * Writing the dump.
 */

static void
put_record(struct dump *d, const unsigned char *rec)
{
	if (fwrite(rec, 1, DUMP_BSIZE, d->out) != DUMP_BSIZE)
		err(1, "%s", d->o->file);
	d->h.tapea++;
}

/* Write the header in d->h, of type type, for inode t. */
static void
put_header(struct dump *d, int32_t type, uint32_t t)
{
	unsigned char rec[DUMP_BSIZE];

	d->h.type = type;
	d->h.inumber = t;
	dump_put_header(&d->h, rec);
	put_record(d, rec);
}

static void
put_map(struct dump *d, int32_t type, const unsigned char *map,
    uint32_t t)
{
	size_t i;

	d->h.count = (int32_t)(d->mapsize / DUMP_BSIZE);
	(void)memset(d->h.addr, 0, sizeof(d->h.addr));
	put_header(d, type, t);
	for (i = 0; i < d->mapsize; i += DUMP_BSIZE)
		put_record(d, map + i);
}

/* Where the records of a file come from: memory, or the image. */
struct source {
	const unsigned char *buf;	/* a directory, or NULL */
	const struct mfs_inode *ip;
	uint64_t	size;
	uint32_t	cached;		/* block of the image in blk, or 0 */
	unsigned char	*blk;
};

/* The block of the image that holds record n of a file; 0 for a hole. */
static uint32_t
record_block(struct dump *d, const struct source *s, uint64_t n)
{
	uint32_t block;
	int e;

	if (s->buf != NULL)
		return 1;
	e = mfs_bmap(&d->fs, s->ip,
	    (uint32_t)(n * DUMP_BSIZE / d->fs.block_size), &block);
	if (e < 0) {
		problem(d, "%s: inode %" PRIu32 ": %s", d->o->image,
		    s->ip->num, strerror(-e));
		return 0;
	}
	return block;
}

/* Record n of a file, which is not a hole, into rec; 0 if it fails. */
static int
get_record(struct dump *d, struct source *s, uint64_t n, unsigned char *rec)
{
	uint64_t off;
	uint32_t block;
	size_t len;
	int e;

	off = n * DUMP_BSIZE;
	len = s->size - off < DUMP_BSIZE ? (size_t)(s->size - off) :
	    DUMP_BSIZE;
	(void)memset(rec, 0, DUMP_BSIZE);
	if (s->buf != NULL) {
		(void)memcpy(rec, s->buf + off, len);
		return 1;
	}
	if ((block = record_block(d, s, n)) == 0)
		return 0;
	if (block != s->cached) {
		if ((e = mfs_read_block(&d->fs, block, s->blk)) < 0) {
			problem(d, "%s: block %" PRIu32 ": %s", d->o->image,
			    block, strerror(-e));
			return 0;
		}
		s->cached = block;
	}
	(void)memcpy(rec, s->blk + off % d->fs.block_size, len);
	return 1;
}

/*
 * Write the inode in d->h with the records of s: a TS_INODE header and
 * as many TS_ADDR ones as it takes, each followed by the records that
 * are not holes.
 */
static void
put_contents(struct dump *d, uint32_t t, struct source *s)
{
	unsigned char rec[DUMP_BSIZE];
	uint64_t i, n, nrec;
	int32_t type;
	uint64_t k;

	nrec = (s->size + DUMP_BSIZE - 1) / DUMP_BSIZE;
	type = TS_INODE;
	i = 0;
	do {
		n = nrec - i < DUMP_NADDR ? nrec - i : DUMP_NADDR;
		(void)memset(d->h.addr, 0, sizeof(d->h.addr));
		/*
		 * UFS always has the last block of a file, and the restore
		 * of BSD makes a file only as long as its last record of
		 * data: the last record goes in even if it is a hole.
		 */
		for (k = 0; k < n; k++)
			d->h.addr[k] = i + k == nrec - 1 ||
			    record_block(d, s, i + k) != 0;
		d->h.count = (int32_t)n;
		put_header(d, type, t);
		/* A record that cannot be read goes in as zeros. */
		for (k = 0; k < n; k++) {
			if (!d->h.addr[k])
				continue;
			if (!get_record(d, s, i + k, rec))
				(void)memset(rec, 0, sizeof(rec));
			put_record(d, rec);
		}
		type = TS_ADDR;
		i += n;
	} while (i < nrec);
}

/* The type of a directory entry of 4.4BSD for inode m of the image. */
static unsigned
entry_type(struct dump *d, uint32_t m)
{
	struct mfs_inode ip;

	if (mfs_get_inode(&d->fs, m, &ip) < 0)
		return DUMP_DT_UNKNOWN;
	switch (ip.mode & MFS_S_IFMT) {
	case MFS_S_IFDIR:
		return DUMP_DT_DIR;
	case MFS_S_IFREG:
		return DUMP_DT_REG;
	case MFS_S_IFLNK:
		return DUMP_DT_LNK;
	case MFS_S_IFCHR:
		return DUMP_DT_CHR;
	case MFS_S_IFBLK:
		return DUMP_DT_BLK;
	case MFS_S_IFIFO:
		return DUMP_DT_FIFO;
	case MFS_S_IFSOCK:
		return DUMP_DT_SOCK;
	default:
		return DUMP_DT_UNKNOWN;
	}
}

/* Make room for a chunk more at the end of the directory in *buf. */
static void
grow(unsigned char **buf, size_t *max, size_t need)
{
	unsigned char *p;

	if (need <= *max)
		return;
	if ((p = realloc(*buf, *max + DUMP_DIRBLK)) == NULL)
		err(1, NULL);
	(void)memset(p + *max, 0, DUMP_DIRBLK);
	*buf = p;
	*max += DUMP_DIRBLK;
}

/*
 * The entries of directory *dp as those of 4.4BSD, in chunks of
 * DUMP_DIRBLK bytes: the last entry of a chunk reaches its end.
 */
static unsigned char *
convert_dir(struct dump *d, const struct mfs_inode *dp, size_t *sizep)
{
	struct entries es;
	unsigned char *buf;
	size_t i, last, len, loc, max, reclen;

	*sizep = 0;
	if (read_entries(d, dp, &es) == -1)
		return NULL;
	buf = NULL;
	max = 0;
	loc = last = 0;
	for (i = 0; i < es.n; i++) {
		if (es.e[i].ino == 0 || es.e[i].ino > d->fs.ninodes)
			continue;
		len = strlen(es.e[i].name);
		reclen = dump_dirent_size(len);
		if (loc > 0 && loc % DUMP_DIRBLK + reclen > DUMP_DIRBLK) {
			/* The entry before takes the rest of the chunk. */
			loc += DUMP_DIRBLK - loc % DUMP_DIRBLK;
			dump_set_reclen(buf + last, d->fs.order, loc - last);
		}
		grow(&buf, &max, loc + reclen);
		last = loc;
		dump_put_dirent(buf + loc, d->fs.order, es.e[i].ino + 1,
		    reclen, is_dot(es.e[i].name) ? DUMP_DT_DIR :
		    entry_type(d, es.e[i].ino), es.e[i].name, len);
		loc += reclen;
	}
	free(es.e);
	if (loc % DUMP_DIRBLK != 0) {
		loc += DUMP_DIRBLK - loc % DUMP_DIRBLK;
		dump_set_reclen(buf + last, d->fs.order, loc - last);
	}
	*sizep = loc;
	return buf;
}

/* Fill the inode of d->h from *ip. */
static void
set_inode(struct dump *d, const struct mfs_inode *ip)
{
	d->h.mode = ip->mode;
	d->h.nlink = ip->nlinks;
	d->h.size = ip->size;
	d->h.atime = ip->atime;
	d->h.mtime = ip->mtime;
	d->h.ctime = ip->ctime;
	d->h.uid = ip->uid;
	d->h.gid = ip->gid;
	d->h.rdev = mfs_is_dev(ip) ? mfs_rdev(ip) : 0;
}

/* Write inode m of the image, as inode m + 1 of the dump. */
static void
dump_inode(struct dump *d, uint32_t m)
{
	struct mfs_inode ip;
	struct source s;
	size_t size;
	unsigned char *dir;
	int e;

	if ((e = mfs_read_inode(&d->fs, m, &ip)) < 0) {
		problem(d, "%s: inode %" PRIu32 ": %s", d->o->image, m,
		    strerror(-e));
		return;
	}
	set_inode(d, &ip);
	(void)memset(&s, 0, sizeof(s));
	s.ip = &ip;
	dir = NULL;
	if (mfs_is_dir(&ip)) {
		dir = convert_dir(d, &ip, &size);
		s.buf = dir;
		s.size = d->h.size = size;
		d->ndirs++;
	} else if (mfs_is_reg(&ip) || mfs_is_lnk(&ip)) {
		s.size = ip.size;
		if ((s.blk = malloc(d->fs.block_size)) == NULL)
			err(1, NULL);
		d->nfiles++;
	} else {
		d->nfiles++;
	}
	put_contents(d, m + 1, &s);
	free(s.blk);
	free(dir);
}

/* Write the dump: the header, the maps, directories, files, the end. */
static void
write_dump(struct dump *d)
{
	uint32_t m;

	d->h.flags = DR_NEWHEADER | DR_NEWINODEFMT;
	d->h.count = 1;
	put_header(d, TS_TAPE, 0);
	d->h.flags = DR_NEWINODEFMT;
	put_map(d, TS_CLRI, d->used, d->maxino - 1);
	put_map(d, TS_BITS, d->files, DUMP_ROOTINO);
	for (m = 1; m <= d->fs.ninodes; m++)
		if (has_bit(d->dirs, m + 1))
			dump_inode(d, m);
	for (m = 1; m <= d->fs.ninodes; m++)
		if (has_bit(d->files, m + 1) && !has_bit(d->dirs, m + 1) &&
		    has_bit(d->used, m + 1))
			dump_inode(d, m);
	(void)memset(d->h.addr, 0, sizeof(d->h.addr));
	d->h.count = 0;
	d->h.mode = 0;
	d->h.size = 0;
	do
		put_header(d, TS_END, d->maxino - 1);
	while (d->h.tapea % DUMP_NTREC != 0);
}

/* Tell which dates the dump goes between, as BSD dump does. */
static void
print_dates(const struct dump *d, int lastlevel)
{
	time_t t;

	t = (time_t)d->h.date;
	(void)fprintf(stderr, "DUMP: Date of this level %d dump: %s",
	    d->o->level, ctime(&t));
	t = (time_t)d->h.ddate;
	(void)fprintf(stderr, "DUMP: Date of last level %d dump: %s",
	    lastlevel, d->h.ddate == 0 ? "the epoch\n" : ctime(&t));
}

/* The header that every record of the dump starts from. */
static void
start_header(struct dump *d)
{
	int lastlevel;

	d->h.order = d->fs.order;
	d->h.volume = 1;
	d->h.level = d->o->level;
	d->h.date = now();
	lastlevel = 0;
	if (d->o->since >= 0)
		d->h.ddate = d->o->since;
	else if (d->o->level > 0)
		d->h.ddate = last_date(d, &lastlevel);
	(void)snprintf(d->h.label, sizeof(d->h.label), "%s", d->o->label);
	(void)snprintf(d->h.filesys, sizeof(d->h.filesys), "%s",
	    d->o->path != NULL ? d->o->path : "/");
	(void)snprintf(d->h.dev, sizeof(d->h.dev), "%s", d->o->image);
	if (gethostname(d->h.host, sizeof(d->h.host)) == -1)
		d->h.host[0] = '\0';
	d->h.host[sizeof(d->h.host) - 1] = '\0';
	print_dates(d, lastlevel);
}

static void
open_output(struct dump *d)
{
	if (strcmp(d->o->file, "-") == 0) {
		if (isatty(STDOUT_FILENO))
			errx(1, "standard output is a terminal");
		d->out = stdout;
		d->o->file = "standard output";
	} else if ((d->out = fopen(d->o->file, "wb")) == NULL) {
		err(1, "%s", d->o->file);
	}
}

/* Take the maps, and find what goes in. */
static void
map_inodes(struct dump *d)
{
	d->maxino = d->fs.ninodes + 2;
	d->mapsize = ((d->maxino + 7) / 8 + DUMP_BSIZE - 1) / DUMP_BSIZE *
	    DUMP_BSIZE;
	if ((d->used = calloc(d->mapsize, 1)) == NULL ||
	    (d->dirs = calloc(d->mapsize, 1)) == NULL ||
	    (d->files = calloc(d->mapsize, 1)) == NULL)
		err(1, NULL);
	if (d->o->path != NULL)
		mark_subtree(d);
	else
		mark_all(d);
	/* The restore of BSD cannot do without the root. */
	set_bit(d->files, DUMP_ROOTINO);
	while (prune_once(d))
		continue;
}

int
main(int argc, char **argv)
{
	struct options o;
	struct dump d;
	int e;

	parse(argc, argv, &o);
	(void)memset(&d, 0, sizeof(d));
	d.o = &o;
	if ((e = mfs_open_tracks(&d.fs, o.image, 0, &o.tracks)) < 0)
		errx(1, "%s: %s", o.image, e == -EINVAL ?
		    "not a MINIX file system" : strerror(-e));
	if (o.map_word != 0)
		mfs_set_map_word(&d.fs, o.map_word);
	if (!mfs_is_clean(&d.fs))
		warnx("warning: %s is not marked clean; the dump may not be "
		    "consistent", o.image);
	if (o.path != NULL && (o.level > 0 || o.update)) {
		warnx("a directory is dumped at level 0, without -u");
		o.level = 0;
		o.update = 0;
	}
	start_header(&d);
	map_inodes(&d);
	open_output(&d);
	write_dump(&d);
	if (fflush(d.out) == EOF || ferror(d.out) ||
	    (d.out != stdout && fclose(d.out) == EOF))
		err(1, "%s", o.file);
	(void)fprintf(stderr, "DUMP: %lu directories, %lu files, %jd "
	    "records\n", d.ndirs, d.nfiles, (intmax_t)d.h.tapea);
	if (o.update && d.status == 0)
		note_date(&d);
	mfs_close(&d.fs);
	free(d.used);
	free(d.dirs);
	free(d.files);
	return d.status;
}
