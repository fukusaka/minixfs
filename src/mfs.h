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

/* What the fields of an inode hold. */
#define MFS_MAX_UID	65535
#define MFS_MAX_GID	65535
#define MFS_MAX_GID_V1	255		/* the gid of a V1 inode is a byte */
#define MFS_MAX_LINKS	65535
#define MFS_MAX_LINKS_V1 255		/* and so is its link count */
#define MFS_MAX_DEV_PART 255		/* major and minor device numbers */

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

/*
 * The slots of 8 bytes that an entry of a flex directory of Minix-vmd
 * takes for a name of len bytes: the first holds 5 bytes of the name and
 * the rest 8 each, the name ending with a NUL.
 */
#define MFS_FLEX_SLOTS(len)	(1 + ((len) + 3) / 8)

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
	off_t		file_size;	/* bytes in the image file; -1 for
					   a device of a size not known */
	off_t		image_size;	/* bytes of the device in it, or -1 */
	int		regular;	/* the image is a regular file */
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
	int		maps_through;	/* write a changed map block at once */
	int		(*keep)(void *, uint32_t);	/* or NULL; see
					   mfs_unlink() */
	void		*keep_arg;	/* its first argument */
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

/*
 * As mfs_open(), for reading and writing.  The image is locked for
 * writing with fcntl(2) until mfs_close(); -EBUSY if another program
 * holds the lock.
 */
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

/*
 * Lock the image open as fd for writing with fcntl(2), so that two
 * programs do not change it at the same time; programs that only read
 * take no lock.  The lock goes when the program closes any descriptor of
 * the image.  Returns 0, -EBUSY if another program holds it, or -errno.
 */
int	mfs_lock(int);

/* Release what mfs_open() acquired. */
void	mfs_close(struct mfs *);

/*
 * Read len bytes of the device at byte offset off, through the tracks
 * of -M if any.  Returns 0, or -EIO past the end of the image.
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
 * Count the inodes and zones that the bit maps mark free: those that
 * mfs_write.c keeps in memory, if it does.  Returns 0 or a negative errno
 * value.
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

/*
 * Take the clean mark away while the file system is mounted for writing,
 * as Linux and MINIX 3 do: in V1 and V2 the valid bit of the state goes,
 * and no mark of errors comes.  mfs_mark_clean(fs, 1) puts it back.
 * Returns 0 or -errno.
 */
int	mfs_mark_in_use(struct mfs *);

/* Write *ip back to inode number ip->num.  Returns 0 or -errno. */
int	mfs_put_inode(struct mfs *, const struct mfs_inode *);

/*
 * Set the inode number of the directory entry at byte offset off of the
 * directory *dp; 0 removes the entry.  Returns 0 or a negative errno
 * value.  This works on the flex directories of Minix-vmd too, where
 * removing an entry frees each of its slots, as Minix-vmd does.
 */
int	mfs_set_entry(struct mfs *, const struct mfs_inode *, uint32_t,
	    uint32_t);

/*
 * Write a whole directory entry, inode number ino and name, at byte
 * offset off of the directory *dp.  The offset may be anywhere in the
 * zones of *dp, also past its size.  In a flex directory of Minix-vmd the
 * entry takes as many slots as the name needs, which must lie within one
 * block, and the name is padded with NULs to the end of its last slot.
 * Returns 0, -ENAMETOOLONG, -EINVAL for an offset that is not that of an
 * entry or a slot, or for slots that would cross a block, or another
 * negative errno value.
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
 * Should it fail, what it wrote past the old end is taken back, with the
 * zones it took for that; what it wrote before the old end stays.  *ip
 * changes in memory only, for the caller to write back with
 * mfs_put_inode().  Returns 0, -EFBIG past mfs_max_write(), -ENOSPC,
 * -EIO for a zone number on the way outside the data area, which is not
 * written through, or another negative errno value.
 */
int	mfs_pwrite(struct mfs *, struct mfs_inode *, const void *, size_t,
	    uint32_t);

/*
 * Add an entry name, naming ino, to the directory *dp: in the first free
 * entry, or at the end.  In a flex directory of Minix-vmd, where an entry
 * takes a slot of 8 bytes and one more for each 8 bytes of the name
 * after its first 4, as Minix-vmd adds one: where enough free slots
 * follow one another within a block, or else at the start of a new
 * block.  *dp changes as for mfs_pwrite().  Returns 0, -ENAMETOOLONG or
 * another negative errno value.
 */
