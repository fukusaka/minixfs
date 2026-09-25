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
 *
 * An mtree(8) specification works as with makefs -F: what an entry gives
 * overrides the host file, an entry that the host does not have is made
 * as it says (a regular file empty), an optional one is left out, and a
 * type that differs from that of the host file is an error.  With -x,
 * only what the specification names goes in.
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
#include <time.h>
#include <unistd.h>

#include "mfs.h"
#include "spec.h"
#include "tree.h"

#define COPY_SIZE	65536		/* bytes copied at a time */
#define NR_DZONES	7		/* direct zones in an inode */

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

/*
 * Nodes: what the host and the specification say of one thing.
 */

/* One thing of the tree, from the host, the specification or both. */
struct node {
	char		*host;		/* path on the host, or NULL */
	char		*rel;		/* path from the root, "" for it */
	struct spec_entry *spec;
	struct stat	st;		/* of host, if there is one */
	const char	*link;		/* target from the spec, or NULL */
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	atime;
	uint32_t	mtime;
	uint32_t	ctime;
	uint32_t	major;
	uint32_t	minor;
	uint16_t	type;		/* MFS_S_IF* */
	uint16_t	perm;
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
	if (S_ISSOCK(mode))
		return MFS_S_IFSOCK;
	return S_ISREG(mode) ? MFS_S_IFREG : 0;
}

static int
is_dev(const struct node *n)
{
	return n->type == MFS_S_IFCHR || n->type == MFS_S_IFBLK;
}

static char *
join_rel(const char *rel, const char *name)
{
	char *s;

	if (rel[0] == '\0') {
		if ((s = strdup(name)) == NULL)
			err(1, NULL);
		return s;
	}
	return join(rel, name);
}

/*
 * Fill in *n from the host file at n->host, if there is one, and the
 * spec entry n->spec, which overrides it.  Returns 0; 1 if it is left
 * out; or -1 with the reason in *why, which the caller reports.
 */
static int
resolve(const struct tree_fs *f, struct node *n, const char **why)
{
	struct spec_entry *e;

	e = n->spec;
	if (e != NULL)
		e->seen = 1;
	n->link = NULL;
	if (n->host != NULL) {
		n->type = file_type(n->st.st_mode);
		n->perm = (uint16_t)(n->st.st_mode & 07777);
		n->uid = (uint32_t)n->st.st_uid;
		n->gid = (uint32_t)n->st.st_gid;
		n->atime = (uint32_t)n->st.st_atime;
		n->mtime = (uint32_t)n->st.st_mtime;
		n->ctime = (uint32_t)n->st.st_ctime;
		n->major = (uint32_t)major(n->st.st_rdev);
		n->minor = (uint32_t)minor(n->st.st_rdev);
		if (e != NULL && (e->set & SPEC_TYPE) && e->type != n->type) {
			*why = "the type in the specification is not that of "
			    "the file";
			return -1;
		}
	} else {
		/* Only in the specification: it has to say it all. */
		if (e->set & SPEC_OPTIONAL)
			return 1;
		if ((e->set & (SPEC_TYPE | SPEC_MODE | SPEC_UID | SPEC_GID)) !=
		    (SPEC_TYPE | SPEC_MODE | SPEC_UID | SPEC_GID)) {
			*why = "not in the directory, and the specification "
			    "does not give its type, mode, owner and group";
			return -1;
		}
		n->type = e->type;
		n->atime = n->mtime = n->ctime = (uint32_t)time(NULL);
		if (n->type == MFS_S_IFLNK && !(e->set & SPEC_LINK)) {
			*why = "a symbolic link without link=";
			return -1;
		}
		if (is_dev(n) && !(e->set & SPEC_DEVICE)) {
			*why = "a device without device=";
			return -1;
		}
	}
	if (e != NULL) {
		if (e->set & SPEC_MODE)
			n->perm = e->mode;
		if (e->set & SPEC_UID)
			n->uid = e->uid;
		if (e->set & SPEC_GID)
			n->gid = e->gid;
		if (e->set & SPEC_TIME)
			n->atime = n->mtime = n->ctime = e->time;
		if ((e->set & SPEC_LINK) && n->type == MFS_S_IFLNK)
			n->link = e->link;
		if ((e->set & SPEC_DEVICE) && is_dev(n)) {
			n->major = e->major;
			n->minor = e->minor;
		}
	}
	if (f->owned) {
		n->uid = f->uid;
		n->gid = f->gid;
	}
	/* The BSDs apply the umask to links, which means nothing to them. */
	if (n->type == MFS_S_IFLNK)
		n->perm = 0777;
	if (n->type == MFS_S_IFSOCK)
		return 1;
	return 0;
}

