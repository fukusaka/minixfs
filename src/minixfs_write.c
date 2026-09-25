/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * minixfs_write.c - the commands of minixfs that change an image, as
 * mtools change an MS-DOS image, without mounting it:
 *
 *	minixfs [-M ...] [-f] put [-Rdin] [-o uid:gid] IMAGE HOST... PATH
 *	minixfs [-M ...] [-f] mkdir [-p] [-m mode] [-o uid:gid] IMAGE PATH...
 *	minixfs [-M ...] [-f] rm [-r] IMAGE PATH...
 *	minixfs [-M ...] [-f] mv IMAGE OLD NEW
 *	minixfs [-M ...] [-f] ln [-s] IMAGE TARGET PATH
 *	minixfs [-M ...] [-f] chmod IMAGE MODE PATH...
 *	minixfs [-M ...] [-f] chown IMAGE UID:GID PATH...
 *
 * put copies files of the host into the image, as extract copies them
 * out: into the directory PATH under their own names, or as PATH if that
 * is not a directory yet.  A name in the way is replaced, unless -n keeps
 * it or -i asks first; a directory in the way of a directory is entered
 * (-R copies directories).  Devices go in with -d only; sockets cannot,
 * and are counted in a warning at the end.  Regular files, symbolic links
 * and pipes keep their modes and mtimes.
 *
 * What is made belongs to the directory it is made in, as the owner of a
 * file in an image is a matter of the system that will use the image,
 * not of who runs this; -o gives another owner.  mkdir gives a directory
 * mode 0755 unless -m says otherwise.  chmod takes an octal mode, chown a
 * numeric owner, group, or both.
 *
 * The image is locked while it is open, as mount_minixfs -w locks it, and
 * its clean mark is away until the command is done; it stays away if a
 * write failed in a way that may have left the image out of order, such
 * as an error of the device (mfs_failure_breaks()).  A file system that
 * is not marked clean may be mounted elsewhere, so it is refused unless
 * -f is given; so are the flex directories of Minix-vmd, which cannot be
 * written.  The bit maps go to the image at the end.
 *
 * Each command reports every problem it meets and goes on where it can.
 * The exit status is 0 on success, 1 if anything failed and 2 for a
 * usage error.
 */

#include "compat.h"

#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "minixfs.h"

#define COPY_SIZE	65536		/* bytes copied at a time */

/* -o uid:gid */
struct owner {
	int		set;
	uint32_t	uid;
	uint32_t	gid;
};

/*
 * Whether the image was marked clean when it was opened, and the state
 * of its super block then, which -f gives back to one that was not.
 */
static int was_clean;
static uint16_t state_before;

/* The options of put. */
struct put {
	struct cmd	*c;
	struct owner	o;
	int		recurse;	/* -R */
	int		devices;	/* -d */
	int		ask;		/* -i */
	int		keep;		/* -n */
	unsigned long	left_out;	/* devices without -d, and sockets */
};

/* The time now, as an inode holds it. */
static uint32_t
now(void)
{
	int64_t t;

	if ((t = compat_now()) < 0)
		return 0;
	return t > UINT32_MAX ? UINT32_MAX : (uint32_t)t;
}

/* A time of the host as an inode holds it. */
static uint32_t
time32(time_t t)
{
	if (t < 0)
		return 0;
	return (uint64_t)t > UINT32_MAX ? UINT32_MAX : (uint32_t)t;
}

/* The largest gid an inode of the image holds. */
static uint32_t
max_gid(const struct cmd *c)
{
	return c->fs.version == 1 ? MFS_MAX_GID_V1 : MFS_MAX_GID;
}

/* -o: two numbers with a colon between; a usage error otherwise. */
static void
parse_owner(const char *s, struct owner *o)
{
	unsigned long uid, gid;
	char *end;

	errno = 0;
	uid = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != ':' || s[0] < '0' || s[0] > '9')
		errx(2, "%s: give the owner as uid:gid", s);
	s = end + 1;
	gid = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || s[0] < '0' || s[0] > '9')
		errx(2, "%s: give the owner as uid:gid", s);
	if (uid > MFS_MAX_UID || gid > MFS_MAX_GID)
		errx(2, "%lu:%lu: bad owner", uid, gid);
	o->set = 1;
	o->uid = (uint32_t)uid;
	o->gid = (uint32_t)gid;
}

