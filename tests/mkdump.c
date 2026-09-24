/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mkdump - write dumps in the formats of BSD and Linux for the test suite.
 *
 *	mkdump [-arxz] [-B le|be] [-d new|old|v7] [-h ofs|nfs|ufs2] SPEC DUMP
 *
 * This is test scaffolding.  It shares no code with src/, so that
 * restore_minixfs is checked against an independent writer.  SPEC is a
 * spec of mkimage; its fs line is ignored, and "raw" is not taken.  One
 * more directive makes a socket:
 *
 *	sock PATH MODE UID GID MTIME
 *
 * The root is inode 2 and the others follow in the order of the spec, as
 * a full dump of level 0 holds them all.  Atime is MTIME + 1 and ctime
 * MTIME + 2.  The last record of a regular file is data even inside a
 * hole, as UFS keeps its last block, except with -r, since ext2 need
 * not.
 *
 *	-h	the header: that of a file system older than 4.2BSD
 *		(ofs), that of FFS and UFS1 (nfs, the default) or that of
 *		UFS2
 *	-d	directory entries of 4.4BSD (new, the default), of 4.2BSD
 *		and 4.3BSD (old, which also keeps ids where they keep them
 *		in the nfs header) or of V7 (v7, which ofs has)
 *	-B	the byte order, little-endian by default
 *	-r	c_addr in runs, as Linux dump since 0.4b49 writes it
 *	-a	extended attributes of Linux after each regular file
 *	-x	extended attributes of UFS2 after each regular file
 *	-z	mark the dump compressed, as Linux dump does with -z
 */

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BSIZE		1024		/* a record */
#define NTREC		10
#define NADDR		512		/* c_addr of a new header */
#define OLD_NADDR	256
#define ROOTINO		2
#define DIRBLK_NEW	1024		/* chunks of 4.4BSD directories */
#define DIRBLK_OLD	512		/* and of 4.2BSD ones */
#define V7_ENTRY	16
#define V7_NAME		14
#define MAX_NAME	255
#define MAX_HOLES	8
#define ATTR_SIZE	100		/* bytes of extended attributes */
#define DUMP_DATE	1000000000	/* 2001-09-09 01:46:40 UTC */
#define LINUX_RUN	64		/* longest run in one byte */
#define LINUX_MAXRUN	16383		/* longest in two */

#define OFS_MAGIC	60011
#define NFS_MAGIC	60012
#define UFS2_MAGIC	0x19540119
#define CHECKSUM	84446

#define TS_TAPE		1
#define TS_INODE	2
#define TS_BITS		3
#define TS_ADDR		4
#define TS_END		5
#define TS_CLRI		6

#define DR_NEWHEADER	0x0001
#define DR_NEWINODEFMT	0x0002
#define DR_COMPRESSED	0x0080
#define DR_EXTATTRIBUTES 0x8000

/* A new header (NFS_MAGIC, UFS2_MAGIC): byte offsets. */
#define H_TYPE		0
#define H_OLD_DATE	4
#define H_VOLUME	12
#define H_OLD_TAPEA	16
#define H_INUMBER	20
#define H_MAGIC		24
#define H_CHECKSUM	28
#define H_INODE		32
#define H_COUNT		160
#define H_ADDR		164
#define H_LABEL		676
#define H_LEVEL		692
#define H_FLAGS		888
#define H_DATE		896
#define H_TAPEA		912

/* Its inode, from H_INODE. */
#define I_MODE		0
#define I_NLINK		2
#define I_OLDUID	4
#define I_OLDGID	6
#define I_SIZE		8
#define I_ATIME		16
#define I_MTIME		24
#define I_CTIME		32
#define I_RDEV		40
#define I2_ATIME	56
#define I2_MTIME	64
#define I2_EXTSIZE	72
#define I_UID		112
#define I_GID		116

