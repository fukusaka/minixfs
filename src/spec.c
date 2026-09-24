/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * spec.c - read an mtree(8) specification, as makefs -F reads it.
 *
 * Both forms are read: relative names below the current directory, with
 * ".." to go up, and full paths such as those of the METALOG that a
 * build writes.  "/set" and "/unset" give and take away defaults, "#"
 * starts a comment, a backslash at the end of a line joins the next, and
 * names and link targets may use the escapes of vis(3): "\ooo" in octal,
 * "\s", "\t", "\n", "\\" and "\#".  Of the keywords, type, mode (octal),
 * uid, uname, gid, gname, time, link, device and optional are used; the
 * checksums, size, flags, tags and the like are accepted and ignored.
 * Names with the patterns of mtree(8) are not supported.
 */

#include "compat.h"

#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfs.h"
#include "spec.h"

#define MAX_LINE	65536		/* joined lines included */

/* The state of reading one file. */
struct reader {
	struct spec	*spec;
	struct spec_entry glob;		/* /set */
	const char	*file;
	const char	*dbdir;
	char		*cur;		/* directory of relative names */
	size_t		max;		/* room in spec->e */
	size_t		*hash;		/* index + 1 of each path, or 0 */
	size_t		hmax;		/* a power of 2 */
	unsigned long	line;
	int		errors;
};

static void
error(struct reader *rd, const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "%s:%lu: ", rd->file, rd->line);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	rd->errors++;
}

static char *
xstrdup(const char *s)
{
	char *p;

	if ((p = strdup(s)) == NULL) {
		perror(NULL);
		exit(1);
	}
	return p;
}

/* Undo the escapes of vis(3) in s, in place; -1 for a bad one. */
static int
unvis(char *s)
{
	char *d;
	int i, v;

	for (d = s; *s != '\0'; s++) {
		if (*s != '\\') {
			*d++ = *s;
			continue;
		}
		s++;
		if (*s >= '0' && *s <= '7') {
			for (i = 0, v = 0; i < 3; i++, s++) {
				if (*s < '0' || *s > '7')
					return -1;
				v = v * 8 + (*s - '0');
			}
			s--;
			*d++ = (char)v;
			continue;
		}
		switch (*s) {
		case 's':
			*d++ = ' ';
			break;
		case 't':
			*d++ = '\t';
			break;
		case 'n':
			*d++ = '\n';
			break;
		case '\\':
		case '#':
		case '*':
		case '?':
		case '[':
			*d++ = *s;
			break;
		default:
			return -1;
		}
	}
	*d = '\0';
	return 0;
}

/* A user or group of the file name in dbdir: its number, or -1. */
static long
db_lookup(const char *dbdir, const char *file, const char *name)
{
	char path[PATH_MAX], buf[1024], *colon, *num;
	long id;
	FILE *f;

	(void)snprintf(path, sizeof(path), "%s/%s", dbdir, file);
	if ((f = fopen(path, "r")) == NULL)
		return -1;
	id = -1;
	while (fgets(buf, sizeof(buf), f) != NULL) {
		if ((colon = strchr(buf, ':')) == NULL)
			continue;
		*colon = '\0';
		if (strcmp(buf, name) != 0)
			continue;
		/* name:password:number: */
		if ((num = strchr(colon + 1, ':')) != NULL)
			id = strtol(num + 1, NULL, 10);
		break;
	}
	(void)fclose(f);
	return id;
}

static long
user_id(const struct reader *rd, const char *name)
{
	struct passwd *pw;
	long id;

	if (rd->dbdir != NULL) {
		if ((id = db_lookup(rd->dbdir, "master.passwd", name)) >= 0)
			return id;
		return db_lookup(rd->dbdir, "passwd", name);
	}
	return (pw = getpwnam(name)) != NULL ? (long)pw->pw_uid : -1;
}

static long
group_id(const struct reader *rd, const char *name)
{
	struct group *gr;

	if (rd->dbdir != NULL)
		return db_lookup(rd->dbdir, "group", name);
	return (gr = getgrnam(name)) != NULL ? (long)gr->gr_gid : -1;
}

/* A decimal number up to max, or -1. */
static long
number(const char *s, unsigned long max)
{
	unsigned long v;
	char *end;

	if (!isdigit((unsigned char)*s))
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || *end != '\0' || v > max)
		return -1;
	return (long)v;
}

static int
set_type(struct spec_entry *e, const char *v)
{
	static const struct {
		const char	*name;
		uint16_t	type;
	} types[] = {
		{ "file", MFS_S_IFREG }, { "dir", MFS_S_IFDIR },
		{ "link", MFS_S_IFLNK }, { "char", MFS_S_IFCHR },
		{ "block", MFS_S_IFBLK }, { "fifo", MFS_S_IFIFO },
		{ "socket", MFS_S_IFSOCK }
	};
	size_t i;

	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (strcmp(v, types[i].name) == 0) {
			e->type = types[i].type;
			return 0;
		}
	}
	return -1;
}

