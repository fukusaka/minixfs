/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * tree.c - copy a directory tree of the host into a new file system.
 *
 * tree_scan() walks the tree before anything is written: it counts the
 * inodes and, at most, the zones the tree needs, and finds what cannot be
 * copied.  tree_copy() then walks it again and writes it.  Both take the
 * names of a directory in sorted order, so that the same tree always
 * makes the same image.  Files with more than one link inside the tree
 * stay one file; blocks of zeros stay holes.
 */

#include "compat.h"

#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mfs.h"
#include "tree.h"

#define COPY_SIZE	65536		/* bytes copied at a time */
#define NR_DZONES	7		/* direct zones in an inode */
#define MAX_DEV_PART	255		/* major and minor numbers of MINIX */
#define MAX_UID		65535
#define MAX_GID_V1	255		/* the gid of a V1 inode is a byte */
#define MAX_GID		65535
#define MAX_LINKS_V1	255		/* and so is its link count */
#define MAX_LINKS	65535

/* A file with more than one link, by its identity on the host. */
struct link {
	dev_t		dev;
	ino_t		ino;
	uint32_t	mino;		/* its inode in the image, or 0 */
	uint32_t	names;		/* names it has in the tree */
	int		used;		/* a slot in use */
};

/* A hash table of them, by open addressing. */
struct links {
	struct link	*tab;
	size_t		n;
	size_t		max;		/* a power of 2 */
};

static size_t
link_hash(dev_t dev, ino_t ino, size_t max)
{
	return ((size_t)ino * 31 + (size_t)dev) & (max - 1);
}

/* The entry of dev and ino, added if it is not there. */
static struct link *
link_find(struct links *l, dev_t dev, ino_t ino)
{
	struct link *old;
	size_t i, j, max;

	if (l->n * 2 >= l->max) {
		old = l->tab;
		max = l->max;
		l->max = max == 0 ? 64 : max * 2;
		if ((l->tab = calloc(l->max, sizeof(*l->tab))) == NULL)
			err(1, NULL);
		for (i = 0; i < max; i++) {
			if (!old[i].used)
				continue;
			j = link_hash(old[i].dev, old[i].ino, l->max);
			while (l->tab[j].used)
				j = (j + 1) & (l->max - 1);
			l->tab[j] = old[i];
		}
		free(old);
	}
	for (i = link_hash(dev, ino, l->max); l->tab[i].used;
	    i = (i + 1) & (l->max - 1)) {
		if (l->tab[i].dev == dev && l->tab[i].ino == ino)
			return &l->tab[i];
	}
	l->tab[i].used = 1;
	l->tab[i].dev = dev;
	l->tab[i].ino = ino;
	l->n++;
	return &l->tab[i];
}

static char *
join(const char *dir, const char *name)
{
	char *s;

	if ((s = malloc(strlen(dir) + strlen(name) + 2)) == NULL)
		err(1, NULL);
	(void)sprintf(s, "%s/%s", dir, name);
	return s;
}

static int
by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in a directory but "." and "..", sorted; NULL on error. */
static char **
read_names(const char *path, size_t *np)
{
	struct dirent *de;
	char **v, **nv;
	size_t n, max;
	DIR *d;

	if ((d = opendir(path)) == NULL) {
		warn("%s", path);
		return NULL;
	}
	v = NULL;
	n = max = 0;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (n == max) {
			max = max == 0 ? 32 : max * 2;
			if ((nv = realloc(v, max * sizeof(*v))) == NULL)
				err(1, NULL);
			v = nv;
		}
		if ((v[n++] = strdup(de->d_name)) == NULL)
			err(1, NULL);
	}
	(void)closedir(d);
	if (n > 0)
		qsort(v, n, sizeof(*v), by_name);
	if (v == NULL && (v = malloc(sizeof(*v))) == NULL)
		err(1, NULL);
	*np = n;
	return v;
}

