/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mkimage - build MINIX file system images for the test suite.
 *
 *	mkimage [-e EXPECTDIR] SPEC IMAGE
 *
 * This is test scaffolding.  It shares no code with src/ so that the
 * reader is checked against an independent writer.  SPEC ("-" for
 * standard input) holds one directive per line; fields are separated by
 * blanks and '#' starts a comment:
 *
 *	fs   [version=1|2|3] order=le|be [namelen=14|30] [block=N]
 *	     blocks=N inodes=N [logzone=N]
 *	dir  PATH MODE UID GID MTIME
 *	file PATH MODE UID GID MTIME SIZE SEED [HOLESTART:HOLELEN ...]
 *	link PATH TARGET UID GID MTIME
 *	dev  PATH c|b MAJOR MINOR MODE UID GID MTIME
 *	fifo PATH MODE UID GID MTIME
 *	hard PATH EXISTING
 *	raw  DIR NAME INO
 *
 * The fs line comes first.  namelen applies to V1 and V2; V3 names are
 * always 60 characters.  block is the block size of V3 (default 1024); V1
 * and V2 always use 1024.  blocks counts blocks of that size.
 *
 * MODE is octal permission bits.  File contents are a pattern made from
 * SEED; bytes inside a hole are zero, and zones that lie wholly inside a
 * hole are not allocated.  "raw" adds a directory entry with any name and
 * inode number, to make damaged images.  In V2 and V3 inodes, atime is
 * MTIME + 1 and ctime MTIME + 2, so that a reader that mixes them up is
 * caught.
 *
 * With -e, the regular files, directories and symbolic links are also
 * written below EXPECTDIR, as "minixfs extract" should reproduce them.
 */

#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUPER_OFFSET	1024		/* byte offset of the super block */
#define START_BLOCK	2		/* first block of the inode map */
#define NR_DZONES	7		/* direct zones in any inode */
#define MAX_HOLES	8		/* holes in one file */
#define MAX_NAME	60		/* longest name of any version */

/* V1 and V2 super block: byte offsets. */
#define SB12_NINODES	0
#define SB12_NZONES	2
#define SB12_IMAP	4
#define SB12_ZMAP	6
#define SB12_FIRSTDATA	8
#define SB12_LOGZONE	10
#define SB12_MAXSIZE	12
#define SB12_MAGIC	16
#define SB12_STATE	18		/* Linux: 1 means cleanly unmounted */
#define SB12_ZONES	20

/* V3 super block. */
#define SB3_NINODES	0
#define SB3_IMAP	6
#define SB3_ZMAP	8
#define SB3_FIRSTDATA	10
#define SB3_LOGZONE	12
#define SB3_MAXSIZE	16
#define SB3_ZONES	20
#define SB3_MAGIC	24
#define SB3_BLOCKSIZE	28

/* V1 inode. */
#define I1_MODE		0
#define I1_UID		2
#define I1_FSIZE	4
#define I1_MTIME	8
#define I1_GID		12
#define I1_NLINKS	13
#define I1_ZONE		14
#define I1_NZONES	9

/* V2 and V3 inode. */
#define I2_MODE		0
#define I2_NLINKS	2
#define I2_UID		4
#define I2_GID		6
#define I2_FSIZE	8
#define I2_ATIME	12
#define I2_MTIME	16
#define I2_CTIME	20
#define I2_ZONE		24
#define I2_NZONES	10

enum type { T_DIR, T_FILE, T_LINK, T_CHR, T_BLK, T_FIFO, T_HARD, T_RAW };

struct node {
	struct node	*parent;
	struct node	*child;		/* first entry of a directory */
	struct node	*last;		/* last entry of a directory */
	struct node	*next;		/* next entry of the same directory */
	struct node	*to;		/* T_HARD: the node linked to */
	char		*target;	/* T_LINK */
	enum type	type;
	uint32_t	hole[MAX_HOLES][2];
	uint32_t	zone[I2_NZONES];
	uint32_t	ino;
	uint32_t	mtime;
	uint32_t	rdev;
	uint32_t	seed;
	uint32_t	size;
	int		nholes;
	uint16_t	gid;
	uint16_t	mode;
	uint16_t	nlinks;
	uint16_t	uid;
	char		name[MAX_NAME + 1];
};

