# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
# Makefile for minixfs.  It is written for both GNU make and BSD make, so
# it keeps to suffix rules and plain variables.

CC ?=		cc
CFLAGS ?=	-O2 -g
WARNFLAGS =	-std=c99 -D_XOPEN_SOURCE=700 -Wall -Wextra -Wshadow \
		-Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith \
		-Wcast-qual -Wwrite-strings

OBJS =		src/mfs.o src/minixfs.o
PROG =		minixfs
MKIMAGE =	tests/mkimage

SANFLAGS =	-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
		-fno-sanitize-recover=all
# A command run on the sanitizer binary after it is linked, for systems
# that need it adjusted before AddressSanitizer can run (see README).
SAN_POSTLINK =

all: $(PROG)

$(MKIMAGE): tests/mkimage.c
	$(CC) $(CFLAGS) $(WARNFLAGS) -o $(MKIMAGE) tests/mkimage.c

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(PROG) $(OBJS)

.c.o:
	$(CC) $(CFLAGS) $(WARNFLAGS) -c -o $@ $<

src/mfs.o: src/mfs.h
src/minixfs.o: src/mfs.h

check: $(PROG) $(MKIMAGE)
	MINIXFS=./$(PROG) sh tests/run.sh

# Build with AddressSanitizer and UBSan in a separate directory, then run
# the whole test suite against that binary.
check-sanitize: $(MKIMAGE)
	rm -rf build-san && mkdir build-san
	$(CC) $(SANFLAGS) $(WARNFLAGS) -o build-san/$(PROG) \
	    src/mfs.c src/minixfs.c
	if [ -n "$(SAN_POSTLINK)" ]; then $(SAN_POSTLINK) build-san/$(PROG); fi
	MINIXFS=./build-san/$(PROG) sh tests/run.sh

clean:
	rm -f $(PROG) $(OBJS) $(MKIMAGE)
	rm -rf build-san

.PHONY: all check check-sanitize clean
