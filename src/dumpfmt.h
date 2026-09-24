/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * dumpfmt.h - the dump format of BSD, as dump_minixfs writes it and
 * restore_minixfs reads it.
 *
 * A dump is a stream of records of DUMP_BSIZE bytes.  A header record
 * names its type; the data records that follow it are counted in the
 * header.  In order:
 *
 *	TS_TAPE			the dump: dates, level, label, names
 *	TS_CLRI + map		a bit for each inode in use when dumped
 *	TS_BITS + map		a bit for each inode in this dump
 *	TS_INODE + data		each directory, then each other file, in
 *	[TS_ADDR + data ...]	inode order; TS_ADDR goes on with a file
 *				whose blocks do not fit one header
 *	TS_END			the end
 *
 * Bit i - 1 of a map (bit 0 is the low bit of the first byte) stands for
 * inode i.  Inode 2 is the root and 1 is unused.  In a TS_INODE or TS_ADDR
 * header, each c_addr entry stands for one data record of the file: 1 if
 * the record follows, 0 for a record of a hole, which does not.  The data
 * of a directory is its entries, in chunks of 1024 bytes that no entry
 * crosses.  Every number is in the byte order of the machine that wrote
 * the dump; a reader finds it from the magic number.  The words of a
 * header add up to DUMP_CHECKSUM.
 *
 * There are three headers:
 *
 *	OFS_MAGIC	the dump of an old file system, of V7 or 4.1BSD:
 *			16-bit inode numbers and ids, 256 c_addr entries
 *	NFS_MAGIC	that of the new file system of 4.2BSD, FFS: the
 *			inode of UFS1, 512 entries; NetBSD and FreeBSD for
 *			UFS1, and Linux for ext2, 3 and 4
 *	UFS2_MAGIC	NetBSD and FreeBSD for UFS2: 64-bit times, no
 *			ctime, and extended attributes after the data
 *
 * The directory entries of 4.4BSD (DR_NEWINODEFMT) carry a type and an
 * 8-bit name length; those of 4.2BSD and 4.3BSD a 16-bit name length;
 * those of V7, in dumps of OFS_MAGIC, are 16 bytes, 2 for the inode
 * number and 14 for the name.  Linux dump adds
 * its extended attributes as TS_INODE records with DR_EXTATTRIBUTES, and
 * since 0.4b49 packs runs of blocks into c_addr: an entry 0nnnnnnd is n +
 * 1 records, and 1nnnnnnd NNNNNNNN is N * 64 + n + 65 of them, d telling
 * data from hole; c_count counts bytes.  Entries of 0 and 1 mean the same
 * in both forms.
 *
 * The layout follows include/protocols/dumprestore.h of NetBSD 10 and
 * compat/include/protocols/dumprestore.h of Linux dump 0.4b56.
 */

#ifndef DUMPFMT_H
#define DUMPFMT_H

#include <stddef.h>
#include <stdint.h>

#include "mfs.h"

#define DUMP_BSIZE	1024		/* TP_BSIZE: bytes of a record */
#define DUMP_NTREC	10		/* records written at a time */
#define DUMP_NADDR	512		/* c_addr entries of a new header */
#define DUMP_OLD_NADDR	256		/* of an OFS header */
#define DUMP_LABEL	16		/* bytes of c_label */
#define DUMP_NAMELEN	64		/* of c_filesys, c_dev and c_host */
#define DUMP_WINO	1		/* whiteouts in directories */
#define DUMP_ROOTINO	2		/* the root directory */
#define DUMP_MAXNAME	255		/* longest name in a directory */
#define DUMP_DIRBLK	1024		/* a chunk of a directory */
#define DUMP_V7_NAME	14		/* bytes of a name of V7 */

#define DUMP_OFS_MAGIC	60011
#define DUMP_NFS_MAGIC	60012
#define DUMP_UFS2_MAGIC	0x19540119
#define DUMP_CHECKSUM	84446

/* Record types. */
#define TS_TAPE		1
#define TS_INODE	2
#define TS_BITS		3
#define TS_ADDR		4
#define TS_END		5
#define TS_CLRI		6

/* c_flags. */
#define DR_NEWHEADER	0x0001		/* the TS_TAPE of a new header */
#define DR_NEWINODEFMT	0x0002		/* 4.4BSD inodes and entries */
#define DR_COMPRESSED	0x0080		/* Linux: compressed records */
#define DR_METAONLY	0x0100		/* Linux: inodes without data */
#define DR_EXTATTRIBUTES 0x8000		/* Linux: extended attributes */

