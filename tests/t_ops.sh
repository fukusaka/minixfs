#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Changing names and files through the library (mfs_ops.c, mfs_resize),
# as mount_minixfs -w does, driven by tests/mfsop: the same commands run
# on a directory of the host, and the image then extracts as that
# directory, with the same link counts, and passes fsck_minixfs.  In
# every version and both byte orders, on an empty file system and on the
# test tree; files grow and shrink across the indirect zones.  Then what
# is refused, a file system that fills up, the maps written as they
# change, and the lock that keeps two writers apart.

. ./tests/lib.sh

# must CMD ARGS... - a step the checks depend on.
must() {
	if ! "$@" >"$T/must.out" 2>&1; then
		echo "Bail out! failed: $* $(head -3 "$T/must.out")"
		exit 1
	fi
}

# Data to write: a pattern of the given number of kilobytes.
data() {
	LC_ALL=C awk -v n="$1" -v s="$2" 'BEGIN {
	    for (i = 0; i < n * 64; i++) printf "%015d\n", i * 7 + s }'
}

# The script for mfsop, and the same on the host directory $H.
script=
op() {
	script="$script$*
"
}
host() {
	"$@" || echo "Bail out! the host failed: $*"
}

mkdir_() {
	op mkdir "/$1" "$2"
	host mkdir -m "$2" "$H/$1"
}
file_() {
	op mknod "/$1" f "$2"
	host sh -c ": >\"\$1\" && chmod \"\$2\" \"\$1\"" sh "$H/$1" "$2"
}
fifo_() {
	op mknod "/$1" p "$2"
	host mkfifo -m "$2" "$H/$1"
}
link_() {
	op link "/$1" "/$2"
	host ln "$H/$1" "$H/$2"
}
symlink_() {
	op symlink "/$1" "$2"
	host ln -s "$2" "$H/$1"
}
unlink_() {
	op unlink "/$1"
	host rm -f "$H/$1"
}
rmdir_() {
	op rmdir "/$1"
	host rmdir "$H/$1"
}
rename_() {
	op rename "/$1" "/$2"
	host mv "$H/$1" "$H/$2"
}
# write_ PATH KILOBYTES SEED OFFSET-IN-KILOBYTES
write_() {
	data "$2" "$3" >"$T/data.$3"
	op write "/$1" "$(($4 * 1024))" "$T/data.$3"
	host dd if="$T/data.$3" of="$H/$1" bs=1024 seek="$4" conv=notrunc \
	    2>/dev/null
}
truncate_() {
	op truncate "/$1" "$2"
	host dd if=/dev/null of="$H/$1" bs=1 seek="$2" 2>/dev/null
}
chmod_() {
	op chmod "/$1" "$2"
	host chmod "$2" "$H/$1"
}

# check_ops V IMAGE - run the script on IMAGE, and compare with $H.
check_ops() {
	printf '%s' "$script" >"$T/script"
	run "$MFSOP" "$2" <"$T/script"
	check_true "$1: every command succeeds" test -z "$(grep -v ' ok$' \
	    "$T/out")"
	run "$FSCK_MINIXFS" "$2"
	check_status "$1: fsck finds nothing wrong" 0
	rm -rf "$T/x"
	must "$MINIXFS" extract "$2" "$T/x"
	tree_listing "$H" >"$T/want"
	tree_listing "$T/x" >"$T/got"
	check_same_file "$1: the image holds what the host does" \
	    "$T/want" "$T/got"
}

# The commands, on an empty file system or on the test tree.
commands() {
	script=
	mkdir_ a 0755
	file_ a/f 0644
	write_ a/f 600 1 0
	link_ a/f g
	symlink_ s a/f
	mkdir_ b 0700
	rename_ a/f b/h
	mkdir_ b/c 0755
	rename_ b/c a/c
	fifo_ p 0600
	truncate_ g 300000
	truncate_ b/h 5000
	truncate_ g 700000
	write_ g 3 2 400
	file_ x 0640
	write_ x 20 3 0
	rename_ x g
	chmod_ b 0755
	unlink_ s
	file_ a/c/deep 0600
	rename_ a/c b/c2
	rmdir_ a
}

for fs in "1 le 14" "1 be 30" "2 le 14" "2 be 30" "3 le 60" "3 be 60"; do
	# The fields are split on purpose.
	# shellcheck disable=SC2086
	set -- $fs
	v="V$1/$2/$3"
	opts="-V $1 -B $2"
	if [ "$1" -lt 3 ]; then
		opts="$opts -l $3"
	fi
	H=$T/host
	rm -rf "$H" "$T/img"
	mkdir "$H"
	# $opts is several options on purpose.
	# shellcheck disable=SC2086
	must "$NEWFS_MINIXFS" $opts -s 4096 "$T/img"
	commands
	check_ops "$v, empty" "$T/img"

	sed -e "s/@FS@/version=$1 namelen=$3/" -e "s/@ORDER@/$2/" \
	    -e "s/@BLOCKS@/4096/" -e "s/@LOGZONE@/0/" tests/tree.spec \
	    >"$T/spec"
	mkimage "$T/spec" "$T/img"
	rm -rf "$H"
	must "$MINIXFS" extract "$T/img" "$H"
	commands
	unlink_ bin/sh
	truncate_ etc/dindirect 1000
	truncate_ usr/holes 50000
	rename_ usr/a/b usr/b
	check_ops "$v, the test tree" "$T/img"
