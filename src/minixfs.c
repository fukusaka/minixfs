/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * minixfs - inspect and extract MINIX file system images.
 *
 *	minixfs info IMAGE
 *	minixfs ls [-lR] IMAGE [PATH]
 *	minixfs cat IMAGE PATH
 *	minixfs extract [-v] IMAGE DEST [PATH]
 *
 * Each command reports every problem it meets and goes on where it can.
 * The exit status is 0 on success, 1 if anything failed and 2 for a
 * usage error.
 */

#include <sys/stat.h>
#include <sys/time.h>

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

#ifndef O_NOFOLLOW
#define O_NOFOLLOW	0
#endif

#define MAX_DEPTH	256		/* deepest directory entered */
#define COPY_SIZE	65536		/* bytes copied at a time */

/* One command on one image. */
struct cmd {
	struct mfs	fs;
	const char	*image;
	int		status;		/* exit status so far */
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

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: minixfs info IMAGE\n"
	    "       minixfs ls [-lR] IMAGE [PATH]\n"
	    "       minixfs cat IMAGE PATH\n"
	    "       minixfs extract [-v] IMAGE DEST [PATH]\n");
	exit(2);
}

/* Report a problem and remember that the command failed. */
static void
problem(struct cmd *c, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarnx(fmt, ap);
	va_end(ap);
	c->status = 1;
}

/* Open the image, or report why not and exit. */
static void
open_image(struct cmd *c, const char *image)
{
	int r;

	c->image = image;
	c->status = 0;
	if ((r = mfs_open(&c->fs, image)) == 0)
		return;
	if (r == -EINVAL)
		errx(1, "%s: not a MINIX file system", image);
	errx(1, "%s: %s", image, strerror(-r));
}

/* Look up path in the image; report and return -1 if that fails. */
static int
lookup(struct cmd *c, const char *path, struct mfs_inode *ip)
{
	int r;

	if ((r = mfs_namei(&c->fs, path, ip)) < 0) {
		problem(c, "%s:%s: %s", c->image, path, strerror(-r));
		return -1;
	}
	return 0;
}

/* dir/name in new memory, or name alone if dir is empty. */
static char *
join(struct cmd *c, const char *dir, const char *name)
{
	size_t dlen, nlen;
	char *s;

	dlen = strlen(dir);
	nlen = strlen(name);
	if ((s = malloc(dlen + nlen + 2)) == NULL) {
		problem(c, "%s", strerror(ENOMEM));
		return NULL;
	}
	if (dlen == 0)
		(void)memcpy(s, name, nlen + 1);
	else if (dir[dlen - 1] == '/')
		(void)snprintf(s, dlen + nlen + 2, "%s%s", dir, name);
	else
		(void)snprintf(s, dlen + nlen + 2, "%s/%s", dir, name);
	return s;
}

static int
walk_enter(struct cmd *c, struct walk *w, uint32_t ino, const char *path)
{
	int i;

	if (w->depth == MAX_DEPTH) {
		problem(c, "%s:%s: directories nested too deeply", c->image,
		    path);
		return -1;
	}
	for (i = 0; i < w->depth; i++) {
		if (w->stack[i] == ino) {
			problem(c, "%s:%s: directory loop", c->image, path);
			return -1;
		}
	}
	w->stack[w->depth++] = ino;
	return 0;
}

static void
walk_leave(struct walk *w)
{
	w->depth--;
}

static int
collect_fn(const struct mfs_dirent *de, void *arg)
{
	struct mfs_dirent *p;
	struct dirlist *dl;

	dl = arg;
	if (strcmp(de->name, ".") == 0 || strcmp(de->name, "..") == 0)
		return 0;
	if (dl->n == dl->max) {
		dl->max = dl->max == 0 ? 32 : dl->max * 2;
		if ((p = realloc(dl->ent, dl->max * sizeof(*p))) == NULL)
			return -ENOMEM;
		dl->ent = p;
	}
	dl->ent[dl->n++] = *de;
	return 0;
}

/*
 * Collect the entries of a directory, since a walk cannot recurse from
 * inside mfs_readdir().  Returns -1 after reporting a failure.
 */
