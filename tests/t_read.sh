#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Read the tree of tests/tree.spec from V1, V2 and V3 images, little- and
# big-endian, with every name length and V3 block size, and with one- and
# two-block zones.

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

# The regular files of the tree.
files="bin/sh bin/login bin/sh2 etc/empty etc/one etc/b1023 etc/b1024
etc/b1025 etc/direct etc/direct1 etc/indirect etc/dindirect usr/a/b/c/deep
usr/big usr/holes usr/sparse 12345678901234"

# A name longer than any version allows.
toolong=1234567890123456789012345678901234567890123456789012345678901

# The names of the tree, in directory order.
grep -v '^#' tests/tree.names >"$T/names"

touch -t 200001010000 "$T/y2000"

# ls, run on the image of the current variant $v.
read_ls() {
	run "$MINIXFS" ls -R "$img"
	check_out "$v: ls -R lists the tree in directory order" "$T/names"

	run "$MINIXFS" ls "$img" /usr/a/b
	echo c >"$T/want"
	check_out "$v: ls of a subdirectory" "$T/want"

	run "$MINIXFS" ls -l "$img" /bin/login
	echo "-rwsr-xr-x   1     0     0      7675 1990-06-01 00:01" \
	    "/bin/login" >"$T/want"
	check_out "$v: ls -l of a set-uid file" "$T/want"

	run "$MINIXFS" ls -l "$img" /bin
	check_out_has "$v: ls -l counts the links of a file" \
	    '^-rwxr-xr-x   2     2     2     34782 .* sh$'
	check_out_has "$v: ls -l shows a hard link like its file" \
	    '^-rwxr-xr-x   2     2     2     34782 .* sh2$'

	run "$MINIXFS" ls -l "$img" /dev
	cat >"$T/want" <<'EOF'
crw--w----   1     0     0    4,   0 1990-06-01 00:14 tty0
brw-rw-rw-   1     0     0    2,   1 1990-06-01 00:15 fd0
prw-------   1     0     0         0 1990-06-01 00:16 fifo
EOF
	check_out "$v: ls -l of devices and a pipe" "$T/want"

	run "$MINIXFS" ls -l "$img" /etc/sh.link
	check_out_has "$v: ls -l shows the target of a link" \
	    '^lrwxrwxrwx .* /etc/sh.link -> \.\./bin/sh$'

	# /usr/a holds one directory, so it has three links.
	run "$MINIXFS" ls -l "$img" /usr
	check_out_has "$v: ls -l counts the links of a directory" \
	    '^drwx------   3     3     4 .* 1990-06-01 00:18 a$'
}

# cat, run on the image of the current variant $v.
read_cat() {
	# $files is a list of names, split on purpose.
	for f in $files; do
		run "$MINIXFS" cat "$img" "/$f"
		check_out "$v: cat /$f" "$exp/$f"
	done

	run "$MINIXFS" cat "$img" usr//a/./b/c/deep
	check_out "$v: cat with a relative path, // and ." \
	    "$exp/usr/a/b/c/deep"

	run "$MINIXFS" cat "$img" /usr/a/b/c/../c/deep
	check_out "$v: cat through .." "$exp/usr/a/b/c/deep"

	run "$MINIXFS" cat "$img" /usr
	check_err "$v: cat of a directory fails" "Is a directory"

	run "$MINIXFS" cat "$img" /dev/tty0
	check_err "$v: cat of a device fails" "not a regular file"

	run "$MINIXFS" cat "$img" /nonexistent
	check_err "$v: cat of a missing file fails" "No such file"

	run "$MINIXFS" cat "$img" /bin/sh/x
	check_err "$v: a file used as a directory fails" "Not a directory"

	run "$MINIXFS" cat "$img" "/$toolong"
	check_err "$v: a name longer than any entry fails" "too long"
}

# Fold the output of blocks into that of blocks -r; a line out of order
# is printed as it is.
fold_runs() {
	awk -F '\t' '
	$1 != NR - 1 { print; next }
	n > 0 && ($2 == "-" ? s == "-" : s != "-" && $2 == s + n) { n++; next }
	{ if (n > 0) print s "\t" n; s = $2; n = 1 }
	END { if (n > 0) print s "\t" n }'
}

