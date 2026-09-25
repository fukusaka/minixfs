#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Mount images with mount_minixfs and read them through the kernel; then
# mount them with -w and change them through the kernel as a directory of
# the host is changed, and the image holds what the host does.  The
# checks need a FUSE build ("make fuse") and the right to mount; without
# them they are skipped.
#
# On MINIX 3, mount_minixfs is a service that mount(8) starts on a vnd
# device ("make fuse-minix", and installed as mount_minixfs(8) says); the
# checks then need it installed as MINIXFS_FUSE was built, and root or
# FUSE_SUDO for vndconfig and mount, and are skipped otherwise.  What only
# a command run by hand can show is not checked there, but that such a
# command shows how to mount; nor is writing, which libpuffs of MINIX 3
# gets wrong (see BUGS in mount_minixfs(8)).
#
#   MINIXFS_FUSE  the mount_minixfs to test (default: ./mount_minixfs)
#   FUSE_SUDO     a command to mount, unmount and read the mount with,
#                 where users cannot mount, such as "sudo" on NetBSD
#                 (default: none)
#   MINIXFS_TYPE  on MINIX 3, the type it is installed as (default:
#                 minixfs, as /usr/pkg/service/minixfs)
#   MINIXFS_VND   on MINIX 3, the vnd device to use (default: vnd0)

. ./tests/lib.sh

: "${MINIXFS_FUSE:=./mount_minixfs}"
: "${FUSE_SUDO:=}"
: "${MINIXFS_TYPE:=minixfs}"
: "${MINIXFS_VND:=vnd0}"

minix=no
if [ "$(uname -s)" = Minix ]; then
	minix=yes
fi

# as_mounter CMD ARGS... - run a command as the user who mounted.
as_mounter() {
	# FUSE_SUDO may be empty or carry options, so it is split on purpose.
	# shellcheck disable=SC2086
	$FUSE_SUDO "$@"
}

# The options of mount_minixfs as MINIX 3 takes them: "-w -u X" as
# "-o rw,update=X".
minix_opts() {
	_minix_opts_o=
	while [ $# -gt 0 ]; do
		case $1 in
		-w)
			_minix_opts_o="$_minix_opts_o,rw"
			;;
		-u)
			_minix_opts_o="$_minix_opts_o,update=$2"
			shift
			;;
		esac
		shift
	done
	if [ -n "$_minix_opts_o" ]; then
		echo "-o ${_minix_opts_o#,}"
	fi
}