/* An old header (OFS_MAGIC). */
#define O_TYPE		0
#define O_DATE		4
#define O_VOLUME	12
#define O_TAPEA		16
#define O_INUMBER	20
#define O_MAGIC		24
#define O_CHECKSUM	28
#define O_INODE		32
#define O_COUNT		96
#define O_ADDR		100
#define OI_MODE		0
#define OI_NLINK	2
#define OI_UID		4
#define OI_GID		6
#define OI_SIZE		8
#define OI_RDEV		12
#define OI_ATIME	52
#define OI_MTIME	56
#define OI_CTIME	60

enum hdr { HDR_OFS, HDR_NFS, HDR_UFS2 };
enum dirs { DIRS_NEW, DIRS_OLD, DIRS_V7 };
enum type { T_DIR, T_FILE, T_LINK, T_CHR, T_BLK, T_FIFO, T_SOCK, T_HARD };

struct node {
	struct node	*parent;
	struct node	*child;
	struct node	*last;
	struct node	*next;
	struct node	*to;		/* T_HARD */
	char		*target;	/* T_LINK */
	enum type	type;
	uint32_t	hole[MAX_HOLES][2];
	int		nholes;
	uint32_t	ino;
	uint32_t	mtime;
	uint32_t	rdev;
	uint32_t	seed;
	uint32_t	size;
	uint16_t	mode;
	uint16_t	uid;
	uint16_t	gid;
	uint16_t	nlinks;
	char		name[MAX_NAME + 1];
};

/* The dump being written. */
struct dump {
	struct node	root;
	struct node	**byino;	/* nodes by inode number */
	uint32_t	next_ino;
	FILE		*out;
	enum hdr	hdr;
	enum dirs	dirs;
	int		big_endian;
	int		runs;		/* -r */
	int		linux_attrs;	/* -a */
	int		ufs2_attrs;	/* -x */
	int		compressed;	/* -z */
	uint32_t	tapea;
	int		lineno;
};

static void
usage(void)
{
	(void)fprintf(stderr, "usage: mkdump [-arxz] [-B le|be] "
	    "[-d new|old|v7] [-h ofs|nfs|ufs2] SPEC DUMP\n");
	exit(2);
}

/* Byte order. */

static void
put16(const struct dump *d, unsigned char *p, uint32_t v)
{
	if (d->big_endian) {
		p[0] = (unsigned char)(v >> 8);
		p[1] = (unsigned char)v;
	} else {
		p[0] = (unsigned char)v;
		p[1] = (unsigned char)(v >> 8);
	}
}

static void
put32(const struct dump *d, unsigned char *p, uint32_t v)
{
	if (d->big_endian) {
		put16(d, p, v >> 16);
		put16(d, p + 2, v & 0xffff);
	} else {
		put16(d, p, v & 0xffff);
		put16(d, p + 2, v >> 16);
	}
}

static void
put64(const struct dump *d, unsigned char *p, uint64_t v)
{
	if (d->big_endian) {
		put32(d, p, (uint32_t)(v >> 32));
		put32(d, p + 4, (uint32_t)v);
	} else {
		put32(d, p, (uint32_t)v);
		put32(d, p + 4, (uint32_t)(v >> 32));
	}
}

static uint32_t
get32(const struct dump *d, const unsigned char *p)
{
	if (d->big_endian)
		return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		    (uint32_t)p[2] << 8 | p[3];
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[1] << 8 | p[0];
}

/* File contents: the pattern of mkimage. */

static unsigned char
pattern(uint32_t seed, uint32_t i)
{
	return (unsigned char)((i * 31u + (i >> 10) * 17u + seed * 101u) ^
	    (i >> 8));
}

static int
in_hole(const struct node *n, uint64_t lo, uint64_t hi)
{
	int h;

	for (h = 0; h < n->nholes; h++)
		if (lo >= n->hole[h][0] &&
		    hi <= (uint64_t)n->hole[h][0] + n->hole[h][1])
			return 1;
	return 0;
}

