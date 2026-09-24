#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Large sparse files that reach the triple indirect zone, and trees whose
# paths are longer than PATH_MAX.

. ./tests/lib.sh

# The longest line of a file.
longest_line() {
	awk '{ if (length($0) > m) m = length($0) } END { print m + 0 }' "$1"
}

# With 1024-byte blocks and 32-bit zone numbers, the single indirect zone
# covers 256 zones and the double 65536, so the triple indirect zone
# starts at byte (7 + 256 + 65536) * 1024 = 67378176.  Only the last
# zones of the file are allocated.
for fs in "version=2 namelen=14" "version=3"; do
for order in le be; do
	v="$fs/$order"
	rm -rf "$T/exp"
	cat >"$T/spec" <<EOF
fs $fs order=$order blocks=2048 inodes=16
file /tind 0644 0 0 0 67400000 7 0:67380000
file /dind 0644 0 0 0 1000000 8 0:900000
EOF
	mkimage "$T/spec" "$T/img" "$T/exp"
	run "$MINIXFS" cat "$T/img" /tind
	check_out "$v: a file reaching the triple indirect zone" "$T/exp/tind"
	run "$MINIXFS" cat "$T/img" /dind
	check_out "$v: a sparse file in the double indirect zone" \
	    "$T/exp/dind"
	rm -f "$T/out" "$T/exp/tind"
done
done

# 140 directories with 30-character names make paths of about 4300
# bytes, longer than PATH_MAX on the systems we know (1024 on NetBSD, 4096
# on Linux).  ls prints them; extract cannot create them and says so.
name=abcdefghijklmnopqrstuvwxyz0123
{
	echo "fs version=1 namelen=30 order=le blocks=1024 inodes=160"
	path=
	i=0
	while [ "$i" -lt 140 ]; do
		path="$path/$name"
		echo "dir $path 0755 0 0 0"
		i=$((i + 1))
	done
	echo "file $path/leaf 0644 0 0 0 10 1"
} >"$T/spec"
mkimage "$T/spec" "$T/img"

run "$MINIXFS" ls -R "$T/img"
check_status "ls -R lists a tree deeper than PATH_MAX" 0
check_true "ls -R prints paths longer than 4096 bytes" \
    test "$(longest_line "$T/out")" -gt 4096

run "$MINIXFS" extract "$T/img" "$T/deep"
check_err "extract reports paths that are too long" "File name too long"
rm -rf "$T/deep"

finish