static int
read_dir(struct cmd *c, const struct mfs_inode *dp, const char *path,
    struct dirlist *dl)
{
	int r;

	dl->ent = NULL;
	dl->max = 0;
	dl->n = 0;
	if ((r = mfs_readdir(&c->fs, dp, collect_fn, dl)) < 0) {
		problem(c, "%s:%s: %s", c->image, path, strerror(-r));
		free(dl->ent);
		dl->ent = NULL;
		dl->n = 0;
		return -1;
	}
	return 0;
}

/* Read the inode of a directory entry; report and return -1 on failure. */
static int
entry_inode(struct cmd *c, const struct mfs_dirent *de, const char *path,
    struct mfs_inode *ip)
{
	int r;

	if ((r = mfs_read_inode(&c->fs, de->ino, ip)) < 0) {
		problem(c, "%s:%s: inode %" PRIu32 ": %s", c->image, path,
		    de->ino, strerror(-r));
		return -1;
	}
	return 0;
}

/* info */

static int
cmd_info(int argc, char **argv)
{
	struct cmd c;
	const struct mfs *fs;
	uint32_t inodes, zones;
	int r;

	if (argc != 2)
		usage();
	open_image(&c, argv[1]);
	fs = &c.fs;
	(void)printf("version: %d\n", fs->version);
	(void)printf("byte order: %s\n",
	    fs->order == MFS_BIG_ENDIAN ? "big-endian" : "little-endian");
	(void)printf("magic: 0x%04" PRIx16 "\n", fs->magic);
	(void)printf("name length: %" PRIu32 "\n", fs->namelen);
	(void)printf("block size: %" PRIu32 "\n", fs->block_size);
	(void)printf("inodes: %" PRIu32 "\n", fs->ninodes);
	(void)printf("zones: %" PRIu32 "\n", fs->nzones);
	(void)printf("inode map blocks: %" PRIu32 "\n", fs->imap_blocks);
	(void)printf("zone map blocks: %" PRIu32 "\n", fs->zmap_blocks);
	(void)printf("first data zone: %" PRIu32 "\n", fs->firstdatazone);
	(void)printf("log zone size: %" PRIu32 "\n", fs->log_zone_size);
	(void)printf("max file size: %" PRIu32 "\n", fs->max_size);
	(void)printf("clean: %s\n", mfs_is_clean(fs) ? "yes" : "no");
	if ((r = mfs_count_free(&c.fs, &inodes, &zones)) < 0) {
		problem(&c, "%s: bit maps: %s", c.image, strerror(-r));
	} else {
		(void)printf("free inodes: %" PRIu32 "\n", inodes);
		(void)printf("free zones: %" PRIu32 "\n", zones);
	}
	mfs_close(&c.fs);
	return c.status;
}

/* ls */

struct ls {
	struct walk	w;
	struct cmd	*c;
	int		longfmt;
	int		recurse;
};

/* The mode word in the form of "ls -l". */
static void
mode_string(uint16_t mode, char *s)
{
	static const char rwx[] = "rwxrwxrwx";
	int i;

	switch (mode & MFS_S_IFMT) {
	case MFS_S_IFDIR:
		s[0] = 'd';
		break;
	case MFS_S_IFCHR:
		s[0] = 'c';
		break;
	case MFS_S_IFBLK:
		s[0] = 'b';
		break;
	case MFS_S_IFIFO:
		s[0] = 'p';
		break;
	case MFS_S_IFLNK:
		s[0] = 'l';
		break;
	case MFS_S_IFSOCK:
		s[0] = 's';
		break;
	case MFS_S_IFREG:
		s[0] = '-';
		break;
	default:
		s[0] = '?';
		break;
	}
	for (i = 0; i < 9; i++)
		s[i + 1] = (mode & (0400 >> i)) != 0 ? rwx[i] : '-';
	if ((mode & 04000) != 0)
		s[3] = s[3] == 'x' ? 's' : 'S';
	if ((mode & 02000) != 0)
		s[6] = s[6] == 'x' ? 's' : 'S';
	if ((mode & 01000) != 0)
		s[9] = s[9] == 'x' ? 't' : 'T';
	s[10] = '\0';
}