/* Parsing, as mkimage does. */

static void
syntax(const struct dump *d, const char *what, const char *arg)
{
	errx(1, "line %d: %s \"%s\"", d->lineno, what, arg);
}

static char *
field(char **p)
{
	char *e, *s;

	for (s = *p; *s == ' ' || *s == '\t'; s++)
		continue;
	if (*s == '\0' || *s == '\n' || *s == '#')
		return NULL;
	for (e = s; *e != '\0' && *e != ' ' && *e != '\t' && *e != '\n'; e++)
		continue;
	if (*e != '\0')
		*e++ = '\0';
	*p = e;
	return s;
}

static char *
need(const struct dump *d, char **p, const char *what)
{
	char *s;

	if ((s = field(p)) == NULL)
		syntax(d, "missing", what);
	return s;
}

static uint32_t
number(const struct dump *d, const char *s, int base)
{
	unsigned long v;
	char *end;

	v = strtoul(s, &end, base);
	if (*end != '\0' || end == s || v > UINT32_MAX)
		syntax(d, "bad number", s);
	return (uint32_t)v;
}

static struct node *
find(struct dump *d, const char *path)
{
	struct node *c, *n;
	const char *e, *p;
	size_t len;

	n = &d->root;
	for (p = path; *p != '\0'; p += len) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		e = strchr(p, '/');
		len = e != NULL ? (size_t)(e - p) : strlen(p);
		for (c = n->child; c != NULL; c = c->next)
			if (strlen(c->name) == len &&
			    strncmp(c->name, p, len) == 0)
				break;
		if (c == NULL)
			return NULL;
		n = c->type == T_HARD ? c->to : c;
	}
	return n;
}

static struct node *
new_node(struct dump *d, char *path, enum type type)
{
	struct node *dir, *n;
	char *slash;

	slash = strrchr(path, '/');
	if (find(d, path) != NULL || slash == NULL || slash[1] == '\0' ||
	    strlen(slash + 1) > MAX_NAME) {
		syntax(d, "bad path", path);
		return NULL;
	}
	*slash = '\0';
	dir = find(d, path);
	*slash = '/';
	if (dir == NULL || dir->type != T_DIR)
		syntax(d, "no parent directory", path);
	if ((n = calloc(1, sizeof(*n))) == NULL)
		err(1, NULL);
	(void)strcpy(n->name, slash + 1);
	n->type = type;
	n->parent = dir;
	if (dir->last != NULL)
		dir->last->next = n;
	else
		dir->child = n;
	dir->last = n;
	if (type != T_HARD)
		n->ino = ++d->next_ino;
	return n;
}

static void
attrs(struct dump *d, struct node *n, char **p, uint16_t fmt, int mode)
{
	n->mode = (uint16_t)(fmt | (mode ?
	    number(d, need(d, p, "mode"), 8) & 07777 : 0777));
	n->uid = (uint16_t)number(d, need(d, p, "uid"), 10);
	n->gid = (uint16_t)number(d, need(d, p, "gid"), 10);
	n->mtime = number(d, need(d, p, "mtime"), 10);
}

static void
parse_file(struct dump *d, char *path, char **p)
{
	struct node *n;
	char *s;

	n = new_node(d, path, T_FILE);
	attrs(d, n, p, 0100000, 1);
	n->size = number(d, need(d, p, "size"), 10);
	n->seed = number(d, need(d, p, "seed"), 10);
	while ((s = field(p)) != NULL) {
		if (n->nholes == MAX_HOLES ||
		    sscanf(s, "%u:%u", &n->hole[n->nholes][0],
		    &n->hole[n->nholes][1]) != 2)
			syntax(d, "bad hole", s);
		n->nholes++;
	}
}

