/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * minixfs.h - what the commands of minixfs share: the open image, the
 * reporting of problems, and the walk of directories.  The commands that
 * read are in minixfs.c, those that write in minixfs_write.c.
 */

#ifndef MINIXFS_H
#define MINIXFS_H

#include <stdint.h>

#include "mfs.h"

#define MAX_DEPTH	256		/* deepest directory entered */

/* How the image file holds the file system: -M. */
extern struct mfs_tracks tracks;

/* -W: the bytes in a word of the bit maps, or 0 as the version has it. */
extern uint32_t map_word;

/* -f: write a file system that is not marked clean all the same. */
extern int force;

/* One command on one image. */
struct cmd {
	struct mfs	fs;
	const char	*image;
	int		status;		/* exit status so far */
	int		broken;		/* a write may have left it out of
					   order: see mfs_failure_breaks() */
};

/*
 * The directories on the path of a walk.  A damaged image can make a
 * directory contain itself; a walk refuses to enter one twice.
 */
struct walk {
	uint32_t	stack[MAX_DEPTH];
	int		depth;
};

/* The entries of a directory other than "." and "..". */
struct dirlist {
	struct mfs_dirent	*ent;
	size_t			max;
	size_t			n;
};

void	usage(void);

/* Report a problem and remember that the command failed. */
void	problem(struct cmd *, const char *, ...);

/* Open the image for reading, or report why not and exit. */
void	open_image(struct cmd *, const char *);

/* Look up path in the image; report and return -1 if that fails. */
int	lookup(struct cmd *, const char *, struct mfs_inode *);

/* dir/name in new memory, or name alone if dir is empty; NULL if none. */
char	*join(struct cmd *, const char *, const char *);

/* Enter directory ino at path, or report a loop or too deep a nest. */
int	walk_enter(struct cmd *, struct walk *, uint32_t, const char *);
void	walk_leave(struct walk *);

/*
 * Collect the entries of a directory, since a walk cannot recurse from
 * inside mfs_readdir().  Returns -1 after reporting a failure; the caller
 * frees dl->ent.
 */
int	read_dir(struct cmd *, const struct mfs_inode *, const char *,
	    struct dirlist *);

/* Read the inode of a directory entry; report and return -1 on failure. */
int	entry_inode(struct cmd *, const struct mfs_dirent *, const char *,
	    struct mfs_inode *);

/* The commands that write (minixfs_write.c). */
int	cmd_put(int, char **);
int	cmd_mkdir(int, char **);
int	cmd_rm(int, char **);
int	cmd_mv(int, char **);
int	cmd_ln(int, char **);
int	cmd_chmod(int, char **);
int	cmd_chown(int, char **);

#endif /* MINIXFS_H */
