/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mfs.h - read access to MINIX file system images, and making new ones.
 *
 * The library opens an image file, recognises the file system version and
 * byte order from the super block, and gives access to inodes, file data
 * and directories; mfs_plan() and mfs_format() make empty file systems.
 * The library never prints and has no global state.  Functions
 * return 0 (or a byte count) on success and a negative errno value on
 * failure, so that the result can be handed straight back to FUSE.
 */

#ifndef MFS_H
#define MFS_H

#include <sys/types.h>

#include <stddef.h>
#include <stdint.h>

#define MFS_ROOT_INO	1		/* inode number of the root directory */
#define MFS_MAX_NAME	60		/* longest name of any version */
#define MFS_NR_ZONES	10		/* zone slots in the largest inode */

/* Super block magic numbers, in the byte order of the image. */
#define MFS_MAGIC_V1	0x137f		/* V1, 14-character names */
#define MFS_MAGIC_V1L	0x138f		/* V1, 30-character names (Linux) */
#define MFS_MAGIC_V2	0x2468		/* V2, 14-character names */
#define MFS_MAGIC_V2L	0x2478		/* V2, 30-character names (Linux) */
#define MFS_MAGIC_V3	0x4d5a		/* V3, 60-character names */

/*
 * Whether the file system was left in order.  MINIX leaves this word
 * zero in V1 and V2, where Linux keeps its state in it; MINIX 3 keeps
 * flags in V3, and mounts a file system that is not clean read-only.
 */
#define MFS_STATE_VALID	0x0001		/* V1, V2: cleanly unmounted */
#define MFS_STATE_ERROR	0x0002		/* V1, V2: errors were found */
#define MFS_FLAG_CLEAN	0x0001		/* V3: cleanly unmounted */
#define MFS_FLAG_MANDATORY 0xff00	/* V3: features one must know */
#define MFS_VMD_FLEX	0x01		/* Minix-vmd: flex directories */
#define MFS_VMD_CLEAN	0x02		/* Minix-vmd: cleanly unmounted */

/* File types in the mode word; the values are those of MINIX. */
#define MFS_S_IFMT	0170000
#define MFS_S_IFIFO	0010000
#define MFS_S_IFCHR	0020000
#define MFS_S_IFDIR	0040000
#define MFS_S_IFBLK	0060000
#define MFS_S_IFREG	0100000
#define MFS_S_IFLNK	0120000
#define MFS_S_IFSOCK	0140000

enum mfs_order {
	MFS_LITTLE_ENDIAN,
	MFS_BIG_ENDIAN
};

/*
 * How the image file holds the device.  With size 0, byte for byte.
 * Otherwise the file holds tracks of size bytes, heads of them to each
 * cylinder, and the device is the tracks of one side: as when a
 * single-sided disk was read as double-sided, and every other track of
 * the image is empty.
 */
struct mfs_tracks {
	uint32_t	size;		/* bytes in a track; 0: no tracks */
	uint32_t	heads;		/* tracks in a cylinder */
	uint32_t	side;		/* which of them: 0 .. heads - 1 */
};

/* An open file system image. */
struct mfs {
	struct mfs_tracks tracks;	/* how the file holds the device */
	off_t		file_size;	/* bytes in the image file */
	off_t		image_size;	/* bytes of the device in it */
	uint64_t	max_file;	/* bytes the zone slots can address */
	unsigned char	*ibuf;		/* one block: inodes, indirects */
	unsigned char	*dbuf;		/* one block, for file data */
	enum mfs_order	order;
	int		fd;
	int		writable;	/* opened by mfs_open_rw() */
	unsigned char	*imap;		/* kept by mfs_write.c, or NULL */
	unsigned char	*zmap;
	uint32_t	inext;		/* where to look for a free inode */
	uint32_t	znext;		/* and for a free zone, as map bits */
	int		version;	/* 1, 2 or 3 */