static void
parse_rest(struct dump *d, const char *cmd, char *path, char **p)
{
	struct node *n;
	char *s;

	if (strcmp(cmd, "dir") == 0) {
		attrs(d, new_node(d, path, T_DIR), p, 040000, 1);
	} else if (strcmp(cmd, "file") == 0) {
		parse_file(d, path, p);
	} else if (strcmp(cmd, "link") == 0) {
		n = new_node(d, path, T_LINK);
		if ((n->target = strdup(need(d, p, "target"))) == NULL)
			err(1, NULL);
		attrs(d, n, p, 0120000, 0);
		n->size = (uint32_t)strlen(n->target);
	} else if (strcmp(cmd, "dev") == 0) {
		s = need(d, p, "c or b");
		n = new_node(d, path, strcmp(s, "b") == 0 ? T_BLK : T_CHR);
		n->rdev = number(d, need(d, p, "major"), 10) << 8;
		n->rdev |= number(d, need(d, p, "minor"), 10);
		attrs(d, n, p, n->type == T_BLK ? 060000 : 020000, 1);
	} else if (strcmp(cmd, "fifo") == 0) {
		attrs(d, new_node(d, path, T_FIFO), p, 010000, 1);
	} else if (strcmp(cmd, "sock") == 0) {
		attrs(d, new_node(d, path, T_SOCK), p, 0140000, 1);
	} else if (strcmp(cmd, "hard") == 0) {
		s = need(d, p, "existing path");
		n = find(d, s);
		if (n == NULL || n->type == T_DIR)
			syntax(d, "bad hard link target", s);
		new_node(d, path, T_HARD)->to = n;
	} else {
		syntax(d, "unknown directive", cmd);
	}
}

static void
read_spec(struct dump *d, const char *spec)
{
	size_t cap;
	char *cmd, *line, *p, *path;
	FILE *in;

	if ((in = fopen(spec, "r")) == NULL)
		err(1, "%s", spec);
	line = NULL;
	cap = 0;
	while (getline(&line, &cap, in) != -1) {
		d->lineno++;
		p = line;
		if ((cmd = field(&p)) == NULL || strcmp(cmd, "fs") == 0)
			continue;
		path = need(d, &p, "path");
		parse_rest(d, cmd, path, &p);
	}
	free(line);
	(void)fclose(in);
}

/* Link counts, and the nodes by inode number. */
static void
index_nodes(struct dump *d, struct node *dir)
{
	struct node *c, *t;

	d->byino[dir->ino] = dir;
	dir->nlinks = 2;
	for (c = dir->child; c != NULL; c = c->next) {
		t = c->type == T_HARD ? c->to : c;
		if (t->type == T_DIR) {
			dir->nlinks++;
			index_nodes(d, t);
		} else {
			t->nlinks++;
			d->byino[t->ino] = t;
		}
	}
}

/* Writing. */

static void
record(struct dump *d, const unsigned char *rec)
{
	if (fwrite(rec, 1, BSIZE, d->out) != BSIZE)
		err(1, "write");
	d->tapea++;
}

/* Finish a header: type, inode, dates, magic, checksum; write it. */
static void
header(struct dump *d, unsigned char *rec, uint32_t type, uint32_t ino,
    uint32_t flags)
{
	uint32_t sum;
	int i;

	if (d->hdr == HDR_OFS) {
		put32(d, rec + O_TYPE, type);
		put32(d, rec + O_DATE, DUMP_DATE);
		put32(d, rec + O_VOLUME, 1);
		put32(d, rec + O_TAPEA, d->tapea);
		put16(d, rec + O_INUMBER, ino);
		put32(d, rec + O_MAGIC, OFS_MAGIC);
	} else {
		put32(d, rec + H_TYPE, type);
		put32(d, rec + H_VOLUME, 1);
		put32(d, rec + H_INUMBER, ino);
		put32(d, rec + H_FLAGS, flags);
		(void)memcpy(rec + H_LABEL, "none", 4);
		if (d->hdr == HDR_UFS2) {
			put32(d, rec + H_MAGIC, UFS2_MAGIC);
			put64(d, rec + H_DATE, DUMP_DATE);
			put64(d, rec + H_TAPEA, d->tapea);
		} else {
			put32(d, rec + H_MAGIC, NFS_MAGIC);
			put32(d, rec + H_OLD_DATE, DUMP_DATE);
			put32(d, rec + H_OLD_TAPEA, d->tapea);
		}
	}
	sum = 0;
	for (i = 0; i < BSIZE; i += 4)
		sum += get32(d, rec + i);
	put32(d, rec + (d->hdr == HDR_OFS ? O_CHECKSUM : H_CHECKSUM),
	    CHECKSUM - sum);
	record(d, rec);
}

