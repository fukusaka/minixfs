/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * minixfs - inspect and extract MINIX file system images.
 *
 *	minixfs [-T SIZE:HEADS:SIDE] info IMAGE
 *	minixfs [-T ...] ls [-lR] IMAGE [PATH]
 *	minixfs [-T ...] cat IMAGE PATH
 *	minixfs [-T ...] extract [-dv] IMAGE DEST [PATH]
 *	minixfs [-T ...] tar IMAGE [PATH] > ARCHIVE
 *
 * -T reads an image that holds the file system in the tracks of one side
 * only, such as a single-sided disk read as double-sided: tracks of SIZE
 * bytes, HEADS to a cylinder, of which side SIDE (from 0) is used.
 *
 * Each command reports every problem it meets and goes on where it can.
 * The exit status is 0 on success, 1 if anything failed and 2 for a
 * usage error.
 */

#include "compat.h"

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

/* How the image file holds the file system: -T. */
static struct mfs_tracks tracks;

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
	    "usage: minixfs [-T SIZE:HEADS:SIDE] info IMAGE\n"
	    "       minixfs [-T ...] ls [-lR] IMAGE [PATH]\n"
	    "       minixfs [-T ...] cat IMAGE PATH\n"
	    "       minixfs [-T ...] extract [-dv] IMAGE DEST [PATH]\n"
	    "       minixfs [-T ...] tar IMAGE [PATH] > ARCHIVE\n");
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
	if ((r = mfs_open_tracks(&c->fs, image, 0, &tracks)) == 0)
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

#define SECTOR		512		/* the unit of the fill scan */
#define FILL_E5		0xe5		/* what formats leave in sectors */
#define FILL_A5		0xa5

/* Whether a sector holds nothing but what a format leaves in it. */
static int
is_fill(const unsigned char *p)
{
	size_t i;

	if (p[0] != FILL_E5 && p[0] != FILL_A5)
		return 0;
	for (i = 1; i < SECTOR; i++)
		if (p[i] != p[0])
			return 0;
	return 1;
}

/* A run length of fill sectors, and how often it came. */
struct run {
	uint64_t	len;
	uint64_t	count;
};

static void
count_run(struct run **runs, size_t *nruns, uint64_t len)
{
	struct run *r;
	size_t i;

	for (i = 0; i < *nruns; i++) {
		if ((*runs)[i].len == len) {
			(*runs)[i].count++;
			return;
		}
	}
	if ((r = realloc(*runs, (*nruns + 1) * sizeof(*r))) == NULL)
		err(1, NULL);
	*runs = r;
	r[*nruns].len = len;
	r[*nruns].count = 1;
	(*nruns)++;
}

/*
 * The sectors of the image that hold nothing but 0xe5 or 0xa5, what
 * formats write and nothing else does: a disk read with more sides than
 * it was written on, or a copy cut short, shows up as runs of them.
 */
static void
print_fill(struct cmd *c)
{
	static unsigned char buf[SECTOR];
	struct run *runs, *best;
	uint64_t bytes, len, nrun;
	size_t i, nruns;
	off_t off;

	runs = NULL;
	nruns = 0;
	bytes = 0;
	len = 0;
	for (off = 0; off + SECTOR <= c->fs.image_size; off += SECTOR) {
		if (mfs_read_device(&c->fs, buf, SECTOR, off) < 0)
			break;
		if (is_fill(buf)) {
			len += SECTOR;
			continue;
		}
		if (len > 0)
			count_run(&runs, &nruns, len);
		bytes += len;
		len = 0;
	}
	if (len > 0)
		count_run(&runs, &nruns, len);
	bytes += len;
	nrun = 0;
	best = NULL;
	for (i = 0; i < nruns; i++) {
		nrun += runs[i].count;
		if (best == NULL || runs[i].count > best->count)
			best = &runs[i];
	}
	(void)printf("fill sectors: %" PRIu64 " bytes, %" PRIu64 "%% of the "
	    "image, in %" PRIu64 " runs\n", bytes,
	    c->fs.image_size > 0 ? bytes * 100 / (uint64_t)c->fs.image_size :
	    0, nrun);
	if (best != NULL)
		(void)printf("commonest fill run: %" PRIu64 " bytes, %" PRIu64
		    " time%s\n", best->len, best->count,
		    best->count == 1 ? "" : "s");
	free(runs);
}