	/* From the super block. */
	uint32_t	ninodes;
	uint32_t	nzones;
	uint32_t	imap_blocks;
	uint32_t	zmap_blocks;
	uint32_t	firstdatazone;
	uint32_t	log_zone_size;
	uint32_t	max_size;
	uint32_t	block_size;	/* V1 and V2: always 1024 */
	uint16_t	magic;
	uint16_t	state;		/* V1, V2: MFS_STATE_*; V3: flags;
					   Minix-vmd: MFS_VMD_* */
	int		vmd;		/* the super block of Minix-vmd */
	int		flex;		/* flex directories of Minix-vmd */

	/* Derived from the super block. */
	uint32_t	namelen;	/* bytes of a name in an entry; flex:
					   the longest name */
	uint32_t	dirent_size;	/* bytes of a directory entry; flex:
					   of a slot */
	uint32_t	dirent_ino;	/* bytes of the inode number in it */
	uint32_t	inode_size;	/* bytes of an on-disk inode */
	uint32_t	zone_num_size;	/* bytes of a zone number */
	uint32_t	ndzones;	/* direct zones in an inode */
	uint32_t	nlevels;	/* levels of indirection: 2 or 3 */
	uint32_t	nindirs;	/* zone numbers in an indirect block */
	uint32_t	inode_start;	/* first block of the inode table */
	uint32_t	nblocks;	/* blocks the file system claims */
};

/* An inode, in host byte order. */
struct mfs_inode {
	uint32_t	num;
	uint32_t	size;
	uint32_t	atime;		/* V1: a copy of mtime */
	uint32_t	mtime;
	uint32_t	ctime;		/* V1: a copy of mtime */
	uint32_t	zone[MFS_NR_ZONES];
	uint16_t	mode;
	uint16_t	nlinks;
	uint16_t	uid;
	uint16_t	gid;
};

/* A directory entry that is in use. */
struct mfs_dirent {
	uint32_t	ino;
	uint32_t	off;			/* offset in the directory */
	char		name[MFS_MAX_NAME + 1];	/* NUL-terminated */
};

/*
 * Called by mfs_readdir() for each entry in use.  A non-zero return value
 * stops the walk and becomes the return value of mfs_readdir().
 */
typedef int (*mfs_dirent_fn)(const struct mfs_dirent *, void *);

/*
 * Open the image at path and check its super block.  Returns 0, -EINVAL
 * if the image is not a consistent MINIX file system, -ENOTSUP if it
 * needs features that this library does not know, or another negative
 * errno value.  On success the caller releases fs with mfs_close().
 */
int	mfs_open(struct mfs *, const char *);

/* As mfs_open(), for reading and writing. */
int	mfs_open_rw(struct mfs *, const char *);

/*
 * As mfs_open() or, if rw is not 0, mfs_open_rw(), for an image file that
 * holds the device as *tracks says; NULL means byte for byte.
 */
int	mfs_open_tracks(struct mfs *, const char *, int,
	    const struct mfs_tracks *);

/*
 * Parse "SIZE:HEADS:SIDE" into *tracks.  Returns 0, or -EINVAL if the
 * text is not three numbers, SIZE is 0 or SIDE is not below HEADS.
 */
int	mfs_parse_tracks(const char *, struct mfs_tracks *);

/* Release what mfs_open() acquired. */
void	mfs_close(struct mfs *);

/*
 * Read len bytes of the device at byte offset off, through the tracks
 * of -T if any.  Returns 0, or -EIO past the end of the image.
 */
int	mfs_read_device(struct mfs *, void *, size_t, off_t);

/* As mfs_read_device(), for writing.  Returns 0 or -errno. */
int	mfs_write_device(struct mfs *, const void *, size_t, off_t);

/*
 * Read block number block of the file system into buf, which holds
 * block_size bytes.  Returns 0, or -EIO for a block outside the file
 * system or the image.
 */
int	mfs_read_block(struct mfs *, uint32_t, void *);

/*
 * Read inode number num into *ip.  Returns 0, or -EIO for a number
 * outside the inode table or an inode whose size its zones cannot reach.
 */
int	mfs_read_inode(struct mfs *, uint32_t, struct mfs_inode *);

/*
 * Map block fblock of the file *ip to a block of the file system in
 * *block; a hole maps to 0.  Returns 0, -EIO for a zone number outside
 * the data area, or -EFBIG for a block beyond the zone slots.
 */