static uint32_t
flags(const struct dump *d)
{
	return d->hdr == HDR_UFS2 || d->dirs == DIRS_NEW ? DR_NEWINODEFMT : 0;
}

static void
put_inode(struct dump *d, unsigned char *rec, const struct node *n,
    uint64_t size)
{
	unsigned char *p;

	if (d->hdr == HDR_OFS) {
		p = rec + O_INODE;
		put16(d, p + OI_MODE, n->mode);
		put16(d, p + OI_NLINK, n->nlinks);
		put16(d, p + OI_UID, n->uid);
		put16(d, p + OI_GID, n->gid);
		put32(d, p + OI_SIZE, (uint32_t)size);
		put32(d, p + OI_RDEV, n->rdev);
		put32(d, p + OI_ATIME, n->mtime + 1);
		put32(d, p + OI_MTIME, n->mtime);
		put32(d, p + OI_CTIME, n->mtime + 2);
		return;
	}
	p = rec + H_INODE;
	put16(d, p + I_MODE, n->mode);
	put64(d, p + I_SIZE, size);
	put32(d, p + I_RDEV, n->rdev);
	if (d->hdr == HDR_UFS2) {
		put64(d, p + I2_ATIME, (uint64_t)n->mtime + 1);
		put64(d, p + I2_MTIME, n->mtime);
		if (d->ufs2_attrs && n->type == T_FILE)
			put32(d, p + I2_EXTSIZE, ATTR_SIZE);
	} else {
		put16(d, p + I_NLINK, n->nlinks);
		put32(d, p + I_ATIME, n->mtime + 1);
		put32(d, p + I_MTIME, n->mtime);
		put32(d, p + I_CTIME, n->mtime + 2);
	}
	if (d->hdr == HDR_NFS && d->dirs != DIRS_NEW) {
		put16(d, p + I_OLDUID, n->uid);
		put16(d, p + I_OLDGID, n->gid);
	} else {
		put32(d, p + I_UID, n->uid);
		put32(d, p + I_GID, n->gid);
	}
}

/* The records of a file: its bytes, and which records are data. */
struct recs {
	unsigned char	*buf;
	int		*data;
	uint64_t	n;
};

static uint64_t
run_length(const struct recs *r, uint64_t i)
{
	uint64_t j;

	for (j = i; j < r->n && j - i < LINUX_MAXRUN &&
	    r->data[j] == r->data[i]; j++)
		continue;
	return j - i;
}

/* c_addr for records i onwards, as many as fit: returns how many. */
static uint64_t
fill_addr(struct dump *d, const struct recs *r, uint64_t i,
    unsigned char *addr, uint32_t *count)
{
	uint64_t j, len;
	uint32_t naddr, v;

	naddr = d->hdr == HDR_OFS ? OLD_NADDR : NADDR;
	*count = 0;
	if (!d->runs) {
		for (j = i; j < r->n && j - i < naddr; j++)
			addr[(*count)++] = (unsigned char)r->data[j];
		return j - i;
	}
	for (j = i; j < r->n; j += len) {
		len = run_length(r, j);
		if (*count + (len <= LINUX_RUN ? 1 : 2) > naddr)
			break;
		if (len <= LINUX_RUN) {
			addr[(*count)++] = (unsigned char)((len - 1) << 1 |
			    (uint64_t)r->data[j]);
		} else {
			v = (uint32_t)(len - LINUX_RUN - 1);
			addr[(*count)++] = (unsigned char)(0x80 |
			    (v % LINUX_RUN) << 1 | (uint32_t)r->data[j]);
			addr[(*count)++] = (unsigned char)(v / LINUX_RUN);
		}
	}
	return j - i;
}