/* The image being built. */
struct image {
	struct node	root;
	unsigned char	*data;
	int		big_endian;
	int		version;
	uint32_t	bsize;		/* block size */
	uint32_t	dino;		/* bytes of a directory inode number */
	uint32_t	firstdatazone;
	uint32_t	imap_blocks;
	uint32_t	inode_start;
	uint32_t	isize;		/* bytes of an inode */
	uint32_t	logzone;
	uint32_t	namelen;
	uint32_t	nblocks;
	uint32_t	next_ino;
	uint32_t	next_zone;
	uint32_t	nind;		/* zone numbers in an indirect block */
	uint32_t	ninodes;
	uint32_t	nlevels;	/* levels of indirection */
	uint32_t	nzones;
	uint32_t	zbytes;		/* bytes of a zone number */
	uint32_t	zmap_blocks;
};

/* Reading the spec. */
struct parser {
	struct image	*img;
	int		lineno;
};

static void
usage(void)
{
	(void)fprintf(stderr, "usage: mkimage [-e EXPECTDIR] SPEC IMAGE\n");
	exit(2);
}

/* Byte order. */

static void
put16(const struct image *img, unsigned char *p, uint32_t v)
{
	if (img->big_endian) {
		p[0] = (unsigned char)(v >> 8);
		p[1] = (unsigned char)v;
	} else {
		p[0] = (unsigned char)v;
		p[1] = (unsigned char)(v >> 8);
	}
}

static void
put32(const struct image *img, unsigned char *p, uint32_t v)
{
	if (img->big_endian) {
		put16(img, p, v >> 16);
		put16(img, p + 2, v & 0xffff);
	} else {
		put16(img, p, v & 0xffff);
		put16(img, p + 2, v >> 16);
	}
}

static uint32_t
get16(const struct image *img, const unsigned char *p)
{
	if (img->big_endian)
		return (uint32_t)(p[0] << 8 | p[1]);
	return (uint32_t)(p[1] << 8 | p[0]);
}

/* A zone number, 16 or 32 bits wide. */
static void
put_zone(const struct image *img, unsigned char *p, uint32_t v)
{
	if (img->zbytes == 2)
		put16(img, p, v);
	else
		put32(img, p, v);
}

static uint32_t
get_zone(const struct image *img, const unsigned char *p)
{
	if (img->zbytes == 2)
		return get16(img, p);
	if (img->big_endian)
		return get16(img, p) << 16 | get16(img, p + 2);
	return get16(img, p + 2) << 16 | get16(img, p);
}

/*
 * MINIX keeps its bit maps as arrays of words: 16 bits in MINIX 1 and 2,
 * 32 bits in MINIX 3.  On a big-endian machine the bytes of each word
 * are therefore reversed relative to the PC.
 */
static void
set_bit(const struct image *img, unsigned char *map, uint32_t bit)
{
	uint32_t byte;

	byte = bit / 8;
	if (img->big_endian)
		byte ^= img->version == 3 ? 3 : 1;
	map[byte] |= (unsigned char)(1 << (bit % 8));
}

/* File contents. */

static unsigned char
pattern(uint32_t seed, uint32_t i)
{
	return (unsigned char)((i * 31u + (i >> 10) * 17u + seed * 101u) ^
	    (i >> 8));
}

/* Does [lo, hi) lie wholly inside one hole of n? */
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

static unsigned char *
contents(const struct node *n)
{
	unsigned char *buf;
	uint32_t i;

	if ((buf = calloc(1, n->size != 0 ? n->size : 1)) == NULL)
		err(1, NULL);
	for (i = 0; i < n->size; i++)
		buf[i] = in_hole(n, i, (uint64_t)i + 1) ? 0 :
		    pattern(n->seed, i);
	return buf;
}