static void
free_names(char **v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

/* Zone numbers in an indirect block. */
static uint64_t
indirects(const struct tree_fs *f)
{
	return f->block_size / (f->version == 1 ? 2 : 4);
}

/* The largest file the zones of f reach. */
static uint64_t
max_file(const struct tree_fs *f)
{
	uint64_t n, zones;

	n = indirects(f);
	zones = NR_DZONES + n + n * n;
	if (f->version != 1)
		zones += n * n * n;
	zones *= (uint64_t)f->block_size << f->log_zone_size;
	return zones < UINT32_MAX ? zones : UINT32_MAX;
}

/* Zones for a file of bytes bytes: data, and the indirect zones. */
static uint64_t
file_zones(const struct tree_fs *f, uint64_t bytes)
{
	uint64_t data, n, rest, zbytes, zones;

	zbytes = (uint64_t)f->block_size << f->log_zone_size;
	n = indirects(f);
	data = (bytes + zbytes - 1) / zbytes;
	zones = data;
	if (data <= NR_DZONES)
		return zones;
	rest = data - NR_DZONES;
	zones++;
	if (rest <= n)
		return zones;
	rest -= n;
	zones += 1 + ((rest < n * n ? rest : n * n) + n - 1) / n;
	if (rest <= n * n)
		return zones;
	rest -= n * n;
	return zones + 1 + (rest + n * n - 1) / (n * n) + (rest + n - 1) / n;
}

struct scan {
	const struct tree_fs *f;
	struct tree_need *need;
	struct links	links;
};

/* Count one problem, with its path. */
static void
problem(struct scan *s, const char *path, const char *fmt, ...)
{
	va_list ap;

	s->need->problems++;
	(void)fprintf(stderr, "%s: ", path);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
}

static int scan_dir(struct scan *, const char *);

/* One entry below a directory; returns 1 if it takes an entry there. */
static int
scan_entry(struct scan *s, const char *path, const char *name,
    uint32_t *subdirs)
{
	const struct tree_fs *f;
	struct link *l;
	struct stat st;

	f = s->f;
	if (strlen(name) > f->namelen)
		problem(s, path, "name longer than %" PRIu32 " characters",
		    f->namelen);
	if (lstat(path, &st) == -1) {
		problem(s, path, "%s", strerror(errno));
		return 0;
	}
	if (S_ISSOCK(st.st_mode)) {
		warnx("%s: a socket cannot be copied; left out", path);
		s->need->sockets++;
		return 0;
	}
	if (!f->owned && (uintmax_t)st.st_uid > MAX_UID) {
		problem(s, path, "owner %ju does not fit",
		    (uintmax_t)st.st_uid);
		s->need->owners++;
	}
	if (!f->owned &&
	    (uintmax_t)st.st_gid > (f->version == 1 ? MAX_GID_V1 : MAX_GID)) {
		problem(s, path, "group %ju does not fit in V%d",
		    (uintmax_t)st.st_gid, f->version);
		s->need->owners++;
	}
	if (S_ISDIR(st.st_mode)) {
		(*subdirs)++;
		s->need->inodes++;
		if (scan_dir(s, path) == -1)
			return -1;
		return 1;
	}
	if (st.st_nlink > 1) {
		l = link_find(&s->links, st.st_dev, st.st_ino);
		if (l->names++ > 0)
			return 1;
	}
	s->need->inodes++;
	if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
		if ((uint64_t)st.st_size > max_file(f))
			problem(s, path, "%ju bytes are more than a file of "
			    "V%d holds", (uintmax_t)st.st_size, f->version);
		else
			s->need->zones += file_zones(f, (uint64_t)st.st_size);
	} else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		if (major(st.st_rdev) > MAX_DEV_PART ||
		    minor(st.st_rdev) > MAX_DEV_PART)
			problem(s, path, "device %ju,%ju does not fit",
			    (uintmax_t)major(st.st_rdev),
			    (uintmax_t)minor(st.st_rdev));
	} else if (!S_ISFIFO(st.st_mode)) {
		problem(s, path, "not a kind of file that can be copied");
	}
	return 1;
}

static int
scan_dir(struct scan *s, const char *path)
{
	uint64_t entries;
	uint32_t subdirs;
	size_t i, n;
	char **names, *child;
	int r;

	if ((names = read_names(path, &n)) == NULL)
		return -1;
	entries = 2;
	subdirs = 0;
	r = 0;
	for (i = 0; i < n && r != -1; i++) {
		child = join(path, names[i]);
		if ((r = scan_entry(s, child, names[i], &subdirs)) == 1)
			entries++;
		free(child);
	}
	free_names(names, n);
	if (r == -1)
		return -1;
	if (subdirs + 2 > (s->f->version == 1 ? MAX_LINKS_V1 : MAX_LINKS))
		problem(s, path, "%" PRIu32 " directories below it are more "
		    "than a link count holds", subdirs);
	s->need->zones += file_zones(s->f,
	    entries * ((s->f->version == 3 ? 4 : 2) + s->f->namelen));
	return 0;
}