/* Whether an owner fits the inodes of the open image; say so if not. */
static int
owner_fits(struct cmd *c, uint32_t uid, uint32_t gid)
{
	if (uid <= MFS_MAX_UID && gid <= max_gid(c))
		return 1;
	problem(c, "%s: owner %" PRIu32 ":%" PRIu32 " does not fit in V%d",
	    c->image, uid, gid, c->fs.version);
	return 0;
}

/*
 * Open the image for writing, or report why not and exit: it is locked
 * by another program, it has the flex directories of Minix-vmd, or it is
 * not marked clean and -f was not given.  The clean mark then goes until
 * finish_writing().
 */
static void
open_for_writing(struct cmd *c, const char *image)
{
	int r;

	c->image = image;
	c->status = 0;
	c->broken = 0;
	if ((r = mfs_open_tracks(&c->fs, image, 1, &tracks)) < 0) {
		if (r == -EINVAL)
			errx(1, "%s: not a MINIX file system", image);
		errx(1, "%s: %s", image, strerror(-r));
	}
	if (c->fs.flex)
		errx(1, "%s: the flex directories of Minix-vmd cannot be "
		    "written", image);
	was_clean = mfs_is_clean(&c->fs);
	state_before = c->fs.state;
	if (!force && !was_clean)
		errx(1, "%s: not marked clean, so it may be mounted; unmount "
		    "it and run fsck_minixfs -y, or give -f", image);
	if ((r = mfs_mark_in_use(&c->fs)) < 0)
		errx(1, "%s: %s", image, strerror(-r));
}

/*
 * Write the bit maps, put the clean mark back if the image had it, or
 * else the state it had, and close the image.  A write that may have
 * left the image out of order leaves the mark away.  Returns the exit
 * status of the command.
 */
static int
finish_writing(struct cmd *c)
{
	int r;

	/* The mark comes back once the rest is on the disk. */
	if ((r = mfs_sync(&c->fs)) == 0 && fsync(c->fs.fd) == -1)
		r = -errno;
	if (r < 0) {
		problem(c, "%s: %s", c->image, strerror(-r));
		c->broken = 1;
	}
	if (c->broken) {
		warnx("warning: %s is left marked not clean; check it with "
		    "fsck_minixfs -y", c->image);
	} else {
		if (was_clean) {
			r = mfs_mark_clean(&c->fs, 1);
		} else {
			c->fs.state = state_before;
			r = mfs_put_super(&c->fs);
		}
		if (r == 0 && fsync(c->fs.fd) == -1)
			r = -errno;
		if (r < 0)
			problem(c, "%s: %s", c->image, strerror(-r));
	}
	mfs_close(&c->fs);
	return c->status;
}

/*
 * Where the last component of path starts, and in *len its length, the
 * slashes after it left out; NULL for "/", "." or "..".
 */
static const char *
last_part(const char *path, size_t *len)
{
	const char *end, *p;

	end = path + strlen(path);
	while (end > path && end[-1] == '/')
		end--;
	p = end;
	while (p > path && p[-1] != '/')
		p--;
	*len = (size_t)(end - p);
	if (*len == 0 || (*len <= 2 && strncmp(p, "..", *len) == 0))
		return NULL;
	return p;
}

/*
 * The last component of a path of the host, without the slashes after
 * it, in buf of PATH_MAX bytes; NULL for "/", "." or "..".
 */
static const char *
base_name(const char *path, char *buf)
{
	const char *p;
	size_t len;

	if ((p = last_part(path, &len)) == NULL || len >= PATH_MAX)
		return NULL;
	(void)memcpy(buf, p, len);
	buf[len] = '\0';
	return buf;
}

/*
 * The directory that holds path in the image, and the last component of
 * path in name, which holds MFS_MAX_NAME + 1 bytes.  Trailing slashes
 * are dropped; the root has no name.  Returns 0 or a negative errno value.
 */
