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
	check_info "V1 is marked clean as Linux marks it" "$T/img" clean yes

	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 3 -s 2048 "$T/img"
	check_status "V3 with defaults succeeds" 0
	check_info "V3 has 4096-byte blocks by default" "$T/img" \
	    "block size" 4096
	check_info "V3 inodes fill the blocks of the inode table" "$T/img" \
	    inodes 704
	check_info "V3 has 60-character names" "$T/img" "name length" 60
	check_info "V3 is marked clean, or MINIX 3 mounts it read-only" \
	    "$T/img" clean yes

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

# ls -lR in the order of the names, without the link counts and owners
# and without /dev; with set-user-ID dropped and without the times of
# symbolic links, as extract leaves them: what a copy must keep.
listing() {
	"$MINIXFS" ls -lR "$1" | sed -e 's/rws/rwx/' | awk '!/ dev(\/|$)/ {
		$2 = ""; $3 = ""; $4 = ""
		if ($1 ~ /^l/)
			$6 = $7 = ""
		print $8, $0 }' | sort
}

# -d: the test tree, taken out of an image by extract with its modes and
# times, goes into every format and comes back as it was.
from_tree() {
	for fs in "1 14 le" "1 30 be" "2 14 be" "2 30 le" "3 60 le" \
	    "3 60 be"; do
		set -- $fs
		v="V$1/$2/$3"
		opts="-V $1 -B $3"
		spec="version=$1 order=$3"
		if [ "$1" -ne 3 ]; then
			opts="$opts -l $2"
			spec="$spec namelen=$2"
		fi
		sed -e "s/@FS@/$spec/" -e "s/@ORDER@/$3/" \
		    -e "s/@BLOCKS@/4096/" -e "s/@LOGZONE@/0/" \
		    -e "s/order=$3 order=$3/order=$3/" tests/tree.spec \
		    >"$T/spec"
		rm -rf "$T/exp" "$T/x"
		mkimage "$T/spec" "$T/want.img"
		"$MINIXFS" extract "$T/want.img" "$T/exp" 2>/dev/null
		rm -f "$T/img"
		# The options are split on purpose.
		# shellcheck disable=SC2086
		run "$NEWFS_MINIXFS" $opts -d "$T/exp" -o 0:0 "$T/img"
		check_status "$v: -d makes an image of the tree" 0
		run "$FSCK_MINIXFS" "$T/img"
		check_status "$v: fsck passes the image of the tree" 0
		run "$MINIXFS" extract "$T/img" "$T/x"
		check_true "$v: the pipe comes back out" test -p "$T/x/dev/fifo"
		# diff(1) cannot compare pipes.
		rm -f "$T/exp/dev/fifo" "$T/x/dev/fifo"
		check_same_tree "$v: the tree comes back out" "$T/exp" "$T/x"
		listing "$T/want.img" >"$T/want"
		listing "$T/img" >"$T/got"
		check_same_file "$v: modes, sizes and times are kept" \
		    "$T/want" "$T/got"
	done
}

# -d: hard links, symbolic links, pipes, owners and sizes.
tree_kinds() {
	rm -rf "$T/t"
	mkdir -p "$T/t/d"
	echo hello >"$T/t/a"
	ln "$T/t/a" "$T/t/d/b"
	awk 'BEGIN { for (i = 0; i < 40000; i++) printf "%c", 65 + i % 26 }' \
	    >"$T/t/big"
	ln -s a "$T/t/s"
	mkfifo "$T/t/p"
	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 2 -d "$T/t" "$T/img"
	check_status "-d copies links and pipes" 0
	run "$MINIXFS" ls -l "$T/img" /d/b
	check_out_has "a hard link stays one file" "^-rw-.*  *2 "
	run "$MINIXFS" ls -l "$T/img" /s
	check_out_has "a symbolic link keeps its text" "/s -> a\$"
	run "$MINIXFS" ls -l "$T/img" /p
	check_out_has "a pipe stays a pipe" "^p"
	check_info "without -s the image is just large enough" "$T/img" \
	    "free zones" 0

	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 2 -d "$T/t" -o 7:3 "$T/img"
	run "$MINIXFS" ls -l "$T/img" /a
	check_out_has "-o gives every file its owner" "^-rw-.*  *7  *3 "

	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 2 -d "$T/t" -N "$T/img"
	check_out_has "-N -d says what the tree needs" "^the tree needs: "
	check_true "-N -d makes no image" test ! -e "$T/img"

	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 2 -d "$T/t" -s 20 "$T/img"
	check_err "a size too small is refused" "give more with -s"
	check_true "an image too small is not made" test ! -e "$T/img"

	mkdir "$T/t/a_name_of_twenty_c"
	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 1 -d "$T/t" -o 0:0 "$T/img"
	check_err "names too long for V1 are refused" \
	    "a_name_of_twenty_c: name longer than 14 characters"
	check_true "nothing is made for names too long" test ! -e "$T/img"
	rmdir "$T/t/a_name_of_twenty_c"

	if [ "$(id -g)" -gt 255 ]; then
		run "$NEWFS_MINIXFS" -V 1 -d "$T/t" "$T/img"
		check_err "a group too large for V1 is refused" \
		    "does not fit in V1"
		check_true "the refusal says what to do" \
		    grep -q -e "can be set with -o uid:gid" "$T/err"
	fi
	run "$NEWFS_MINIXFS" -V 2 -o 0:0 -s 100 "$T/img"
	check_status "-o without -d is a usage error" 2

	if command -v fakeroot >/dev/null 2>&1; then
		rm -f "$T/img"
		cmd="mknod \"$T/t/c\" c 4 0 && mknod \"$T/t/b\" b 2 1 &&"
		cmd="$cmd \"$NEWFS_MINIXFS\" -V 2 -d \"$T/t\" \"$T/img\""
		# fakeroot preloads its library, which AddressSanitizer
		# would refuse to follow.
		ASAN_OPTIONS=verify_asan_link_order=0 fakeroot sh -c "$cmd" \
		    >"$T/out" 2>"$T/err"
		status=$?
		check_status "-d copies devices" 0
		run "$MINIXFS" ls -l "$T/img" /c
		check_out_has "a character device keeps its numbers" \
		    "^c.* 4, *0 "
		run "$MINIXFS" ls -l "$T/img" /b
		check_out_has "a block device keeps its numbers" "^b.* 2, *1 "
	else
		skip "-d copies devices" "no fakeroot"
	fi
}