int
tree_scan(const char *dir, const struct tree_fs *f, struct tree_need *need)
{
	struct scan s;
	struct stat st;
	size_t i;
	int r;

	(void)memset(need, 0, sizeof(*need));
	(void)memset(&s, 0, sizeof(s));
	if (f->owned && (f->uid > MAX_UID ||
	    f->gid > (f->version == 1 ? MAX_GID_V1 : MAX_GID))) {
		warnx("owner %" PRIu32 ":%" PRIu32 " does not fit in V%d",
		    f->uid, f->gid, f->version);
		need->problems++;
	}
	s.f = f;
	s.need = need;
	if (lstat(dir, &st) == -1) {
		warn("%s", dir);
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		warnx("%s: %s", dir, strerror(ENOTDIR));
		return -1;
	}
	need->inodes = 1;
	r = scan_dir(&s, dir);
	for (i = 0; i < s.links.max; i++) {
		if (s.links.tab[i].used && s.links.tab[i].names >
		    (f->version == 1 ? MAX_LINKS_V1 : MAX_LINKS)) {
			need->problems++;
			warnx("a file with %" PRIu32 " names in the tree has "
			    "more than a link count holds",
			    s.links.tab[i].names);
		}
	}
	free(s.links.tab);
	return r;
}

/*
 * Copying.
 */

struct copy {
	struct mfs	*fs;
	const struct tree_fs *f;
	struct links	links;
};

static uint16_t
file_type(mode_t mode)
{
	if (S_ISDIR(mode))
		return MFS_S_IFDIR;
	if (S_ISLNK(mode))
		return MFS_S_IFLNK;
	if (S_ISCHR(mode))
		return MFS_S_IFCHR;
	if (S_ISBLK(mode))
		return MFS_S_IFBLK;
	if (S_ISFIFO(mode))
		return MFS_S_IFIFO;
	return MFS_S_IFREG;
}

/*
 * The mode, owners and times of st.  A symbolic link gets 0777, as MINIX
 * and Linux make them: the BSDs apply the umask to them, which means
 * nothing to a link.
 */
static void
set_attrs(const struct copy *c, struct mfs_inode *ip, const struct stat *st)
{
	ip->mode = (uint16_t)(file_type(st->st_mode) |
	    (S_ISLNK(st->st_mode) ? 0777 : st->st_mode & 07777));
	ip->uid = (uint16_t)(c->f->owned ? c->f->uid : st->st_uid);
	ip->gid = (uint16_t)(c->f->owned ? c->f->gid : st->st_gid);
	ip->atime = (uint32_t)st->st_atime;
	ip->mtime = (uint32_t)st->st_mtime;
	ip->ctime = (uint32_t)st->st_ctime;
}

/* Report the failure of r, a negative errno value, and return -1. */
static int
failed(const char *path, int r)
{
	warnx("%s: %s", path, strerror(-r));
	return -1;
}

/* The contents of the file at path, into *ip. */
static int
copy_data(struct copy *c, const char *path, struct mfs_inode *ip)
{
	static unsigned char buf[COPY_SIZE];
	uint32_t off;
	ssize_t n;
	int fd, r;

	if ((fd = open(path, O_RDONLY)) == -1) {
		warn("%s", path);
		return -1;
	}
	r = 0;
	for (off = 0; r == 0; off += (uint32_t)n) {
		if ((n = read(fd, buf, sizeof(buf))) == -1 && errno == EINTR)
			n = 0;
		else if (n <= 0)
			break;
		if (n > 0 &&
		    (r = mfs_pwrite(c->fs, ip, buf, (size_t)n, off)) < 0)
			r = failed(path, r);
	}
	if (n == -1) {
		warn("%s", path);
		r = -1;
	}
	(void)close(fd);
	return r;
}

/* The text of the symbolic link at path, into *ip. */
static int
copy_link(struct copy *c, const char *path, struct mfs_inode *ip)
{
	char text[PATH_MAX];
	ssize_t n;
	int r;

	if ((n = readlink(path, text, sizeof(text))) == -1) {
		warn("%s", path);
		return -1;
	}
	if ((r = mfs_pwrite(c->fs, ip, text, (size_t)n, 0)) < 0)
		return failed(path, r);
	return 0;
}

