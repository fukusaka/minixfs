/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * compat.h - the differences between the systems the code runs on.
 *
 * This is the only place where the name of a system may appear.  It has
 * to come before every other header, since it can select what the
 * system headers declare.
 */

#ifndef COMPAT_H
#define COMPAT_H

/*
 * NetBSD and MINIX 3 hide makedev() when _XOPEN_SOURCE is defined, unless
 * _NETBSD_SOURCE is defined as well.
 */
#if defined(__NetBSD__) || defined(__minix)
#define _NETBSD_SOURCE
#endif

/*
 * FreeBSD leaves __BSD_VISIBLE undefined, and so hides the types of
 * <sys/disk.h> and makedev(), when _XOPEN_SOURCE is defined.
 */
#if defined(__FreeBSD__)
#define __BSD_VISIBLE	1
#endif

#include <sys/types.h>

/* makedev(): <sys/types.h> on the BSDs and MINIX, its own header on Linux. */
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif

/*
 * The ioctl that gives the size of a disk device: stat(2) leaves st_size
 * of a device unspecified, and Linux, NetBSD and MINIX 3 give 0 for a
 * block device.  <linux/fs.h> clashes with the <sys/mount.h> of glibc,
 * which has BLKGETSIZE64 too.
 */
#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/mount.h>
#elif defined(__NetBSD__)
#include <sys/ioctl.h>
#include <sys/dkio.h>
#elif defined(__FreeBSD__)
#include <sys/ioctl.h>
#include <sys/disk.h>
#elif defined(__minix)
#include <sys/ioctl.h>
#include <sys/ioc_disk.h>
#include <minix/partition.h>
#endif

#include <stdint.h>

/* The size in bytes of the disk device open on fd, or -1 if not known. */
static inline off_t
compat_disk_size(int fd)
{
#if defined(__linux__)
	uint64_t n;

	if (ioctl(fd, BLKGETSIZE64, &n) == 0 && n <= INT64_MAX)
		return (off_t)n;
#elif defined(__NetBSD__) || defined(__FreeBSD__)
	off_t n;

	if (ioctl(fd, DIOCGMEDIASIZE, &n) == 0)
		return n;
#elif defined(__minix)
	struct part_geom pg;

	if (ioctl(fd, DIOCGETP, &pg) == 0 && pg.size <= INT64_MAX)
		return (off_t)pg.size;
#else
	(void)fd;
#endif
	return -1;
}

#endif /* COMPAT_H */