int	mfs_bmap(struct mfs *, const struct mfs_inode *, uint32_t, uint32_t *);

/*
 * Read up to len bytes of the file *ip at offset off into buf.  Returns
 * the number of bytes read (0 at the end of the file) or a negative errno
 * value.  Device inodes give -EINVAL, since their zone holds a device
 * number.
 */
ssize_t	mfs_pread(struct mfs *, const struct mfs_inode *, void *, size_t,
	    uint32_t);

/*
 * Call fn for each entry in use of the directory *dp, including "." and
 * "..".  Returns 0, the first non-zero value fn returned, or a negative
 * errno value (-ENOTDIR if *dp is not a directory).
 */
int	mfs_readdir(struct mfs *, const struct mfs_inode *, mfs_dirent_fn,
	    void *);

/*
 * Look up name in the directory *dp and store its inode number in *ino.
 * Returns 0, -ENOENT, -ENAMETOOLONG or another negative errno value.
 */
int	mfs_lookup(struct mfs *, const struct mfs_inode *, const char *,
	    uint32_t *);

/*
 * Find the inode of path, taken from the root directory whether or not
 * it starts with '/', and read it into *ip.  Symbolic links are not
 * followed.  Returns 0 or a negative errno value.
 */
int	mfs_namei(struct mfs *, const char *, struct mfs_inode *);

/* The file type of an inode. */
int	mfs_is_dir(const struct mfs_inode *);
int	mfs_is_reg(const struct mfs_inode *);
int	mfs_is_lnk(const struct mfs_inode *);
int	mfs_is_dev(const struct mfs_inode *);

/* The device number of a device inode. */
uint32_t mfs_rdev(const struct mfs_inode *);

/* Whether the super block says that the file system is clean. */
int	mfs_is_clean(const struct mfs *);

/*
 * Checking file systems.
 */

/*
 * Read inode number num into *ip without checking its contents.  Returns
 * 0, or -EIO for a number outside the inode table.
 */
int	mfs_get_inode(struct mfs *, uint32_t, struct mfs_inode *);

/*
 * Where a zone number is kept: slot index of the inode if block is 0,
 * entry index of the indirect block number block otherwise.
 */
struct mfs_zref {
	uint32_t	block;
	uint32_t	index;
};

/*
 * Called by mfs_walk_zones() for each zone number in use, with level 0
 * for a data zone and 1, 2 or 3 for an indirect zone of that level, and
 * where the number is kept.  For an indirect zone, MFS_WALK_SKIP skips
 * the zones it lists.  Any other non-zero return value stops the walk and
 * becomes its return value.
 */
typedef int (*mfs_zone_fn)(uint32_t, int, const struct mfs_zref *, void *);

#define MFS_WALK_SKIP	1

/*
 * Call fn for each zone of the file *ip: the direct zones, then each
 * indirect zone followed by what it lists.  Zone numbers outside the data
 * area are reported but not followed.  Returns 0, the first non-zero
 * value fn returned, or a negative errno value.
 */
int	mfs_walk_zones(struct mfs *, const struct mfs_inode *, mfs_zone_fn,
	    void *);

enum mfs_map {
	MFS_IMAP,			/* the inode map */
	MFS_ZMAP			/* the zone map */
};

/*
 * Read a whole bit map into new memory in *mapp, which the caller frees.
 * Returns 0 or a negative errno value.
 */
int	mfs_load_map(struct mfs *, enum mfs_map, unsigned char **);

/*
 * Bit n of a map from mfs_load_map().  Bit i of the inode map is inode
 * i; bit i of the zone map is zone firstdatazone + i - 1; bit 0 of both
 * is never used.
 */
int	mfs_map_bit(const struct mfs *, const unsigned char *, uint32_t);

/*
 * Count the inodes and zones that the bit maps mark free.  Returns 0 or a
 * negative errno value.
 */
int	mfs_count_free(struct mfs *, uint32_t *, uint32_t *);

/*
 * Changing file systems.  These need a file system opened with
 * mfs_open_rw(); otherwise they return -EROFS.
 */

/*
 * Write buf, block_size bytes, to block number block.  Returns 0, -EIO
 * for a block outside the file system, or another negative errno value.
 */