# mount_image IMAGE DIR [OPTIONS] - mount in the background and wait until
# the root directory shows the tree.  Sets fuse_pid; returns 1 on failure.
mount_image() {
	fuse_pid=
	if [ "$minix" = yes ]; then
		as_mounter vndconfig "$MINIXFS_VND" "$1" 2>"$T/fuse.err" ||
		    return 1
		# The options are several words or none, split on purpose.
		# shellcheck disable=SC2046
		if ! as_mounter mount -t "$MINIXFS_TYPE" $(minix_opts $3) \
		    "/dev/$MINIXFS_VND" "$2" >"$T/fuse.err" 2>&1; then
			as_mounter vndconfig -u "$MINIXFS_VND"
			return 1
		fi
		return 0
	fi
	# OPTIONS are several words or none, split on purpose.
	# shellcheck disable=SC2086
	as_mounter "$MINIXFS_FUSE" -f $3 "$1" "$2" 2>"$T/fuse.err" &
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

# Stop a mount_minixfs that did not mount.
kill_mount() {
	if [ -n "$fuse_pid" ]; then
		kill "$fuse_pid" 2>/dev/null
	fi
}

# unmount_image DIR - unmount and wait for mount_minixfs to exit, if it
# was started in the foreground (fuse_pid).  Sets
# status to the exit status of the unmount command, and fuse_status to
# that of mount_minixfs.
unmount_image() {
	if [ "$minix" = yes ]; then
		# The service is out of sight: its status is that of umount.
		run as_mounter umount "$1"
		fuse_status=$status
		as_mounter vndconfig -u "$MINIXFS_VND"
		return
	fi
	if command -v fusermount3 >/dev/null 2>&1; then
		run fusermount3 -u "$1"
	elif command -v fusermount >/dev/null 2>&1; then
		run fusermount -u "$1"
	else
		run as_mounter umount "$1"
	fi
	# In the background mount_minixfs is no child of the test.
	fuse_status=0
	if [ -n "$fuse_pid" ]; then
		wait "$fuse_pid"
		fuse_status=$?
	fi
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

	# Last, as find(1) looks up "..", after which librefuse of NetBSD
	# 10.1 has freed the root and the other lookups fail (BUGS in
	# mount_minixfs(8)).
	if [ "$(uname -s)" = NetBSD ]; then
		skip "$v: every name is there" \
		    "librefuse of NetBSD frees the root on a lookup of .."
	else
		as_mounter find "$mnt" ! -path "$mnt" | sed "s|^$mnt/||" |
		    sort >"$T/found"
		check_same_file "$v: every name is there" "$T/sorted" \
		    "$T/found"
	fi
}

if [ ! -x "$MINIXFS_FUSE" ]; then
	skip "mount_minixfs" "no $MINIXFS_FUSE; build it with \"make fuse\""
	finish
fi
# Run by hand on MINIX 3, the service says how to mount.
if [ "$minix" = yes ]; then
	run "$MINIXFS_FUSE" -w "$T/img" "$T/mnt"
	check_status "run by hand, the service exits with 2" 2
	check_true "run by hand, the service shows how to mount" \
	    grep -q "usage: mount -t minixfs" "$T/err"
fi
if [ "$minix" = yes ] &&
    ! cmp -s "$MINIXFS_FUSE" "/usr/pkg/service/$MINIXFS_TYPE"; then
	skip "mount_minixfs" "$MINIXFS_FUSE is not installed as\
 /usr/pkg/service/$MINIXFS_TYPE; see mount_minixfs(8)"
	finish
fi
if [ "$minix" = yes ] && [ "$(id -u)" -ne 0 ] && [ -z "$FUSE_SUDO" ]; then
	skip "mount_minixfs" "mounting takes root on MINIX 3; see FUSE_SUDO"
	finish
fi

grep -v '^#' tests/tree.names | sort >"$T/sorted"
touch -t 200001010000 "$T/y2000"
mnt="$T/mnt"
mkdir "$mnt"

dd if=/dev/zero of="$T/zero" bs=1024 count=64 2>/dev/null
# On MINIX 3, mount_minixfs runs only as a service.
if [ "$minix" = no ]; then
	run "$MINIXFS_FUSE" "$T/zero" "$mnt"
	check_err "a file of zeros is refused" "not a MINIX file system"

	run "$MINIXFS_FUSE" "$T/zero"
	check_status "no mount point is a usage error" 2
fi

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
		kill_mount
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

# Writing.  The same commands run on the mount and on a copy of the tree
# on the host; the image is then a copy of that.
change() {
	# The copy on the host is changed by who runs the test, not through
	# FUSE_SUDO, so that it can read and remove it.
	for _change_d in "$mnt" "$H"; do
		_change_as=as_mounter
		if [ "$_change_d" = "$H" ]; then
			_change_as=
		fi
		$_change_as sh -c '
			cd "$1" || exit 1
			mkdir -p new/deep &&
			cp "$2" new/data &&
			ln new/data link &&
			ln -s new/data sym &&
			mv new/deep moved &&
			mv etc/b1025 moved/b &&
			mkfifo fifo &&
			chmod 0600 link &&
			dd if=/dev/null of=usr/big bs=1 seek=5000 \
			    2>/dev/null &&
			dd if=/dev/null of=etc/one bs=1 seek=300000 \
			    2>/dev/null &&
			echo more >>new/data &&
			rm bin/sh etc/sh.link &&
			rm -r usr/a/b/c &&
			touch -t 200101010000 moved
		' sh "$_change_d" "$T/zero" || return 1
	done
}

# writable NAME NEWFS-OPTIONS FLUSH STATUS - make the test tree into a
# file system with NEWFS-OPTIONS, mount it with -w -u FLUSH, change it,
# and check it: while mounted, fsck exits with STATUS, as the maps are
# behind the inodes or not.
writable() {
	v="-w $1"
	exp="$T/exp"
	H="$T/host"
	rm -rf "$exp" "$H" "$T/img"
	sed -e "s/@FS@/version=2/" -e "s/@ORDER@/le/" -e "s/@BLOCKS@/4096/" \
	    -e "s/@LOGZONE@/0/" tests/tree.spec >"$T/spec"
	mkimage "$T/spec" "$T/tree.img" "$exp"
	# Owned by who runs the test, so that the mount may be written.
	# NEWFS-OPTIONS are several words, split on purpose.
	# shellcheck disable=SC2086
	run "$NEWFS_MINIXFS" $2 -d "$exp" -s 8192 "$T/img"
	cp -R "$exp" "$H"
	if ! mount_image "$T/img" "$mnt" "-w -u $3"; then
		kill_mount
		fail "$v: the image is mounted for writing" \
		    "$(head -5 "$T/fuse.err")"
		return
	fi
	check_true "$v: the image is not marked clean while mounted" \
	    test "$(info_field "$T/img" clean)" = no
	run "$TUNEFS_MINIXFS" -c clean "$T/img"
	check_err "$v: another writer is refused" "busy"
	check_true "$v: the tree is changed through the kernel" change
	# -u seconds writes the maps with the first change after them.
	case $3 in
	[0-9]*)
		sleep $(($3 + 1))
		as_mounter touch "$mnt/late" "$H/late"
		;;
	esac
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$v: -u $3, while mounted: fsck exits with $4" "$4"
	unmount_image "$mnt"
	check_true "$v: mount_minixfs exits cleanly" test "$fuse_status" -eq 0
	check_true "$v: unmounted, the image is marked clean" \
	    test "$(info_field "$T/img" clean)" = yes
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$v: fsck finds nothing wrong" 0
	rm -rf "$T/x"
	run "$MINIXFS" extract "$T/img" "$T/x"
	tree_listing "$H" >"$T/want"
	tree_listing "$T/x" >"$T/got"
	check_same_file "$v: the image holds what the host does" \
	    "$T/want" "$T/got"
}