/* The size of the image against that of the file system. */
static void
print_sizes(const struct cmd *c)
{
	uint64_t fsbytes, image;

	image = (uint64_t)c->fs.image_size;
	fsbytes = (uint64_t)c->fs.nblocks * c->fs.block_size;
	(void)printf("image size: %" PRIu64 "\n", image);
	(void)printf("file system size: %" PRIu64, fsbytes);
	if (image > 0 && fsbytes != image)
		(void)printf(" (%" PRIu64 "%% of the image)",
		    fsbytes * 100 / image);
	(void)putchar('\n');
}

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
	print_sizes(&c);
	print_fill(&c);
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
	unsigned long	devs;
	unsigned long	pipes;
	unsigned long	devs_skipped;	/* without -d */
	unsigned long	sockets;	/* cannot be made */
	int		devices;	/* -d: make devices, as root */
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

/*
 * A device, which is made only with -d, as root, or a pipe.  Like files,
 * they get the permission bits without set-uid, set-gid and sticky bits.
 */
static void
extract_special(struct extract *x, const struct mfs_inode *ip,
    const char *src, const char *dest)
{
	mode_t type;
	dev_t dev;

	if (mfs_is_dev(ip) && !x->devices) {
		if (x->verbose)
			(void)printf("%s: device not made\n", src);
		x->devs_skipped++;
		return;
	}
	if (mfs_is_dev(ip)) {
		type = (ip->mode & MFS_S_IFMT) == MFS_S_IFCHR ? S_IFCHR :
		    S_IFBLK;
		dev = makedev(mfs_rdev(ip) >> 8 & 0xff, mfs_rdev(ip) & 0xff);
		if (mknod(dest, type | 0600, dev) == -1) {
			problem(x->c, "%s: %s", dest, strerror(errno));
			return;
		}
		x->devs++;
	} else {
		if (mkfifo(dest, 0600) == -1) {
			problem(x->c, "%s: %s", dest, strerror(errno));
			return;
		}
		x->pipes++;
	}
	if (chmod(dest, ip->mode & 0777) == -1)
		problem(x->c, "%s: %s", dest, strerror(errno));
	set_times(x->c, dest, ip);
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
	case MFS_S_IFCHR:
	case MFS_S_IFBLK:
	case MFS_S_IFIFO:
		extract_special(x, ip, src, dest);
		break;
	default:
		/* A socket makes no sense as a copy. */
		if (x->verbose)
			(void)printf("%s: socket not made\n", src);
		x->sockets++;
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
	while ((ch = getopt(argc, argv, "dv")) != -1) {
		switch (ch) {
		case 'd':
			x.devices = 1;
			break;
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
	if (x.devices && geteuid() != 0)
		errx(1, "extract -d makes devices, which takes root");

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
	(void)fprintf(stderr, "%lu files, %lu directories, %lu symbolic links,"
	    " %lu devices, %lu pipes\n", x.files, x.dirs, x.links, x.devs,
	    x.pipes);
	/* What is missing from the copy must not go unnoticed. */
	if (x.devs_skipped > 0)
		warnx("warning: %lu devices not made; make them with extract "
		    "-d as root, or keep them with \"minixfs tar\"",
		    x.devs_skipped);
	if (x.sockets > 0)
		warnx("warning: %lu sockets not made; a socket cannot be "
		    "copied", x.sockets);
	return c.status;
}

/* tar */

/*
 * A POSIX ustar archive on standard output.  Names longer than ustar holds
 * go into pax extended headers.  Files with more than one link are stored
 * once and then as hard links; sockets cannot be stored.
 */

#define TAR_BLOCK	512		/* the unit of an archive */
#define TAR_RECORD	10240		/* archives end on a whole record */
#define TAR_NAME	100		/* bytes of name and linkname */
#define TAR_PREFIX	155		/* bytes of prefix */

/* Where the first name of a file with more than one link went. */
struct tar_link {
	uint32_t	ino;		/* 0: a free slot */
	char		*name;
};

struct tar {
	struct walk	w;
	struct cmd	*c;
	struct tar_link	*links;		/* open addressing, by inode */
	size_t		nlinks;
	size_t		maxlinks;	/* a power of 2 */
	uint64_t	written;	/* bytes, for the last record */
	unsigned long	dirs;
	unsigned long	files;
	unsigned long	symlinks;
	unsigned long	hardlinks;
	unsigned long	special;
	unsigned long	skipped;
	int		failed;		/* standard output failed */
};

/* A ustar header, as it is laid out. */
struct tar_header {
	char	name[TAR_NAME];
	char	mode[8];
	char	uid[8];
	char	gid[8];
	char	size[12];
	char	mtime[12];
	char	chksum[8];
	char	typeflag;
	char	linkname[TAR_NAME];
	char	magic[6];
	char	version[2];
	char	uname[32];
	char	gname[32];
	char	devmajor[8];
	char	devminor[8];
	char	prefix[TAR_PREFIX];
	char	pad[12];
};

static void
tar_write(struct tar *t, const void *buf, size_t len)
{
	if (t->failed)
		return;
	if (fwrite(buf, 1, len, stdout) != len) {
		problem(t->c, "standard output: %s", strerror(errno));
		t->failed = 1;
		return;
	}
	t->written += len;
}

/* Zeros up to the next multiple of unit. */
static void
tar_pad(struct tar *t, size_t unit)
{
	static const char zero[TAR_BLOCK];
	size_t n;

	n = (unit - t->written % unit) % unit;
	while (n > 0) {
		tar_write(t, zero, n < sizeof(zero) ? n : sizeof(zero));
		n -= n < sizeof(zero) ? n : sizeof(zero);
	}
}

/* An octal number in a field of len bytes, NUL-terminated. */
static void
octal(char *field, size_t len, uint64_t v)
{
	char buf[24];

	(void)snprintf(buf, sizeof(buf), "%0*" PRIo64, (int)len - 1, v);
	(void)memcpy(field, buf, len - 1);
	field[len - 1] = '\0';
}

/*
 * Split name into prefix and name of a ustar header at a '/'.  Returns
 * -1 if it does not fit.
 */
static int
split_name(struct tar_header *h, const char *name)
{
	const char *p;
	size_t len;

	len = strlen(name);
	if (len <= TAR_NAME) {
		(void)memcpy(h->name, name, len);
		return 0;
	}
	for (p = name + len - TAR_NAME - 1; *p != '\0'; p++) {
		if (*p != '/' || p == name || p[1] == '\0')
			continue;
		if ((size_t)(p - name) > TAR_PREFIX)
			return -1;
		(void)memcpy(h->prefix, name, (size_t)(p - name));
		(void)memcpy(h->name, p + 1, len - (size_t)(p - name) - 1);
		return 0;
	}
	return -1;
}

/* One "LEN key=value\n" record of a pax header, into buf. */
static size_t
pax_record(char *buf, size_t max, const char *key, const char *value)
{
	size_t len, n;

	/* The length counts its own digits. */
	len = strlen(key) + strlen(value) + 3;
	for (n = 1; n < 20; n++) {
		if (snprintf(NULL, 0, "%zu", len + n) == (int)n)
			break;
	}
	len += n;
	if (len >= max)
		return 0;
	(void)snprintf(buf, max, "%zu %s=%s\n", len, key, value);
	return len;
}

static void
tar_header(struct tar *t, struct tar_header *h)
{
	unsigned char *p;
	unsigned sum;
	size_t i;

	(void)memcpy(h->magic, "ustar", 6);
	(void)memcpy(h->version, "00", 2);
	(void)memset(h->chksum, ' ', sizeof(h->chksum));
	sum = 0;
	p = (unsigned char *)h;
	for (i = 0; i < sizeof(*h); i++)
		sum += p[i];
	(void)snprintf(h->chksum, sizeof(h->chksum), "%06o", sum);
	h->chksum[7] = ' ';
	tar_write(t, h, sizeof(*h));
}

/*
 * Write the header of a member: name, and for a link or symbolic link,
 * target.  What does not fit goes into a pax header before it.
 */
static void
tar_member(struct tar *t, const struct mfs_inode *ip, const char *name,
    char type, const char *target, uint32_t size)
{
	struct tar_header h, x;
	char pax[2 * PATH_MAX + 64];
	size_t n;

	(void)memset(&h, 0, sizeof(h));
	n = 0;
	if (split_name(&h, name) == -1) {
		(void)memcpy(h.name, name, TAR_NAME);
		n += pax_record(pax + n, sizeof(pax) - n, "path", name);
	}
	if (target != NULL) {
		if (strlen(target) <= TAR_NAME)
			(void)memcpy(h.linkname, target, strlen(target));
		else
			n += pax_record(pax + n, sizeof(pax) - n, "linkpath",
			    target);
	}
	if (n > 0) {
		(void)memset(&x, 0, sizeof(x));
		(void)memcpy(x.name, "PaxHeader", 9);
		octal(x.mode, sizeof(x.mode), 0644);
		octal(x.uid, sizeof(x.uid), 0);
		octal(x.gid, sizeof(x.gid), 0);
		octal(x.size, sizeof(x.size), n);
		octal(x.mtime, sizeof(x.mtime), ip->mtime);
		x.typeflag = 'x';
		tar_header(t, &x);
		tar_write(t, pax, n);
		tar_pad(t, TAR_BLOCK);
	}
	octal(h.mode, sizeof(h.mode), ip->mode & 07777);
	octal(h.uid, sizeof(h.uid), ip->uid);
	octal(h.gid, sizeof(h.gid), ip->gid);
	octal(h.size, sizeof(h.size), size);
	octal(h.mtime, sizeof(h.mtime), ip->mtime);
	h.typeflag = type;
	if (type == '3' || type == '4') {
		octal(h.devmajor, sizeof(h.devmajor), mfs_rdev(ip) >> 8 & 0xff);
		octal(h.devminor, sizeof(h.devminor), mfs_rdev(ip) & 0xff);
	}
	tar_header(t, &h);
}

/*
 * The name under which inode ino went into the archive first, or NULL
 * after noting that it goes in now as name.
 */
static const char *
first_name(struct tar *t, uint32_t ino, const char *name)
{
	struct tar_link *old;
	size_t i, j, max;

	if (t->nlinks * 2 >= t->maxlinks) {
		old = t->links;
		max = t->maxlinks;
		t->maxlinks = max == 0 ? 64 : max * 2;
		t->links = calloc(t->maxlinks, sizeof(*t->links));
		if (t->links == NULL)
			err(1, NULL);
		for (i = 0; i < max; i++) {
			if (old[i].ino == 0)
				continue;
			j = old[i].ino & (t->maxlinks - 1);
			while (t->links[j].ino != 0)
				j = (j + 1) & (t->maxlinks - 1);
			t->links[j] = old[i];
		}
		free(old);
	}
	for (i = ino & (t->maxlinks - 1); t->links[i].ino != 0;
	    i = (i + 1) & (t->maxlinks - 1)) {
		if (t->links[i].ino == ino)
			return t->links[i].name;
	}
	t->links[i].ino = ino;
	if ((t->links[i].name = strdup(name)) == NULL)
		err(1, NULL);
	t->nlinks++;
	return NULL;
}

/* The contents of a regular file, padded to whole blocks. */
static void
tar_contents(struct tar *t, const struct mfs_inode *ip, const char *name)
{
	static unsigned char buf[COPY_SIZE];
	uint32_t off;
	ssize_t n;

	for (off = 0; off < ip->size; off += (uint32_t)n) {
		n = mfs_pread(&t->c->fs, ip, buf, sizeof(buf), off);
		if (n <= 0) {
			problem(t->c, "%s:%s: %s", t->c->image, name,
			    n == 0 ? "short file" : strerror((int)-n));
			/* The header gave the size: fill it with zeros. */
			(void)memset(buf, 0, sizeof(buf));
			for (; off < ip->size; off += (uint32_t)n) {
				n = (ssize_t)(ip->size - off < sizeof(buf) ?
				    ip->size - off : sizeof(buf));
				tar_write(t, buf, (size_t)n);
			}
			break;
		}
		tar_write(t, buf, (size_t)n);
	}
	tar_pad(t, TAR_BLOCK);
}

static void tar_dir(struct tar *, const struct mfs_inode *, const char *,
    const char *);

/* One member; src names it in the image and name in the archive. */
static void
tar_entry(struct tar *t, const struct mfs_inode *ip, const char *src,
    const char *name)
{
	char target[PATH_MAX], *dname;
	const char *first;
	ssize_t n;

	if (!mfs_is_dir(ip) && ip->nlinks > 1 &&
	    (first = first_name(t, ip->num, name)) != NULL) {
		tar_member(t, ip, name, '1', first, 0);
		t->hardlinks++;
		return;
	}
	switch (ip->mode & MFS_S_IFMT) {
	case MFS_S_IFDIR:
		if ((dname = join(t->c, name, "")) == NULL)
			return;
		tar_member(t, ip, dname, '5', NULL, 0);
		free(dname);
		t->dirs++;
		if (walk_enter(t->c, &t->w, ip->num, src) == 0) {
			tar_dir(t, ip, src, name);
			walk_leave(&t->w);
		}
		break;
	case MFS_S_IFREG:
		tar_member(t, ip, name, '0', NULL, ip->size);
		tar_contents(t, ip, src);
		t->files++;
		break;
	case MFS_S_IFLNK:
		if (ip->size >= sizeof(target)) {
			problem(t->c, "%s:%s: symbolic link too long",
			    t->c->image, src);
			return;
		}
		if ((n = mfs_pread(&t->c->fs, ip, target, ip->size, 0)) < 0) {
			problem(t->c, "%s:%s: %s", t->c->image, src,
			    strerror((int)-n));
			return;
		}
		target[n] = '\0';
		tar_member(t, ip, name, '2', target, 0);
		t->symlinks++;
		break;
	case MFS_S_IFCHR:
	case MFS_S_IFBLK:
	case MFS_S_IFIFO:
		tar_member(t, ip, name, mfs_is_dev(ip) ?
		    ((ip->mode & MFS_S_IFMT) == MFS_S_IFCHR ? '3' : '4') : '6',
		    NULL, 0);
		t->special++;
		break;
	default:
		t->skipped++;
		break;
	}
}

static void
tar_dir(struct tar *t, const struct mfs_inode *dp, const char *src,
    const char *name)
{
	struct mfs_inode ino;
	struct dirlist dl;
	size_t i;
	char *m, *s;

	if (read_dir(t->c, dp, src, &dl) == -1)
		return;
	for (i = 0; i < dl.n && !t->failed; i++) {
		if ((s = join(t->c, src, dl.ent[i].name)) == NULL)
			break;
		if (!safe_name(dl.ent[i].name)) {
			problem(t->c, "%s:%s: unsafe name, not archived",
			    t->c->image, s);
		} else if ((m = join(t->c, name, dl.ent[i].name)) != NULL) {
			if (entry_inode(t->c, &dl.ent[i], s, &ino) == 0)
				tar_entry(t, &ino, s, m);
			free(m);
		}
		free(s);
	}
	free(dl.ent);
}

static int
cmd_tar(int argc, char **argv)
{
	static const char zero[2 * TAR_BLOCK];
	struct mfs_inode ino;
	struct tar t;
	struct cmd c;
	const char *path;
	size_t i;

	if (argc < 2 || argc > 3)
		usage();
	path = argc == 3 ? argv[2] : "/";
	if (isatty(STDOUT_FILENO))
		errx(1, "standard output is a terminal; redirect it");

	(void)memset(&t, 0, sizeof(t));
	open_image(&c, argv[1]);
	t.c = &c;
	if (lookup(&c, path, &ino) == 0) {
		if (!mfs_is_dir(&ino))
			problem(&c, "%s:%s: %s", c.image, path,
			    strerror(ENOTDIR));
		else if (walk_enter(&c, &t.w, ino.num, path) == 0)
			tar_dir(&t, &ino, path, "");
	}
	tar_write(&t, zero, sizeof(zero));
	tar_pad(&t, TAR_RECORD);
	if (fflush(stdout) == EOF && !t.failed)
		problem(&c, "standard output: %s", strerror(errno));
	mfs_close(&c.fs);
	for (i = 0; i < t.maxlinks; i++)
		free(t.links[i].name);
	free(t.links);
	(void)fprintf(stderr, "%lu files, %lu directories, %lu symbolic links,"
	    " %lu hard links, %lu devices and pipes; %lu sockets skipped\n",
	    t.files, t.dirs, t.symlinks, t.hardlinks, t.special, t.skipped);
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
		{ "extract", cmd_extract },
		{ "tar", cmd_tar }
	};
	size_t i;

	/* -T comes before the command, whose options getopt() reads. */
	if (argc > 2 && strcmp(argv[1], "-T") == 0) {
		if (mfs_parse_tracks(argv[2], &tracks) < 0)
			usage();
		argc -= 2;
		argv += 2;
	}
	if (argc < 2)
		usage();
	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (strcmp(argv[1], cmds[i].name) == 0)
			return cmds[i].fn(argc - 1, argv + 1);
	usage();
	/* NOTREACHED */
	return 2;
}
