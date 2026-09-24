/*
 * mfs.h - read access to MINIX file system images.
 *
 * The library opens an image file, recognises the file system version and
 * byte order from the super block, and gives access to the inodes, file
 * data and directories of V1 file systems.  It never prints and has no
 * global state.  Functions return 0 (or a byte count) on success and a
 * negative errno value on failure, so that the result can be handed
 * straight back to FUSE.
 */

#ifndef MFS_H
#define MFS_H

#include <sys/types.h>

#include <stddef.h>
#include <stdint.h>

#define MFS_ROOT_INO	1		/* inode number of the root directory */
#define MFS_MAX_NAME	30		/* longest name of V1 */
#define MFS_NR_ZONES	9		/* zone slots in a V1 inode */

/* Super block magic numbers, in the byte order of the image. */
#define MFS_MAGIC_V1	0x137f		/* V1, 14-character names */
#define MFS_MAGIC_V1L	0x138f		/* V1, 30-character names (Linux) */
#define MFS_MAGIC_V2	0x2468		/* V2, 14-character names */
#define MFS_MAGIC_V2L	0x2478		/* V2, 30-character names (Linux) */
#define MFS_MAGIC_V3	0x4d5a		/* V3, 60-character names */

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

/* An open file system image. */
struct mfs {
	off_t		image_size;	/* bytes in the image file */
	uint64_t	max_file;	/* bytes the zone slots can address */
	unsigned char	*ibuf;		/* one block: inodes, indirects */
	unsigned char	*dbuf;		/* one block, for file data */
	enum mfs_order	order;
	int		fd;
	int		version;	/* 1, 2 or 3 */

	/* From the super block. */
	uint32_t	ninodes;
	uint32_t	nzones;
	uint32_t	imap_blocks;
	uint32_t	zmap_blocks;
	uint32_t	firstdatazone;
	uint32_t	log_zone_size;
	uint32_t	max_size;
	uint32_t	block_size;	/* always 1024 */
	uint16_t	magic;

	/* Derived from the super block. */
	uint32_t	namelen;	/* bytes of a name in an entry */
	uint32_t	dirent_size;	/* bytes of a directory entry */
	uint32_t	inode_size;	/* bytes of an on-disk inode */
	uint32_t	ndzones;	/* direct zones in an inode */
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
	char		name[MFS_MAX_NAME + 1];	/* NUL-terminated */
};

/*
 * Called by mfs_readdir() for each entry in use.  A non-zero return value
 * stops the walk and becomes the return value of mfs_readdir().
 */
typedef int (*mfs_dirent_fn)(const struct mfs_dirent *, void *);

/*
 * Open the image at path and check its super block.  Returns 0, -EINVAL
 * if the image is not a consistent MINIX file system, -ENOTSUP for a V2
 * or V3 file system, which cannot be read yet, or another negative errno
 * value.  On success the caller releases fs with mfs_close().
 */
int	mfs_open(struct mfs *, const char *);

/* Release what mfs_open() acquired. */
void	mfs_close(struct mfs *);

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

#endif /* MFS_H */
