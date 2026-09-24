#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Cross-checks with the MINIX tools of util-linux, where they exist.  They
# are optional: without them the checks are skipped.
#
#  - fsck.minix must accept the images that mkimage writes, which shows
#    that the test images themselves are sound.  It reads little-endian
#    file systems with 1024-byte blocks and one-block zones only.
#  - Images made by mkfs.minix must be readable.
#
# FSCK_MINIX and MKFS_MINIX name the tools (default: from PATH).

. ./tests/lib.sh

: "${FSCK_MINIX:=fsck.minix}"
: "${MKFS_MINIX:=mkfs.minix}"

have() {
	command -v "$1" >/dev/null 2>&1
}

# fsck_accepts: fsck.minix accepts the test tree in every format it
# reads, and notices damage.
fsck_accepts() {
	for fs in "version=1 namelen=14" "version=1 namelen=30" \
	    "version=2 namelen=14" "version=2 namelen=30" "version=3"; do
		sed -e "s/@FS@/$fs/" -e "s/@ORDER@/le/" -e "s/@BLOCKS@/4096/" \
		    -e "s/@LOGZONE@/0/" tests/tree.spec >"$T/spec"
		mkimage "$T/spec" "$T/img"
		run "$FSCK_MINIX" -f "$T/img"
		check_status "fsck.minix accepts the test tree ($fs)" 0
	done

	# fsck.minix must notice damage, or the checks above say nothing.
	cp "$T/img" "$T/bad"
	poke "$T/bad" $((3 * 1024)) 000		# part of the zone map
	run "$FSCK_MINIX" -f "$T/bad"
	check_true "fsck.minix notices a damaged zone map" \
	    test "$status" -ne 0
}

# fsck_triple: a file that reaches the triple indirect zone (see
# t_big.sh).
fsck_triple() {
	for fs in "version=2 namelen=14" "version=3"; do
		cat >"$T/spec" <<EOF
fs $fs order=le blocks=2048 inodes=16
file /tind 0644 0 0 0 67400000 7 0:67380000
EOF
		mkimage "$T/spec" "$T/img"
		run "$FSCK_MINIX" -f "$T/img"
		check_status "fsck.minix accepts a triple indirect file ($fs)" 0

		# Inode 2 is the file.
		cp "$T/img" "$T/bad"
		set_inode "$T/bad" 2 zone9 0
		run "$FSCK_MINIX" -f "$T/bad"
		check_true "fsck.minix notices a lost triple indirect ($fs)" \
		    test "$status" -ne 0
	done
}

# mkfs_readable: images made by mkfs.minix can be read.  Each item is
# the options, then the version and name length they give.
mkfs_readable() {
	for item in "-1 -n 14:1:14" "-1 -n 30:1:30" "-2 -n 14:2:14" \
	    "-2 -n 30:2:30" "-3:3:60"; do
		opts=${item%%:*}
		ver=${item#*:}
		n=${ver#*:}
		ver=${ver%:*}
		dd if=/dev/zero of="$T/img" bs=1024 count=2048 2>/dev/null
		# The options are split on purpose.
		# shellcheck disable=SC2086
		run "$MKFS_MINIX" $opts "$T/img"
		check_status "mkfs.minix $opts succeeds" 0
		what="an image from mkfs.minix $opts"
		check_info "$what has version $ver" "$T/img" version "$ver"
		check_info "$what has $n-character names" "$T/img" \
		    "name length" "$n"
		run "$MINIXFS" ls -lR "$T/img"
		check_status "$what can be listed" 0
	done
}

if have "$FSCK_MINIX"; then
	fsck_accepts
	fsck_triple
else
	skip "fsck.minix accepts the test images" "no $FSCK_MINIX"
fi

if have "$MKFS_MINIX"; then
	mkfs_readable
else
	skip "images from mkfs.minix are readable" "no $MKFS_MINIX"
fi

finish