static int copy_dir(struct copy *, const char *, uint32_t);

/* Enter name, naming ino, into the directory *dp and write it back. */
static int
enter(struct copy *c, struct mfs_inode *dp, const char *name, uint32_t ino,
    const char *path)
{
	int r;

	if ((r = mfs_add_entry(c->fs, dp, name, ino)) < 0 ||
	    (r = mfs_put_inode(c->fs, dp)) < 0)
		return failed(path, r);
	return 0;
}

/* Make one entry of the directory *dp from the host file at path. */
static int
copy_entry(struct copy *c, struct mfs_inode *dp, const char *path,
    const char *name)
{
	struct mfs_inode ip;
	struct link *l;
	struct stat st;
	uint32_t ino;
	int r;

	if (lstat(path, &st) == -1) {
		warn("%s", path);
		return -1;
	}
	if (S_ISSOCK(st.st_mode))
		return 0;
	l = NULL;
	if (!S_ISDIR(st.st_mode) && st.st_nlink > 1) {
		l = link_find(&c->links, st.st_dev, st.st_ino);
		if (l->mino != 0) {
			/* Another name of a file already made. */
			if ((r = mfs_get_inode(c->fs, l->mino, &ip)) < 0)
				return failed(path, r);
			ip.nlinks++;
			if ((r = mfs_put_inode(c->fs, &ip)) < 0)
				return failed(path, r);
			return enter(c, dp, name, l->mino, path);
		}
	}
	if ((r = mfs_alloc_inode(c->fs, &ino)) < 0)
		return failed(path, r);
	(void)memset(&ip, 0, sizeof(ip));
	ip.num = ino;
	ip.nlinks = 1;
	set_attrs(c, &ip, &st);
	r = 0;
	if (S_ISDIR(st.st_mode)) {
		ip.nlinks = 2;
		if ((r = mfs_add_entry(c->fs, &ip, ".", ino)) < 0 ||
		    (r = mfs_add_entry(c->fs, &ip, "..", dp->num)) < 0)
			return failed(path, r);
	} else if (S_ISREG(st.st_mode)) {
		r = copy_data(c, path, &ip);
	} else if (S_ISLNK(st.st_mode)) {
		r = copy_link(c, path, &ip);
	} else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		ip.zone[0] = (uint32_t)(major(st.st_rdev) << 8 |
		    minor(st.st_rdev));
	}
	if (r < 0)
		return -1;
	if ((r = mfs_put_inode(c->fs, &ip)) < 0)
		return failed(path, r);
	if (l != NULL)
		l->mino = ino;
	if (S_ISDIR(st.st_mode)) {
		dp->nlinks++;
		if (enter(c, dp, name, ino, path) == -1)
			return -1;
		return copy_dir(c, path, ino);
	}
	return enter(c, dp, name, ino, path);
}

static int
copy_dir(struct copy *c, const char *path, uint32_t dino)
{
	struct mfs_inode dp;
	size_t i, n;
	char **names, *child;
	int r;

	if ((names = read_names(path, &n)) == NULL)
		return -1;
	r = 0;
	for (i = 0; i < n && r == 0; i++) {
		/* The directory changes as entries go in: read it anew. */
		if ((r = mfs_get_inode(c->fs, dino, &dp)) < 0) {
			r = failed(path, r);
			break;
		}
		child = join(path, names[i]);
		r = copy_entry(c, &dp, child, names[i]);
		free(child);
	}
	free_names(names, n);
	return r;
}

int
tree_copy(struct mfs *fs, const char *dir, const struct tree_fs *f)
{
	struct mfs_inode root;
	struct copy c;
	struct stat st;
	int r;

	(void)memset(&c, 0, sizeof(c));
	c.fs = fs;
	c.f = f;
	if (lstat(dir, &st) == -1) {
		warn("%s", dir);
		return -1;
	}
	if ((r = mfs_get_inode(fs, MFS_ROOT_INO, &root)) < 0)
		return failed(dir, r);
	set_attrs(&c, &root, &st);
	if ((r = mfs_put_inode(fs, &root)) < 0)
		return failed(dir, r);
	r = copy_dir(&c, dir, MFS_ROOT_INO);
	free(c.links.tab);
	if (r == 0 && (r = mfs_sync(fs)) < 0)
		return failed(dir, r);
	return r;
}