static int
split_path(struct cmd *c, const char *path, uint32_t *dir, char *name)
{
	struct mfs_inode dp;
	char buf[PATH_MAX];
	const char *base;
	size_t len;
	int r;

	if (strlen(path) >= sizeof(buf))
		return -ENAMETOOLONG;
	if ((base = last_part(path, &len)) == NULL)
		return -EINVAL;
	if (len > MFS_MAX_NAME)
		return -ENAMETOOLONG;
	(void)memcpy(name, base, len);
	name[len] = '\0';
	(void)memcpy(buf, path, (size_t)(base - path));
	buf[base - path] = '\0';
	if ((r = mfs_namei(&c->fs, buf[0] == '\0' ? "/" : buf, &dp)) < 0)
		return r;
	if (!mfs_is_dir(&dp))
		return -ENOTDIR;
	*dir = dp.num;
	return 0;
}

/* The inode of name in directory dir, if there is one. */
static int
entry_in(struct cmd *c, uint32_t dir, const char *name, struct mfs_inode *ip)
{
	struct mfs_inode dp;
	uint32_t ino;
	int r;

	if ((r = mfs_read_inode(&c->fs, dir, &dp)) < 0 ||
	    (r = mfs_lookup(&c->fs, &dp, name, &ino)) < 0 ||
	    (r = mfs_read_inode(&c->fs, ino, ip)) < 0)
		return r;
	return 0;
}

/*
 * What a new file in directory dir gets: the owner of the directory, or
 * that of -o, and the mode and time given.
 */
static int
new_in(struct cmd *c, uint32_t dir, const struct owner *o, uint16_t mode,
    uint32_t time, struct mfs_new *n)
{
	struct mfs_inode dp;
	int r;

	(void)memset(n, 0, sizeof(*n));
	if ((r = mfs_read_inode(&c->fs, dir, &dp)) < 0)
		return r;
	n->uid = (uint16_t)(o->set ? o->uid : dp.uid);
	n->gid = (uint16_t)(o->set ? o->gid : dp.gid);
	n->mode = mode;
	n->time = time;
	return 0;
}

/* Report a failure on a path of the image. */
static int
failed(struct cmd *c, const char *path, int e)
{
	problem(c, "%s:%s: %s", c->image, path, strerror(-e));
	if (mfs_failure_breaks(e))
		c->broken = 1;
	return -1;
}

/*
 * put
 */

/*
 * Get a name out of the way of what put brings, if the caller does not
 * want to keep it.  Returns 0 with *ip the entry left in place (a
 * directory for a directory, or what -n and -i keep, with *skip set),
 * 0 with ip->num 0 if the way is clear, or -1 after reporting: a
 * directory in the way of a file, or a file in the way of a directory.
 */
static int
clear_way(struct put *p, uint32_t dir, const char *name, const char *path,
    int isdir, struct mfs_inode *ip, int *skip)
{
	char line[16];
	int ch, r;

	*skip = 0;
	if ((r = entry_in(p->c, dir, name, ip)) == -ENOENT) {
		ip->num = 0;
		return 0;
	}
	if (r < 0)
		return failed(p->c, path, r);
	if (mfs_is_dir(ip) && isdir)
		return 0;
	if (mfs_is_dir(ip))
		return failed(p->c, path, -EISDIR);
	if (isdir)
		return failed(p->c, path, -EEXIST);
	if (p->keep) {
		*skip = 1;
		return 0;
	}
	if (p->ask) {
		(void)fprintf(stderr, "overwrite %s:%s? ", p->c->image, path);
		if (fgets(line, sizeof(line), stdin) == NULL)
			line[0] = '\0';
		else if (strchr(line, '\n') == NULL)
			/* The rest of the answer is no answer to the next. */
			while ((ch = getchar()) != EOF && ch != '\n')
				continue;
		if (line[0] != 'y' && line[0] != 'Y') {
			*skip = 1;
			return 0;
		}
	}
	if ((r = mfs_unlink(&p->c->fs, dir, name, now())) < 0)
		return failed(p->c, path, r);
	ip->num = 0;
	return 0;
}