/* Zones. */

static uint32_t
alloc_zone(struct image *img)
{
	if (img->next_zone >= img->nzones)
		errx(1, "image full");
	return img->next_zone++;
}

static unsigned char *
zone_ptr(const struct image *img, uint32_t zone)
{
	return img->data + ((size_t)zone << img->logzone) * img->bsize;
}

/*
 * Enter zone z at index idx below the indirect block that starts zone
 * ind, level levels above the data, allocating indirect blocks on the
 * way.  Returns the zone of that indirect block, new if ind was 0.
 */
static uint32_t
chain(struct image *img, uint32_t ind, uint32_t level, uint64_t idx,
    uint32_t z)
{
	unsigned char *p;
	uint64_t per;
	uint32_t i;

	per = 1;
	for (i = 1; i < level; i++)
		per *= img->nind;
	if (ind == 0)
		ind = alloc_zone(img);
	p = zone_ptr(img, ind) + idx / per * img->zbytes;
	if (level == 1)
		put_zone(img, p, z);
	else
		put_zone(img, p, chain(img, get_zone(img, p), level - 1,
		    idx % per, z));
	return ind;
}

/* Record data zone z as zone number zi of the file n. */
static void
place_zone(struct image *img, struct node *n, uint64_t zi, uint32_t z)
{
	uint64_t idx, per;
	uint32_t level;

	if (zi < NR_DZONES) {
		n->zone[zi] = z;
		return;
	}
	idx = zi - NR_DZONES;
	per = 1;
	for (level = 1; level <= img->nlevels; level++) {
		per *= img->nind;
		if (idx < per)
			break;
		idx -= per;
	}
	if (level > img->nlevels)
		errx(1, "%s: file too large", n->name);
	n->zone[NR_DZONES + level - 1] =
	    chain(img, n->zone[NR_DZONES + level - 1], level, idx, z);
}

/* Write size bytes of buf as the data of n, leaving holes unallocated. */
static void
write_data(struct image *img, struct node *n, const unsigned char *buf,
    uint32_t size)
{
	uint64_t hi, lo, zb, zi;
	uint32_t z;

	zb = (uint64_t)img->bsize << img->logzone;
	for (zi = 0; zi * zb < size; zi++) {
		lo = zi * zb;
		hi = lo + zb < size ? lo + zb : size;
		if (in_hole(n, lo, hi))
			continue;
		z = alloc_zone(img);
		(void)memcpy(zone_ptr(img, z), buf + lo, (size_t)(hi - lo));
		place_zone(img, n, zi, z);
	}
}

/* Parsing. */

static void
syntax(const struct parser *ps, const char *fmt, const char *arg)
{
	errx(1, "line %d: %s \"%s\"", ps->lineno, fmt, arg);
}

/* The next blank-separated field of *p, or NULL at the end. */
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
need(const struct parser *ps, char **p, const char *what)
{
	char *s;

	if ((s = field(p)) == NULL)
		syntax(ps, "missing", what);
	return s;
}

static uint32_t
number(const struct parser *ps, const char *s, int base)
{
	unsigned long v;
	char *end;

	errno = 0;
	v = strtoul(s, &end, base);
	if (errno != 0 || *end != '\0' || end == s || v > UINT32_MAX)
		syntax(ps, "bad number", s);
	return (uint32_t)v;
}

/* The node for path, following hard links, or NULL. */
static struct node *
find(struct image *img, const char *path)
{
	struct node *c, *n;
	const char *e, *p;
	size_t len;

	n = &img->root;
	for (p = path; *p != '\0'; p += len) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		if ((e = strchr(p, '/')) != NULL)
			len = (size_t)(e - p);
		else
			len = strlen(p);
		for (c = n->child; c != NULL; c = c->next)
			if (c->type != T_RAW && strlen(c->name) == len &&
			    strncmp(c->name, p, len) == 0)
				break;
		if (c == NULL)
			return NULL;
		n = c->type == T_HARD ? c->to : c;
	}
	return n;
}