/* A file: TS_INODE and TS_ADDR headers with the records of r. */
static void
put_file(struct dump *d, const struct node *n, uint64_t size,
    const struct recs *r)
{
	unsigned char rec[BSIZE];
	uint64_t done, i, k;
	uint32_t count, type;

	type = TS_INODE;
	i = 0;
	do {
		(void)memset(rec, 0, sizeof(rec));
		put_inode(d, rec, n, size);
		done = fill_addr(d, r, i, rec + (d->hdr == HDR_OFS ?
		    O_ADDR : H_ADDR), &count);
		put32(d, rec + (d->hdr == HDR_OFS ? O_COUNT : H_COUNT),
		    count);
		header(d, rec, type, n->ino, flags(d));
		for (k = i; k < i + done; k++)
			if (r->data[k])
				record(d, r->buf + k * BSIZE);
		type = TS_ADDR;
		i += done;
	} while (i < r->n);
}

/* Make room for len more bytes at the end of r->buf, in whole records. */
static unsigned char *
recs_room(struct recs *r, uint64_t *used, uint64_t len)
{
	uint64_t n;

	n = (*used + len + BSIZE - 1) / BSIZE;
	if (n > r->n) {
		if ((r->buf = realloc(r->buf, n * BSIZE)) == NULL)
			err(1, NULL);
		(void)memset(r->buf + r->n * BSIZE, 0, (n - r->n) * BSIZE);
		r->n = n;
	}
	*used += len;
	return r->buf + *used - len;
}

static unsigned
dtype(const struct node *n)
{
	static const unsigned types[] = { 4, 8, 10, 2, 6, 1, 12, 0 };

	return types[n->type];
}

/* One entry of a directory, in the format of -d. */
static void
dir_entry(struct dump *d, struct recs *r, uint64_t *used, uint32_t ino,
    const char *name, unsigned type)
{
	unsigned char *p;
	uint64_t chunk, len, reclen;

	len = strlen(name);
	if (d->dirs == DIRS_V7) {
		if (len > V7_NAME || ino > 0xffff)
			errx(1, "%s: does not fit a directory of V7", name);
		p = recs_room(r, used, V7_ENTRY);
		put16(d, p, ino);
		(void)memcpy(p + 2, name, len);
		return;
	}
	chunk = d->dirs == DIRS_NEW ? DIRBLK_NEW : DIRBLK_OLD;
	reclen = (8 + len + 1 + 3) & ~(uint64_t)3;
	if (*used % chunk + reclen > chunk)
		errx(1, "an entry crosses a chunk");
	p = recs_room(r, used, reclen);
	put32(d, p, ino);
	put16(d, p + 4, (uint32_t)reclen);
	if (d->dirs == DIRS_NEW) {
		p[6] = (unsigned char)type;
		p[7] = (unsigned char)len;
	} else {
		put16(d, p + 6, (uint32_t)len);
	}
	(void)memcpy(p + 8, name, len);
}

/*
 * The entries of a directory, each chunk filled out by a free entry at
 * its end, as the file systems of BSD leave them.
 */