# blocks, run on the image of the current variant $v: the blocks listed,
# with zeros for the holes, put together, are the file up to a whole
# block, and the list folds into what -r gives.
read_blocks() {
	for f in bin/sh etc/empty etc/b1025 etc/dindirect usr/holes \
	    usr/sparse; do
		run "$MINIXFS" blocks -r "$img" "/$f"
		check_status "$v: blocks -r /$f" 0
		cp "$T/out" "$T/runs"
		while read -r _b _n; do
			if [ "$_b" = - ]; then
				dd if=/dev/zero bs="$bsize" count="$_n"
			else
				dd if="$img" bs="$bsize" skip="$_b" count="$_n"
			fi
		done <"$T/runs" >"$T/got" 2>/dev/null
		cp "$exp/$f" "$T/want"
		_blocks_r=$(($(wc -c <"$T/want") % bsize))
		if [ "$_blocks_r" -gt 0 ]; then
			dd if=/dev/zero bs=1 count=$((bsize - _blocks_r)) \
			    2>/dev/null >>"$T/want"
		fi
		check_same_file "$v: the blocks of /$f hold the file" \
		    "$T/want" "$T/got"

		run "$MINIXFS" blocks "$img" "/$f"
		fold_runs <"$T/out" >"$T/folded"
		check_same_file "$v: blocks /$f, folded, is blocks -r" \
		    "$T/runs" "$T/folded"
	done

	run "$MINIXFS" blocks -r "$img" /usr/holes
	check_out_has "$v: blocks -r marks the holes" "^-	"
	run "$MINIXFS" blocks "$img" /usr/a
	check_true "$v: blocks of a directory" test "$(wc -l <"$T/out")" -eq 1
	run "$MINIXFS" blocks "$img" /etc/sh.link
	check_true "$v: blocks of a symbolic link" \
	    test "$(wc -l <"$T/out")" -eq 1
	run "$MINIXFS" blocks "$img" /dev/tty0
	check_err "$v: blocks of a device fails" "has no blocks"
}

# extract, run on the image of the current variant $v.
read_extract() {
	_read_extract_made="^16 files, 7 directories, 1 symbolic links,"
	_read_extract_made="$_read_extract_made 1 hard links,"
	rm -rf "$T/x"
	run "$MINIXFS" extract "$img" "$T/x"
	check_note "$v: extract counts what it did" \
	    "$_read_extract_made 0 devices, 1 pipes\$"
	check_note "$v: extract warns of the devices it did not make" \
	    "warning: 2 devices not made; make them with extract -d as root"
	check_true "$v: extract makes a hard link one file" \
	    test "$(inode_of "$T/x/bin/sh")" = "$(inode_of "$T/x/bin/sh2")"
	check_true "$v: extract makes the pipe" test -p "$T/x/dev/fifo"
	check_mode "$v: extract keeps the mode of the pipe" "$T/x/dev/fifo" \
	    prw-------
	check_older "$v: extract sets the time of the pipe" "$T/x/dev/fifo" \
	    "$T/y2000"
	rm "$T/x/dev/fifo"
	check_same_tree "$v: extract reproduces the tree" "$exp" "$T/x"
	check_mode "$v: extract drops set-uid" "$T/x/bin/login" -rwxr-xr-x
	check_mode "$v: extract keeps the mode of a file" "$T/x/etc/one" \
	    -rw-------
	check_mode "$v: extract keeps the mode of a directory" "$T/x/usr/a" \
	    drwx------
	check_older "$v: extract sets the time of a file" "$T/x/bin/sh" \
	    "$T/y2000"
	check_older "$v: extract sets the time of a large file" \
	    "$T/x/etc/indirect" "$T/y2000"
	check_older "$v: extract sets the time of a directory" "$T/x/usr/a" \
	    "$T/y2000"
	check_link "$v: extract makes the symbolic link" "$T/x/etc/sh.link" \
	    ../bin/sh

	rm -rf "$T/x"
	run "$MINIXFS" extract "$img" "$T/x" /usr/a
	check_status "$v: extract of a subdirectory succeeds" 0
	check_same_tree "$v: extract of a subdirectory" "$exp/usr/a" "$T/x"

	run "$MINIXFS" extract "$img" "$T/x" /bin/sh
	check_err "$v: extract of a file fails" "Not a directory"

	# Devices are made with -d, as root only.
	rm -rf "$T/x"
	if [ "$(id -u)" -ne 0 ]; then
		run "$MINIXFS" extract -d "$img" "$T/x"
		check_err "$v: extract -d takes root" "takes root"
		check_true "$v: extract -d does nothing without root" \
		    test ! -e "$T/x"
	fi
	if command -v fakeroot >/dev/null 2>&1; then
		# fakeroot preloads its library, which AddressSanitizer
		# would refuse to follow.
		cmd="\"$MINIXFS\" extract -d \"$img\" \"$T/x\""
		cmd="$cmd && ls -l \"$T/x/dev\""
		ASAN_OPTIONS=verify_asan_link_order=0 fakeroot sh -c "$cmd" \
		    >"$T/out" 2>"$T/err"
		status=$?
		check_note "$v: extract -d as root makes the devices" \
		    "$_read_extract_made 2 devices,"
		check_out_has "$v: the character device has its numbers" \
		    "^crw--w---- .* 4, *0 .*tty0\$"
		check_out_has "$v: the block device has its numbers" \
		    "^brw-rw-rw- .* 2, *1 .*fd0\$"
	else
		skip "$v: extract -d as root" "no fakeroot"
	fi
}

# format: version/name length/block size; "vmd" for Minix-vmd, whose
# flex directories take names of up to 60 characters.
for format in 1/14/1024 1/30/1024 2/14/1024 2/30/1024 3/60/1024 3/60/4096 \
    1/vmd/1024 2/vmd/1024; do
