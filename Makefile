# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
# Makefile for minixfs.  It is written for both GNU make and BSD make, so
# it keeps to suffix rules and plain variables.

CC ?=		cc
CFLAGS ?=	-O2 -g
WARNFLAGS =	-std=c99 -D_XOPEN_SOURCE=700 -Wall -Wextra -Wshadow \
		-Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith \
		-Wcast-qual -Wwrite-strings

LIBOBJS =	src/mfs.o src/mfs_format.o src/mfs_tune.o src/mfs_write.o
PROG =		minixfs
NEWFS =		newfs_minixfs
FSCK =		fsck_minixfs
TUNEFS =	tunefs_minixfs
DUMP =		dump_minixfs
RESTORE =	restore_minixfs
MKIMAGE =	tests/mkimage
MKDUMP =	tests/mkdump

# mount_minixfs needs a FUSE library, so it is built on request: "make
# fuse".  FUSE_CFLAGS and FUSE_LIBS come from pkg-config where libfuse 3
# is installed; on NetBSD, give FUSE_LIBS="-lrefuse -lpuffs".
# FUSE_VERSION is the FUSE_USE_VERSION to build against: 31, or 26 for
# the FUSE 2 interface.
FUSEPROG =	mount_minixfs
FUSE_VERSION =	31
FUSE_CFLAGS !=	(pkg-config --cflags fuse3) 2>/dev/null || true
FUSE_LIBS !=	(pkg-config --libs fuse3) 2>/dev/null || true

# Where "make install" puts the commands and the manuals, English in
# MANDIR and Japanese in MANDIR/ja, below DESTDIR.
PREFIX ?=	/usr/local
BINDIR ?=	$(PREFIX)/bin
SBINDIR ?=	$(PREFIX)/sbin
MANDIR ?=	$(PREFIX)/man
INSTALL ?=	install
MANS =		man/minixfs.1 man/minixfs.5 man/dump_minixfs.8 \
		man/fsck_minixfs.8 man/mount_minixfs.8 man/newfs_minixfs.8 \
		man/restore_minixfs.8 man/tunefs_minixfs.8
MANS_JA =	man/ja/minixfs.1 man/ja/minixfs.5 man/ja/dump_minixfs.8 \
		man/ja/fsck_minixfs.8 man/ja/mount_minixfs.8 \
		man/ja/newfs_minixfs.8 man/ja/restore_minixfs.8 \
		man/ja/tunefs_minixfs.8

SANFLAGS =	-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
		-fno-sanitize-recover=all
# A command run on the sanitizer binary after it is linked, for systems
# that need it adjusted before AddressSanitizer can run (see README).
SAN_POSTLINK =

all: $(PROG) $(NEWFS) $(FSCK) $(TUNEFS) $(DUMP) $(RESTORE)

$(MKIMAGE): tests/mkimage.c
	$(CC) $(CFLAGS) $(WARNFLAGS) -o $(MKIMAGE) tests/mkimage.c

$(MKDUMP): tests/mkdump.c
	$(CC) $(CFLAGS) $(WARNFLAGS) -o $(MKDUMP) tests/mkdump.c

$(PROG): src/minixfs.o $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(PROG) src/minixfs.o $(LIBOBJS)

$(NEWFS): src/newfs_minixfs.o src/tree.o src/spec.o $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(NEWFS) src/newfs_minixfs.o src/tree.o \
	    src/spec.o $(LIBOBJS)

$(FSCK): src/fsck_minixfs.o $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(FSCK) src/fsck_minixfs.o $(LIBOBJS)

$(TUNEFS): src/tunefs_minixfs.o $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TUNEFS) src/tunefs_minixfs.o \
	    $(LIBOBJS)

$(DUMP): src/dump_minixfs.o src/dumpfmt.o $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(DUMP) src/dump_minixfs.o \
	    src/dumpfmt.o $(LIBOBJS)