/* Copy the contents of the host file into the new file *ip. */
static int
copy_in(struct put *p, const char *host, struct mfs_inode *ip)
{
	unsigned char *buf;
	ssize_t n;
	uint32_t off;
	int fd, r;

	if ((fd = open(host, O_RDONLY)) == -1) {
		problem(p->c, "%s: %s", host, strerror(errno));
		return -1;
	}
	if ((buf = malloc(COPY_SIZE)) == NULL) {
		problem(p->c, "%s", strerror(ENOMEM));
		(void)close(fd);
		return -1;
	}
	r = 0;
	for (off = 0; (n = read(fd, buf, COPY_SIZE)) > 0; off += (uint32_t)n) {
		if ((uint64_t)off + (uint64_t)n > UINT32_MAX) {
			r = -EFBIG;
			break;
		}
		if ((r = mfs_pwrite(&p->c->fs, ip, buf, (size_t)n, off)) < 0)
			break;
	}
	if (n == -1) {
		problem(p->c, "%s: %s", host, strerror(errno));
		r = -1;
	} else if (r < 0) {
		problem(p->c, "%s: %s", host, strerror(-r));
		if (mfs_failure_breaks(r))
			p->c->broken = 1;
		r = -1;
	}
	free(buf);
	(void)close(fd);
	return r;
}

/* A regular file of the host, with its mode and mtime. */
static int
put_file(struct put *p, const char *host, const struct stat *st,
    uint32_t dir, const char *name, const char *path)
{
	struct mfs_inode ip;
	struct mfs_new n;
	int r, skip;

	if (clear_way(p, dir, name, path, 0, &ip, &skip) == -1 || skip)
		return skip ? 0 : -1;
	if ((r = new_in(p->c, dir, &p->o, (uint16_t)(MFS_S_IFREG |
	    (st->st_mode & 07777)), now(), &n)) < 0 ||
	    (r = mfs_make(&p->c->fs, dir, name, &n, &ip)) < 0)
		return failed(p->c, path, r);
	if (copy_in(p, host, &ip) == -1) {
		/* Nothing half copied stays. */
		(void)mfs_unlink(&p->c->fs, dir, name, now());
		return -1;
	}
	ip.mtime = time32(st->st_mtime);
	if ((r = mfs_put_inode(&p->c->fs, &ip)) < 0)
		return failed(p->c, path, r);
	return 0;
}

/* A symbolic link of the host. */
static int
put_link(struct put *p, const char *host, const struct stat *st,
    uint32_t dir, const char *name, const char *path)
{
	char target[PATH_MAX];
	struct mfs_inode ip;
	struct mfs_new n;
	ssize_t len;
	int r, skip;

	if ((len = readlink(host, target, sizeof(target) - 1)) == -1) {
		problem(p->c, "%s: %s", host, strerror(errno));
		return -1;
	}
	target[len] = '\0';
	if (clear_way(p, dir, name, path, 0, &ip, &skip) == -1 || skip)
		return skip ? 0 : -1;
	if ((r = new_in(p->c, dir, &p->o, 0, now(), &n)) < 0 ||
	    (r = mfs_symlink(&p->c->fs, dir, name, target, &n, &ip)) < 0)
		return failed(p->c, path, r);
	ip.mtime = time32(st->st_mtime);
	if ((r = mfs_put_inode(&p->c->fs, &ip)) < 0)
		return failed(p->c, path, r);
	return 0;
}

/* A pipe or, with -d, a device of the host. */
static int
put_node(struct put *p, const struct stat *st, uint32_t dir,
    const char *name, const char *path)
{
	struct mfs_inode ip;
	struct mfs_new n;
	uint16_t mode;
	int r, skip;

	mode = (uint16_t)(st->st_mode & 07777);
	if (S_ISFIFO(st->st_mode)) {
		mode |= MFS_S_IFIFO;
	} else {
		mode |= S_ISCHR(st->st_mode) ? MFS_S_IFCHR : MFS_S_IFBLK;
		if (major(st->st_rdev) > MFS_MAX_DEV_PART ||
		    minor(st->st_rdev) > MFS_MAX_DEV_PART) {
			problem(p->c, "%s: device numbers %u,%u do not fit",
			    path, (unsigned)major(st->st_rdev),
			    (unsigned)minor(st->st_rdev));
			return -1;
		}
	}
	if (clear_way(p, dir, name, path, 0, &ip, &skip) == -1 || skip)
		return skip ? 0 : -1;
	if ((r = new_in(p->c, dir, &p->o, mode, time32(st->st_mtime),
	    &n)) < 0)
		return failed(p->c, path, r);
	if (!S_ISFIFO(st->st_mode))
		n.rdev = (uint32_t)(major(st->st_rdev) << 8 |
		    minor(st->st_rdev));
	if ((r = mfs_make(&p->c->fs, dir, name, &n, &ip)) < 0)
		return failed(p->c, path, r);
	return 0;
}

