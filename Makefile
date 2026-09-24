# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
# Makefile for minixfs.  It is written for both GNU make and BSD make, so
# it keeps to suffix rules and plain variables.

CC ?=		cc
CFLAGS ?=	-O2 -g
WARNFLAGS =	-std=c99 -D_XOPEN_SOURCE=700 -Wall -Wextra -Wshadow \
		-Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith \
		-Wcast-qual -Wwrite-strings

LIBOBJS =	src/mfs.o src/mfs_format.o
PROG =		minixfs
NEWFS =		newfs_minixfs
MKIMAGE =	tests/mkimage

# mount_minixfs needs a FUSE library, so it is built on request: "make
# fuse".  FUSE_CFLAGS and FUSE_LIBS come from pkg-config where libfuse 3
# is installed; on NetBSD, give FUSE_LIBS="-lrefuse -lpuffs".
# FUSE_VERSION is the FUSE_USE_VERSION to build against: 31, or 26 for
# the FUSE 2 interface.
FUSEPROG =	mount_minixfs
FUSE_VERSION =	31
FUSE_CFLAGS !=	pkg-config --cflags fuse3 2>/dev/null || true
FUSE_LIBS !=	pkg-config --libs fuse3 2>/dev/null || true

SANFLAGS =	-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
		-fno-sanitize-recover=all
# A command run on the sanitizer binary after it is linked, for systems
# that need it adjusted before AddressSanitizer can run (see README).
SAN_POSTLINK =

all: $(PROG) $(NEWFS)

$(MKIMAGE): tests/mkimage.c
	$(CC) $(CFLAGS) $(WARNFLAGS) -o $(MKIMAGE) tests/mkimage.c

$(PROG): src/minixfs.o $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(PROG) src/minixfs.o $(LIBOBJS)

$(NEWFS): src/newfs_minixfs.o $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(NEWFS) src/newfs_minixfs.o $(LIBOBJS)

.c.o:
	$(CC) $(CFLAGS) $(WARNFLAGS) -c -o $@ $<

src/mfs.o: src/mfs.h src/layout.h
src/mfs_format.o: src/mfs.h src/layout.h
src/minixfs.o: src/mfs.h
src/newfs_minixfs.o: src/mfs.h

fuse: $(FUSEPROG)

$(FUSEPROG): src/mount_minixfs.c src/compat.h src/mfs.h $(LIBOBJS)
	$(CC) $(CFLAGS) $(WARNFLAGS) -D_FILE_OFFSET_BITS=64 \
	    -DFUSE_USE_VERSION=$(FUSE_VERSION) $(FUSE_CFLAGS) \
	    -o $(FUSEPROG) src/mount_minixfs.c $(LIBOBJS) $(LDFLAGS) \
	    $(FUSE_LIBS)

check: $(PROG) $(NEWFS) $(MKIMAGE)
	MINIXFS=./$(PROG) NEWFS_MINIXFS=./$(NEWFS) sh tests/run.sh

# Build with AddressSanitizer and UBSan in a separate directory, then run
# the whole test suite against that binary.
check-sanitize: $(MKIMAGE)
	rm -rf build-san && mkdir build-san
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(PROG) \
	    src/minixfs.c src/mfs.c src/mfs_format.c
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(NEWFS) \
	    src/newfs_minixfs.c src/mfs.c src/mfs_format.c
	if [ -n "$(SAN_POSTLINK)" ]; then \
	    $(SAN_POSTLINK) build-san/$(PROG); \
	    $(SAN_POSTLINK) build-san/$(NEWFS); \
	fi
	MINIXFS=./build-san/$(PROG) NEWFS_MINIXFS=./build-san/$(NEWFS) \
	    sh tests/run.sh

clean:
	rm -f $(PROG) $(NEWFS) src/*.o $(MKIMAGE) $(FUSEPROG)
	rm -rf build-san

.PHONY: all check check-sanitize clean fuse
