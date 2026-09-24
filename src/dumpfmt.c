/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * dumpfmt.c - encode and decode the records of the dump format; see
 * dumpfmt.h.  Like the library, this never prints and keeps no state.
 */

#include <errno.h>
#include <string.h>

#include "dumpfmt.h"
#include "layout.h"

/*
 * A header of NFS_MAGIC or UFS2_MAGIC: byte offsets.  The fields for
 * which UFS2 has 64-bit ones keep the 32-bit value that NFS_MAGIC uses.
 */
#define DH_TYPE		0
#define DH_OLD_DATE	4
#define DH_OLD_DDATE	8
#define DH_VOLUME	12
#define DH_OLD_TAPEA	16
#define DH_INUMBER	20
#define DH_MAGIC	24		/* also in an OFS header */
#define DH_CHECKSUM	28
#define DH_DINODE	32		/* 128 bytes, below */
#define DH_COUNT	160
#define DH_ADDR		164		/* DUMP_NADDR bytes */
#define DH_LABEL	676
#define DH_LEVEL	692
#define DH_FILESYS	696
#define DH_DEV		760
#define DH_HOST		824
#define DH_FLAGS	888
#define DH_OLD_FIRSTREC	892
#define DH_DATE		896		/* 64-bit from here */
#define DH_DDATE	904
#define DH_TAPEA	912
#define DH_FIRSTREC	920

/* The inode, from DH_DINODE: that of UFS1 and the fields of UFS2. */
#define DI_MODE		0		/* 16 */
#define DI_NLINK	2		/* 16, UFS1 */
#define DI_OLDUID	4		/* 16, before DR_NEWINODEFMT */
#define DI_OLDGID	6		/* 16 */
#define DI_SIZE		8		/* 64 */
#define DI_ATIME	16		/* 32, UFS1 */
#define DI_MTIME	24		/* 32, UFS1 */
#define DI_CTIME	32		/* 32, UFS1 */
#define DI_RDEV		40		/* 32: the first block pointer */
#define DI2_ATIME	56		/* 64, UFS2 */
#define DI2_MTIME	64		/* 64, UFS2 */
#define DI2_EXTSIZE	72		/* 32, UFS2 */
#define DI_UID		112		/* 32 */
#define DI_GID		116		/* 32 */

/* A header of OFS_MAGIC. */
#define OH_TYPE		0
#define OH_DATE		4
#define OH_DDATE	8
#define OH_VOLUME	12
#define OH_TAPEA	16
#define OH_INUMBER	20		/* 16 */
#define OH_CHECKSUM	28
#define OH_DINODE	32		/* 64 bytes, below */
#define OH_COUNT	96
#define OH_ADDR		100		/* DUMP_OLD_NADDR bytes */

#define ODI_MODE	0		/* 16 */
#define ODI_NLINK	2		/* 16 */
#define ODI_UID		4		/* 16 */
#define ODI_GID		6		/* 16 */
#define ODI_SIZE	8		/* 32 */
#define ODI_RDEV	12		/* 32 */
#define ODI_ATIME	52		/* 32 */
#define ODI_MTIME	56
#define ODI_CTIME	60

/* A directory entry of 4.2BSD and later, and one of V7. */
#define DE_INO		0		/* 32 */
#define DE_RECLEN	4		/* 16 */
#define DE_TYPE		6		/* 8, new format */
#define DE_NAMLEN	7		/* 8, new format */
#define DE_OLDNAMLEN	6		/* 16, old format */
#define DE_NAME		8
#define DE_ALIGN	4		/* entries start at multiples of 4 */
#define V7_INO		0		/* 16 */
#define V7_NAME		2
#define V7_SIZE		16

/* Runs of Linux: one byte up to this many records, two past it. */
#define RUN_SHORT	64
#define RUN_LONG_FLAG	0x80

#define WORDS		(DUMP_BSIZE / 4)

static uint64_t
load64(enum mfs_order order, const unsigned char *p)
{
	uint64_t lo, hi;

	lo = load32(order, p);
	hi = load32(order, p + 4);
	if (order == MFS_BIG_ENDIAN)
		return (lo << 32) | hi;
	return (hi << 32) | lo;
}

static void
put64(enum mfs_order order, unsigned char *p, uint64_t v)
{
	if (order == MFS_BIG_ENDIAN) {
		put32(order, p, (uint32_t)(v >> 32));
		put32(order, p + 4, (uint32_t)v);
	} else {
		put32(order, p, (uint32_t)v);
		put32(order, p + 4, (uint32_t)(v >> 32));
	}
}

