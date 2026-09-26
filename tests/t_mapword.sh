#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# The bit maps of a big-endian file system in words of 8, 16, 32 and 64
# bits: MINIX keeps words of 16 bits, util-linux bytes, and the Linux
# kernel what the machine has.  The test tree in each width, as mkimage
# writes it, is read and written with -W as it is: fsck_minixfs passes it
# and finds the maps wrong in another width, minixfs lists it and puts a
# file in it, and dump_minixfs and restore_minixfs carry it over.

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

TZ=UTC
export TZ

for format in 1/14 2/14 2/30 3/1024; do
	version=${format%/*}
	if [ "$version" -eq 3 ]; then
		fs="version=3 block=${format#*/}"
	else
		fs="version=$version namelen=${format#*/}"
	fi
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/le/" -e "s/@BLOCKS@/4096/" \
	    -e "s/@LOGZONE@/0/" tests/tree.spec >"$T/spec.le"
	mkimage "$T/spec.le" "$T/le"
	"$MINIXFS" tar "$T/le" >"$T/le.tar"
	for bits in 8 16 32 64; do
		v="V$format/$bits"
		sed -e '/^fs /s/order=le/order=be/' \
		    -e "/^fs /s/\$/ mapword=$bits/" "$T/spec.le" >"$T/spec"
		mkimage "$T/spec" "$T/img"

		run "$MINIXFS" -W "$bits" info "$T/img"
		check_out_has "$v: info gives the width" \
		    "^bit map words: $bits bits\$"
		run "$FSCK_MINIXFS" -W "$bits" "$T/img"
		check_status "$v: fsck -W $bits passes it" 0
		other=16
		if [ "$bits" -eq 16 ]; then
			other=8
		fi
		run "$FSCK_MINIXFS" -W "$other" "$T/img"
		check_found "$v: fsck -W $other finds the maps wrong" \
		    "but free in the inode map"

		run "$MINIXFS" -W "$bits" tar "$T/img"
		check_out "$v: minixfs -W reads the tree" "$T/le.tar"
		printf 'new\n' >"$T/new"
		cp "$T/img" "$T/w.img"
		run "$MINIXFS" -W "$bits" put "$T/w.img" "$T/new" new
		check_status "$v: minixfs -W puts a file" 0
		run "$FSCK_MINIXFS" -W "$bits" "$T/w.img"
		check_status "$v: and fsck -W passes it" 0

		rm -f "$T/dump" "$T/r.img" "$T/symtab"
		run "$DUMP_MINIXFS" -W "$bits" -f "$T/dump" "$T/img"
		check_status "$v: dump_minixfs -W dumps it" 0
		if [ "$version" -eq 3 ]; then
			opts="-V 3 -b ${format#*/}"
		else
			opts="-V $version -l ${format#*/}"
		fi
		# The options are split on purpose.
		# shellcheck disable=SC2086
		"$NEWFS_MINIXFS" $opts -B be -W "$bits" -s 4096 -i 256 \
		    "$T/r.img" >/dev/null
		run "$RESTORE_MINIXFS" -r -W "$bits" -s "$T/symtab" \
		    -f "$T/dump" "$T/r.img"
		check_status "$v: restore_minixfs -W restores it" 0
		run "$FSCK_MINIXFS" -W "$bits" "$T/r.img"
		check_status "$v: into maps of that width" 0
		run "$MINIXFS" -W "$bits" tar "$T/r.img"
		check_out "$v: as the same tree" "$T/le.tar"
	done
done

finish