static int put_one(struct put *, const char *, uint32_t, const char *,
    const char *);

/*
 * A directory of the host, with -R: made unless one is in the way, then
 * entered.  What is inside belongs to it.
 */
static int
put_dir(struct put *p, const char *host, const struct stat *st,
    uint32_t dir, const char *name, const char *path)
{
	struct mfs_inode ip;
	struct mfs_new n;
	struct dirent *de;
	char *hostent, *pathent;
	DIR *dp;
	int r, rv, skip;

	if (!p->recurse) {
		problem(p->c, "%s: is a directory; give -R to copy it", host);
		return -1;
	}
	if (clear_way(p, dir, name, path, 1, &ip, &skip) == -1 || skip)
		return skip ? 0 : -1;
	if (ip.num == 0) {
		if ((r = new_in(p->c, dir, &p->o, (uint16_t)(MFS_S_IFDIR |
		    (st->st_mode & 07777)), now(), &n)) < 0 ||
		    (r = mfs_make(&p->c->fs, dir, name, &n, &ip)) < 0)
			return failed(p->c, path, r);
	}
	if ((dp = opendir(host)) == NULL) {
		problem(p->c, "%s: %s", host, strerror(errno));
		return -1;
	}
	rv = 0;
	while ((de = readdir(dp)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		hostent = join(p->c, host, de->d_name);
		pathent = join(p->c, path, de->d_name);
		if (hostent == NULL || pathent == NULL ||
		    put_one(p, hostent, ip.num, de->d_name, pathent) == -1)
			rv = -1;
		free(hostent);
		free(pathent);
	}
	(void)closedir(dp);
	/* The directory keeps the mtime of the host once it is filled. */
	if ((r = mfs_read_inode(&p->c->fs, ip.num, &ip)) < 0)
		return failed(p->c, path, r);
	ip.mtime = time32(st->st_mtime);
	if ((r = mfs_put_inode(&p->c->fs, &ip)) < 0)
		return failed(p->c, path, r);
	return rv;
}

/* One thing of the host into directory dir of the image, as name. */
static int
put_one(struct put *p, const char *host, uint32_t dir, const char *name,
    const char *path)
{
	struct stat st;

	if (lstat(host, &st) == -1) {
		problem(p->c, "%s: %s", host, strerror(errno));
		return -1;
	}
	if (strlen(name) > MFS_MAX_NAME)
		return failed(p->c, path, -ENAMETOOLONG);
	if (S_ISREG(st.st_mode))
		return put_file(p, host, &st, dir, name, path);
	if (S_ISDIR(st.st_mode))
		return put_dir(p, host, &st, dir, name, path);
	if (S_ISLNK(st.st_mode))
		return put_link(p, host, &st, dir, name, path);
	if (S_ISFIFO(st.st_mode) ||
	    (p->devices && (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))))
		return put_node(p, &st, dir, name, path);
	p->left_out++;
	return 0;
}