/* Print " -> target" for a symbolic link. */
static void
print_target(struct cmd *c, const struct mfs_inode *ip, const char *path)
{
	char target[PATH_MAX];
	ssize_t n;

	if (ip->size >= sizeof(target)) {
		problem(c, "%s:%s: symbolic link too long", c->image, path);
		return;
	}
	if ((n = mfs_pread(&c->fs, ip, target, ip->size, 0)) < 0) {
		problem(c, "%s:%s: %s", c->image, path, strerror((int)-n));
		return;
	}
	target[n] = '\0';
	(void)printf(" -> %s", target);
}

static void
print_entry(struct ls *l, const struct mfs_inode *ip, const char *name)
{
	char mode[11], when[32];
	struct tm *tm;
	time_t t;

	if (!l->longfmt) {
		(void)printf("%s\n", name);
		return;
	}
	mode_string(ip->mode, mode);
	t = (time_t)ip->mtime;
	if ((tm = gmtime(&t)) == NULL ||
	    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm) == 0)
		(void)snprintf(when, sizeof(when), "%" PRIu32, ip->mtime);
	(void)printf("%s %3" PRIu16 " %5" PRIu16 " %5" PRIu16 " ", mode,
	    ip->nlinks, ip->uid, ip->gid);
	if (mfs_is_dev(ip))
		(void)printf("%4" PRIu32 ",%4" PRIu32,
		    mfs_rdev(ip) >> 8 & 0xff, mfs_rdev(ip) & 0xff);
	else
		(void)printf("%9" PRIu32, ip->size);
	(void)printf(" %s %s", when, name);
	if (mfs_is_lnk(ip))
		print_target(l->c, ip, name);
	(void)putchar('\n');
}

/*
 * List the directory *dp.  Names are printed below prefix, which is empty
 * at the top; label names the directory in messages.
 */
static void
ls_dir(struct ls *l, const struct mfs_inode *dp, const char *prefix,
    const char *label)
{
	struct mfs_inode ino;
	struct dirlist dl;
	size_t i;
	char *name;

	if (read_dir(l->c, dp, label, &dl) == -1)
		return;
	for (i = 0; i < dl.n; i++) {
		if ((name = join(l->c, prefix, dl.ent[i].name)) == NULL)
			break;
		if (entry_inode(l->c, &dl.ent[i], name, &ino) == 0) {
			print_entry(l, &ino, name);
			if (l->recurse && mfs_is_dir(&ino) &&
			    walk_enter(l->c, &l->w, ino.num, name) == 0) {
				ls_dir(l, &ino, name, name);
				walk_leave(&l->w);
			}
		}
		free(name);
	}
	free(dl.ent);
}

