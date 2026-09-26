#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# The bit maps of a big-endian file system in words of 8, 16, 32 and 64
# bits: MINIX keeps words of 16 bits, util-linux bytes, and the Linux
# kernel what the machine has.  The test tree in each width, as mkimage
# writes it, is taken in that width as it is opened: minixfs info gives
# it, fsck_minixfs passes the tree and says so, and with -B another width
# finds the maps wrong; minixfs lists it and puts a file in it, and
# dump_minixfs and restore_minixfs carry it over.  Where the inode map
# reads the same in every width, the zone map tells.

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

		check_info "$v: the width is found" "$T/img" \
		    "bit map words" "$bits bits"
		run "$FSCK_MINIXFS" "$T/img"
		check_status "$v: fsck passes it" 0
		check_out_has "$v: fsck says the width it found" \
		    ": bit map words: $bits bits, as found\$"
		other=16
		if [ "$bits" -eq 16 ]; then
			other=8
		fi
		run "$FSCK_MINIXFS" -B "be$other" "$T/img"
		check_found "$v: fsck -B be$other finds the maps wrong" \
		    "but free in the inode map"
		run "$FSCK_MINIXFS" -B "be$bits" "$T/img"
		check_out_has "$v: fsck -B be$bits takes the width given" \
		    ": bit map words: $bits bits, as given\$"

		run "$MINIXFS" tar "$T/img"
		check_out "$v: minixfs reads the tree" "$T/le.tar"
		printf 'new\n' >"$T/new"
		cp "$T/img" "$T/w.img"
		run "$MINIXFS" put "$T/w.img" "$T/new" new
		check_status "$v: minixfs puts a file" 0
		run "$FSCK_MINIXFS" "$T/w.img"
		check_status "$v: and fsck passes it" 0

		rm -f "$T/dump" "$T/r.img" "$T/symtab"
		run "$DUMP_MINIXFS" -f "$T/dump" "$T/img"
		check_status "$v: dump_minixfs dumps it" 0
		if [ "$version" -eq 3 ]; then
			opts="-V 3 -b ${format#*/}"
		else
			opts="-V $version -l ${format#*/}"
		fi
		# The options are split on purpose.
		# shellcheck disable=SC2086
		"$NEWFS_MINIXFS" $opts -B "be$bits" -s 4096 -i 256 \
		    "$T/r.img" >/dev/null
		run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" \
		    "$T/r.img"
		check_status "$v: restore_minixfs restores it" 0
		check_info "$v: into maps of that width" "$T/r.img" \
		    "bit map words" "$bits bits"
		run "$FSCK_MINIXFS" "$T/r.img"
		check_status "$v: which fsck passes" 0
		run "$MINIXFS" tar "$T/r.img"
		check_out "$v: as the same tree" "$T/le.tar"
	done
done

# Inodes 0 to 63 in use: the first 8 bytes of the inode map are all set,
# the same in every width, and the zones of the files tell instead.
for bits in 8 16 32 64; do
	{
		echo "fs version=2 order=be blocks=2048 inodes=200 end=0" \
		    "mapword=$bits"
		i=2
		while [ "$i" -le 63 ]; do
			echo "file /f$i 0644 0 0 0 $((i * 100)) $i"
			i=$((i + 1))
		done
	} >"$T/spec"
	mkimage "$T/spec" "$T/img"
	check_info "$bits bits: the zone map tells the width" "$T/img" \
	    "bit map words" "$bits bits"
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$bits bits: and fsck passes it" 0
done

run "$FSCK_MINIXFS" -B be24 "$T/img"
check_status "fsck -B be24 is a usage error" 2
run "$FSCK_MINIXFS" -B be "$T/img"
check_status "fsck -B be without a width is a usage error" 2
run "$FSCK_MINIXFS" -B be8 "$T/le"
check_status "fsck -B be8 is a usage error on a little-endian image" 2

finish