/*
 * The names in the directory rel, sorted: those of the host directory
 * host, if there is one, and those that the specification gives; with
 * -x, only the names the specification gives.
 */
static char **
node_names(const struct tree_fs *f, const char *host, const char *rel,
    size_t *np)
{
	char **v, **nv, *path;
	const char *name;
	size_t first, i, j, k, last, n;

	if (host != NULL) {
		if ((v = read_names(host, &n)) == NULL)
			return NULL;
	} else {
		if ((v = malloc(sizeof(*v))) == NULL)
			err(1, NULL);
		n = 0;
	}
	if (f->exclude) {
		for (i = j = 0; i < n; i++) {
			path = join_rel(rel, v[i]);
			if (spec_find(f->spec, path) != NULL)
				v[j++] = v[i];
			else
				free(v[i]);
			free(path);
		}
		n = j;
	}
	last = spec_below(f->spec, rel, &first);
	for (k = first; k < last; k++) {
		name = f->spec->e[k].path + (rel[0] == '\0' ? 0 :
		    strlen(rel) + 1);
		if (name[0] == '\0' || strchr(name, '/') != NULL)
			continue;
		for (i = 0; i < n && strcmp(v[i], name) != 0; i++)
			continue;
		if (i < n)
			continue;
		if ((nv = realloc(v, (n + 1) * sizeof(*v))) == NULL)
			err(1, NULL);
		v = nv;
		if ((v[n++] = strdup(name)) == NULL)
			err(1, NULL);
	}
	if (n > 1)
		qsort(v, n, sizeof(*v), by_name);
	*np = n;
	return v;
}

/*
 * The node name below the directory host (or NULL) and rel.  Returns 0,
 * 1 if there is nothing there to copy, or -1 if it cannot be read.
 */
static int
node_at(const struct tree_fs *f, const char *host, const char *rel,
    const char *name, struct node *n)
{
	(void)memset(n, 0, sizeof(*n));
	n->rel = join_rel(rel, name);
	n->spec = spec_find(f->spec, n->rel);
	if (host != NULL) {
		n->host = join(host, name);
		if (lstat(n->host, &n->st) == -1) {
			if (errno != ENOENT || n->spec == NULL) {
				warn("%s", n->host);
				return -1;
			}
			free(n->host);
			n->host = NULL;
		}
	}
	return n->host == NULL && n->spec == NULL ? 1 : 0;
}

static void
node_free(struct node *n)
{
	free(n->host);
	free(n->rel);
}

/* Where to report on a node: its host path, or its path in the tree. */
static const char *
where(const struct node *n)
{
	return n->host != NULL ? n->host : n->rel;
}

/*
 * Scanning.
 */

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

static int scan_dir(struct scan *, const char *, const char *);

/* One entry below a directory; returns 1 if it takes an entry there. */
static int
scan_entry(struct scan *s, struct node *n, const char *name,
    uint32_t *subdirs)
{
	const struct tree_fs *f;
	const char *why;
	struct link *l;
	uint64_t size;
	int r;

	f = s->f;
	if (strlen(name) > f->namelen)
		problem(s, where(n), "name longer than %" PRIu32 " characters",
		    f->namelen);
	if ((r = resolve(f, n, &why)) == -1) {
		problem(s, where(n), "%s", why);
		return 0;
	}
	if (r == 1) {
		if (n->type == MFS_S_IFSOCK) {
			warnx("%s: a socket cannot be copied; left out",
			    where(n));
			s->need->sockets++;
		}
		return 0;
	}
	if (n->uid > MFS_MAX_UID) {
		problem(s, where(n), "owner %" PRIu32 " does not fit", n->uid);
		s->need->owners++;
	}
	if (n->gid > (f->version == 1 ? MFS_MAX_GID_V1 : MFS_MAX_GID)) {
		problem(s, where(n), "group %" PRIu32 " does not fit in V%d",
		    n->gid, f->version);
		s->need->owners++;
	}
	if (n->type == MFS_S_IFDIR) {
		(*subdirs)++;
		s->need->inodes++;
		return scan_dir(s, n->host, n->rel) == -1 ? -1 : 1;
	}
	if (n->host != NULL && n->st.st_nlink > 1) {
		l = link_find(&s->links, n->st.st_dev, n->st.st_ino);
		if (l->names++ > 0)
			return 1;
	}
	s->need->inodes++;
	if (n->type == MFS_S_IFREG || n->type == MFS_S_IFLNK) {
		size = n->link != NULL ? strlen(n->link) :
		    n->host != NULL ? (uint64_t)n->st.st_size : 0;
		if (size > max_file(f))
			problem(s, where(n), "%ju bytes are more than a file "
			    "of V%d holds", (uintmax_t)size, f->version);
		/* MINIX reads a link from its first block, with a NUL. */
		else if (n->type == MFS_S_IFLNK &&
		    (size == 0 || size >= f->block_size))
			problem(s, where(n), "a symbolic link of %ju bytes, "
			    "where MINIX takes 1 to %" PRIu32, (uintmax_t)size,
			    f->block_size - 1);
		else
			s->need->zones += file_zones(f, size);
	} else if (is_dev(n)) {
		if (n->major > MFS_MAX_DEV_PART || n->minor > MFS_MAX_DEV_PART)
			problem(s, where(n), "device %" PRIu32 ",%" PRIu32
			    " does not fit", n->major, n->minor);
	} else if (n->type != MFS_S_IFIFO) {
		problem(s, where(n), "not a kind of file that can be copied");
	}
	return 1;
}