int	mfs_write_block(struct mfs *, uint32_t, const void *);

/*
 * Write max_size and state back to the super block.  Returns 0 or
 * -errno.
 */
int	mfs_put_super(struct mfs *);

/*
 * Mark the file system clean, or not clean, in the super block: in V1
 * and V2 as Linux does, valid and with or without errors, and in V3 with
 * the clean flag of MINIX 3.  Returns 0 or -errno.
 */
int	mfs_mark_clean(struct mfs *, int);

/* Write *ip back to inode number ip->num.  Returns 0 or -errno. */
int	mfs_put_inode(struct mfs *, const struct mfs_inode *);

/*
 * Set the inode number of the directory entry at byte offset off of the
 * directory *dp; 0 removes the entry.  Returns 0 or a negative errno
 * value.  This works on the flex directories of Minix-vmd too, where
 * mfs_put_entry() and mfs_add_entry() give -ENOTSUP.
 */
int	mfs_set_entry(struct mfs *, const struct mfs_inode *, uint32_t,
	    uint32_t);

/*
 * Write a whole directory entry, inode number ino and name, at byte
 * offset off of the directory *dp.  The offset may be anywhere in the
 * zones of *dp, also past its size.  Returns 0, -ENAMETOOLONG, or
 * another negative errno value.
 */
int	mfs_put_entry(struct mfs *, const struct mfs_inode *, uint32_t,
	    uint32_t, const char *);

/*
 * Clear the zone number that ref points to.  A slot of the inode is
 * cleared in *ip only, for the caller to write back with mfs_put_inode();
 * an entry of an indirect block is written at once.  Returns 0 or -errno.
 */
int	mfs_clear_zref(struct mfs *, struct mfs_inode *,
	    const struct mfs_zref *);

/* As mfs_clear_zref(), but store zone number zone.  Returns 0 or -errno. */
int	mfs_set_zref(struct mfs *, struct mfs_inode *,
	    const struct mfs_zref *, uint32_t);

/* Set bit n of a map from mfs_load_map() to v (0 or 1). */
void	mfs_set_map_bit(const struct mfs *, unsigned char *, uint32_t, int);

/* Write a map from mfs_load_map() back.  Returns 0 or -errno. */
int	mfs_store_map(struct mfs *, enum mfs_map, const unsigned char *);

/*
 * Writing files (mfs_write.c).  The bit maps are read on the first
 * allocation and kept in memory; mfs_sync() writes them back, and
 * mfs_close() drops them without writing.
 */

/*
 * Take a free inode, mark it in use and store its number in *ino; the
 * caller fills it in with mfs_put_inode().  Returns 0, -ENOSPC or -errno.
 */
int	mfs_alloc_inode(struct mfs *, uint32_t *);

/* Take a free zone, fill it with zeros and mark it in use. */
int	mfs_alloc_zone(struct mfs *, uint32_t *);

/*
 * Write len bytes of buf at offset off of the file *ip, taking zones and
 * indirect zones as it needs them; a block of zeros that falls into a
 * hole stays a hole.  The size grows to the end of what was written.
 * *ip changes in memory only, for the caller to write back with
 * mfs_put_inode().  Returns 0, -EFBIG past the reach of the zones, -ENOSPC
 * or another negative errno value.
 */
int	mfs_pwrite(struct mfs *, struct mfs_inode *, const void *, size_t,
	    uint32_t);

/*
 * Add an entry name, naming ino, to the directory *dp: in the first free
 * entry, or at the end.  *dp changes as for mfs_pwrite().  Returns 0,
 * -ENAMETOOLONG or another negative errno value.
 */
int	mfs_add_entry(struct mfs *, struct mfs_inode *, const char *,
	    uint32_t);

/* Write the bit maps back.  Returns 0 or -errno. */
int	mfs_sync(struct mfs *);

/*
 * Changing file systems in place (mfs_tune.c).  These need a file system
 * opened with mfs_open_rw() that fsck_minixfs passes; they read what they
 * need before they write, but a change cut short leaves the file system
 * half changed.
 */