int	mfs_add_entry(struct mfs *, struct mfs_inode *, const char *,
	    uint32_t);

/*
 * Where an entry of a name of len bytes goes in the flex directory *dp,
 * as mfs_add_entry() puts it: into *off, which is the size rounded up
 * to a whole block where the directory has to grow.  Returns 0 or a
 * negative errno value.
 */
int	mfs_flex_place(struct mfs *, const struct mfs_inode *, size_t,
	    uint32_t *);

/*
 * Mark inode ino free in the inode map; the caller clears the inode.
 * Returns 0, -EIO for a number outside the inode table, or -errno.
 */
int	mfs_free_inode(struct mfs *, uint32_t);

/*
 * Mark zone free in the zone map.  Returns 0, -EIO for a zone outside
 * the data area, or -errno.
 */
int	mfs_free_zone(struct mfs *, uint32_t);

/*
 * Free every zone of the file *ip, data and indirect, and clear its zone
 * slots and size; a device keeps no zones and only has its slots
 * cleared.  *ip changes in memory only, for the caller to write back with
 * mfs_put_inode().  Returns 0 or a negative errno value.
 */
int	mfs_truncate(struct mfs *, struct mfs_inode *);

/*
 * With on set, write each block of a bit map as it changes, rather than
 * all of them at mfs_sync(), so that the maps on the image are right
 * even if the program dies.
 */
void	mfs_maps_through(struct mfs *, int);

/*
 * Make the file *ip size bytes long: the zones past the new end, or past
 * the old end when it grows, are freed, indirect zones that list nothing
 * more with them, and the rest of the last zone is cleared, so that the
 * file reads as zeros where it grows.  *ip changes in memory only, for
 * the caller to write back with mfs_put_inode().  A zone number outside
 * the data area is dropped from what goes; in the zone that is cleared,
 * it is -EIO, and nothing changes.  Returns 0, -EINVAL for a device,
 * -EFBIG past mfs_max_write(), -EIO, or another negative errno value.
 */
int	mfs_resize(struct mfs *, struct mfs_inode *, uint32_t);

/* Write the bit maps back.  Returns 0 or -errno. */
int	mfs_sync(struct mfs *);

/*
 * Changing names (mfs_ops.c), as the system calls of the same names do.
 * Directories are given by inode number; names are single components.
 * Each returns 0 or a negative errno value: -EEXIST, -ENOENT, -ENOTDIR,
 * -EISDIR, -ENOTEMPTY, -EMLINK, -ENAMETOOLONG, -ENOSPC, among others.
 * Times are those to give the inodes that change.
 */

/* What a new inode gets. */
struct mfs_new {
	uint32_t	rdev;		/* of a device */
	uint32_t	time;		/* atime, mtime and ctime */
	uint16_t	mode;		/* type and permissions */
	uint16_t	uid;
	uint16_t	gid;
};

/*
 * Make name in directory dir: a regular file, a directory, a pipe, a
 * socket or a device, as the type of n->mode says, and read it into *ip;
 * any other type is -EINVAL.  A directory gets "." and "..", and its
 * parent one more link.
 */
int	mfs_make(struct mfs *, uint32_t, const char *, const struct mfs_new *,
	    struct mfs_inode *);

/*
 * Make name in directory dir a symbolic link to target, which has to be
 * shorter than a block, and read it into *ip; the mode of n is ignored.
 */
int	mfs_symlink(struct mfs *, uint32_t, const char *, const char *,
	    const struct mfs_new *, struct mfs_inode *);

/* Give inode ino, which is not a directory, one more name, in dir. */
int	mfs_link(struct mfs *, uint32_t, uint32_t, const char *, uint32_t);

/*
 * Remove name from dir, and free its inode if that was its last name.
 * If fs->keep is set and says so for the inode, it is kept instead, with
 * no links, for a file that is still open: mfs_free_orphan() frees it
 * once it is closed.  The same holds for a file that mfs_rename()
 * replaces.
 */
int	mfs_unlink(struct mfs *, uint32_t, const char *, uint32_t);