static uint64_t
dir_recs(struct dump *d, const struct node *dir, struct recs *r)
{
	const struct node *c, *t;
	uint64_t chunk, left, used;
	unsigned char *p;

	used = 0;
	chunk = d->dirs == DIRS_NEW ? DIRBLK_NEW : DIRBLK_OLD;
	dir_entry(d, r, &used, dir->ino, ".", 4);
	dir_entry(d, r, &used, dir->parent->ino, "..", 4);
	for (c = dir->child; c != NULL; c = c->next) {
		t = c->type == T_HARD ? c->to : c;
		left = chunk - used % chunk;
		if (d->dirs != DIRS_V7 &&
		    left < ((8 + strlen(c->name) + 1 + 3) & ~(uint64_t)3)) {
			p = recs_room(r, &used, left);
			put16(d, p + 4, (uint32_t)left);
		}
		dir_entry(d, r, &used, t->ino, c->name, dtype(t));
	}
	if (d->dirs != DIRS_V7 && used % chunk != 0) {
		left = chunk - used % chunk;
		p = recs_room(r, &used, left);
		put16(d, p + 4, (uint32_t)left);
	}
	return used;
}

/* The records of a regular file or a symbolic link. */
static void
file_recs(struct dump *d, const struct node *n, struct recs *r)
{
	unsigned char *p;
	uint64_t i, used;

	used = 0;
	p = recs_room(r, &used, n->size);
	for (i = 0; i < n->size; i++)
		p[i] = n->type == T_LINK ? (unsigned char)n->target[i] :
		    in_hole(n, i, i + 1) ? 0 : pattern(n->seed, (uint32_t)i);
	if (d->ufs2_attrs && n->type == T_FILE) {
		p = recs_room(r, &used, (r->n * BSIZE - used) + ATTR_SIZE);
		(void)memset(p, 0, r->n * BSIZE - used);
	}
}

static void
dump_node(struct dump *d, const struct node *n)
{
	unsigned char rec[BSIZE];
	struct recs r;
	uint64_t i, size;

	(void)memset(&r, 0, sizeof(r));
	size = 0;
	if (n->type == T_DIR)
		size = dir_recs(d, n, &r);
	else if (n->type == T_FILE || n->type == T_LINK)
		file_recs(d, n, &r);
	if (n->type == T_FILE || n->type == T_LINK)
		size = n->size;
	if ((r.data = calloc(r.n + 1, sizeof(*r.data))) == NULL)
		err(1, NULL);
	for (i = 0; i < r.n; i++)
		r.data[i] = n->type != T_FILE || i * BSIZE >= n->size ||
		    (!d->runs && (i + 1) * BSIZE >= n->size) ||
		    !in_hole(n, i * BSIZE, (i + 1) * BSIZE > n->size ?
		    n->size : (i + 1) * BSIZE);
	put_file(d, n, size, &r);
	free(r.buf);
	free(r.data);
	if (d->linux_attrs && n->type == T_FILE) {
		(void)memset(rec, 0, sizeof(rec));
		put_inode(d, rec, n, ATTR_SIZE);
		put32(d, rec + H_COUNT, 1);
		rec[H_ADDR] = 1;
		header(d, rec, TS_INODE, n->ino, flags(d) | DR_EXTATTRIBUTES);
		(void)memset(rec, 'a', sizeof(rec));
		record(d, rec);
	}
}

/* TS_CLRI and TS_BITS: every inode is in use and in the dump. */
static void
dump_map(struct dump *d, uint32_t type)
{
	unsigned char rec[BSIZE];
	uint32_t count, i, ino;

	count = (d->next_ino / 8 + BSIZE) / BSIZE;
	(void)memset(rec, 0, sizeof(rec));
	put32(d, rec + (d->hdr == HDR_OFS ? O_COUNT : H_COUNT), count);
	header(d, rec, type, d->next_ino, flags(d));
	for (i = 0; i < count; i++) {
		(void)memset(rec, 0, sizeof(rec));
		for (ino = ROOTINO; ino <= d->next_ino; ino++)
			if ((ino - 1) / 8 / BSIZE == i)
				rec[(ino - 1) / 8 % BSIZE] |=
				    (unsigned char)(1 << ((ino - 1) % 8));
		record(d, rec);
	}
}

