/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * spec.h - read an mtree(8) specification, for newfs_minixfs -F.
 */

#ifndef SPEC_H
#define SPEC_H

#include <stddef.h>
#include <stdint.h>

/* Which attributes an entry gives. */
#define SPEC_TYPE	0x01
#define SPEC_MODE	0x02
#define SPEC_UID	0x04
#define SPEC_GID	0x08
#define SPEC_TIME	0x10
#define SPEC_LINK	0x20
#define SPEC_DEVICE	0x40
#define SPEC_OPTIONAL	0x80

/* One entry: a path from the root and what it says of it. */
struct spec_entry {
	char		*path;		/* "" for the root, no "./" */
	char		*link;		/* SPEC_LINK */
	uint32_t	set;		/* SPEC_* */
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	time;		/* seconds */
	uint32_t	major;
	uint32_t	minor;
	uint16_t	type;		/* MFS_S_IF* */
	uint16_t	mode;		/* permission bits */
	int		seen;		/* reached by the walk of the tree */
};

/* A specification, its entries sorted by path. */
struct spec {
	struct spec_entry *e;
	size_t		n;
};

/*
 * Read the specification in file.  User and group names are looked up
 * in the passwd and group files of dbdir, or of the system if dbdir is
 * NULL.  Returns 0, or -1 after printing what is wrong, with the line.
 */
int	spec_read(const char *, const char *, struct spec *);

/* The entry of path, or NULL. */
struct spec_entry *spec_find(const struct spec *, const char *);

/*
 * The entries directly below the directory dir: *first is the index of
 * the first entry that may be one, and the return value the index past
 * the last.  An entry is directly below dir if its path is dir, a slash
 * and a name without one.
 */
size_t	spec_below(const struct spec *, const char *, size_t *);

void	spec_free(struct spec *);

#endif /* SPEC_H */