/*
 * Free inode ino and its zones if it has no links left, as fs->keep had
 * it kept; otherwise do nothing.  Returns 0 or a negative errno value.
 */
int	mfs_free_orphan(struct mfs *, uint32_t);

/*
 * Whether a failure of the functions that change a file system, a
 * negative errno value, may leave it out of order, so that it is not to
 * be marked clean.  A refusal made before anything is written is not,
 * nor a lack of room or a limit that gives back what was taken; an error
 * of the device, or of memory, is.
 */
int	mfs_failure_breaks(int);

/* Remove the empty directory name from dir. */
int	mfs_rmdir(struct mfs *, uint32_t, const char *, uint32_t);

/*
 * Rename oname in odir to nname in ndir, replacing a file of that name,
 * or an empty directory in place of a directory, unless noreplace is set
 * (then -EEXIST).  A directory cannot move below itself (-EINVAL).
 */
int	mfs_rename(struct mfs *, uint32_t, const char *, uint32_t,
	    const char *, int, uint32_t);

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
 * With check set, only find out whether it can be done, writing nothing,
 * on an image open for reading too.  Returns 0; -EINVAL for V3 or another
 * length; -ENOTSUP for Minix-vmd; -ENAMETOOLONG if a name is longer than
 * namelen; -ENOSPC if the directories would not fit; -EFBIG if a
 * directory would outgrow its double indirect zone; or another negative
 * errno value.  Only the last can follow a write.
 */
int	mfs_change_namelen(struct mfs *, uint32_t, int);

/* Flags of mfs_grow() and mfs_shrink(). */
#define MFS_RESIZE_KEEP		1	/* keep the size of the image */
#define MFS_RESIZE_CHECK	2	/* only whether it can be done */

/*
 * Grow the file system to nblocks blocks, cut down to whole zones, and
 * the image file with it, unless flags has MFS_RESIZE_KEEP or the image is
 * not a regular file; the image must then hold them already.  With
 * MFS_RESIZE_CHECK, only find out whether it can be done, writing nothing,
 * on an image open for reading too.  When the zone map
 * has no room for the new zones, it gets more blocks: the inode table and
 * every data zone in use move up, and every zone number is changed to
 * match.  Returns 0; -EINVAL to shrink or with -M tracks; -EFBIG for more
 * zones than the version counts; -ENOSPC if the zones that move up would
 * not fit; -ENXIO if the image does not hold the new size, or its size is
 * not known; or another negative errno value.
 */
int	mfs_grow(struct mfs *, uint32_t, int);

/*
 * Shrink the file system to nblocks blocks, cut down to whole zones, and
 * the image file with it, unless flags has MFS_RESIZE_KEEP or the image is
 * not a regular file, which then keeps what is past the end; flags as for
 * mfs_grow().  The zones in use past the new end move to
 * free zones before it, and the zone numbers in inodes and indirect
 * zones follow them; the zone map keeps its blocks.  Returns 0; -EINVAL
 * to grow or with -M tracks; -ENOSPC if what is in use does not fit; or
 * another negative errno value, after which nothing has been written if
 * it came from reading.
 */
int	mfs_shrink(struct mfs *, uint32_t, int);

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
	uint32_t	max_size;	/* 0: what MINIX works out */
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
 * The s_max_size that mkfs.minix of Linux writes for a version and zone
 * size: what the zones of V1 reach, and the largest signed 32-bit size
 * for V2 and V3.
 */
uint32_t mfs_linux_max_size(int, uint32_t);

/*
 * The s_max_size that MINIX works out for a version, block size and zone
 * size: what the zones reach up to the double indirect zone, which is
 * all that MINIX uses, but no more than the largest signed 32-bit size.
 * A new file system gets it unless asked for another.
 */
uint32_t mfs_minix_size(int, uint32_t, uint32_t);

/* mfs_minix_size() for an open file system. */
uint32_t mfs_minix_max_size(const struct mfs *);

/*
 * The largest file that may be written: what the zone slots reach, and
 * no more than the maximum file size of the super block, as Linux keeps
 * to it, unless that is 0 or past the largest signed 32-bit size, which
 * fsck_minixfs puts right.
 */
uint64_t mfs_max_write(const struct mfs *);

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