/*
 * Store every number of the file system in byte order order: the super
 * block, the words of the bit maps, the inodes, the indirect zones and
 * the inode numbers of directory entries.  Returns 0, -ENOTSUP for
 * Minix-vmd, or a negative errno value, after which nothing has been
 * written if it came from reading.
 */
int	mfs_convert_order(struct mfs *, enum mfs_order);

/*
 * Give a V1 or V2 file system names of namelen (14 or 30) characters:
 * the magic number changes, and every directory is written anew with
 * entries of the new size, in zones that may differ from the old ones.
 * Returns 0; -EINVAL for V3 or another length; -ENOTSUP for Minix-vmd;
 * -ENAMETOOLONG if a name
 * is longer than namelen; -ENOSPC if the directories would not fit;
 * -EFBIG if a directory would outgrow its double indirect zone; or
 * another negative errno value.  Only the last can follow a write.
 */
int	mfs_change_namelen(struct mfs *, uint32_t);

/*
 * Grow the file system to nblocks blocks, cut down to whole zones, and
 * the image file with it.  When the zone map has no room for the new
 * zones, it gets more blocks: the inode table and every data zone in use
 * move up, and every zone number is changed to match.  Returns 0;
 * -EINVAL to shrink or with -T tracks; -EFBIG for more zones than the
 * version counts; -ENOSPC if the zones that move up would not fit; or
 * another negative errno value.
 */
int	mfs_grow(struct mfs *, uint32_t);

/*
 * Shrink the file system to nblocks blocks, cut down to whole zones, and
 * the image file with it.  The zones in use past the new end move to
 * free zones before it, and the zone numbers in inodes and indirect
 * zones follow them; the zone map keeps its blocks.  Returns 0; -EINVAL
 * to grow or with -T tracks; -ENOSPC if what is in use does not fit; or
 * another negative errno value, after which nothing has been written if
 * it came from reading.
 */
int	mfs_shrink(struct mfs *, uint32_t);

/*
 * Making file systems (mfs_format.c).
 */

/* What the caller asks of a new file system. */
struct mfs_params {
	enum mfs_order	order;
	int		version;	/* 1, 2 or 3 */
	uint32_t	block_size;	/* 0: 1024, or 4096 for V3 */
	uint32_t	namelen;	/* 0: 14, or 60 for V3 */
	uint32_t	nblocks;	/* size in blocks */
	uint32_t	ninodes;	/* 0: one for every 3 blocks */
	uint32_t	log_zone_size;
	uint32_t	time;		/* of the root directory */
};

/* Where everything goes; mfs_plan() works it out. */
struct mfs_layout {
	uint32_t	block_size;
	uint32_t	namelen;
	uint32_t	nblocks;	/* a whole number of zones */
	uint32_t	nzones;
	uint32_t	ninodes;
	uint32_t	imap_blocks;
	uint32_t	zmap_blocks;
	uint32_t	inode_start;	/* first block of the inode table */
	uint32_t	itable_blocks;
	uint32_t	firstdatazone;
	uint32_t	max_size;	/* for the super block */
	uint16_t	magic;
};

/*
 * The s_max_size that a new file system of a version and zone size
 * gets: what the zones of V1 reach, and the largest signed 32-bit size
 * for V2 and V3, as Linux writes it.
 */
uint32_t mfs_max_size(int, uint32_t);

/*
 * The s_max_size that MINIX works out for an open file system: what the
 * zones reach up to the double indirect zone, which is all that MINIX
 * uses, but no more than the largest signed 32-bit size.
 */
uint32_t mfs_minix_max_size(const struct mfs *);

/*
 * Check the parameters and lay the file system out in *l.  Returns 0,
 * -EINVAL for parameters that do not fit the version, -ENOSPC if the
 * file system is too small to hold its root directory, or -EFBIG if it
 * has more blocks or inodes than the version can count.
 */
int	mfs_plan(const struct mfs_params *, struct mfs_layout *);

/*
 * Write a new, empty file system laid out by mfs_plan() to the open
 * descriptor fd: the super block, the bit maps, the inode table and the
 * root directory.  The rest of the device is left as it is.  Returns 0
 * or a negative errno value.
 */
int	mfs_format(int, const struct mfs_params *, const struct mfs_layout *);

#endif /* MFS_H */
