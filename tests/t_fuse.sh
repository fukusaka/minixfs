#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Mount images with mount_minixfs and read them through the kernel.  The
# checks need a FUSE build ("make fuse") and the right to mount; without
# them they are skipped.
#
#   MINIXFS_FUSE  the mount_minixfs to test (default: ./mount_minixfs)
#   FUSE_SUDO     a command to mount, unmount and read the mount with,
#                 where users cannot mount, such as "sudo" on NetBSD
#                 (default: none)

. ./tests/lib.sh

: "${MINIXFS_FUSE:=./mount_minixfs}"
: "${FUSE_SUDO:=}"

# as_mounter CMD ARGS... - run a command as the user who mounted.
as_mounter() {
	# FUSE_SUDO may be empty or carry options, so it is split on purpose.
	# shellcheck disable=SC2086
	$FUSE_SUDO "$@"
}

# mount_image IMAGE DIR - mount in the background and wait until the
# root directory shows the tree.  Sets fuse_pid; returns 1 on failure.
mount_image() {
	as_mounter "$MINIXFS_FUSE" -f "$1" "$2" 2>"$T/fuse.err" &
	fuse_pid=$!
	_mount_image_i=0
	while [ "$_mount_image_i" -lt 10 ]; do
		if as_mounter test -d "$2/bin"; then
			return 0
		fi
		sleep 1
		_mount_image_i=$((_mount_image_i + 1))
	done
	return 1
}

# unmount_image DIR - unmount and wait for mount_minixfs to exit.  Sets
# status to the exit status of the unmount command, and fuse_status to
# that of mount_minixfs.
unmount_image() {
	if command -v fusermount3 >/dev/null 2>&1; then
		run fusermount3 -u "$1"
	elif command -v fusermount >/dev/null 2>&1; then
		run fusermount -u "$1"
	else
		run as_mounter umount "$1"
	fi
	wait "$fuse_pid"
	fuse_status=$?
}

# field_of FILE N - field N of "ls -lin FILE".
field_of() {
	as_mounter ls -lind "$1" | awk -v n="$2" '{ print $n }'
}

# can_write FILE - does creating FILE succeed?
can_write() {
	as_mounter sh -c ': >"$1"' sh "$1" 2>/dev/null
}

# read_mount: the checks on the mount $mnt of variant $v.
read_mount() {
	as_mounter find "$mnt" ! -path "$mnt" | sed "s|^$mnt/||" | sort \
	    >"$T/found"
	check_same_file "$v: every name is there" "$T/sorted" "$T/found"

	# $T/files is a list of names, split on purpose.
	for f in $(cat "$T/files"); do
		as_mounter cat "$mnt/$f" >"$T/got"
		check_same_file "$v: /$f reads back" "$exp/$f" "$T/got"
	done

	check_mode "$v: a set-uid file keeps its mode" "$mnt/bin/login" \
	    -rwsr-xr-x
	check_mode "$v: a directory keeps its mode" "$mnt/usr/a" drwx------
	check_mode "$v: a character device" "$mnt/dev/tty0" crw--w----
	check_mode "$v: a block device" "$mnt/dev/fd0" brw-rw-rw-
	check_mode "$v: a pipe" "$mnt/dev/fifo" prw-------
	check_link "$v: a symbolic link" "$mnt/etc/sh.link" ../bin/sh
	check_older "$v: files keep their time" "$mnt/bin/sh" "$T/y2000"

	# ls -lin: inode, mode, links, owner, group, then size or device.
	check_true "$v: the inode number is that of the image" \
	    test "$(field_of "$mnt/bin/sh" 1)" -eq 3
	check_true "$v: a hard link has the same inode" \
	    test "$(field_of "$mnt/bin/sh2" 1)" -eq 3
	check_true "$v: links are counted" \
	    test "$(field_of "$mnt/bin/sh" 3)" -eq 2
	check_true "$v: the owner is that of the image" \
	    test "$(field_of "$mnt/usr/a" 4)" -eq 3
	check_true "$v: the group is that of the image" \
	    test "$(field_of "$mnt/usr/a" 5)" -eq 4
	check_true "$v: the device number is that of the image" \
	    test "$(field_of "$mnt/dev/fd0" 6)" = "2,"

	# df -P: file system, size, used, available, capacity, mount point.
	check_true "$v: df shows free space" \
	    test "$(as_mounter df -P "$mnt" | awk 'NR == 2 { print $4 }')" \
	    -gt 0

	check_true "$v: the mount cannot be written" \
	    test "$(can_write "$mnt/new" && echo yes)" != yes
}

if [ ! -x "$MINIXFS_FUSE" ]; then
	skip "mount_minixfs" "no $MINIXFS_FUSE; build it with \"make fuse\""
	finish
fi

grep -v '^#' tests/tree.names | sort >"$T/sorted"
touch -t 200001010000 "$T/y2000"
mnt="$T/mnt"
mkdir "$mnt"

dd if=/dev/zero of="$T/zero" bs=1024 count=64 2>/dev/null
run "$MINIXFS_FUSE" "$T/zero" "$mnt"
check_err "a file of zeros is refused" "not a MINIX file system"

run "$MINIXFS_FUSE" "$T/zero"
check_status "no mount point is a usage error" 2

mounted=0
for variant in "version=1 namelen=14:be:1024" \
    "version=2 namelen=30:le:1024" "version=3 block=4096:be:4096" \
    "version=3:le:1024"; do
	fs=${variant%%:*}
	order=${variant#*:}
	bsize=${order#*:}
	order=${order%:*}
	v="$fs/$order"
	exp="$T/exp"
	rm -rf "$exp"
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/$order/" \
	    -e "s/@BLOCKS@/$((4194304 / bsize))/" -e "s/@LOGZONE@/0/" \
	    tests/tree.spec >"$T/spec"
	mkimage "$T/spec" "$T/img" "$exp"
	(cd "$exp" && find . -type f | sed 's|^\./||' | sort) >"$T/files"

	if ! mount_image "$T/img" "$mnt"; then
		kill "$fuse_pid" 2>/dev/null
		if [ "$mounted" -eq 0 ]; then
			why=$(head -1 "$T/fuse.err")
			skip "mount_minixfs" "cannot mount: $why"
			finish
		fi
		fail "$v: the image is mounted" "$(head -5 "$T/fuse.err")"
		continue
	fi
	mounted=1
	read_mount
	unmount_image "$mnt"
	check_status "$v: the mount is unmounted" 0
	check_true "$v: mount_minixfs exits cleanly" test "$fuse_status" -eq 0
done

finish