/* A signed 32-bit field, widened. */
static int64_t
load_s32(enum mfs_order order, const unsigned char *p)
{
	uint32_t v;

	v = load32(order, p);
	return v >= 0x80000000U ? (int64_t)v - 0x100000000LL : (int64_t)v;
}

/* The sum of the words of a header, which is DUMP_CHECKSUM if it is good. */
static uint32_t
sum_words(enum mfs_order order, const unsigned char *rec)
{
	uint32_t sum;
	int i;

	sum = 0;
	for (i = 0; i < WORDS; i++)
		sum += load32(order, rec + 4 * i);
	return sum;
}

/* Copy a string field of len bytes, which need not end with a NUL. */
static void
get_string(char *dst, const unsigned char *src, size_t len)
{
	(void)memcpy(dst, src, len);
	dst[len] = '\0';
}

/* The byte orders a dump may have been written in. */
static const enum mfs_order orders[] = {
	MFS_LITTLE_ENDIAN, MFS_BIG_ENDIAN
};

/* The magic number of rec, which also gives its byte order. */
static int
find_magic(const unsigned char *rec, struct dump_header *h)
{
	uint32_t magic;
	size_t i;

	for (i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
		magic = load32(orders[i], rec + DH_MAGIC);
		h->order = orders[i];
		if (magic == DUMP_NFS_MAGIC)
			h->kind = DUMP_NFS;
		else if (magic == DUMP_UFS2_MAGIC)
			h->kind = DUMP_UFS2;
		else if (magic == DUMP_OFS_MAGIC)
			h->kind = DUMP_OFS;
		else
			continue;
		return 0;
	}
	return -EINVAL;
}

static void
get_old_header(const unsigned char *rec, struct dump_header *h)
{
	enum mfs_order o;
	const unsigned char *di;

	o = h->order;
	di = rec + OH_DINODE;
	h->type = (int32_t)load32(o, rec + OH_TYPE);
	h->date = load_s32(o, rec + OH_DATE);
	h->ddate = load_s32(o, rec + OH_DDATE);
	h->volume = (int32_t)load32(o, rec + OH_VOLUME);
	h->tapea = load_s32(o, rec + OH_TAPEA);
	h->inumber = load16(o, rec + OH_INUMBER);
	h->count = (int32_t)load32(o, rec + OH_COUNT);
	h->mode = (uint16_t)load16(o, di + ODI_MODE);
	h->nlink = (uint16_t)load16(o, di + ODI_NLINK);
	h->uid = load16(o, di + ODI_UID);
	h->gid = load16(o, di + ODI_GID);
	h->size = load32(o, di + ODI_SIZE);
	h->rdev = load32(o, di + ODI_RDEV);
	h->atime = load_s32(o, di + ODI_ATIME);
	h->mtime = load_s32(o, di + ODI_MTIME);
	h->ctime = load_s32(o, di + ODI_CTIME);
	h->naddr = DUMP_OLD_NADDR;
	(void)memcpy(h->addr, rec + OH_ADDR, DUMP_OLD_NADDR);
}

static void
get_new_inode(const unsigned char *rec, struct dump_header *h)
{
	enum mfs_order o;
	const unsigned char *di;

	o = h->order;
	di = rec + DH_DINODE;
	h->mode = (uint16_t)load16(o, di + DI_MODE);
	h->size = load64(o, di + DI_SIZE);
	h->rdev = load32(o, di + DI_RDEV);
	if (h->kind == DUMP_UFS2) {
		h->atime = (int64_t)load64(o, di + DI2_ATIME);
		h->mtime = (int64_t)load64(o, di + DI2_MTIME);
		h->extsize = load32(o, di + DI2_EXTSIZE);
	} else {
		h->nlink = (uint16_t)load16(o, di + DI_NLINK);
		h->atime = load_s32(o, di + DI_ATIME);
		h->mtime = load_s32(o, di + DI_MTIME);
		h->ctime = load_s32(o, di + DI_CTIME);
	}
	if (h->kind == DUMP_NFS && (h->flags & DR_NEWINODEFMT) == 0) {
		h->uid = load16(o, di + DI_OLDUID);
		h->gid = load16(o, di + DI_OLDGID);
	} else {
		h->uid = load32(o, di + DI_UID);
		h->gid = load32(o, di + DI_GID);
	}
}

