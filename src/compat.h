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

#include <sys/types.h>

/* makedev(): <sys/types.h> on the BSDs and MINIX, its own header on Linux. */
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif

#endif /* COMPAT_H */