# -F: an mtree(8) specification in both forms overrides and adds, with
# names from the files of -P; -x leaves out what it does not name.
from_spec() {
	rm -rf "$T/t" "$T/db"
	mkdir -p "$T/t/bin" "$T/t/etc" "$T/db"
	echo sh >"$T/t/bin/sh"
	echo pw >"$T/t/etc/passwd"
	echo junk >"$T/t/junk"
	printf 'root:*:0:0::/root:/bin/sh\nbin:*:3:7::/bin:/sbin/nologin\n' \
	    >"$T/db/master.passwd"
	printf 'wheel:*:0:root\noperator:*:5:root\nbin:*:7:\n' >"$T/db/group"
	cat >"$T/spec" <<EOF
# full paths, as in a METALOG, and relative ones
/set type=file uname=root gname=wheel mode=0644
. type=dir mode=0755
./bin type=dir mode=0755
./bin/sh mode=0555 uname=bin gname=bin time=1234567890.123456789 size=3
./dev type=dir mode=0755
./dev/tty0 type=char mode=0620 gname=operator device=native,4,0
./dev/fd0 type=block mode=0666 device=netbsd,2,1
./dev/opt type=char mode=0600 device=native,9,9 optional
./etc type=dir mode=0755
    passwd mode=0600
    motd mode=0644
    sh.link type=link mode=0777 link=../bin/sh
    with\040space mode=0644
..
./var type=dir mode=0755
./var/run type=fifo mode=0600
EOF
	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 3 -d "$T/t" -F "$T/spec" -P "$T/db" "$T/img"
	check_status "-F makes an image" 0
	run "$FSCK_MINIXFS" "$T/img"
	check_status "fsck passes the image of -F" 0
	run "$MINIXFS" ls -lR "$T/img"
	check_out_has "-F sets the mode, owner, group and time" \
	    "^-r-xr-xr-x   1     3     7         3 2009-02-13 23:31 bin/sh\$"
	check_out_has "-F adds a character device" \
	    "^crw--w----   1     0     5    4,   0 .* dev/tty0\$"
	check_out_has "-F adds a block device" \
	    "^brw-rw-rw- .* 2,   1 .* dev/fd0\$"
	check_true "-F leaves out an optional entry" \
	    test "$(grep -c 'dev/opt' "$T/out")" -eq 0
	check_out_has "-F overrides a host file" \
	    "^-rw-------   1     0     0         3 .* etc/passwd\$"
	check_out_has "-F adds an empty file" \
	    "^-rw-r--r--   1     0     0         0 .* etc/motd\$"
	check_out_has "-F adds a symbolic link" " etc/sh.link -> \.\./bin/sh\$"
	check_out_has "-F takes escaped names" " etc/with space\$"
	check_out_has "-F adds a pipe" "^prw------- .* var/run\$"
	check_out_has "-F keeps what it does not name" " junk\$"

	rm -f "$T/img"
	run "$NEWFS_MINIXFS" -V 3 -d "$T/t" -F "$T/spec" -P "$T/db" -x "$T/img"
	check_status "-x makes an image" 0
	run "$MINIXFS" ls "$T/img"
	printf 'bin\ndev\netc\nvar\n' >"$T/want"
	check_out "-x leaves out what the specification does not name" \
	    "$T/want"

	for bad in "./junk type=dir mode=0755 uname=root gname=wheel" \
	    "./new type=file uname=root gname=wheel" \
	    "./new/x type=file mode=0644 uname=root gname=wheel" \
	    "./bin/sh colour=red" "./bin/sh mode=u+x" \
	    "./bin/sh uname=nobody-at-all"; do
		printf '. type=dir mode=0755\n%s\n' "$bad" >"$T/bad"
		rm -f "$T/img"
		run "$NEWFS_MINIXFS" -V 3 -d "$T/t" -F "$T/bad" -P "$T/db" \
		    "$T/img"
		check_status "-F refuses: $bad" 1
		check_true "nothing is made for: $bad" test ! -e "$T/img"
	done

	run "$NEWFS_MINIXFS" -V 3 -F "$T/spec" -s 100 "$T/img"
	check_status "-F without -d is a usage error" 2
	run "$NEWFS_MINIXFS" -V 3 -d "$T/t" -x -s 100 "$T/img"
	check_status "-x without -F is a usage error" 2
}

formats
defaults
sizes
refusals
from_tree
tree_kinds
from_spec

finish
