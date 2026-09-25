#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Images that are not MINIX file systems, and damaged file systems of
# every version.  Every case must end in an error message and exit status
# 1, never in a crash.

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

# Files that are not file systems.
not_file_systems() {
	: >"$T/empty"
	run "$MINIXFS" info "$T/empty"
	check_err "an empty file is refused" "not a MINIX file system"

	dd if=/dev/zero of="$T/short" bs=1 count=1500 2>/dev/null
	run "$MINIXFS" info "$T/short"
	check_err "a file shorter than a super block is refused" \
	    "not a MINIX file system"

	dd if=/dev/zero of="$T/zero" bs=1024 count=64 2>/dev/null
	run "$MINIXFS" ls "$T/zero"
	check_err "a file of zeros is refused" "not a MINIX file system"

	run "$MINIXFS" info "$T/nonexistent"
	check_err "a missing image is reported" "No such file"

	# A magic number alone does not make a file system.  The magic is at
	# byte 16 of the super block in V1 and V2, and at byte 24 in V3.
	for order in 1 0; do
		for m in 1040:0x137f 1040:0x2468 1040:0x2478 1048:0x4d5a; do
			cp "$T/zero" "$T/img"
			magic=$((${m#*:}))
			if [ "$order" -eq 1 ]; then
				hi=$((magic >> 8))
				lo=$((magic & 255))
				o=be
			else
				hi=$((magic & 255))
				lo=$((magic >> 8))
				o=le
			fi
			poke "$T/img" "${m%%:*}" "$(printf '%03o' "$hi")" \
			    "$(printf '%03o' "$lo")"
			run "$MINIXFS" info "$T/img"
			check_err "$o: magic ${m#*:} alone is refused" \
			    "not a MINIX file system"
		done
	done
}

usage_errors() {
	run "$MINIXFS" frobnicate "$T/zero"
	check_status "an unknown command is a usage error" 2

	run "$MINIXFS" ls -x "$T/zero"
	check_status "an unknown option is a usage error" 2

	run "$MINIXFS"
	check_status "no command is a usage error" 2
}

# damaged_super: super blocks that do not add up, on $good of variant $v.
damaged_super() {
	cp "$T/good" "$T/img"
	set_super "$T/img" ninodes 0
	run "$MINIXFS" info "$T/img"
	check_err "$v: a super block without inodes is refused" \
	    "not a MINIX file system"

	cp "$T/good" "$T/img"
	set_super "$T/img" ninodes 60000
	run "$MINIXFS" info "$T/img"
	check_err "$v: more inodes than the map covers are refused" \
	    "not a MINIX file system"

	cp "$T/good" "$T/img"
	set_super "$T/img" zones 0
	run "$MINIXFS" info "$T/img"
	check_err "$v: a file system without zones is refused" \
	    "not a MINIX file system"

	cp "$T/good" "$T/img"
	set_super "$T/img" firstdata 2000
	run "$MINIXFS" info "$T/img"
	check_err "$v: a first data zone past the end is refused" \
	    "not a MINIX file system"

	cp "$T/good" "$T/img"
	set_super "$T/img" firstdata 3
	run "$MINIXFS" info "$T/img"
	check_err "$v: data zones over the inode table are refused" \
	    "not a MINIX file system"

	cp "$T/good" "$T/img"
	set_super "$T/img" logzone 30
	run "$MINIXFS" info "$T/img"
	check_err "$v: an absurd zone size is refused" \
	    "not a MINIX file system"
}

# damaged_v3: what only V3 has.
damaged_v3() {
	for bs in 0 512 1000; do
		cp "$T/good" "$T/img"
		set_super "$T/img" blocksize "$bs"
		run "$MINIXFS" info "$T/img"
		check_err "$v: block size $bs is refused" \
		    "not a MINIX file system"
	done

	# MINIX 3 writes 0 when the first data zone does not fit in 16
	# bits, and computes it from the layout.
	cp "$T/good" "$T/img"
	set_super "$T/img" firstdata 0
	run "$MINIXFS" cat "$T/img" /big
	check_out "$v: a first data zone of 0 is computed" "$T/exp/big"
}

# damaged_inodes: zone numbers and sizes that cannot be right.  The spec
# gives /f inode 4, /big (with a single indirect zone) 5 and /g 6.
damaged_inodes() {
	cp "$T/good" "$T/img"
	set_inode "$T/img" 4 zone0 60000
	run "$MINIXFS" cat "$T/img" /f
	check_err "$v: a zone past the end is an I/O error" \
	    "Input/output error"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 4 zone0 1
	run "$MINIXFS" cat "$T/img" /f
	check_err "$v: a zone below the data area is an I/O error" \
	    "Input/output error"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 5 zone7 2
	run "$MINIXFS" cat "$T/img" /big
	check_err "$v: an indirect zone below the data area fails" \
	    "Input/output error"

	# Only V1 limits the size of a file to less than 4 GiB.
	if [ "$version" -eq 1 ]; then
		cp "$T/good" "$T/img"
		set_inode "$T/img" 6 size 2147483647
		run "$MINIXFS" cat "$T/img" /g
		check_err "$v: an impossible file size is an I/O error" \
		    "Input/output error"
	fi
}

# damaged_dirs: directories that point to the wrong places.
damaged_dirs() {
	cat >"$T/spec2" <<EOF
fs $fs order=$order blocks=256 inodes=16
file /a 0644 0 0 0 10 1
raw  / bad 999
file /z 0644 0 0 0 10 2
EOF
	mkimage "$T/spec2" "$T/img"
	run "$MINIXFS" ls -R "$T/img"
	check_err "$v: an entry with a bad inode number is reported" \
	    "bad: inode 999: Input/output error"
	printf 'a\nz\n' >"$T/want"
	check_same_file "$v: the other entries are still listed" "$T/want" \
	    "$T/out"

	cat >"$T/spec2" <<EOF
fs $fs order=$order blocks=256 inodes=16
dir  /d 0755 0 0 0
dir  /d/e 0755 0 0 0
raw  /d/e up 2
EOF
	mkimage "$T/spec2" "$T/img"
	run "$MINIXFS" ls -R "$T/img"
	check_err "$v: ls -R stops at a directory loop" "directory loop"
	run "$MINIXFS" extract "$T/img" "$T/loop"
	check_err "$v: extract stops at a directory loop" "directory loop"
	rm -rf "$T/loop"

	# Names that would escape the destination.
	mkdir "$T/cage"
	cat >"$T/spec2" <<EOF
fs $fs order=$order blocks=256 inodes=16
file /f 0644 0 0 0 10 1
dir  /d 0755 0 0 0
raw  /d ../../esc 2
raw  / a/b 2
raw  / .. 2
raw  / . 2
EOF
	mkimage "$T/spec2" "$T/img"
	run "$MINIXFS" extract "$T/img" "$T/cage/out"
	check_err "$v: unsafe names are refused" "unsafe name"
	check_contents "$v: nothing is written outside the destination" \
	    "$T/cage" "out out/d out/f"
	rm -rf "$T/cage"
}

# truncated: an image cut short after the directories.
truncated() {
	bs=$(info_field "$T/good" "block size")
	first=$(info_field "$T/good" "first data zone")
	dd if="$T/good" of="$T/img" bs="$bs" count=$((first + 4)) 2>/dev/null
	run "$MINIXFS" ls "$T/img"
	printf 'd\nf\nbig\ng\n' >"$T/want"
	check_out "$v: a truncated image can still be listed" "$T/want"
	run "$MINIXFS" cat "$T/img" /big
	check_err "$v: data past the end of a truncated image fails" \
	    "Input/output error"

	run "$MINIXFS" ls "$T/good" /f/x
	check_err "$v: a path through a file fails" "Not a directory"
}

not_file_systems
usage_errors

for fs in "version=1" "version=2" "version=3" "version=3 block=4096"; do
for order in le be; do
	v="$fs/$order"
	version=${fs#version=}
	version=${version%% *}
	cat >"$T/spec" <<EOF
fs $fs order=$order blocks=1024 inodes=64
dir  /d 0755 0 0 0
dir  /d/e 0755 0 0 0
file /f 0644 0 0 0 3000 1
file /big 0644 0 0 0 40000 2
file /g 0644 0 0 0 10 3
EOF
	rm -rf "$T/exp"
	mkimage "$T/spec" "$T/good" "$T/exp"

	damaged_super
	if [ "$version" -eq 3 ]; then
		damaged_v3
	fi
	damaged_inodes
	damaged_dirs
	truncated
done
done

finish
