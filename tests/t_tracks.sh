#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# -M SIZE:HEADS:SIDE: an image that holds a file system in the tracks of
# one side only, as when a single-sided disk is read as double-sided,
# reads as the file system itself, and fsck_minixfs -y repairs it in
# place without touching the other side.

. ./tests/lib.sh

track=4608				# 9 sectors of 512 bytes
tracks=80				# a 360K disk

# The file system, on a single-sided 360K disk.
spec() {
	cat <<EOF
fs version=1 order=be blocks=360 inodes=64
dir  /bin 0755 2 2 644198400
file /bin/sh 0755 2 2 644198400 34782 1
hard /bin/sh2 /bin/sh
dir  /etc 0755 0 0 644198520
file /etc/passwd 0644 0 0 644198580 2000 2
link /etc/sh.link ../bin/sh 0 0 644199120
dir  /dev 0755 0 0 644199180
dev  /dev/tty0 c 4 0 0620 0 0 644199240
file /big 0644 0 0 644199720 200000 3
EOF
}

# fill N - N bytes of 0xe5, what a format leaves on a track.
fill() {
	LC_ALL=C awk -v n="$1" 'BEGIN { for (i = 0; i < n; i++)
	    printf "%c", 229 }'
}

# spread IMAGE OUT SIDE - OUT holds the tracks of IMAGE on side SIDE of a
# double-sided disk, and tracks of 0xe5, as a format leaves them, on the
# other side.
spread() {
	fill "$track" >"$T/empty"
	: >"$2"
	_spread_t=0
	while [ "$_spread_t" -lt "$tracks" ]; do
		if [ "$3" -eq 1 ]; then
			cat "$T/empty" >>"$2"
		fi
		dd if="$1" bs="$track" skip="$_spread_t" count=1 2>/dev/null \
		    >>"$2"
		if [ "$3" -eq 0 ]; then
			cat "$T/empty" >>"$2"
		fi
		_spread_t=$((_spread_t + 1))
	done
}

# gather IMAGE OUT - the tracks of side 0 of IMAGE, and the other side.
gather() {
	: >"$2"
	: >"$2.other"
	_gather_t=0
	while [ "$_gather_t" -lt "$tracks" ]; do
		dd if="$1" bs="$track" skip=$((2 * _gather_t)) count=1 \
		    2>/dev/null >>"$2"
		dd if="$1" bs="$track" skip=$((2 * _gather_t + 1)) count=1 \
		    2>/dev/null >>"$2.other"
		_gather_t=$((_gather_t + 1))
	done
}

spec >"$T/spec"
mkimage "$T/spec" "$T/img" "$T/exp"
spread "$T/img" "$T/two" 0
check_true "the spread image is twice the size" \
    test "$(($(wc -c <"$T/two")))" -eq $((2 * track * tracks))

run "$FSCK_MINIXFS" "$T/two"
check_status "without -M the spread image is damaged" 1

# info tells what is odd about the spread image, and nothing with -M.
size=$((track * tracks))
check_info "info gives the size of the image" "$T/two" "image size" \
    $((2 * size))
check_info "info notes a file system smaller than the image" "$T/two" \
    "file system size" "$size (50% of the image)"
check_info "info counts the tracks of 0xe5" "$T/two" "fill sectors" \
    "$size bytes, 50% of the image, in $tracks runs"
check_info "info gives the commonest length of them" "$T/two" \
    "commonest fill run" "$track bytes, $tracks times"
run "$MINIXFS" -M "$track:2:0" info "$T/two"
check_out_has "with -M the file system fills the image" \
    "^file system size: $size\$"
check_out_has "with -M there is no fill" \
    "^fill sectors: 0 bytes, 0% of the image, in 0 runs\$"

for side in 0 1; do
	M_OPT="$track:2:$side"
	spread "$T/img" "$T/two" "$side"

	for cmd in info "ls -lR"; do
		# The command is split on purpose.
		# shellcheck disable=SC2086
		run "$MINIXFS" $cmd "$T/img"
		cp "$T/out" "$T/want"
		# shellcheck disable=SC2086
		run "$MINIXFS" -M "$M_OPT" $cmd "$T/two"
		check_out "side $side: $cmd with -M is as without tracks" \
		    "$T/want"
	done

	run "$MINIXFS" -M "$M_OPT" cat "$T/two" /big
	check_out "side $side: cat with -M reads across tracks" "$T/exp/big"

	run "$MINIXFS" blocks -r "$T/img" /big
	cp "$T/out" "$T/want"
	run "$MINIXFS" -M "$M_OPT" blocks -r "$T/two" /big
	check_out "side $side: blocks with -M gives blocks of the file system" \
	    "$T/want"

	rm -rf "$T/x"
	run "$MINIXFS" -M "$M_OPT" extract "$T/two" "$T/x"
	check_status "side $side: extract with -M succeeds" 0
	mkdir -p "$T/exp/dev"
	check_same_tree "side $side: extract with -M gives the tree" \
	    "$T/exp" "$T/x"

	run "$MINIXFS" tar "$T/img"
	cp "$T/out" "$T/want"
	run "$MINIXFS" -M "$M_OPT" tar "$T/two"
	check_out "side $side: tar with -M is that of the file system" \
	    "$T/want"

	run "$FSCK_MINIXFS" -M "$M_OPT" "$T/two"
	check_status "side $side: fsck with -M finds nothing" 0
done

# fsck -y -M repairs the tracks of the file system as fsck -y repairs the
# file system itself, and leaves the other side as it was.
set_map_bit "$T/img" imap 20 1
set_inode "$T/img" 4 nlinks 5
spread "$T/img" "$T/two" 0
run "$FSCK_MINIXFS" -M "$track:2:0" "$T/two"
check_found "fsck -M finds the damage" "inode 20 is free but marked"
run "$FSCK_MINIXFS" -y -M "$track:2:0" "$T/two"
check_status "fsck -y -M repairs it" 0
run "$FSCK_MINIXFS" -y "$T/img"
check_status "fsck -y repairs the file system itself" 0
gather "$T/two" "$T/one"
check_same_file "the repair with -M is the repair of the file system" \
    "$T/img" "$T/one"
fill $((track * tracks)) >"$T/want"
check_same_file "the repair with -M leaves the other side alone" \
    "$T/want" "$T/one.other"

for bad in 0:2:0 4608:2:2 4608:2 4608:x:0 -1:2:0; do
	run "$MINIXFS" -M "$bad" info "$T/two"
	check_status "minixfs -M $bad is a usage error" 2
	run "$FSCK_MINIXFS" -M "$bad" "$T/two"
	check_status "fsck_minixfs -M $bad is a usage error" 2
done

finish
