/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * tree.h - copy a directory tree of the host into a new file system, for
 * newfs_minixfs -d.
 */

#ifndef TREE_H
#define TREE_H

#include <stdint.h>

#include "mfs.h"
#include "spec.h"

/* What a tree needs of a file system, and what does not fit in it. */
struct tree_need {
	uint64_t	inodes;		/* with the root */
	uint64_t	zones;		/* data and indirect, at most */
	unsigned long	problems;	/* things that cannot be copied */
	unsigned long	owners;		/* of them, owners that do not fit */
	unsigned long	sockets;	/* left out */
};

/*
 * The shape of the file system to be: enough of it to count zones and
 * to check names, owners and device numbers.
 */
struct tree_fs {
	int		version;
	uint32_t	block_size;
	uint32_t	log_zone_size;
	uint32_t	namelen;
	uint32_t	max_size;	/* of the super block */
	int		owned;		/* every file gets uid and gid */
	uint32_t	uid;
	uint32_t	gid;
	struct spec	*spec;		/* -F, or NULL */
	int		exclude;	/* -x: only what the spec gives */
};

/*
 * Walk the tree at dir, with what the specification of *f adds and
 * overrides, and add up what it needs in *f.  Each thing that cannot be
 * copied (a name too long, an owner or a device number too large, an
 * entry of the specification that does not say enough) is printed and
 * counted in problems.  Returns 0, or -1 after printing why the tree
 * cannot be read.
 */
int	tree_scan(const char *, const struct tree_fs *, struct tree_need *);

/*
 * Copy the tree at dir into fs, just made, whose root directory becomes
 * dir itself; the owners come from the files, or from *f if it says so.
 * Returns 0, or -1 after printing what failed.
 */
int	tree_copy(struct mfs *, const char *, const struct tree_fs *);

#endif /* TREE_H */