/*
 * device=format,major,minor, or a number as the file system of the host
 * stores it, which the macros of the host take apart.
 */
static int
set_device(struct spec_entry *e, const char *v)
{
	char buf[64], *end, *f[4], *p;
	unsigned long dev;
	long maj, min;
	int n;

	if (strchr(v, ',') == NULL) {
		errno = 0;
		dev = strtoul(v, &end, 0);
		if (!isdigit((unsigned char)v[0]) || errno != 0 ||
		    *end != '\0')
			return -1;
		e->major = (uint32_t)major((dev_t)dev);
		e->minor = (uint32_t)minor((dev_t)dev);
		return 0;
	}
	(void)snprintf(buf, sizeof(buf), "%s", v);
	for (n = 0, p = buf; n < 4 && p != NULL; n++) {
		f[n] = p;
		if ((p = strchr(p, ',')) != NULL)
			*p++ = '\0';
	}
	if (n != 3 || p != NULL)
		return -1;
	if ((maj = number(f[1], 255)) < 0 || (min = number(f[2], 255)) < 0)
		return -1;
	e->major = (uint32_t)maj;
	e->minor = (uint32_t)min;
	return 0;
}

/* Apply one keyword=value to *e; complain and return -1 if it is bad. */
static int
keyword(struct reader *rd, struct spec_entry *e, char *kv)
{
	static const char *const ignored[] = {
		"cksum", "flags", "ignore", "md5", "md5digest", "nlink",
		"nochange", "ripemd160digest", "rmd160", "rmd160digest",
		"sha1", "sha1digest", "sha256", "sha256digest", "sha384",
		"sha384digest", "sha512", "sha512digest", "size", "tags"
	};
	unsigned long secs;
	char *end, *v;
	long id;
	size_t i;

	if (strcmp(kv, "optional") == 0) {
		e->set |= SPEC_OPTIONAL;
		return 0;
	}
	if ((v = strchr(kv, '=')) == NULL) {
		error(rd, "%s: not keyword=value", kv);
		return -1;
	}
	*v++ = '\0';
	if (strcmp(kv, "type") == 0) {
		if (set_type(e, v) < 0)
			goto bad;
		e->set |= SPEC_TYPE;
	} else if (strcmp(kv, "mode") == 0) {
		errno = 0;
		id = strtol(v, &end, 8);
		if (errno != 0 || *end != '\0' || v[0] < '0' || v[0] > '7' ||
		    id > 07777)
			goto bad;
		e->mode = (uint16_t)id;
		e->set |= SPEC_MODE;
	} else if (strcmp(kv, "uid") == 0 || strcmp(kv, "uname") == 0) {
		id = kv[1] == 'i' ? number(v, 65535) : user_id(rd, v);
		if (id < 0)
			goto bad;
		e->uid = (uint32_t)id;
		e->set |= SPEC_UID;
	} else if (strcmp(kv, "gid") == 0 || strcmp(kv, "gname") == 0) {
		id = kv[1] == 'i' ? number(v, 65535) : group_id(rd, v);
		if (id < 0)
			goto bad;
		e->gid = (uint32_t)id;
		e->set |= SPEC_GID;
	} else if (strcmp(kv, "time") == 0) {
		/* Seconds, a period and nanoseconds, which are dropped. */
		if (strchr(v, '.') != NULL)
			*strchr(v, '.') = '\0';
		errno = 0;
		secs = strtoul(v, &end, 10);
		if (!isdigit((unsigned char)v[0]) || errno != 0 ||
		    *end != '\0' || secs > UINT32_MAX)
			goto bad;
		e->time = (uint32_t)secs;
		e->set |= SPEC_TIME;
	} else if (strcmp(kv, "link") == 0) {
		if (unvis(v) < 0)
			goto bad;
		free(e->link);
		e->link = xstrdup(v);
		e->set |= SPEC_LINK;
	} else if (strcmp(kv, "device") == 0) {
		if (set_device(e, v) < 0)
			goto bad;
		e->set |= SPEC_DEVICE;
	} else {
		for (i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
			if (strcmp(kv, ignored[i]) == 0)
				return 0;
		error(rd, "%s: unknown keyword", kv);
		return -1;
	}
	return 0;
bad:
	error(rd, "%s=%s: bad value", kv, v);
	return -1;
}

static size_t
path_hash(const char *s, size_t max)
{
	size_t h;

	for (h = 5381; *s != '\0'; s++)
		h = h * 33 + (unsigned char)*s;
	return h & (max - 1);
}

/* Make room in the hash table of paths, which keeps it at most half full. */
static void
grow_hash(struct reader *rd)
{
	struct spec *sp;
	size_t i, j;

	sp = rd->spec;
	if ((sp->n + 1) * 2 < rd->hmax)
		return;
	rd->hmax = rd->hmax == 0 ? 1024 : rd->hmax * 2;
	free(rd->hash);
	if ((rd->hash = calloc(rd->hmax, sizeof(*rd->hash))) == NULL) {
		perror(NULL);
		exit(1);
	}
	for (i = 0; i < sp->n; i++) {
		j = path_hash(sp->e[i].path, rd->hmax);
		while (rd->hash[j] != 0)
			j = (j + 1) & (rd->hmax - 1);
		rd->hash[j] = i + 1;
	}
}

/* The entry of path, added with the defaults if it is not there yet. */
static struct spec_entry *
entry(struct reader *rd, const char *path)
{
	struct spec *sp;
	struct spec_entry *e;
	size_t h;

	sp = rd->spec;
	grow_hash(rd);
	for (h = path_hash(path, rd->hmax); rd->hash[h] != 0;
	    h = (h + 1) & (rd->hmax - 1))
		if (strcmp(sp->e[rd->hash[h] - 1].path, path) == 0)
			return &sp->e[rd->hash[h] - 1];
	rd->hash[h] = sp->n + 1;
	if (sp->n == rd->max) {
		rd->max = rd->max == 0 ? 256 : rd->max * 2;
		if ((e = realloc(sp->e, rd->max * sizeof(*e))) == NULL) {
			perror(NULL);
			exit(1);
		}
		sp->e = e;
	}
	e = &sp->e[sp->n++];
	(void)memset(e, 0, sizeof(*e));
	e->path = xstrdup(path);
	return e;
}

/* Apply the defaults, then the keywords of a line, to *e. */
static void
apply(struct reader *rd, struct spec_entry *e, char *rest)
{
	struct spec_entry t;
	char *kv;

	t = rd->glob;
	t.link = rd->glob.link != NULL ? xstrdup(rd->glob.link) : NULL;
	while ((kv = strtok(rest, " \t")) != NULL) {
		rest = NULL;
		(void)keyword(rd, &t, kv);
	}
	/* A later entry of the same path overrides what it gives. */
	if (t.set & SPEC_TYPE)
		e->type = t.type;
	if (t.set & SPEC_MODE)
		e->mode = t.mode;
	if (t.set & SPEC_UID)
		e->uid = t.uid;
	if (t.set & SPEC_GID)
		e->gid = t.gid;
	if (t.set & SPEC_TIME)
		e->time = t.time;
	if (t.set & SPEC_DEVICE) {
		e->major = t.major;
		e->minor = t.minor;
	}
	if (t.set & SPEC_LINK) {
		free(e->link);
		e->link = t.link;
		t.link = NULL;
	}
	e->set = (e->set & ~(uint32_t)SPEC_OPTIONAL) | t.set;
	free(t.link);
}

/* The directory part of a path, in place: "" for a name alone. */
static void
up(char *path)
{
	char *slash;

	if ((slash = strrchr(path, '/')) != NULL)
		*slash = '\0';
	else
		path[0] = '\0';
}

static char *
join_path(const char *dir, const char *name)
{
	char *s;

	if (dir[0] == '\0')
		return xstrdup(name);
	if ((s = malloc(strlen(dir) + strlen(name) + 2)) == NULL) {
		perror(NULL);
		exit(1);
	}
	(void)sprintf(s, "%s/%s", dir, name);
	return s;
}

/* One line, with continuations joined and the newline taken off. */
static void
parse_line(struct reader *rd, char *p)
{
	struct spec_entry *e;
	char *name, *path, *rest;

	while (isspace((unsigned char)*p))
		p++;
	if (*p == '\0' || *p == '#')
		return;
	name = p;
	while (*p != '\0' && !isspace((unsigned char)*p))
		p++;
	rest = p;
	if (*p != '\0')
		*rest++ = '\0';

	if (strcmp(name, "/set") == 0) {
		while ((p = strtok(rest, " \t")) != NULL) {
			rest = NULL;
			(void)keyword(rd, &rd->glob, p);
		}
		rd->glob.set &= ~(uint32_t)SPEC_OPTIONAL;
		return;
	}
	if (strcmp(name, "/unset") == 0) {
		while ((p = strtok(rest, " \t")) != NULL) {
			rest = NULL;
			if (strcmp(p, "all") == 0)
				rd->glob.set = 0;
			else if (strcmp(p, "type") == 0)
				rd->glob.set &= ~(uint32_t)SPEC_TYPE;
			else if (strcmp(p, "mode") == 0)
				rd->glob.set &= ~(uint32_t)SPEC_MODE;
			else if (strcmp(p, "uid") == 0 ||
			    strcmp(p, "uname") == 0)
				rd->glob.set &= ~(uint32_t)SPEC_UID;
			else if (strcmp(p, "gid") == 0 ||
			    strcmp(p, "gname") == 0)
				rd->glob.set &= ~(uint32_t)SPEC_GID;
			else if (strcmp(p, "time") == 0)
				rd->glob.set &= ~(uint32_t)SPEC_TIME;
		}
		return;
	}
	if (strcmp(name, "..") == 0) {
		if (rd->cur[0] == '\0')
			error(rd, "\"..\" above the root");
		up(rd->cur);
		return;
	}
	if (name[0] == '/' || strpbrk(name, "*?[") != NULL) {
		error(rd, "%s: patterns and absolute paths are not supported",
		    name);
		return;
	}
	if (unvis(name) < 0) {
		error(rd, "%s: bad escape", name);
		return;
	}
	if (strcmp(name, ".") == 0) {
		path = xstrdup("");
		free(rd->cur);
		rd->cur = xstrdup("");
	} else if (strchr(name, '/') != NULL) {
		/* A full path, from the root. */
		while (strncmp(name, "./", 2) == 0)
			name += 2;
		path = xstrdup(name);
		free(rd->cur);
		rd->cur = xstrdup(name);
		up(rd->cur);
	} else {
		path = join_path(rd->cur, name);
	}
	e = entry(rd, path);
	apply(rd, e, rest);
	/* Relative names that follow a directory are in it. */
	if ((e->set & SPEC_TYPE) && e->type == MFS_S_IFDIR &&
	    path[0] != '\0') {
		free(rd->cur);
		rd->cur = xstrdup(path);
	}
	free(path);
}

static int
by_path(const void *a, const void *b)
{
	return strcmp(((const struct spec_entry *)a)->path,
	    ((const struct spec_entry *)b)->path);
}

int
spec_read(const char *file, const char *dbdir, struct spec *sp)
{
	struct reader rd;
	char *buf;
	size_t len;
	FILE *f;

	(void)memset(sp, 0, sizeof(*sp));
	(void)memset(&rd, 0, sizeof(rd));
	rd.spec = sp;
	rd.file = file;
	rd.dbdir = dbdir;
	rd.cur = xstrdup("");
	if ((f = fopen(file, "r")) == NULL) {
		perror(file);
		free(rd.cur);
		return -1;
	}
	if ((buf = malloc(MAX_LINE)) == NULL) {
		perror(NULL);
		exit(1);
	}
	len = 0;
	while (fgets(buf + len, (int)(MAX_LINE - len), f) != NULL) {
		rd.line++;
		len += strlen(buf + len);
		if (len > 0 && buf[len - 1] == '\n')
			buf[--len] = '\0';
		/* A backslash at the end joins the next line. */
		if (len > 0 && buf[len - 1] == '\\' && len < MAX_LINE - 2) {
			buf[len - 1] = ' ';
			continue;
		}
		parse_line(&rd, buf);
		len = 0;
	}
	if (len > 0)
		parse_line(&rd, buf);
	if (ferror(f)) {
		perror(file);
		rd.errors++;
	}
	(void)fclose(f);
	free(buf);
	free(rd.cur);
	free(rd.glob.link);
	free(rd.hash);
	if (sp->n > 0)
		qsort(sp->e, sp->n, sizeof(*sp->e), by_path);
	if (rd.errors > 0) {
		spec_free(sp);
		return -1;
	}
	return 0;
}

struct spec_entry *
spec_find(const struct spec *sp, const char *path)
{
	struct spec_entry key;

	if (sp == NULL || sp->n == 0)
		return NULL;
	key.path = (char *)(uintptr_t)path;
	return bsearch(&key, sp->e, sp->n, sizeof(*sp->e), by_path);
}

size_t
spec_below(const struct spec *sp, const char *dir, size_t *first)
{
	struct spec_entry key;
	size_t len, lo, hi, mid;
	char *prefix;

	*first = 0;
	if (sp == NULL || sp->n == 0)
		return 0;
	/* The paths that start with "dir/" come together, sorted. */
	len = strlen(dir);
	if ((prefix = malloc(len + 2)) == NULL) {
		perror(NULL);
		exit(1);
	}
	(void)sprintf(prefix, "%s%s", dir, len > 0 ? "/" : "");
	key.path = prefix;
	lo = 0;
	hi = sp->n;
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (by_path(&sp->e[mid], &key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (hi = lo; hi < sp->n; hi++)
		if (strncmp(sp->e[hi].path, prefix, strlen(prefix)) != 0)
			break;
	free(prefix);
	*first = lo;
	return hi;
}

void
spec_free(struct spec *sp)
{
	size_t i;

	for (i = 0; i < sp->n; i++) {
		free(sp->e[i].path);
		free(sp->e[i].link);
	}
	free(sp->e);
	sp->e = NULL;
	sp->n = 0;
}