static void
add_child(const struct parser *ps, struct node *dir, struct node *n)
{
	if (dir->type != T_DIR)
		syntax(ps, "parent is not a directory:", n->name);
	n->parent = dir;
	if (dir->last != NULL)
		dir->last->next = n;
	else
		dir->child = n;
	dir->last = n;
}

/* Make a node for path below its (existing) parent directory. */
static struct node *
new_node(struct parser *ps, char *path, enum type type)
{
	struct image *img;
	struct node *dir, *n;
	char *slash;

	img = ps->img;
	if (find(img, path) != NULL)
		syntax(ps, "already exists:", path);
	if ((slash = strrchr(path, '/')) == NULL)
		syntax(ps, "path must be absolute:", path);
	if (strlen(slash + 1) > img->namelen || slash[1] == '\0')
		syntax(ps, "bad name length:", path);
	*slash = '\0';
	dir = find(img, path);
	*slash = '/';
	if (dir == NULL)
		syntax(ps, "no parent directory:", path);
	if ((n = calloc(1, sizeof(*n))) == NULL)
		err(1, NULL);
	(void)strcpy(n->name, slash + 1);
	n->type = type;
	add_child(ps, dir, n);
	if (type != T_HARD && type != T_RAW) {
		if (img->next_ino >= img->ninodes)
			errx(1, "line %d: out of inodes", ps->lineno);
		n->ino = ++img->next_ino;
	}
	return n;
}

/* The permission, owner and time fields common to most directives. */
static void
attrs(const struct parser *ps, struct node *n, char **p, uint16_t fmt,
    int with_mode)
{
	if (with_mode)
		n->mode = (uint16_t)(fmt |
		    (number(ps, need(ps, p, "mode"), 8) & 07777));
	else
		n->mode = (uint16_t)(fmt | 0777);
	n->uid = (uint16_t)number(ps, need(ps, p, "uid"), 10);
	n->gid = (uint16_t)number(ps, need(ps, p, "gid"), 10);
	n->mtime = number(ps, need(ps, p, "mtime"), 10);
}

static void
fs_option(struct parser *ps, char *s)
{
	struct image *img;

	img = ps->img;
	if (strcmp(s, "order=le") == 0)
		img->big_endian = 0;
	else if (strcmp(s, "order=be") == 0)
		img->big_endian = 1;
	else if (strncmp(s, "version=", 8) == 0)
		img->version = (int)number(ps, s + 8, 10);
	else if (strncmp(s, "namelen=", 8) == 0)
		img->namelen = number(ps, s + 8, 10);
	else if (strncmp(s, "block=", 6) == 0)
		img->bsize = number(ps, s + 6, 10);
	else if (strncmp(s, "blocks=", 7) == 0)
		img->nblocks = number(ps, s + 7, 10);
	else if (strncmp(s, "inodes=", 7) == 0)
		img->ninodes = number(ps, s + 7, 10);
	else if (strncmp(s, "logzone=", 8) == 0)
		img->logzone = number(ps, s + 8, 10);
	else
		syntax(ps, "unknown fs option", s);
}

/* Check the options of the fs line and fill in the version's layout. */
static void
check_version(const struct parser *ps, struct image *img)
{
	if (img->version < 1 || img->version > 3)
		syntax(ps, "version must be 1, 2 or 3", "");
	if (img->version == 3) {
		if (img->namelen != 0 && img->namelen != 60)
			syntax(ps, "V3 names are 60 characters", "");
		img->namelen = 60;
		if (img->bsize < 1024 || img->bsize > 32768 ||
		    img->bsize % 1024 != 0)
			syntax(ps, "bad block size", "");
	} else {
		if (img->namelen == 0)
			img->namelen = 14;
		if (img->namelen != 14 && img->namelen != 30)
			syntax(ps, "namelen must be 14 or 30", "");
		if (img->bsize != 1024)
			syntax(ps, "only V3 has a block size", "");
	}
	img->zbytes = img->version == 1 ? 2 : 4;
	img->isize = img->version == 1 ? 32 : 64;
	img->dino = img->version == 3 ? 4 : 2;
	img->nind = img->bsize / img->zbytes;
	img->nlevels = img->version == 1 ? 2 : 3;
	if (img->ninodes < 1 || (img->version < 3 && img->ninodes > 65535) ||
	    img->logzone > 4)
		syntax(ps, "bad inodes or logzone", "");
}

