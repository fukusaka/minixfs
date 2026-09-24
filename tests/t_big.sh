#!/bin/sh
# Trees whose paths are longer than PATH_MAX.

. ./tests/lib.sh

# The longest line of a file.
longest_line() {
	awk '{ if (length($0) > m) m = length($0) } END { print m + 0 }' "$1"
}

# 140 directories with 30-character names make paths of about 4300
# bytes, longer than PATH_MAX on the systems we know (1024 on NetBSD, 4096
# on Linux).  ls prints them; extract cannot create them and says so.
name=abcdefghijklmnopqrstuvwxyz0123
{
	echo "fs namelen=30 order=le blocks=1024 inodes=160"
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
