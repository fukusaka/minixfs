#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# newfs_minixfs: an empty file system of every format must be the image
# that tests/mkimage, the independent writer, makes from an empty tree;
# the defaults, the size and the options must behave as documented.

. ./tests/lib.sh

# Make both images of one format and compare them.  mkimage gives the
# root directory the times 0, 1 and 2 in V2 and V3, so those are set to 0
# as newfs_minixfs -t 0 writes them.
vs_mkimage() {
	_vs_mkimage_version=$1
	_vs_mkimage_opts=$2
	_vs_mkimage_spec=$3
	_vs_mkimage_name=$4
	echo "fs version=$_vs_mkimage_version $_vs_mkimage_spec" \
	    >"$T/spec"
	mkimage "$T/spec" "$T/want.img"
	if [ "$_vs_mkimage_version" -ne 1 ]; then
		set_inode "$T/want.img" 1 atime 0
		set_inode "$T/want.img" 1 ctime 0
	fi
	rm -f "$T/got.img"
	# The options are split on purpose.
	# shellcheck disable=SC2086
	run "$NEWFS_MINIXFS" -V "$_vs_mkimage_version" \
	    $_vs_mkimage_opts -t 0 "$T/got.img"
	check_status "$_vs_mkimage_name: newfs_minixfs succeeds" 0
	check_same_file "$_vs_mkimage_name: the image is that of mkimage" \
	    "$T/want.img" "$T/got.img"
}

# Every format, byte order and zone size, against mkimage.
formats() {
	for format in 1:14:1024 1:30:1024 2:14:1024 2:30:1024 3:60:1024 \
	    3:60:4096; do
	for order in le be; do
	for logzone in 0 1; do
		version=${format%%:*}
		namelen=${format#*:}
		namelen=${namelen%:*}
		bsize=${format##*:}
		if [ "$version" -eq 3 ]; then
			opts="-b $bsize"
			spec="block=$bsize"
		else
			opts="-l $namelen"
			spec="namelen=$namelen"
		fi
		blocks=$((2097152 / bsize))
		opts="$opts -B $order -s $blocks -i 100 -z $logzone"
		spec="$spec order=$order blocks=$blocks inodes=100"
		spec="$spec logzone=$logzone"
		vs_mkimage "$version" "$opts" "$spec" \
		    "V$format/$order/$logzone"
	done
	done
	done
}

# What is left out takes the documented defaults.
defaults() {
	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 1 -s 1440 "$T/img"
	check_status "V1 with defaults succeeds" 0
	check_info "V1 has 14-character names by default" "$T/img" \
	    "name length" 14
	check_info "V1 is little-endian by default" "$T/img" "byte order" \
	    little-endian
	check_info "V1 has a third as many inodes as blocks" "$T/img" \
	    inodes 480
	check_info "an empty V1 has one inode in use" "$T/img" \
	    "free inodes" 479
	check_info "an empty V1 has one data zone in use" "$T/img" \
	    "free zones" 1420

	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 3 -s 2048 "$T/img"
	check_status "V3 with defaults succeeds" 0
	check_info "V3 has 4096-byte blocks by default" "$T/img" \
	    "block size" 4096
	check_info "V3 inodes fill the blocks of the inode table" "$T/img" \
	    inodes 704
	check_info "V3 has 60-character names" "$T/img" "name length" 60

	run "$MINIXFS" ls "$T/img"
	: >"$T/empty"
	check_out "the root directory is empty" "$T/empty"
}

# The size: from -s, which creates or cuts the file, or from the file.
sizes() {
	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 2 -s 1000 "$T/img"
	check_true "-s makes a file of that many blocks" \
	    test "$(wc -c <"$T/img")" -eq 1024000

	dd if=/dev/zero of="$T/img" bs=1024 count=3000 2>/dev/null
	run "$NEWFS_MINIXFS" -V 2 -s 1000 "$T/img"
	check_true "-s cuts a larger file to size" \
	    test "$(wc -c <"$T/img")" -eq 1024000

	dd if=/dev/zero of="$T/img" bs=1024 count=777 2>/dev/null
	run "$NEWFS_MINIXFS" -V 2 "$T/img"
	check_status "the size can come from the file" 0
	check_info "the file gives the number of zones" "$T/img" zones 777

	# The boot block is left alone.
	awk 'BEGIN { for (i = 0; i < 1024; i++) printf "%c", 65 + i % 26 }' \
	    >"$T/boot"
	cp "$T/boot" "$T/img"
	dd if=/dev/zero bs=1024 count=500 2>/dev/null >>"$T/img"
	run "$NEWFS_MINIXFS" -V 1 "$T/img"
	dd if="$T/img" of="$T/got" bs=1024 count=1 2>/dev/null
	check_same_file "the boot block is kept" "$T/boot" "$T/got"

	# -N prints the layout and writes nothing.
	cp "$T/img" "$T/before"
	run "$NEWFS_MINIXFS" -V 3 -b 1024 -N "$T/img"
	check_out_has "-N prints the layout" "^inode table blocks: "
	check_same_file "-N leaves the image as it was" "$T/before" "$T/img"
	rm -f "$T/none"
	run "$NEWFS_MINIXFS" -V 3 -s 100 -N "$T/none"
	check_true "-N does not create the image" test ! -e "$T/none"
}

# Options that cannot be met.
refusals() {
	run "$NEWFS_MINIXFS" -s 100 "$T/img"
	check_status "the version is required" 2
	run "$NEWFS_MINIXFS" -V 4 -s 100 "$T/img"
	check_status "version 4 is a usage error" 2
	run "$NEWFS_MINIXFS" -V 1 -b 4096 -s 100 "$T/img"
	check_status "V1 has no block size to choose" 2
	run "$NEWFS_MINIXFS" -V 3 -l 30 -s 100 "$T/img"
	check_status "V3 has no name length to choose" 2
	run "$NEWFS_MINIXFS" -V 2 -l 20 -s 100 "$T/img"
	check_status "a name length of 20 is refused" 2
	run "$NEWFS_MINIXFS" -V 3 -b 1500 -s 100 "$T/img"
	check_status "a block size of 1500 is refused" 2
	run "$NEWFS_MINIXFS" -V 1 -B middle -s 100 "$T/img"
	check_status "a byte order other than le and be is refused" 2
	run "$NEWFS_MINIXFS" -V 1 -i 0 -s 100 "$T/img"
	check_status "zero inodes are refused" 2
	run "$NEWFS_MINIXFS" -V 1 -s 1x "$T/img"
	check_status "a size that is not a number is refused" 2

	run "$NEWFS_MINIXFS" -V 1 -s 4 "$T/img"
	check_err "a file system too small for its root is refused" \
	    "too few"
	run "$NEWFS_MINIXFS" -V 1 -s 70000 "$T/img"
	check_err "more zones than V1 can count are refused" "too many"
	run "$NEWFS_MINIXFS" -V 2 -i 70000 -s 10000 "$T/img"
	check_err "more inodes than V2 can count are refused" "too many"
	rm -f "$T/none"
	run "$NEWFS_MINIXFS" -V 1 "$T/none"
	check_err "a missing image needs -s" "does not exist"
}

formats
defaults
sizes
refusals

finish