/* Work out the layout and allocate the image. */
static void
lay_out_blocks(const struct parser *ps, struct image *img)
{
	uint64_t itable;
	uint32_t bits;

	img->nzones = img->nblocks >> img->logzone;
	if (img->nzones < 8 || (img->version == 1 && img->nzones > 65535))
		syntax(ps, "bad number of blocks", "");
	img->nblocks = img->nzones << img->logzone;
	bits = img->bsize * 8;
	img->imap_blocks = (img->ninodes + 1 + bits - 1) / bits;
	img->zmap_blocks = (img->nzones + 1 + bits - 1) / bits;
	img->inode_start = START_BLOCK + img->imap_blocks + img->zmap_blocks;
	itable = ((uint64_t)img->ninodes * img->isize + img->bsize - 1) /
	    img->bsize;
	img->firstdatazone = (uint32_t)((img->inode_start + itable +
	    ((uint64_t)1 << img->logzone) - 1) >> img->logzone);
	if (img->firstdatazone >= img->nzones)
		syntax(ps, "no room for data", "");
	img->next_zone = img->firstdatazone;
	if ((img->data = calloc(img->nblocks, img->bsize)) == NULL)
		err(1, NULL);
}

static void
parse_fs(struct parser *ps, char *p)
{
	struct image *img;
	char *s;

	img = ps->img;
	img->version = 1;
	img->bsize = 1024;
	while ((s = field(&p)) != NULL)
		fs_option(ps, s);
	check_version(ps, img);
	lay_out_blocks(ps, img);
	img->root.type = T_DIR;
	img->root.mode = 040755;
	img->root.ino = 1;
	img->next_ino = 1;
	img->root.parent = &img->root;
	(void)strcpy(img->root.name, "/");
}

static void
parse_file(struct parser *ps, char *path, char **p)
{
	struct node *n;
	char *s;

	n = new_node(ps, path, T_FILE);
	attrs(ps, n, p, 0100000, 1);
	n->size = number(ps, need(ps, p, "size"), 10);
	n->seed = number(ps, need(ps, p, "seed"), 10);
	while ((s = field(p)) != NULL) {
		if (n->nholes == MAX_HOLES)
			syntax(ps, "too many holes", s);
		if (sscanf(s, "%u:%u", &n->hole[n->nholes][0],
		    &n->hole[n->nholes][1]) != 2)
			syntax(ps, "bad hole", s);
		n->nholes++;
	}
}

static void
parse_link(struct parser *ps, char *path, char **p)
{
	struct node *n;

	n = new_node(ps, path, T_LINK);
	if ((n->target = strdup(need(ps, p, "target"))) == NULL)
		err(1, NULL);
	attrs(ps, n, p, 0120000, 0);
	n->size = (uint32_t)strlen(n->target);
}

static void
parse_dev(struct parser *ps, char *path, char **p)
{
	struct node *n;
	char *kind;

	kind = need(ps, p, "c or b");
	n = new_node(ps, path, strcmp(kind, "b") == 0 ? T_BLK : T_CHR);
	n->rdev = number(ps, need(ps, p, "major"), 10) << 8;
	n->rdev |= number(ps, need(ps, p, "minor"), 10);
	attrs(ps, n, p, n->type == T_BLK ? 060000 : 020000, 1);
}

static void
parse_hard(struct parser *ps, char *path, char **p)
{
	struct node *n, *to;
	char *existing;

	existing = need(ps, p, "existing path");
	to = find(ps->img, existing);
	if (to == NULL || to->type == T_DIR)
		syntax(ps, "bad hard link target", existing);
	n = new_node(ps, path, T_HARD);
	n->to = to;
}