static int
scan_dir(struct scan *s, const char *host, const char *rel)
{
	struct node n;
	uint64_t entries;
	uint32_t subdirs;
	size_t i, count;
	char **names;
	int r;

	if ((names = node_names(s->f, host, rel, &count)) == NULL)
		return -1;
	entries = 2;
	subdirs = 0;
	r = 0;
	for (i = 0; i < count && r != -1; i++) {
		if ((r = node_at(s->f, host, rel, names[i], &n)) == 0 &&
		    (r = scan_entry(s, &n, names[i], &subdirs)) == 1)
			entries++;
		node_free(&n);
	}
	free_names(names, count);
	if (r == -1)
		return -1;
	if (subdirs + 2 > (s->f->version == 1 ? MFS_MAX_LINKS_V1 :
	    MFS_MAX_LINKS))
		problem(s, host != NULL ? host : rel, "%" PRIu32 " directories "
		    "below it are more than a link count holds", subdirs);
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
	s.f = f;
	s.need = need;
	if (f->owned && (f->uid > MFS_MAX_UID ||
	    f->gid > (f->version == 1 ? MFS_MAX_GID_V1 : MFS_MAX_GID))) {
		warnx("owner %" PRIu32 ":%" PRIu32 " does not fit in V%d",
		    f->uid, f->gid, f->version);
		need->problems++;
	}
	if (lstat(dir, &st) == -1) {
		warn("%s", dir);
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		warnx("%s: %s", dir, strerror(ENOTDIR));
		return -1;
	}
	need->inodes = 1;
	r = scan_dir(&s, dir, "");
	for (i = 0; i < s.links.max; i++) {
		if (s.links.tab[i].used && s.links.tab[i].names >
		    (f->version == 1 ? MFS_MAX_LINKS_V1 : MFS_MAX_LINKS)) {
			need->problems++;
			warnx("a file with %" PRIu32 " names in the tree has "
			    "more than a link count holds",
			    s.links.tab[i].names);
		}
	}
	free(s.links.tab);
	/* What the walk did not reach lies below no directory of it. */
	for (i = 0; f->spec != NULL && i < f->spec->n; i++) {
		if (!f->spec->e[i].seen && f->spec->e[i].path[0] != '\0' &&
		    !(f->spec->e[i].set & SPEC_OPTIONAL)) {
			need->problems++;
			warnx("%s: in the specification, but below no "
			    "directory of the tree", f->spec->e[i].path);
		}
	}
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

static void
set_attrs(struct mfs_inode *ip, const struct node *n)
{
	ip->mode = (uint16_t)(n->type | n->perm);
	ip->uid = (uint16_t)n->uid;
	ip->gid = (uint16_t)n->gid;
	ip->atime = n->atime;
	ip->mtime = n->mtime;
	ip->ctime = n->ctime;
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

/* The text of a symbolic link, from the spec or the host, into *ip. */
static int
copy_link(struct copy *c, const struct node *n, struct mfs_inode *ip)
{
	char text[PATH_MAX];
	const char *t;
	ssize_t len;
	int r;

	if (n->link != NULL) {
		t = n->link;
		len = (ssize_t)strlen(t);
	} else {
		if ((len = readlink(n->host, text, sizeof(text))) == -1) {
			warn("%s", n->host);
			return -1;
		}
		t = text;
	}
	if ((r = mfs_pwrite(c->fs, ip, t, (size_t)len, 0)) < 0)
		return failed(where(n), r);
	return 0;
}

static int copy_dir(struct copy *, const char *, const char *, uint32_t);

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

/* Make one entry of the directory *dp. */
static int
copy_entry(struct copy *c, struct mfs_inode *dp, struct node *n,
    const char *name)
{
	struct mfs_inode ip;
	struct link *l;
	const char *why;
	uint32_t ino;
	int r;

	if (resolve(c->f, n, &why) != 0)
		return 0;
	l = NULL;
	if (n->type != MFS_S_IFDIR && n->host != NULL &&
	    n->st.st_nlink > 1) {
		l = link_find(&c->links, n->st.st_dev, n->st.st_ino);
		if (l->mino != 0) {
			/* Another name of a file already made. */
			if ((r = mfs_get_inode(c->fs, l->mino, &ip)) < 0)
				return failed(where(n), r);
			ip.nlinks++;
			if ((r = mfs_put_inode(c->fs, &ip)) < 0)
				return failed(where(n), r);
			return enter(c, dp, name, l->mino, where(n));
		}
	}
	if ((r = mfs_alloc_inode(c->fs, &ino)) < 0)
		return failed(where(n), r);
	(void)memset(&ip, 0, sizeof(ip));
	ip.num = ino;
	ip.nlinks = 1;
	set_attrs(&ip, n);
	r = 0;
	if (n->type == MFS_S_IFDIR) {
		ip.nlinks = 2;
		if ((r = mfs_add_entry(c->fs, &ip, ".", ino)) < 0 ||
		    (r = mfs_add_entry(c->fs, &ip, "..", dp->num)) < 0)
			return failed(where(n), r);
	} else if (n->type == MFS_S_IFREG && n->host != NULL) {
		r = copy_data(c, n->host, &ip);
	} else if (n->type == MFS_S_IFLNK) {
		r = copy_link(c, n, &ip);
	} else if (is_dev(n)) {
		ip.zone[0] = n->major << 8 | n->minor;
	}
	if (r < 0)
		return -1;
	if ((r = mfs_put_inode(c->fs, &ip)) < 0)
		return failed(where(n), r);
	if (l != NULL)
		l->mino = ino;
	if (n->type == MFS_S_IFDIR) {
		dp->nlinks++;
		if (enter(c, dp, name, ino, where(n)) == -1)
			return -1;
		return copy_dir(c, n->host, n->rel, ino);
	}
	return enter(c, dp, name, ino, where(n));
}

static int
copy_dir(struct copy *c, const char *host, const char *rel, uint32_t dino)
{
	struct mfs_inode dp;
	struct node n;
	size_t i, count;
	char **names;
	int r;

	if ((names = node_names(c->f, host, rel, &count)) == NULL)
		return -1;
	r = 0;
	for (i = 0; i < count && r == 0; i++) {
		/* The directory changes as entries go in: read it anew. */
		if ((r = mfs_get_inode(c->fs, dino, &dp)) < 0) {
			r = failed(host != NULL ? host : rel, r);
			break;
		}
		if ((r = node_at(c->f, host, rel, names[i], &n)) == 0)
			r = copy_entry(c, &dp, &n, names[i]);
		else if (r == 1)
			r = 0;
		node_free(&n);
	}
	free_names(names, count);
	return r;
}

int
tree_copy(struct mfs *fs, const char *dir, const struct tree_fs *f)
{
	struct mfs_inode root;
	struct copy c;
	struct node n;
	const char *why;
	int r;

	(void)memset(&c, 0, sizeof(c));
	c.fs = fs;
	c.f = f;
	(void)memset(&n, 0, sizeof(n));
	n.rel = (char *)(uintptr_t)"";
	n.host = (char *)(uintptr_t)dir;
	n.spec = spec_find(f->spec, "");
	if (lstat(dir, &n.st) == -1) {
		warn("%s", dir);
		return -1;
	}
	if (resolve(f, &n, &why) != 0) {
		warnx("%s: %s", dir, why);
		return -1;
	}
	if ((r = mfs_get_inode(fs, MFS_ROOT_INO, &root)) < 0)
		return failed(dir, r);
	set_attrs(&root, &n);
	if ((r = mfs_put_inode(fs, &root)) < 0)
		return failed(dir, r);
	r = copy_dir(&c, dir, "", MFS_ROOT_INO);
	free(c.links.tab);
	if (r == 0 && (r = mfs_sync(fs)) < 0)
		return failed(dir, r);
	return r;
}