$(RESTORE): src/restore_minixfs.o src/dumpfmt.o $(LIBOBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(RESTORE) src/restore_minixfs.o \
	    src/dumpfmt.o $(LIBOBJS)

.c.o:
	$(CC) $(CFLAGS) $(WARNFLAGS) -c -o $@ $<

src/mfs.o: src/mfs.h src/layout.h
src/mfs_format.o: src/mfs.h src/layout.h
src/mfs_tune.o: src/mfs.h src/layout.h
src/mfs_write.o: src/mfs.h src/layout.h
src/minixfs.o: src/mfs.h
src/newfs_minixfs.o: src/mfs.h src/spec.h src/tree.h
src/tree.o: src/compat.h src/mfs.h src/spec.h src/tree.h
src/spec.o: src/compat.h src/mfs.h src/spec.h
src/fsck_minixfs.o: src/mfs.h
src/tunefs_minixfs.o: src/mfs.h
src/dumpfmt.o: src/dumpfmt.h src/layout.h src/mfs.h
src/dump_minixfs.o: src/compat.h src/dumpfmt.h src/mfs.h
src/restore_minixfs.o: src/compat.h src/dumpfmt.h src/mfs.h

fuse: $(FUSEPROG)

$(FUSEPROG): src/mount_minixfs.c src/compat.h src/mfs.h $(LIBOBJS)
	$(CC) $(CFLAGS) $(WARNFLAGS) -D_FILE_OFFSET_BITS=64 \
	    -DFUSE_USE_VERSION=$(FUSE_VERSION) $(FUSE_CFLAGS) \
	    -o $(FUSEPROG) src/mount_minixfs.c $(LIBOBJS) $(LDFLAGS) \
	    $(FUSE_LIBS)

check: all $(MKIMAGE) $(MKDUMP)
	MINIXFS=./$(PROG) NEWFS_MINIXFS=./$(NEWFS) FSCK_MINIXFS=./$(FSCK) \
	    TUNEFS_MINIXFS=./$(TUNEFS) DUMP_MINIXFS=./$(DUMP) \
	    RESTORE_MINIXFS=./$(RESTORE) sh tests/run.sh

# Build with AddressSanitizer and UBSan in a separate directory, then run
# the whole test suite against that binary.
check-sanitize: $(MKIMAGE) $(MKDUMP)
	rm -rf build-san && mkdir build-san
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(PROG) \
	    src/minixfs.c src/mfs.c src/mfs_format.c \
	    src/mfs_tune.c src/mfs_write.c
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(NEWFS) \
	    src/newfs_minixfs.c src/tree.c src/spec.c src/mfs.c \
	    src/mfs_format.c src/mfs_tune.c src/mfs_write.c
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(FSCK) \
	    src/fsck_minixfs.c src/mfs.c src/mfs_format.c \
	    src/mfs_tune.c src/mfs_write.c
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(TUNEFS) \
	    src/tunefs_minixfs.c src/mfs.c src/mfs_format.c \
	    src/mfs_tune.c src/mfs_write.c
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(DUMP) \
	    src/dump_minixfs.c src/dumpfmt.c src/mfs.c src/mfs_format.c \
	    src/mfs_tune.c src/mfs_write.c
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(RESTORE) \
	    src/restore_minixfs.c src/dumpfmt.c src/mfs.c \
	    src/mfs_format.c src/mfs_tune.c src/mfs_write.c
	if [ -n "$(SAN_POSTLINK)" ]; then \
	    for p in $(PROG) $(NEWFS) $(FSCK) $(TUNEFS) $(DUMP) \
		$(RESTORE); do \
		$(SAN_POSTLINK) build-san/$$p || exit 1; \
	    done; \
	fi
	MINIXFS=./build-san/$(PROG) NEWFS_MINIXFS=./build-san/$(NEWFS) \
	    FSCK_MINIXFS=./build-san/$(FSCK) \
	    TUNEFS_MINIXFS=./build-san/$(TUNEFS) \
	    DUMP_MINIXFS=./build-san/$(DUMP) \
	    RESTORE_MINIXFS=./build-san/$(RESTORE) sh tests/run.sh

# mount_minixfs goes in too if "make fuse" built it.
install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SBINDIR)
	$(INSTALL) -m 555 $(PROG) $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 555 $(NEWFS) $(FSCK) $(TUNEFS) $(DUMP) $(RESTORE) \
	    $(DESTDIR)$(SBINDIR)
	if [ -f $(FUSEPROG) ]; then \
	    $(INSTALL) -m 555 $(FUSEPROG) $(DESTDIR)$(SBINDIR); \
	fi
	for f in $(MANS) $(MANS_JA); do \
	    case $$f in \
	    man/ja/*) d=$(DESTDIR)$(MANDIR)/ja/man$${f##*.} ;; \
	    *) d=$(DESTDIR)$(MANDIR)/man$${f##*.} ;; \
	    esac; \
	    $(INSTALL) -d $$d && $(INSTALL) -m 444 $$f $$d || exit 1; \
	done

# Check the manuals: with mandoc where there is one, else with groff.
lint-man:
	if command -v mandoc >/dev/null 2>&1; then \
	    mandoc -Tlint -Wwarning $(MANS) $(MANS_JA); \
	else \
	    for f in $(MANS) $(MANS_JA); do \
		preconv $$f | groff -mdoc -Tutf8 -ww -z || exit 1; \
	    done; \
	fi

clean:
	rm -f $(PROG) $(NEWFS) $(FSCK) $(TUNEFS) $(DUMP) $(RESTORE) \
	    src/*.o $(MKIMAGE) $(MKDUMP) $(FUSEPROG)
	rm -rf build-san

.PHONY: all check check-sanitize clean fuse install lint-man