static void
write_dump(struct dump *d)
{
	unsigned char rec[BSIZE];
	uint32_t f, ino;

	(void)memset(rec, 0, sizeof(rec));
	f = flags(d) | (d->hdr == HDR_NFS ? DR_NEWHEADER : 0) |
	    (d->compressed ? DR_COMPRESSED : 0);
	put32(d, rec + (d->hdr == HDR_OFS ? O_COUNT : H_COUNT), 1);
	header(d, rec, TS_TAPE, 0, f);
	dump_map(d, TS_CLRI);
	dump_map(d, TS_BITS);
	for (ino = ROOTINO; ino <= d->next_ino; ino++)
		if (d->byino[ino] != NULL && d->byino[ino]->type == T_DIR)
			dump_node(d, d->byino[ino]);
	for (ino = ROOTINO; ino <= d->next_ino; ino++)
		if (d->byino[ino] != NULL && d->byino[ino]->type != T_DIR)
			dump_node(d, d->byino[ino]);
	do {
		(void)memset(rec, 0, sizeof(rec));
		header(d, rec, TS_END, d->next_ino, flags(d));
	} while (d->tapea % NTREC != 0);
}

static void
options(struct dump *d, int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "aB:d:h:rxz")) != -1) {
		if (ch == 'a')
			d->linux_attrs = 1;
		else if (ch == 'B' && strcmp(optarg, "le") == 0)
			d->big_endian = 0;
		else if (ch == 'B' && strcmp(optarg, "be") == 0)
			d->big_endian = 1;
		else if (ch == 'd' && strcmp(optarg, "new") == 0)
			d->dirs = DIRS_NEW;
		else if (ch == 'd' && strcmp(optarg, "old") == 0)
			d->dirs = DIRS_OLD;
		else if (ch == 'd' && strcmp(optarg, "v7") == 0)
			d->dirs = DIRS_V7;
		else if (ch == 'h' && strcmp(optarg, "ofs") == 0)
			d->hdr = HDR_OFS;
		else if (ch == 'h' && strcmp(optarg, "nfs") == 0)
			d->hdr = HDR_NFS;
		else if (ch == 'h' && strcmp(optarg, "ufs2") == 0)
			d->hdr = HDR_UFS2;
		else if (ch == 'r')
			d->runs = 1;
		else if (ch == 'x')
			d->ufs2_attrs = 1;
		else if (ch == 'z')
			d->compressed = 1;
		else
			usage();
	}
	if ((d->hdr == HDR_OFS && d->dirs != DIRS_V7) ||
	    (d->hdr != HDR_OFS && d->dirs == DIRS_V7) ||
	    (d->hdr == HDR_UFS2 && d->dirs != DIRS_NEW) ||
	    (d->ufs2_attrs && d->hdr != HDR_UFS2) ||
	    (d->hdr == HDR_OFS && (d->runs || d->linux_attrs)))
		usage();
}

int
main(int argc, char **argv)
{
	struct dump d;

	(void)memset(&d, 0, sizeof(d));
	d.hdr = HDR_NFS;
	options(&d, argc, argv);
	if (argc - optind != 2)
		usage();
	d.root.type = T_DIR;
	d.root.mode = 040755;
	d.root.ino = ROOTINO;
	d.root.parent = &d.root;
	d.next_ino = ROOTINO;
	read_spec(&d, argv[optind]);
	if ((d.byino = calloc((size_t)d.next_ino + 1, sizeof(*d.byino))) ==
	    NULL)
		err(1, NULL);
	index_nodes(&d, &d.root);
	if ((d.out = fopen(argv[optind + 1], "wb")) == NULL)
		err(1, "%s", argv[optind + 1]);
	write_dump(&d);
	if (fclose(d.out) != 0)
		err(1, "%s", argv[optind + 1]);
	return 0;
}