if [ "$minix" = yes ]; then
	skip "mount_minixfs -w" "writing is not reliable on MINIX 3; see\
 BUGS in mount_minixfs(8)"
else
	# V1 keeps a group of a byte: the tree gets group 0.
	writable "V1/le" "-V 1 -o $(id -u):0" sync 1
	writable "V2/be, 30-character names" "-V 2 -l 30 -B be" always 0
	writable "V3/le" "-V 3" 1 0

	# In the background, as without -f, mount_minixfs returns once the
	# file system is mounted, and still holds the lock.
	rm -rf "$T/bg.img" "$T/empty"
	mkdir "$T/empty"
	"$NEWFS_MINIXFS" -V 2 -s 1000 -d "$T/empty" -o "$(id -u):$(id -g)" \
	    "$T/bg.img" >/dev/null
	# A set-group-ID directory of another group.
	group=1
	if [ "$(id -g)" -eq 1 ]; then
		group=2
	fi
	printf 'mkdir /d 0777\nchown /d %s %s\nchmod /d 2777\n' \
	    "$(id -u)" "$group" | "$MFSOP" "$T/bg.img" >/dev/null
	run as_mounter "$MINIXFS_FUSE" -w "$T/bg.img" "$mnt"
	check_status "-w in the background: mount_minixfs returns" 0
	check_true "-w in the background: the mount can be written" \
	    can_write "$mnt/new"
	as_mounter mkdir "$mnt/d/sub"
	check_true "-w, set-group-ID directory: a file takes its group" \
	    test "$(can_write "$mnt/d/f" && field_of "$mnt/d/f" 5)" = "$group"
	check_true "-w, set-group-ID directory: so does a directory" \
	    test "$(field_of "$mnt/d/sub" 5)" = "$group"
	# librefuse of NetBSD sets the mode that the kernel gave again.
	if [ "$(uname -s)" = NetBSD ]; then
		skip "-w, set-group-ID directory: which is set-group-ID too" \
		    "librefuse of NetBSD takes the bit away"
	else
		check_true \
		    "-w, set-group-ID directory: which is set-group-ID too" \
		    test "$(as_mounter ls -ld "$mnt/d/sub" | cut -c7 |
		    tr S s)" = s
	fi
	run "$TUNEFS_MINIXFS" -c clean "$T/bg.img"
	check_err "-w in the background: another writer is refused" "busy"
	fuse_pid=
	unmount_image "$mnt"
	check_status "-w in the background: the mount is unmounted" 0
	# The unmount does not wait for mount_minixfs to write the image.
	n=0
	while [ "$(info_field "$T/bg.img" clean)" != yes ] && [ "$n" -lt 10 ]; do
		sleep 1
		n=$((n + 1))
	done
	run "$FSCK_MINIXFS" "$T/bg.img"
	check_status "-w in the background: fsck finds nothing wrong" 0
	check_true "-w in the background: the new file is there" \
	    "$MINIXFS" cat "$T/bg.img" /new
fi

# A file system not marked clean is mounted read-only.
run "$TUNEFS_MINIXFS" -c dirty "$T/img"
if mount_image "$T/img" "$mnt" -w; then
	# A service of MINIX 3 has no standard error to warn on.
	if [ "$minix" = no ]; then
		check_true "-w, not clean: the warning says so" \
		    grep -q "not marked clean; check it with fsck_minixfs" \
		    "$T/fuse.err"
	fi
	check_true "-w, not clean: the mount cannot be written" \
	    test "$(can_write "$mnt/new" && echo yes)" != yes
	unmount_image "$mnt"
else
	kill_mount
	fail "-w, not clean: the image is mounted" "$(head -5 "$T/fuse.err")"
fi
if [ "$minix" = no ]; then
	run "$MINIXFS_FUSE" -u always "$T/img" "$mnt"
	check_status "-u without -w is a usage error" 2
	run "$MINIXFS_FUSE" -o update=always "$T/img" "$mnt"
	check_status "update= without rw is a usage error" 2
fi

finish