static int
cmd_ls(int argc, char **argv)
{
	struct mfs_inode ino;
	struct cmd c;
	struct ls l;
	const char *path;
	int ch;

	(void)memset(&l, 0, sizeof(l));
	optind = 1;
	while ((ch = getopt(argc, argv, "lR")) != -1) {
		switch (ch) {
		case 'l':
			l.longfmt = 1;
			break;
		case 'R':
			l.recurse = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1 || argc > 2)
		usage();
	path = argc == 2 ? argv[1] : "/";

	open_image(&c, argv[0]);
	l.c = &c;
	if (lookup(&c, path, &ino) == 0) {
		if (!mfs_is_dir(&ino))
			print_entry(&l, &ino, path);
		else if (walk_enter(&c, &l.w, ino.num, path) == 0)
			ls_dir(&l, &ino, "", path);
	}
	mfs_close(&c.fs);
	return c.status;
}

/* cat */

/*
 * Copy the contents of *ip to the descriptor fd; what names fd in
 * messages.  Returns -1 after reporting a failure.
 */
static int
copy_out(struct cmd *c, const struct mfs_inode *ip, int fd, const char *what)
{
	unsigned char *buf;
	size_t done;
	ssize_t n, w;
	uint32_t off;
	int r;

	if ((buf = malloc(COPY_SIZE)) == NULL) {
		problem(c, "%s", strerror(ENOMEM));
		return -1;
	}
	r = 0;
	for (off = 0; r == 0; off += (uint32_t)n) {
		if ((n = mfs_pread(&c->fs, ip, buf, COPY_SIZE, off)) <= 0)
			break;
		for (done = 0; done < (size_t)n; done += (size_t)w) {
			w = write(fd, buf + done, (size_t)n - done);
			if (w == -1 && errno == EINTR) {
				w = 0;
			} else if (w == -1) {
				problem(c, "%s: %s", what, strerror(errno));
				r = -1;
				break;
			}
		}
	}
	if (n < 0) {
		problem(c, "%s:inode %" PRIu32 ": %s", c->image, ip->num,
		    strerror((int)-n));
		r = -1;
	}
	free(buf);
	return r;
}

static int
cmd_cat(int argc, char **argv)
{
	struct mfs_inode ino;
	struct cmd c;

	if (argc != 3)
		usage();
	open_image(&c, argv[1]);
	if (lookup(&c, argv[2], &ino) == 0) {
		if (mfs_is_dir(&ino))
			problem(&c, "%s:%s: %s", c.image, argv[2],
			    strerror(EISDIR));
		else if (!mfs_is_reg(&ino))
			problem(&c, "%s:%s: not a regular file", c.image,
			    argv[2]);
		else
			(void)copy_out(&c, &ino, STDOUT_FILENO,
			    "standard output");
	}
	mfs_close(&c.fs);
	return c.status;
}

/* extract */

struct extract {
	struct walk	w;
	struct cmd	*c;
	unsigned long	dirs;
	unsigned long	files;
	unsigned long	links;
	unsigned long	skipped;
	int		verbose;
};

/* A name from the image may only name something inside its directory. */
static int
safe_name(const char *name)
{
	return name[0] != '\0' && strcmp(name, ".") != 0 &&
	    strcmp(name, "..") != 0 && strchr(name, '/') == NULL;
}

static void
set_times(struct cmd *c, const char *path, const struct mfs_inode *ip)
{
	struct timeval tv[2];

	tv[0].tv_sec = (time_t)ip->atime;
	tv[0].tv_usec = 0;
	tv[1].tv_sec = (time_t)ip->mtime;
	tv[1].tv_usec = 0;
	if (utimes(path, tv) == -1)
		problem(c, "%s: %s", path, strerror(errno));
}

static void
extract_file(struct extract *x, const struct mfs_inode *ip,
    const char *dest)
{
	int fd;

	fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd == -1) {
		problem(x->c, "%s: %s", dest, strerror(errno));
		return;
	}
	if (copy_out(x->c, ip, fd, dest) == -1) {
		(void)close(fd);
		return;
	}
	if (fchmod(fd, ip->mode & 0777) == -1)
		problem(x->c, "%s: %s", dest, strerror(errno));
	if (close(fd) == -1) {
		problem(x->c, "%s: %s", dest, strerror(errno));
		return;
	}
	set_times(x->c, dest, ip);
	x->files++;
}

static void
extract_link(struct extract *x, const struct mfs_inode *ip,
    const char *src, const char *dest)
{
	char target[PATH_MAX];
	ssize_t n;

	if (ip->size >= sizeof(target)) {
		problem(x->c, "%s:%s: symbolic link too long", x->c->image,
		    src);
		return;
	}
	if ((n = mfs_pread(&x->c->fs, ip, target, ip->size, 0)) < 0) {
		problem(x->c, "%s:%s: %s", x->c->image, src,
		    strerror((int)-n));
		return;
	}
	target[n] = '\0';
	if (symlink(target, dest) == -1) {
		problem(x->c, "%s: %s", dest, strerror(errno));
		return;
	}
	x->links++;
}

static void extract_dir(struct extract *, const struct mfs_inode *,
    const char *, const char *);