static void
parse_raw(struct parser *ps, char *path, char **p)
{
	struct node *dir, *n;
	char *name;

	if ((dir = find(ps->img, path)) == NULL)
		syntax(ps, "no such directory", path);
	name = need(ps, p, "name");
	if ((n = calloc(1, sizeof(*n))) == NULL)
		err(1, NULL);
	n->type = T_RAW;
	(void)snprintf(n->name, sizeof(n->name), "%s", name);
	n->ino = number(ps, need(ps, p, "inode"), 10);
	add_child(ps, dir, n);
}

static void
parse_line(struct parser *ps, char *line)
{
	struct node *n;
	char *cmd, *p, *path;

	p = line;
	if ((cmd = field(&p)) == NULL)
		return;
	if (strcmp(cmd, "fs") == 0) {
		if (ps->img->data != NULL)
			syntax(ps, "fs given twice", cmd);
		parse_fs(ps, p);
		return;
	}
	if (ps->img->data == NULL)
		syntax(ps, "the fs line must come first", cmd);
	path = need(ps, &p, "path");
	if (strcmp(cmd, "dir") == 0) {
		n = new_node(ps, path, T_DIR);
		attrs(ps, n, &p, 040000, 1);
	} else if (strcmp(cmd, "file") == 0) {
		parse_file(ps, path, &p);
	} else if (strcmp(cmd, "link") == 0) {
		parse_link(ps, path, &p);
	} else if (strcmp(cmd, "dev") == 0) {
		parse_dev(ps, path, &p);
	} else if (strcmp(cmd, "fifo") == 0) {
		n = new_node(ps, path, T_FIFO);
		attrs(ps, n, &p, 010000, 1);
	} else if (strcmp(cmd, "hard") == 0) {
		parse_hard(ps, path, &p);
	} else if (strcmp(cmd, "raw") == 0) {
		parse_raw(ps, path, &p);
	} else {
		syntax(ps, "unknown directive", cmd);
	}
}

/* Layout. */

static uint32_t
entry_ino(const struct node *n)
{
	return n->type == T_HARD ? n->to->ino : n->ino;
}

static void
count_links(struct node *dir)
{
	struct node *c;

	dir->nlinks = 2;
	for (c = dir->child; c != NULL; c = c->next) {
		if (c->type == T_DIR) {
			dir->nlinks++;
			count_links(c);
		} else if (c->type == T_HARD) {
			c->to->nlinks++;
		} else if (c->type != T_RAW) {
			c->nlinks++;
		}
	}
}

/* Write one directory entry at p. */
static void
put_entry(const struct image *img, unsigned char *p, uint32_t ino,
    const char *name)
{
	size_t len;

	if (img->dino == 2)
		put16(img, p, ino);
	else
		put32(img, p, ino);
	len = strlen(name);
	(void)memcpy(p + img->dino, name, len < img->namelen ? len :
	    img->namelen);
}

static void
write_dir(struct image *img, struct node *dir)
{
	unsigned char *buf;
	struct node *c;
	uint32_t esize, n, off;

	esize = img->dino + img->namelen;
	n = 2;
	for (c = dir->child; c != NULL; c = c->next)
		n++;
	if ((buf = calloc(n, esize)) == NULL)
		err(1, NULL);
	put_entry(img, buf, dir->ino, ".");
	put_entry(img, buf + esize, dir->parent->ino, "..");
	off = 2 * esize;
	for (c = dir->child; c != NULL; c = c->next, off += esize)
		put_entry(img, buf + off, entry_ino(c), c->name);
	dir->size = n * esize;
	write_data(img, dir, buf, dir->size);
	free(buf);
}