done

# Zones of two blocks.
H=$T/host
rm -rf "$H" "$T/img"
mkdir "$H"
must "$NEWFS_MINIXFS" -V 2 -z 1 -s 4096 "$T/img"
commands
check_ops "V2, zones of two blocks" "$T/img"

# What is refused, with the error of the system call, and nothing broken.
long=$(LC_ALL=C awk 'BEGIN { for (i = 0; i < 1024; i++) printf "x" }')
rm -f "$T/img"
must "$NEWFS_MINIXFS" -V 1 -s 1440 "$T/img"
cat >"$T/script" <<EOF
mkdir /d 0755
mknod /d/f f 0644
mkdir /d 0755
rmdir /d
unlink /d
rmdir /d/f
link /d /e
rename /d /d/x
mknod /a-name-of-15-ch f 0644
symlink /l $long
mkdir /e 0755
rename /d/f /e
rename /e /d/f
rename /e /d
mknod /g f 0644
rename /g /d/f noreplace
unlink /nothing
EOF
cat >"$T/expected" <<EOF
1 ok
2 ok
3 error: File exists
4 error: Directory not empty
5 error: Is a directory
6 error: Not a directory
7 error: Operation not permitted
8 error: Invalid argument
9 error: File name too long
10 error: File name too long
11 ok
12 error: Is a directory
13 error: Not a directory
14 error: Directory not empty
15 ok
16 error: File exists
17 error: No such file or directory
EOF
run "$MFSOP" "$T/img" <"$T/script"
check_out "each refusal gives the error of the system call" "$T/expected"
run "$FSCK_MINIXFS" "$T/img"
check_status "after the refusals fsck finds nothing wrong" 0

# A file system that fills up: the zones, then the inodes.
rm -f "$T/img"
must "$NEWFS_MINIXFS" -V 2 -i 16 -s 200 "$T/img"
data 300 4 >"$T/big"
{
	echo "mknod /big f 0644"
	echo "write /big 0 $T/big"
	i=0
	while [ "$i" -lt 20 ]; do
		echo "mknod /f$i f 0644"
		i=$((i + 1))
	done
} >"$T/script"
run "$MFSOP" "$T/img" <"$T/script"
check_out_has "a write past the zones fails" "^2 error: No space left"
check_out_has "a file past the inodes fails" \
    "^[0-9]* error: No space left"
run "$FSCK_MINIXFS" "$T/img"
check_status "a full file system passes fsck" 0

# The maps: kept in memory until the end, or written as they change
# (-a).  A writer killed after a change leaves the maps behind the
# inodes in the first case only.
killed() {
	rm -f "$T/img" "$T/fifo" "$T/kout"
	must "$NEWFS_MINIXFS" -V 2 -s 400 "$T/img"
	mkfifo "$T/fifo"
	# $1 is empty or one option.
	# shellcheck disable=SC2086
	"$MFSOP" $1 "$T/img" <"$T/fifo" >"$T/kout" &
	_killed_pid=$!
	exec 3>"$T/fifo"
	echo "mkdir /k 0755" >&3
	_killed_n=0
	while ! grep -q '^1 ok$' "$T/kout" && [ "$_killed_n" -lt 30 ]; do
		sleep 1
		_killed_n=$((_killed_n + 1))
	done
	kill -9 "$_killed_pid"
	wait "$_killed_pid" 2>/dev/null
	exec 3>&-
}
killed -a
run "$FSCK_MINIXFS" "$T/img"
check_status "-a: the maps are right after the writer is killed" 0
killed ""
run "$FSCK_MINIXFS" "$T/img"
check_found "without -a: they are not, and fsck says so" "map"

# A writer holds a lock on the image: another writer is refused while it
# runs, and a reader is not.
rm -f "$T/img" "$T/fifo" "$T/kout"
must "$NEWFS_MINIXFS" -V 2 -s 400 "$T/img"
mkfifo "$T/fifo"
"$MFSOP" -a "$T/img" <"$T/fifo" >"$T/kout" &
pid=$!
exec 3>"$T/fifo"
echo "mkdir /k 0755" >&3
n=0
while ! grep -q '^1 ok$' "$T/kout" && [ "$n" -lt 30 ]; do
	sleep 1
	n=$((n + 1))
done
run "$TUNEFS_MINIXFS" -c clean "$T/img"
check_err "a second writer is refused" "busy"
run "$NEWFS_MINIXFS" -V 1 "$T/img"
check_true "newfs_minixfs is refused too" grep -q busy "$T/err"
run "$FSCK_MINIXFS" "$T/img"
check_status "a reader is not" 0
exec 3>&-
wait "$pid"
run "$TUNEFS_MINIXFS" -c clean "$T/img"
check_status "the lock goes with the writer" 0

finish