/* Make the directory dest, or accept one that is already there. */
static int
make_dir(struct extract *x, const char *dest)
{
	struct stat st;
	int e;

	if (mkdir(dest, 0700) == 0)
		return 0;
	e = errno;
	if (e == EEXIST && lstat(dest, &st) == 0 && S_ISDIR(st.st_mode))
		return 0;
	problem(x->c, "%s: %s", dest, strerror(e));
	return -1;
}

static void
extract_entry(struct extract *x, const struct mfs_inode *ip,
    const char *src, const char *dest)
{
	if (x->verbose)
		(void)printf("%s\n", src);
	switch (ip->mode & MFS_S_IFMT) {
	case MFS_S_IFDIR:
		if (make_dir(x, dest) == -1 ||
		    walk_enter(x->c, &x->w, ip->num, src) == -1)
			return;
		extract_dir(x, ip, src, dest);
		walk_leave(&x->w);
		/* Permissions and times last, once the contents are in. */
		if (chmod(dest, ip->mode & 0777) == -1)
			problem(x->c, "%s: %s", dest, strerror(errno));
		set_times(x->c, dest, ip);
		x->dirs++;
		break;
	case MFS_S_IFREG:
		extract_file(x, ip, dest);
		break;
	case MFS_S_IFLNK:
		extract_link(x, ip, src, dest);
		break;
	default:
		/*
		 * Devices, pipes and sockets need privileges or make no
		 * sense as copies; "ls -l" shows them.
		 */
		x->skipped++;
		break;
	}
}

static void
extract_dir(struct extract *x, const struct mfs_inode *dp, const char *src,
    const char *dest)
{
	struct mfs_inode ino;
	struct dirlist dl;
	size_t i;
	char *d, *s;

	if (read_dir(x->c, dp, src, &dl) == -1)
		return;
	for (i = 0; i < dl.n; i++) {
		if ((s = join(x->c, src, dl.ent[i].name)) == NULL)
			break;
		if (!safe_name(dl.ent[i].name)) {
			problem(x->c, "%s:%s: unsafe name, not extracted",
			    x->c->image, s);
		} else if ((d = join(x->c, dest, dl.ent[i].name)) != NULL) {
			if (entry_inode(x->c, &dl.ent[i], s, &ino) == 0)
				extract_entry(x, &ino, s, d);
			free(d);
		}
		free(s);
	}
	free(dl.ent);
}

static int
cmd_extract(int argc, char **argv)
{
	struct mfs_inode ino;
	struct extract x;
	struct cmd c;
	const char *dest, *path;
	int ch;

	(void)memset(&x, 0, sizeof(x));
	optind = 1;
	while ((ch = getopt(argc, argv, "v")) != -1) {
		switch (ch) {
		case 'v':
			x.verbose = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 2 || argc > 3)
		usage();
	dest = argv[1];
	path = argc == 3 ? argv[2] : "/";

	open_image(&c, argv[0]);
	x.c = &c;
	if (lookup(&c, path, &ino) == 0) {
		if (!mfs_is_dir(&ino))
			problem(&c, "%s:%s: %s", c.image, path,
			    strerror(ENOTDIR));
		else if (mkdir(dest, 0777) == -1 && errno != EEXIST)
			problem(&c, "%s: %s", dest, strerror(errno));
		else if (walk_enter(&c, &x.w, ino.num, path) == 0)
			extract_dir(&x, &ino, path, dest);
	}
	mfs_close(&c.fs);
	(void)fprintf(stderr, "%lu files, %lu directories, %lu symbolic links;"
	    " %lu special files skipped\n", x.files, x.dirs, x.links,
	    x.skipped);
	return c.status;
}

int
main(int argc, char **argv)
{
	static const struct {
		const char	*name;
		int		(*fn)(int, char **);
	} cmds[] = {
		{ "info", cmd_info },
		{ "ls", cmd_ls },
		{ "cat", cmd_cat },
		{ "extract", cmd_extract }
	};
	size_t i;

	if (argc < 2)
		usage();
	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (strcmp(argv[1], cmds[i].name) == 0)
			return cmds[i].fn(argc - 1, argv + 1);
	usage();
	/* NOTREACHED */
	return 2;
}