static void
write_inode(const struct image *img, const struct node *n)
{
	unsigned char *p;
	int i;

	p = img->data + (size_t)img->inode_start * img->bsize +
	    (size_t)(n->ino - 1) * img->isize;
	if (img->version == 1) {
		put16(img, p + I1_MODE, n->mode);
		put16(img, p + I1_UID, n->uid);
		put32(img, p + I1_FSIZE, n->size);
		put32(img, p + I1_MTIME, n->mtime);
		p[I1_GID] = (unsigned char)n->gid;
		p[I1_NLINKS] = (unsigned char)n->nlinks;
		for (i = 0; i < I1_NZONES; i++)
			put16(img, p + I1_ZONE + 2 * i, n->zone[i]);
	} else {
		put16(img, p + I2_MODE, n->mode);
		put16(img, p + I2_NLINKS, n->nlinks);
		put16(img, p + I2_UID, n->uid);
		put16(img, p + I2_GID, n->gid);
		put32(img, p + I2_FSIZE, n->size);
		put32(img, p + I2_ATIME, n->mtime + 1);
		put32(img, p + I2_MTIME, n->mtime);
		put32(img, p + I2_CTIME, n->mtime + 2);
		for (i = 0; i < I2_NZONES; i++)
			put32(img, p + I2_ZONE + 4 * i, n->zone[i]);
	}
}

static void
lay_out(struct image *img, struct node *dir)
{
	unsigned char *buf;
	struct node *c;

	write_dir(img, dir);
	write_inode(img, dir);
	for (c = dir->child; c != NULL; c = c->next) {
		switch (c->type) {
		case T_DIR:
			lay_out(img, c);
			continue;
		case T_FILE:
			buf = contents(c);
			write_data(img, c, buf, c->size);
			free(buf);
			break;
		case T_LINK:
			write_data(img, c, (const unsigned char *)c->target,
			    c->size);
			break;
		case T_CHR:
		case T_BLK:
			c->zone[0] = c->rdev;
			break;
		default:
			break;
		}
		if (c->type != T_HARD && c->type != T_RAW)
			write_inode(img, c);
	}
}

/* The maximum file size stored in the super block, as mkfs writes it. */
static uint32_t
max_size(const struct image *img)
{
	uint64_t bytes, per, zones;
	uint32_t i;

	if (img->version != 1)
		return 0x7fffffff;
	zones = NR_DZONES;
	per = 1;
	for (i = 0; i < img->nlevels; i++) {
		per *= img->nind;
		zones += per;
	}
	bytes = zones * ((uint64_t)img->bsize << img->logzone);
	return bytes < 0x7fffffff ? (uint32_t)bytes : 0x7fffffff;
}

static uint32_t
magic(const struct image *img)
{
	if (img->version == 3)
		return 0x4d5a;
	if (img->version == 2)
		return img->namelen == 14 ? 0x2468 : 0x2478;
	return img->namelen == 14 ? 0x137f : 0x138f;
}

static void
write_super(const struct image *img)
{
	unsigned char *sb;

	sb = img->data + SUPER_OFFSET;
	if (img->version == 3) {
		put32(img, sb + SB3_NINODES, img->ninodes);
		put16(img, sb + SB3_IMAP, img->imap_blocks);
		put16(img, sb + SB3_ZMAP, img->zmap_blocks);
		put16(img, sb + SB3_FIRSTDATA, img->firstdatazone <= 0xffff ?
		    img->firstdatazone : 0);
		put16(img, sb + SB3_LOGZONE, img->logzone);
		put32(img, sb + SB3_MAXSIZE, max_size(img));
		put32(img, sb + SB3_ZONES, img->nzones);
		put16(img, sb + SB3_MAGIC, magic(img));
		put16(img, sb + SB3_BLOCKSIZE, img->bsize);
	} else {
		put16(img, sb + SB12_NINODES, img->ninodes);
		put16(img, sb + SB12_NZONES, img->version == 1 ?
		    img->nzones : 0);
		put16(img, sb + SB12_IMAP, img->imap_blocks);
		put16(img, sb + SB12_ZMAP, img->zmap_blocks);
		put16(img, sb + SB12_FIRSTDATA, img->firstdatazone);
		put16(img, sb + SB12_LOGZONE, img->logzone);
		put32(img, sb + SB12_MAXSIZE, max_size(img));
		put16(img, sb + SB12_MAGIC, magic(img));
		put16(img, sb + SB12_STATE, 1);
		if (img->version == 2)
			put32(img, sb + SB12_ZONES, img->nzones);
	}
}