static void
get_new_header(const unsigned char *rec, struct dump_header *h)
{
	enum mfs_order o;

	o = h->order;
	h->type = (int32_t)load32(o, rec + DH_TYPE);
	h->volume = (int32_t)load32(o, rec + DH_VOLUME);
	h->inumber = load32(o, rec + DH_INUMBER);
	h->count = (int32_t)load32(o, rec + DH_COUNT);
	h->level = (int32_t)load32(o, rec + DH_LEVEL);
	h->flags = (int32_t)load32(o, rec + DH_FLAGS);
	get_string(h->label, rec + DH_LABEL, DUMP_LABEL);
	get_string(h->filesys, rec + DH_FILESYS, DUMP_NAMELEN);
	get_string(h->dev, rec + DH_DEV, DUMP_NAMELEN);
	get_string(h->host, rec + DH_HOST, DUMP_NAMELEN);
	if (h->kind == DUMP_UFS2) {
		h->date = (int64_t)load64(o, rec + DH_DATE);
		h->ddate = (int64_t)load64(o, rec + DH_DDATE);
		h->tapea = (int64_t)load64(o, rec + DH_TAPEA);
		h->firstrec = (int64_t)load64(o, rec + DH_FIRSTREC);
	} else {
		/* Linux keeps other fields where UFS2 has 64-bit ones. */
		h->date = load_s32(o, rec + DH_OLD_DATE);
		h->ddate = load_s32(o, rec + DH_OLD_DDATE);
		h->tapea = load_s32(o, rec + DH_OLD_TAPEA);
		h->firstrec = load_s32(o, rec + DH_OLD_FIRSTREC);
	}
	h->naddr = DUMP_NADDR;
	(void)memcpy(h->addr, rec + DH_ADDR, DUMP_NADDR);
	get_new_inode(rec, h);
}

int
dump_get_header(const unsigned char *rec, struct dump_header *h)
{
	(void)memset(h, 0, sizeof(*h));
	if (find_magic(rec, h) < 0)
		return -EINVAL;
	if (sum_words(h->order, rec) != DUMP_CHECKSUM)
		return -EIO;
	if (h->kind == DUMP_OFS)
		get_old_header(rec, h);
	else
		get_new_header(rec, h);
	return 0;
}

static void
put_string(unsigned char *dst, const char *src, size_t len)
{
	size_t n;

	n = strlen(src);
	(void)memcpy(dst, src, n < len ? n : len);
}

void
dump_put_header(const struct dump_header *h, unsigned char *rec)
{
	enum mfs_order o;
	unsigned char *di;

	o = h->order;
	di = rec + DH_DINODE;
	(void)memset(rec, 0, DUMP_BSIZE);
	put32(o, rec + DH_TYPE, (uint32_t)h->type);
	put32(o, rec + DH_OLD_DATE, (uint32_t)h->date);
	put32(o, rec + DH_OLD_DDATE, (uint32_t)h->ddate);
	put32(o, rec + DH_VOLUME, (uint32_t)h->volume);
	put32(o, rec + DH_OLD_TAPEA, (uint32_t)h->tapea);
	put32(o, rec + DH_INUMBER, h->inumber);
	put32(o, rec + DH_MAGIC, DUMP_NFS_MAGIC);
	put16(o, di + DI_MODE, h->mode);
	put16(o, di + DI_NLINK, h->nlink);
	put64(o, di + DI_SIZE, h->size);
	put32(o, di + DI_ATIME, (uint32_t)h->atime);
	put32(o, di + DI_MTIME, (uint32_t)h->mtime);
	put32(o, di + DI_CTIME, (uint32_t)h->ctime);
	put32(o, di + DI_RDEV, h->rdev);
	put32(o, di + DI_UID, h->uid);
	put32(o, di + DI_GID, h->gid);
	put32(o, rec + DH_COUNT, (uint32_t)h->count);
	(void)memcpy(rec + DH_ADDR, h->addr, DUMP_NADDR);
	put_string(rec + DH_LABEL, h->label, DUMP_LABEL);
	put32(o, rec + DH_LEVEL, (uint32_t)h->level);
	put_string(rec + DH_FILESYS, h->filesys, DUMP_NAMELEN);
	put_string(rec + DH_DEV, h->dev, DUMP_NAMELEN);
	put_string(rec + DH_HOST, h->host, DUMP_NAMELEN);
	put32(o, rec + DH_FLAGS, (uint32_t)h->flags);
	put32(o, rec + DH_OLD_FIRSTREC, (uint32_t)h->firstrec);
	put64(o, rec + DH_DATE, (uint64_t)h->date);
	put64(o, rec + DH_DDATE, (uint64_t)h->ddate);
	put64(o, rec + DH_TAPEA, (uint64_t)h->tapea);
	put64(o, rec + DH_FIRSTREC, (uint64_t)h->firstrec);
	put32(o, rec + DH_CHECKSUM, DUMP_CHECKSUM - sum_words(o, rec));
}