int
cmd_put(int argc, char **argv)
{
	char buf[PATH_MAX], name[MFS_MAX_NAME + 1];
	struct mfs_inode dp;
	struct cmd c;
	struct put p;
	const char *base, *dest;
	char *path;
	uint32_t dir;
	int ch, i, r;

	(void)memset(&p, 0, sizeof(p));
	p.c = &c;
	optind = 1;
	while ((ch = getopt(argc, argv, "Rdino:")) != -1) {
		switch (ch) {
		case 'R':
			p.recurse = 1;
			break;
		case 'd':
			p.devices = 1;
			break;
		case 'i':
			p.ask = 1;
			break;
		case 'n':
			p.keep = 1;
			break;
		case 'o':
			parse_owner(optarg, &p.o);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 3)
		usage();
	dest = argv[argc - 1];

	open_for_writing(&c, argv[0]);
	if (p.o.set && !owner_fits(&c, p.o.uid, p.o.gid))
		return finish_writing(&c);
	/* Into the directory dest under their own names, or as dest. */
	if (mfs_namei(&c.fs, dest, &dp) == 0 && mfs_is_dir(&dp)) {
		for (i = 1; i < argc - 1; i++) {
			if ((base = base_name(argv[i], buf)) == NULL) {
				problem(&c, "%s: give the name it gets as the "
				    "last argument", argv[i]);
				continue;
			}
			if ((path = join(&c, dest, base)) == NULL)
				continue;
			(void)put_one(&p, argv[i], dp.num, base, path);
			free(path);
		}
	} else if (argc > 3) {
		problem(&c, "%s:%s: not a directory", c.image, dest);
	} else if ((r = split_path(&c, dest, &dir, name)) < 0) {
		(void)failed(&c, dest, r);
	} else {
		(void)put_one(&p, argv[1], dir, name, dest);
	}
	if (p.left_out > 0)
		warnx("warning: %lu devices or sockets left out%s",
		    p.left_out, p.devices ? "" : "; devices go in with -d");
	return finish_writing(&c);
}

/*
 * mkdir
 */

/* The number of a component of a path, making it if it is missing. */
static int
make_component(struct cmd *c, uint32_t dir, const char *name,
    const char *path, const struct owner *o, uint16_t mode, uint32_t *ino)
{
	struct mfs_inode ip;
	struct mfs_new n;
	int r;

	if ((r = entry_in(c, dir, name, &ip)) == 0) {
		if (!mfs_is_dir(&ip))
			return failed(c, path, -ENOTDIR);
		*ino = ip.num;
		return 0;
	}
	if (r != -ENOENT)
		return failed(c, path, r);
	if ((r = new_in(c, dir, o, (uint16_t)(MFS_S_IFDIR | mode), now(),
	    &n)) < 0 || (r = mfs_make(&c->fs, dir, name, &n, &ip)) < 0)
		return failed(c, path, r);
	*ino = ip.num;
	return 0;
}

/*
 * mkdir -p: each missing directory on the way, with mode 0755, and the
 * last with the mode given.
 */
static int
make_path(struct cmd *c, const char *path, const struct owner *o,
    uint16_t mode)
{
	char buf[PATH_MAX], sofar[PATH_MAX];
	char *p, *q, *r;
	uint32_t dir;

	if (strlen(path) >= sizeof(buf))
		return failed(c, path, -ENAMETOOLONG);
	(void)strcpy(buf, path);
	sofar[0] = '\0';
	dir = MFS_ROOT_INO;
	for (p = buf; *p != '\0'; p = q) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		for (q = p; *q != '\0' && *q != '/'; q++)
			continue;
		if (*q == '/')
			*q++ = '\0';
		if (strlen(p) > MFS_MAX_NAME)
			return failed(c, path, -ENAMETOOLONG);
		(void)snprintf(sofar + strlen(sofar),
		    sizeof(sofar) - strlen(sofar), "/%s", p);
		/* The last component may have more than one slash after it. */
		for (r = q; *r == '/'; r++)
			continue;
		if (make_component(c, dir, p, sofar, o,
		    *r == '\0' ? mode : 0755, &dir) == -1)
			return -1;
	}
	return 0;
}

