#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# minixfs tar: the test tree of several formats goes into a ustar archive
# that tar(1) reads back: contents, modes, owners, times, devices, pipes,
# symbolic and hard links, and names too long for a plain ustar header.
# Devices cannot be made without privileges, so they are checked in the
# listing and in the headers, not by extracting them.

. ./tests/lib.sh

# The listing of tar -tv, in the C locale that the patterns expect.
list() {
	LC_ALL=C tar -tvf "$1" >"$T/list" 2>&1
}

# listed NAME PATTERN - a line of the listing matches the ERE PATTERN.
listed() {
	check_true "$1" grep -q -E -e "$2" "$T/list"
}

# field ARCHIVE OFFSET LEN - LEN bytes at OFFSET, as text.
field() {
	dd if="$1" bs=1 skip="$2" count="$3" 2>/dev/null
}

if ! command -v tar >/dev/null 2>&1; then
	skip "minixfs tar" "no tar(1) to read the archives"
	finish
fi

for fs in "version=1" "version=2 namelen=30" "version=3 block=4096"; do
for order in le be; do
	v="$fs/$order"
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/$order/" -e "s/@BLOCKS@/4096/" \
	    -e "s/@LOGZONE@/0/" tests/tree.spec >"$T/spec"
	if [ "$fs" = "version=3 block=4096" ]; then
		sed -e "s/blocks=4096/blocks=1024/" "$T/spec" >"$T/spec2"
		mv "$T/spec2" "$T/spec"
	fi
	rm -rf "$T/exp" "$T/x"
	mkimage "$T/spec" "$T/img" "$T/exp"

	run "$MINIXFS" tar "$T/img"
	check_status "$v: tar succeeds" 0
	sum="^16 files, 7 directories, 1 symbolic links, 1 hard links,"
	sum="$sum 3 devices and pipes; 0 sockets skipped\$"
	check_true "$v: tar counts what it stored" grep -q -e "$sum" "$T/err"
	cp "$T/out" "$T/t.tar"
	check_true "$v: the archive is whole records" \
	    test $(($(wc -c <"$T/t.tar") % 10240)) -eq 0

	# The first member is bin/, of owner 2, group 2.
	check_true "$v: the header has the ustar magic" \
	    test "$(field "$T/t.tar" 257 5)" = ustar
	check_true "$v: the header has the owner" \
	    test "$(field "$T/t.tar" 108 7)" = 0000002
	check_true "$v: the header has the time" \
	    test "$(field "$T/t.tar" 136 11)" = "$(printf '%011o' 644198400)"

	list "$T/t.tar"
	listed "$v: tar lists a character device with its numbers" \
	    '^c.* 4, *0 .*dev/tty0$'
	listed "$v: tar lists a block device with its numbers" \
	    '^b.* 2, *1 .*dev/fd0$'
	listed "$v: tar lists a pipe" '^p.*dev/fifo$'
	listed "$v: tar lists a hard link" 'bin/sh2 (link to|==) bin/sh$'
	listed "$v: tar lists a symbolic link" 'etc/sh.link -> \.\./bin/sh$'
	listed "$v: tar lists the set-user-ID bit" '^-rwsr-xr-x .*bin/login$'

	mkdir "$T/x"
	(cd "$T/x" && tar -xf ../t.tar bin etc usr 12345678901234) \
	    >"$T/out" 2>&1
	status=$?
	check_status "$v: tar extracts all but the devices" 0
	mkdir "$T/x/dev"
	check_same_tree "$v: the extracted tree is that of the image" \
	    "$T/exp" "$T/x"
	check_true "$v: the hard link is one file" \
	    test "$(ls -i "$T/x/bin/sh" | awk '{ print $1 }')" = \
	    "$(ls -i "$T/x/bin/sh2" | awk '{ print $1 }')"
done
done

# A directory of the image as the top of the archive.
run "$MINIXFS" tar "$T/img" /usr
cp "$T/out" "$T/t.tar"
LC_ALL=C tar -tf "$T/t.tar" >"$T/list"
# Some tar(1) lists a directory with its "/", some without.
check_true "a directory of the image is the top of the archive" \
    grep -q -x -e 'a/*' "$T/list"
run "$MINIXFS" tar "$T/img" /bin/sh
check_status "a file is not a directory to archive" 1

# Names over 100 bytes go into the prefix, and over 255 bytes into a pax
# header, as does a long symbolic link.  The tree to compare with needs
# paths and a symbolic link of some 400 bytes, and is tried first:
# MINIX 3 makes the paths but takes links of 253 bytes at most, which
# neither getconf(1) nor pathconf(2) tells.
a=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
b=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
p=
for n in 1 2 3 4 5; do
	p="$p/$a$n"
done
if ! mkdir -p "$T/try$p" 2>/dev/null ||
    ! : >"$T/try$p/$b" 2>/dev/null ||
    ! ln -s "${p#/}/$b" "$T/try/l" 2>/dev/null ||
    [ "$(cd "$T/try" && ls -l l | sed 's/.* -> //')" != "${p#/}/$b" ]; then
	skip "tar stores long names" "this system cannot make the tree"
	finish
fi
{
	echo "fs version=3 order=le blocks=2048 inodes=64"
	p=
	for n in 1 2 3 4 5; do
		p="$p/$a$n"
		echo "dir $p 0755 0 0 0"
	done
	echo "file $p/$b 0644 0 0 0 5000 1"
	echo "link /l ${p#/}/$b 0 0 0"
} >"$T/spec"
rm -rf "$T/exp" "$T/x"
mkimage "$T/spec" "$T/img" "$T/exp"
run "$MINIXFS" tar "$T/img"
check_status "tar stores long names" 0
cp "$T/out" "$T/t.tar"
mkdir "$T/x"
(cd "$T/x" && tar -xf ../t.tar) >"$T/out" 2>&1
status=$?
check_status "tar extracts long names" 0
check_same_tree "long names come back whole" "$T/exp" "$T/x"
check_true "a long symbolic link comes back whole" \
    test "$(ls -l "$T/x/l" | sed 's/.* -> //')" = "${p#/}/$b"

finish