/* d_type of a directory entry. */
#define DUMP_DT_UNKNOWN	0
#define DUMP_DT_FIFO	1
#define DUMP_DT_CHR	2
#define DUMP_DT_DIR	4
#define DUMP_DT_BLK	6
#define DUMP_DT_REG	8
#define DUMP_DT_LNK	10
#define DUMP_DT_SOCK	12

enum dump_kind {
	DUMP_OFS,
	DUMP_NFS,
	DUMP_UFS2
};

enum dump_dirfmt {
	DUMP_DIR_NEW,			/* 4.4BSD */
	DUMP_DIR_OLD,			/* 4.2BSD, 4.3BSD */
	DUMP_DIR_V7
};

/* A header, in host byte order. */
struct dump_header {
	enum dump_kind	kind;
	enum mfs_order	order;		/* of the dump */
	int32_t		type;
	int32_t		volume;
	int32_t		count;		/* c_count */
	int32_t		level;
	int32_t		flags;
	int64_t		date;
	int64_t		ddate;		/* of the dump this one follows */
	int64_t		tapea;		/* number of this record */
	int64_t		firstrec;
	uint32_t	inumber;
	char		label[DUMP_LABEL + 1];
	char		filesys[DUMP_NAMELEN + 1];
	char		dev[DUMP_NAMELEN + 1];
	char		host[DUMP_NAMELEN + 1];

	/* The inode of TS_INODE and TS_ADDR. */
	uint64_t	size;
	int64_t		atime;
	int64_t		mtime;
	int64_t		ctime;		/* UFS2: none, 0 */
	uint32_t	rdev;
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	extsize;	/* UFS2: bytes of attributes */
	uint16_t	mode;
	uint16_t	nlink;		/* UFS2: none, 0 */

	int		naddr;		/* entries c_addr can hold */
	unsigned char	addr[DUMP_NADDR];
};

/* A run of records in c_addr: len of them, data or a hole. */
struct dump_run {
	uint32_t	len;
	int		data;
};

/* A directory entry. */
struct dump_dirent {
	uint32_t	ino;		/* 0: not in use */
	unsigned	type;		/* DUMP_DT_*; old formats: unknown */
	size_t		namelen;
	char		name[DUMP_MAXNAME + 1];
};

/*
 * Decode the header record rec into *h.  Returns 0; -EINVAL if rec has no
 * magic number of a header; -EIO if its words do not add up.
 */
int	dump_get_header(const unsigned char *, struct dump_header *);

/*
 * Encode *h as an NFS_MAGIC header in byte order h->order into rec, as
 * NetBSD dump writes one for UFS1: the 32-bit dates, and the 64-bit ones
 * after c_firstrec, which the restore of NetBSD reads from TS_TAPE for
 * the dates of the dump; the inode of UFS1; and the checksum.  The
 * restore of Linux takes the 64-bit date for its c_ntrec, and refuses the
 * dump, as it does those of NetBSD for UFS1.
 */
void	dump_put_header(const struct dump_header *, unsigned char *);

/*
 * The runs of records that the c_addr of the TS_INODE or TS_ADDR header
 * *h lists, into runs, which holds DUMP_NADDR.  Returns their number, or
 * -EINVAL if c_count or an entry is out of range.
 */
int	dump_addr_runs(const struct dump_header *, struct dump_run *);

/* The number of data records that follow the header *h, or -EINVAL. */
int64_t	dump_data_records(const struct dump_header *);

/* The bytes an entry of the new format with a name of len bytes takes. */
size_t	dump_dirent_size(size_t);

/*
 * Write an entry of the new format at p: inode ino, reclen bytes long,
 * of type type, with the name of len bytes at name.
 */
void	dump_put_dirent(unsigned char *, enum mfs_order, uint32_t, size_t,
	    unsigned, const char *, size_t);

/* Make the entry at p reclen bytes long. */
void	dump_set_reclen(unsigned char *, enum mfs_order, size_t);

/*
 * Read the entry at p, of format fmt and byte order order, where avail
 * bytes are left in its chunk.  Returns the bytes it takes, or -EINVAL
 * if it is damaged.
 */
int	dump_get_dirent(const unsigned char *, size_t, enum dump_dirfmt,
	    enum mfs_order, struct dump_dirent *);

#endif /* DUMPFMT_H */