/* Bit 0 of each map is never used; bits past the end are set. */
static void
write_maps(const struct image *img)
{
	unsigned char *map;
	uint32_t bit, bits;

	bits = img->bsize * 8;
	map = img->data + (size_t)START_BLOCK * img->bsize;
	for (bit = 0; bit < img->imap_blocks * bits; bit++)
		if (bit <= img->next_ino || bit > img->ninodes)
			set_bit(img, map, bit);
	map += (size_t)img->imap_blocks * img->bsize;
	for (bit = 0; bit < img->zmap_blocks * bits; bit++)
		if (bit <= img->next_zone - img->firstdatazone ||
		    bit > img->nzones - img->firstdatazone)
			set_bit(img, map, bit);
}

/* The expected result of extracting the image. */

static void
write_file(const char *path, const unsigned char *buf, uint32_t size)
{
	FILE *f;

	if ((f = fopen(path, "wb")) == NULL)
		err(1, "%s", path);
	if (fwrite(buf, 1, size, f) != size || fclose(f) != 0)
		err(1, "%s", path);
}

static void
expect(const struct node *dir, const char *path)
{
	const struct node *c, *t;
	unsigned char *buf;
	char *p;

	if (mkdir(path, 0755) == -1 && errno != EEXIST)
		err(1, "%s", path);
	for (c = dir->child; c != NULL; c = c->next) {
		if ((p = malloc(strlen(path) + strlen(c->name) + 2)) == NULL)
			err(1, NULL);
		(void)sprintf(p, "%s/%s", path, c->name);
		t = c->type == T_HARD ? c->to : c;
		switch (t->type) {
		case T_DIR:
			if (c->type != T_HARD)
				expect(t, p);
			break;
		case T_FILE:
			buf = contents(t);
			write_file(p, buf, t->size);
			free(buf);
			break;
		case T_LINK:
			if (symlink(t->target, p) == -1)
				err(1, "%s", p);
			break;
		default:
			break;
		}
		free(p);
	}
}

static void
read_spec(struct image *img, const char *spec)
{
	struct parser ps;
	size_t cap;
	FILE *in;
	char *line;

	if (strcmp(spec, "-") == 0)
		in = stdin;
	else if ((in = fopen(spec, "r")) == NULL)
		err(1, "%s", spec);
	ps.img = img;
	ps.lineno = 0;
	line = NULL;
	cap = 0;
	while (getline(&line, &cap, in) != -1) {
		ps.lineno++;
		parse_line(&ps, line);
	}
	if (ferror(in))
		err(1, "%s", spec);
	free(line);
	if (img->data == NULL)
		errx(1, "%s: no fs line", spec);
}

int
main(int argc, char **argv)
{
	struct image img;
	FILE *out;
	const char *expectdir;
	int ch;

	expectdir = NULL;
	while ((ch = getopt(argc, argv, "e:")) != -1) {
		switch (ch) {
		case 'e':
			expectdir = optarg;
			break;
		default:
			usage();
		}
	}
	if (argc - optind != 2)
		usage();

	(void)memset(&img, 0, sizeof(img));
	read_spec(&img, argv[optind]);
	count_links(&img.root);
	lay_out(&img, &img.root);
	write_super(&img);
	write_maps(&img);

	if ((out = fopen(argv[optind + 1], "wb")) == NULL)
		err(1, "%s", argv[optind + 1]);
	if (fwrite(img.data, img.bsize, img.nblocks, out) != img.nblocks ||
	    fclose(out) != 0)
		err(1, "%s", argv[optind + 1]);
	if (expectdir != NULL)
		expect(&img.root, expectdir);
	return 0;
}