int
cmd_mkdir(int argc, char **argv)
{
	char name[MFS_MAX_NAME + 1];
	struct mfs_inode ip;
	struct mfs_new n;
	struct owner o;
	struct cmd c;
	unsigned long m;
	uint32_t dir;
	uint16_t mode;
	char *end;
	int ch, i, parents, r;

	(void)memset(&o, 0, sizeof(o));
	mode = 0755;
	parents = 0;
	optind = 1;
	while ((ch = getopt(argc, argv, "m:o:p")) != -1) {
		switch (ch) {
		case 'm':
			errno = 0;
			m = strtoul(optarg, &end, 8);
			if (errno != 0 || *end != '\0' || end == optarg ||
			    m > 07777)
				errx(2, "%s: bad mode", optarg);
			mode = (uint16_t)m;
			break;
		case 'o':
			parse_owner(optarg, &o);
			break;
		case 'p':
			parents = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 2)
		usage();

	open_for_writing(&c, argv[0]);
	if (o.set && !owner_fits(&c, o.uid, o.gid))
		return finish_writing(&c);
	for (i = 1; i < argc; i++) {
		if (parents) {
			(void)make_path(&c, argv[i], &o, mode);
			continue;
		}
		if ((r = split_path(&c, argv[i], &dir, name)) < 0 ||
		    (r = new_in(&c, dir, &o, (uint16_t)(MFS_S_IFDIR | mode),
		    now(), &n)) < 0 ||
		    (r = mfs_make(&c.fs, dir, name, &n, &ip)) < 0)
			(void)failed(&c, argv[i], r);
	}
	return finish_writing(&c);
}

/*
 * rm
 */

/* Remove everything in directory *dp, at path. */
static int
remove_tree(struct cmd *c, struct walk *w, const struct mfs_inode *dp,
    const char *path)
{
	struct mfs_inode ip;
	struct dirlist dl;
	char *sub;
	size_t i;
	int r, rv;

	if (read_dir(c, dp, path, &dl) == -1)
		return -1;
	rv = 0;
	for (i = 0; i < dl.n; i++) {
		if ((sub = join(c, path, dl.ent[i].name)) == NULL ||
		    entry_inode(c, &dl.ent[i], sub, &ip) == -1) {
			free(sub);
			rv = -1;
			continue;
		}
		if (mfs_is_dir(&ip)) {
			if (walk_enter(c, w, ip.num, sub) == 0) {
				if (remove_tree(c, w, &ip, sub) == -1)
					rv = -1;
				walk_leave(w);
			}
			r = mfs_rmdir(&c->fs, dp->num, dl.ent[i].name, now());
		} else {
			r = mfs_unlink(&c->fs, dp->num, dl.ent[i].name, now());
		}
		if (r < 0)
			rv = failed(c, sub, r);
		free(sub);
	}
	free(dl.ent);
	return rv;
}

int
cmd_rm(int argc, char **argv)
{
	char name[MFS_MAX_NAME + 1];
	struct mfs_inode ip;
	struct walk w;
	struct cmd c;
	uint32_t dir;
	int ch, i, r, recurse;

	recurse = 0;
	optind = 1;
	while ((ch = getopt(argc, argv, "r")) != -1) {
		if (ch != 'r')
			usage();
		recurse = 1;
	}
	argc -= optind;
	argv += optind;
	if (argc < 2)
		usage();

	open_for_writing(&c, argv[0]);
	for (i = 1; i < argc; i++) {
		if ((r = split_path(&c, argv[i], &dir, name)) < 0 ||
		    (r = entry_in(&c, dir, name, &ip)) < 0) {
			(void)failed(&c, argv[i], r);
			continue;
		}
		if (!mfs_is_dir(&ip)) {
			r = mfs_unlink(&c.fs, dir, name, now());
		} else if (!recurse) {
			r = -EISDIR;
		} else {
			w.depth = 0;
			if (walk_enter(&c, &w, ip.num, argv[i]) == -1)
				continue;
			(void)remove_tree(&c, &w, &ip, argv[i]);
			walk_leave(&w);
			r = mfs_rmdir(&c.fs, dir, name, now());
		}
		if (r < 0)
			(void)failed(&c, argv[i], r);
	}
	return finish_writing(&c);
}

/*
 * mv and ln
 */

/*
 * Where a new name goes: into the directory dest under base, if dest is
 * a directory, or as dest itself.  Returns 0 or a negative errno value.
 */
static int
place(struct cmd *c, const char *dest, const char *base, uint32_t *dir,
    char *name)
{
	struct mfs_inode dp;

	if (mfs_namei(&c->fs, dest, &dp) == 0 && mfs_is_dir(&dp)) {
		if (base == NULL)
			return -EINVAL;
		if (strlen(base) > MFS_MAX_NAME)
			return -ENAMETOOLONG;
		(void)strcpy(name, base);
		*dir = dp.num;
		return 0;
	}
	return split_path(c, dest, dir, name);
}

int
cmd_mv(int argc, char **argv)
{
	char oname[MFS_MAX_NAME + 1], nname[MFS_MAX_NAME + 1];
	struct cmd c;
	uint32_t ndir, odir;
	int r;

	if (argc != 4)
		usage();
	open_for_writing(&c, argv[1]);
	if ((r = split_path(&c, argv[2], &odir, oname)) < 0)
		(void)failed(&c, argv[2], r);
	else if ((r = place(&c, argv[3], oname, &ndir, nname)) < 0)
		(void)failed(&c, argv[3], r);
	else if ((r = mfs_rename(&c.fs, odir, oname, ndir, nname, 0,
	    now())) < 0)
		(void)failed(&c, argv[2], r);
	return finish_writing(&c);
}

int
cmd_ln(int argc, char **argv)
{
	char buf[PATH_MAX], name[MFS_MAX_NAME + 1];
	struct mfs_inode ip;
	struct mfs_new n;
	struct owner o;
	struct cmd c;
	uint32_t dir;
	int ch, r, symbolic;

	(void)memset(&o, 0, sizeof(o));
	symbolic = 0;
	optind = 1;
	while ((ch = getopt(argc, argv, "s")) != -1) {
		if (ch != 's')
			usage();
		symbolic = 1;
	}
	argc -= optind;
	argv += optind;
	if (argc != 3)
		usage();

	open_for_writing(&c, argv[0]);
	if ((r = place(&c, argv[2], base_name(argv[1], buf), &dir,
	    name)) < 0) {
		(void)failed(&c, argv[2], r);
	} else if (symbolic) {
		if ((r = new_in(&c, dir, &o, 0, now(), &n)) < 0 ||
		    (r = mfs_symlink(&c.fs, dir, name, argv[1], &n, &ip)) < 0)
			(void)failed(&c, argv[2], r);
	} else {
		if ((r = mfs_namei(&c.fs, argv[1], &ip)) < 0)
			(void)failed(&c, argv[1], r);
		else if ((r = mfs_link(&c.fs, ip.num, dir, name, now())) < 0)
			(void)failed(&c, argv[2], r);
	}
	return finish_writing(&c);
}

/*
 * chmod and chown
 */

int
cmd_chmod(int argc, char **argv)
{
	struct mfs_inode ip;
	struct cmd c;
	unsigned long m;
	char *end;
	int i, r;

	if (argc < 4)
		usage();
	errno = 0;
	m = strtoul(argv[2], &end, 8);
	if (errno != 0 || *end != '\0' || end == argv[2] || m > 07777)
		errx(2, "%s: give the mode in octal", argv[2]);
	open_for_writing(&c, argv[1]);
	for (i = 3; i < argc; i++) {
		if (lookup(&c, argv[i], &ip) == -1)
			continue;
		ip.mode = (uint16_t)((ip.mode & MFS_S_IFMT) | m);
		ip.ctime = now();
		if ((r = mfs_put_inode(&c.fs, &ip)) < 0)
			(void)failed(&c, argv[i], r);
	}
	return finish_writing(&c);
}

/*
 * UID, UID:GID or :GID, as chown(1) takes them; what is not given is
 * kept.  Numbers only: the image knows no names.
 */
static void
parse_change(const char *s, long *uid, long *gid)
{
	unsigned long v;
	char *end;

	*uid = *gid = -1;
	if (*s != ':') {
		errno = 0;
		v = strtoul(s, &end, 10);
		if (errno != 0 || end == s || (*end != '\0' && *end != ':') ||
		    *s < '0' || *s > '9' || v > MFS_MAX_UID)
			errx(2, "%s: give the owner as uid, uid:gid or :gid", s);
		*uid = (long)v;
		s = end;
	}
	if (*s == ':') {
		s++;
		errno = 0;
		v = strtoul(s, &end, 10);
		if (errno != 0 || end == s || *end != '\0' || *s < '0' ||
		    *s > '9' || v > MFS_MAX_GID)
			errx(2, "%s: give the owner as uid, uid:gid or :gid", s);
		*gid = (long)v;
	}
}

int
cmd_chown(int argc, char **argv)
{
	struct mfs_inode ip;
	struct cmd c;
	long gid, uid;
	int i, r;

	if (argc < 4)
		usage();
	parse_change(argv[2], &uid, &gid);
	open_for_writing(&c, argv[1]);
	if (gid > (long)max_gid(&c)) {
		problem(&c, "%s: group %ld does not fit in V%d", c.image, gid,
		    c.fs.version);
		return finish_writing(&c);
	}
	for (i = 3; i < argc; i++) {
		if (lookup(&c, argv[i], &ip) == -1)
			continue;
		if (uid >= 0)
			ip.uid = (uint16_t)uid;
		if (gid >= 0)
			ip.gid = (uint16_t)gid;
		ip.ctime = now();
		if ((r = mfs_put_inode(&c.fs, &ip)) < 0)
			(void)failed(&c, argv[i], r);
	}
	return finish_writing(&c);
}