for order in le be; do
for logzone in 0 1; do
	version=${format%%/*}
	namelen=${format#*/}
	namelen=${namelen%/*}
	bsize=${format##*/}
	if [ "$version" -eq 3 ]; then
		fs="version=3 block=$bsize"
	elif [ "$namelen" = vmd ]; then
		fs="version=$version vmd"
		namelen=60
	else
		fs="version=$version namelen=$namelen"
	fi
	# MINIX uses 14-character names in V1 and V2; the 30-character forms
	# are the Linux extensions, with magic numbers of their own.
	# Minix-vmd has the magic numbers of MINIX.
	case $format in
	1/14/*|1/vmd/*)	magic=0x137f ;;
	1/30/*)		magic=0x138f ;;
	2/14/*|2/vmd/*)	magic=0x2468 ;;
	2/30/*)		magic=0x2478 ;;
	*)		magic=0x4d5a ;;
	esac
	if [ "$order" = be ]; then
		byteorder=big-endian
	else
		byteorder=little-endian
	fi

	v="V$format/$order/$logzone"
	img="$T/img"
	exp="$T/expect"
	rm -rf "$exp"
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/$order/" \
	    -e "s/@BLOCKS@/$((4194304 / bsize))/" \
	    -e "s/@LOGZONE@/$logzone/" tests/tree.spec >"$T/spec"
	mkimage "$T/spec" "$img" "$exp"

	check_info "$v: info shows the version" "$img" version "$version"
	check_info "$v: info shows the byte order" "$img" "byte order" \
	    "$byteorder"
	check_info "$v: info shows the magic number" "$img" magic "$magic"
	check_info "$v: info shows the name length" "$img" "name length" \
	    "$namelen"
	check_info "$v: info shows the block size" "$img" "block size" \
	    "$bsize"
	check_info "$v: info shows the zone size" "$img" "log zone size" \
	    "$logzone"

	read_ls
	read_cat
	read_blocks
	read_extract
done
done
done

# extract into a tree that is there: what is in the way goes, as with
# tar(1), and nothing is written through it.  In the first image /a and
# /b are one file, /s a symbolic link and /p a pipe; the second has
# plain files by those names and a file without write permission, which
# a second extract has to replace too.
cat >"$T/first.spec" <<EOF
fs version=2 order=le blocks=256 inodes=16
file /a 0644 0 0 0 100 1
hard /b /a
link /s a 0 0 0
fifo /p 0600 0 0 0
dir  /d 0755 0 0 0
EOF
cat >"$T/second.spec" <<EOF
fs version=2 order=le blocks=256 inodes=16
file /b 0644 0 0 0 50 2
hard /c /b
file /s 0644 0 0 0 10 3
file /p 0644 0 0 0 10 4
file /x 0111 0 0 0 10 5
EOF
mkimage "$T/first.spec" "$T/first.img" "$T/first.exp"
mkimage "$T/second.spec" "$T/second.img" "$T/second.exp"
rm -rf "$T/x"
run "$MINIXFS" extract "$T/first.img" "$T/x"
check_status "a first image extracts" 0
run "$MINIXFS" extract "$T/second.img" "$T/x"
check_status "a second image extracts over the first" 0
check_same_file "the other name of a replaced link keeps its contents" \
    "$T/first.exp/a" "$T/x/a"
check_same_file "a link of the first image is replaced by a file" \
    "$T/second.exp/b" "$T/x/b"
check_true "the new link is a link of the new file" \
    test "$(inode_of "$T/x/b")" = "$(inode_of "$T/x/c")"
check_same_file "a symbolic link is replaced by a file" \
    "$T/second.exp/s" "$T/x/s"
check_same_file "a pipe is replaced by a file" "$T/second.exp/p" "$T/x/p"
run "$MINIXFS" extract "$T/second.img" "$T/x"
check_status "a file without write permission is replaced" 0
cat >"$T/third.spec" <<EOF
fs version=2 order=le blocks=256 inodes=16
file /d 0644 0 0 0 10 6
EOF
mkimage "$T/third.spec" "$T/third.img"
run "$MINIXFS" extract "$T/third.img" "$T/x"
check_err "a directory in the way is not removed" "d: Is a directory"
check_true "and stays a directory" test -d "$T/x/d"

# Names that fill the whole entry have no terminating NUL.
n30=123456789012345678901234567890
for spec in "version=1 namelen=30:$n30" "version=2 namelen=30:$n30" \
    "version=3:$n30$n30"; do
	name=${spec#*:}
	fs=${spec%%:*}
	v="$fs: ${#name}-character names"
	rm -rf "$T/full"
	cat >"$T/spec" <<EOF
fs $fs order=be blocks=256 inodes=16
file /$name 0644 0 0 0 5 1
file /abcdefghijklmn 0644 0 0 0 5 2
EOF
	mkimage "$T/spec" "$T/img" "$T/full"
	run "$MINIXFS" ls "$T/img"
	printf '%s\n' "$name" abcdefghijklmn >"$T/want"
	check_out "$v: a name that fills its entry is listed" "$T/want"
	run "$MINIXFS" cat "$T/img" "/$name"
	check_out "$v: a name that fills its entry is found" "$T/full/$name"
done

finish