/* Whether c_addr holds runs of Linux rather than one entry a record. */
static int
has_runs(const struct dump_header *h)
{
	int i;

	for (i = 0; i < h->count; i++)
		if (h->addr[i] > 1)
			return 1;
	return 0;
}

int
dump_addr_runs(const struct dump_header *h, struct dump_run *runs)
{
	unsigned b;
	int i, n, runs_of_linux;

	if (h->count < 0 || h->count > h->naddr)
		return -EINVAL;
	runs_of_linux = has_runs(h);
	n = 0;
	for (i = 0; i < h->count; i++) {
		b = h->addr[i];
		runs[n].data = b & 1;
		if (!runs_of_linux)
			runs[n].len = 1;
		else if ((b & RUN_LONG_FLAG) == 0)
			runs[n].len = (b >> 1) + 1;
		else if (++i < h->count)
			runs[n].len = (uint32_t)h->addr[i] * RUN_SHORT +
			    ((b >> 1) & (RUN_SHORT - 1)) + RUN_SHORT + 1;
		else
			return -EINVAL;
		n++;
	}
	return n;
}

int64_t
dump_data_records(const struct dump_header *h)
{
	struct dump_run runs[DUMP_NADDR];
	int64_t total;
	int i, n;

	switch (h->type) {
	case TS_BITS:
	case TS_CLRI:
		return h->count < 0 ? -EINVAL : h->count;
	case TS_INODE:
	case TS_ADDR:
		if ((n = dump_addr_runs(h, runs)) < 0)
			return n;
		total = 0;
		for (i = 0; i < n; i++)
			if (runs[i].data)
				total += runs[i].len;
		return total;
	default:
		return 0;
	}
}

size_t
dump_dirent_size(size_t len)
{
	/* The name ends with a NUL; the entry fills out to DE_ALIGN. */
	return (DE_NAME + len + 1 + DE_ALIGN - 1) & ~(size_t)(DE_ALIGN - 1);
}

void
dump_put_dirent(unsigned char *p, enum mfs_order order, uint32_t ino,
    size_t reclen, unsigned type, const char *name, size_t len)
{
	(void)memset(p, 0, reclen);
	put32(order, p + DE_INO, ino);
	put16(order, p + DE_RECLEN, (uint32_t)reclen);
	p[DE_TYPE] = (unsigned char)type;
	p[DE_NAMLEN] = (unsigned char)len;
	(void)memcpy(p + DE_NAME, name, len);
}

void
dump_set_reclen(unsigned char *p, enum mfs_order order, size_t reclen)
{
	put16(order, p + DE_RECLEN, (uint32_t)reclen);
}

static int
get_v7_dirent(const unsigned char *p, size_t avail, enum mfs_order order,
    struct dump_dirent *de)
{
	size_t n;

	if (avail < V7_SIZE)
		return -EINVAL;
	de->ino = load16(order, p + V7_INO);
	de->type = DUMP_DT_UNKNOWN;
	for (n = 0; n < DUMP_V7_NAME && p[V7_NAME + n] != '\0'; n++)
		continue;
	(void)memcpy(de->name, p + V7_NAME, n);
	de->name[n] = '\0';
	de->namelen = n;
	return V7_SIZE;
}

int
dump_get_dirent(const unsigned char *p, size_t avail, enum dump_dirfmt fmt,
    enum mfs_order order, struct dump_dirent *de)
{
	size_t len, reclen;

	if (fmt == DUMP_DIR_V7)
		return get_v7_dirent(p, avail, order, de);
	if (avail < DE_NAME)
		return -EINVAL;
	de->ino = load32(order, p + DE_INO);
	reclen = load16(order, p + DE_RECLEN);
	if (fmt == DUMP_DIR_NEW) {
		de->type = p[DE_TYPE];
		len = p[DE_NAMLEN];
	} else {
		de->type = DUMP_DT_UNKNOWN;
		len = load16(order, p + DE_OLDNAMLEN);
	}
	if (reclen < DE_NAME || reclen % DE_ALIGN != 0 || reclen > avail ||
	    len > DUMP_MAXNAME)
		return -EINVAL;
	/* A free entry may be as short as the header. */
	if (de->ino == 0) {
		de->namelen = 0;
		de->name[0] = '\0';
		return (int)reclen;
	}
	if (reclen < dump_dirent_size(len) ||
	    memchr(p + DE_NAME, '\0', len) != NULL)
		return -EINVAL;
	(void)memcpy(de->name, p + DE_NAME, len);
	de->name[len] = '\0';
	de->namelen = len;
	return (int)reclen;
}
