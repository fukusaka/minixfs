#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# The commands of minixfs that write an image without mounting it: put,
# mkdir, rm, mv, ln, chmod and chown.  The same changes are made on a
# directory of the host, and the image then extracts as that directory
# and passes fsck_minixfs, in every version and byte order.  Then what
# put does to a name in the way (-n, -i), what belongs to whom (the
# directory, or -o), what is refused, -f on a file system not marked
# clean, the lock, and -M.

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

# The times that ls -l shows are in UTC, and the modes of what mkdir
# makes on the host are to match those of the image.
TZ=UTC
export TZ
umask 022

# must CMD ARGS... - a step the checks depend on.
must() {
	if ! "$@" >"$T/must.out" 2>&1; then
		echo "Bail out! failed: $* $(head -3 "$T/must.out")"
		exit 1
	fi
}

# owner IMAGE PATH - "uid gid" of a file of the image, from ls -l of the
# directory that holds it.
owner() {
	_owner_dir=$(dirname "$2")
	_owner_name=$(basename "$2")
	"$MINIXFS" ls -l "$1" "$_owner_dir" |
	    awk -v n="$_owner_name" '$8 == n { print $3, $4 }'
}

# A tree of the host to put: files around the block and indirect limits,
# a symbolic link, a pipe, a nested directory, and an empty directory.
host_tree() {
	rm -rf "$1"
	mkdir -p "$1/d/e" "$1/empty"
	LC_ALL=C awk 'BEGIN { for (i = 0; i < 300; i++) printf "%015d\n",
	    i * 3 }' >"$1/d/e/deep"
	LC_ALL=C awk 'BEGIN { for (i = 0; i < 40000; i++) printf "%015d\n",
	    i * 7 }' >"$1/big"
	: >"$1/zero"
	printf 'hello\n' >"$1/hello"
	ln -s d/e/deep "$1/link"
	mkfifo "$1/fifo"
	chmod 0640 "$1/hello"
	chmod 0700 "$1/d"
	touch -t 200001020304 "$1/hello" "$1/d/e/deep" "$1/d"
}

# Changes made through the image and on the host directory $H alike.
change_both() {
	run "$MINIXFS" put -R "$T/img" "$H/tree" /
	check_status "$v: put -R copies the tree in" 0
	run "$MINIXFS" put "$T/img" "$H/tree/hello" "$H/tree/big" /tree/d
	check_status "$v: put copies files into a directory" 0
	cp "$H/tree/hello" "$H/tree/big" "$H/tree/d"
	run "$MINIXFS" put "$T/img" "$H/tree/hello" /tree/renamed
	check_status "$v: put copies a file as a new name" 0
	cp "$H/tree/hello" "$H/tree/renamed"
	run "$MINIXFS" mkdir "$T/img" /tree/new /tree/d/new2
	check_status "$v: mkdir makes directories" 0
	mkdir "$H/tree/new" "$H/tree/d/new2"
	run "$MINIXFS" mkdir -p -m 700 "$T/img" /tree/a/b/c
	check_status "$v: mkdir -p makes the way" 0
	# As mkdir -p -m: 0755 on the way, and the mode at the end.
	mkdir -p "$H/tree/a/b" && mkdir -m 700 "$H/tree/a/b/c"
	run "$MINIXFS" ln "$T/img" /tree/hello /tree/new/hard
	check_status "$v: ln makes a hard link" 0
	ln "$H/tree/hello" "$H/tree/new/hard"
	run "$MINIXFS" ln -s "$T/img" ../hello /tree/new/soft
	check_status "$v: ln -s makes a symbolic link" 0
	ln -s ../hello "$H/tree/new/soft"
	run "$MINIXFS" mv "$T/img" /tree/big /tree/a/moved
	check_status "$v: mv renames a file" 0
	mv "$H/tree/big" "$H/tree/a/moved"
	run "$MINIXFS" mv "$T/img" /tree/d/e /tree/new
	check_status "$v: mv moves a directory into another" 0
	mv "$H/tree/d/e" "$H/tree/new"
	run "$MINIXFS" chmod "$T/img" 600 /tree/renamed
	check_status "$v: chmod sets a mode" 0
	chmod 600 "$H/tree/renamed"
	run "$MINIXFS" rm "$T/img" /tree/link /tree/fifo /tree/zero
	check_status "$v: rm removes files" 0
	rm "$H/tree/link" "$H/tree/fifo" "$H/tree/zero"
	run "$MINIXFS" rm -r "$T/img" /tree/d
	check_status "$v: rm -r removes a tree" 0
	rm -r "$H/tree/d"
	run "$MINIXFS" rm "$T/img" /tree/empty
	check_err "$v: rm refuses a directory" "Is a directory"
	run "$MINIXFS" rm -r "$T/img" /tree/empty
	check_status "$v: rm -r removes an empty directory" 0
	rmdir "$H/tree/empty"
}

# check_image: the image holds what the host does, and is consistent.
check_image() {
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$v: fsck finds nothing wrong" 0
	check_true "$v: the image is marked clean" \
	    test "$(info_field "$T/img" clean)" = yes
	rm -rf "$T/x"
	must "$MINIXFS" extract "$T/img" "$T/x"
	tree_listing "$H" >"$T/want"
	tree_listing "$T/x" >"$T/got"
	check_same_file "$v: the image holds what the host does" \
	    "$T/want" "$T/got"
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
	host_tree "$H/tree"
	# $opts is several options on purpose.
	# shellcheck disable=SC2086
	must "$NEWFS_MINIXFS" $opts -s 4096 "$T/img"
	change_both
	check_image
	# The mtimes of the host came with the files.
	check_true "$v: put keeps the mtime of a file" \
	    test "$("$MINIXFS" ls -l "$T/img" /tree/renamed |
	    awk '{ print $6 }')" = 2000-01-02
	# extract leaves the set-user-ID bit out, so ls -l shows it.
	run "$MINIXFS" chmod "$T/img" 4755 /tree/renamed
	check_true "$v: chmod sets the set-user-ID bit" \
	    test "$("$MINIXFS" ls -l "$T/img" /tree/renamed |
	    awk '{ print $1 }')" = -rwsr-xr-x
done

# Zones of two blocks.
v="V2, zones of two blocks"
H=$T/host
rm -rf "$H" "$T/img"
mkdir "$H"
host_tree "$H/tree"
must "$NEWFS_MINIXFS" -V 2 -z 1 -s 4096 "$T/img"
change_both
check_image

# What belongs to whom: the directory a thing is made in, or -o.
v="owners"
rm -f "$T/img"
must "$NEWFS_MINIXFS" -V 2 -s 400 "$T/img"
run "$MINIXFS" chown "$T/img" 3:4 /
check_status "$v: chown sets the owner of the root" 0
run "$MINIXFS" mkdir "$T/img" /d
run "$MINIXFS" put "$T/img" "$H/tree/hello" /d
run "$MINIXFS" ln -s "$T/img" hello /d/s
check_true "$v: a directory takes the owner of its directory" \
    test "$(owner "$T/img" /d)" = "3 4"
check_true "$v: a file put takes the owner of its directory" \
    test "$(owner "$T/img" /d/hello)" = "3 4"
check_true "$v: a symbolic link takes the owner of its directory" \
    test "$(owner "$T/img" /d/s)" = "3 4"
run "$MINIXFS" chown "$T/img" 5:6 /d
run "$MINIXFS" put -R "$T/img" "$H/tree/new" /d
check_true "$v: a tree put takes the owner of its directory" \
    test "$(owner "$T/img" /d/new/e/deep)" = "5 6"
run "$MINIXFS" put -o 7:8 "$T/img" "$H/tree/hello" /d/other
check_true "$v: -o gives another owner" \
    test "$(owner "$T/img" /d/other)" = "7 8"
run "$MINIXFS" mkdir -o 9:10 "$T/img" /d/od
check_true "$v: mkdir -o gives another owner" \
    test "$(owner "$T/img" /d/od)" = "9 10"
run "$MINIXFS" chown "$T/img" :11 /d/od
check_true "$v: chown :gid keeps the uid" \
    test "$(owner "$T/img" /d/od)" = "9 11"
run "$MINIXFS" chown "$T/img" 12 /d/od
check_true "$v: chown uid keeps the gid" \
    test "$(owner "$T/img" /d/od)" = "12 11"
run "$FSCK_MINIXFS" "$T/img"
check_status "$v: fsck finds nothing wrong" 0

rm -f "$T/img"
must "$NEWFS_MINIXFS" -V 1 -s 400 "$T/img"
run "$MINIXFS" put -o 1:300 "$T/img" "$H/tree/hello" /h
check_err "$v: -o refuses a group V1 cannot hold" "does not fit in V1"
run "$MINIXFS" chown "$T/img" 1:300 /
check_err "$v: chown refuses a group V1 cannot hold" "does not fit in V1"

# A name in the way: replaced, kept with -n, asked about with -i.
v="in the way"
rm -f "$T/img"
must "$NEWFS_MINIXFS" -V 2 -s 400 "$T/img"
printf 'old\n' >"$T/old"
printf 'new\n' >"$T/new"
run "$MINIXFS" put "$T/img" "$T/old" /f
run "$MINIXFS" put "$T/img" "$T/new" /f
check_status "$v: put replaces a file" 0
run "$MINIXFS" cat "$T/img" /f
check_out "$v: the new contents are there" "$T/new"
run "$MINIXFS" put -n "$T/img" "$T/old" /f
check_status "$v: put -n leaves it" 0
run "$MINIXFS" cat "$T/img" /f
check_out "$v: -n kept the contents" "$T/new"
run sh -c "echo n | \"$MINIXFS\" put -i \"$T/img\" \"$T/old\" /f"
check_status "$v: put -i asks, and n keeps it" 0
check_true "$v: -i asked" grep -q "overwrite" "$T/err"
run "$MINIXFS" cat "$T/img" /f
check_out "$v: n kept the contents" "$T/new"
run sh -c "echo y | \"$MINIXFS\" put -i \"$T/img\" \"$T/old\" /f"
check_status "$v: put -i asks, and y replaces it" 0
run "$MINIXFS" cat "$T/img" /f
check_out "$v: y replaced the contents" "$T/old"
run "$MINIXFS" mkdir "$T/img" /dir
run "$MINIXFS" put "$T/img" "$T/old" /dir
check_status "$v: put into a directory goes under the name of the file" 0
run "$MINIXFS" cat "$T/img" /dir/old
check_out "$v: the file is in the directory" "$T/old"
run "$MINIXFS" put "$T/img" "$T/old" "$T/new" /f
check_err "$v: several files need a directory" "not a directory"
run "$MINIXFS" put "$T/img" "$T/old" /dir/old/x
check_err "$v: a file is no directory to put into" "Not a directory"
run "$MINIXFS" put "$T/img" "$H/tree" /dir
check_err "$v: a directory needs -R" "give -R"
mkdir "$T/host/dir"
run "$MINIXFS" put -R "$T/img" "$T/host/dir" /f
check_err "$v: a directory does not replace a file" "File exists"
run "$MINIXFS" put "$T/img" "$T/old" /dir
run "$MINIXFS" put "$T/img" "$T/old" /nothere/f
check_err "$v: a missing directory is reported" "No such file"
run "$FSCK_MINIXFS" "$T/img"
check_status "$v: fsck finds nothing wrong after the refusals" 0

# What else is refused.
v="refused"
long=$(LC_ALL=C awk 'BEGIN { for (i = 0; i < 61; i++) printf "x" }')
run "$MINIXFS" mkdir "$T/img" "/$long"
check_err "$v: a name too long" "too long"
run "$MINIXFS" mkdir "$T/img" /dir
check_err "$v: mkdir of a name in use" "File exists"
run "$MINIXFS" rm "$T/img" /
check_err "$v: rm of the root" "Invalid argument"
run "$MINIXFS" mv "$T/img" /dir /dir/sub
check_err "$v: mv of a directory below itself" "Invalid argument"
run "$MINIXFS" ln "$T/img" /dir /link
check_err "$v: ln of a directory" "Operation not permitted"
run "$MINIXFS" chmod "$T/img" 8 /f
check_status "$v: chmod with a mode not octal is a usage error" 2
run "$MINIXFS" chown "$T/img" a:b /f
check_status "$v: chown with names is a usage error" 2
run "$MINIXFS" put -o 1 "$T/img" "$T/old" /f
check_status "$v: -o with one number is a usage error" 2
run "$MINIXFS" put "$T/img" /f
check_status "$v: put without a file is a usage error" 2
run "$MINIXFS" mv "$T/img" /f
check_status "$v: mv without a new name is a usage error" 2
run "$MINIXFS" -x mkdir "$T/img" /d
check_status "$v: an unknown option before the command is a usage error" 2

# A file system that fills up leaves nothing half copied.
v="full"
rm -f "$T/img"
must "$NEWFS_MINIXFS" -V 2 -s 40 "$T/img"
run "$MINIXFS" put "$T/img" "$H/tree/a/moved" /big
check_err "$v: a file too large for the free zones is refused" \
    "No space left"
run "$MINIXFS" ls "$T/img" /
check_true "$v: nothing of it stays" test ! -s "$T/out"
run "$FSCK_MINIXFS" "$T/img"
check_status "$v: fsck finds nothing wrong" 0

# A file system not marked clean is refused without -f, and the flex
# directories of Minix-vmd always.
v="not clean"
rm -f "$T/img"
must "$NEWFS_MINIXFS" -V 2 -s 400 "$T/img"
must "$TUNEFS_MINIXFS" -c dirty "$T/img"
cp "$T/img" "$T/before"
run "$MINIXFS" mkdir "$T/img" /d
check_err "$v: refused" "not marked clean"
check_same_file "$v: nothing changed" "$T/before" "$T/img"
run "$MINIXFS" -f mkdir "$T/img" /d
check_status "$v: -f writes all the same" 0
check_true "$v: the mark stays away, as it was" \
    test "$(info_field "$T/img" clean)" = no
run "$FSCK_MINIXFS" -y "$T/img"
check_status "$v: fsck -y passes it and marks it clean" 0
cat >"$T/spec" <<EOF
fs version=2 vmd order=le blocks=400 inodes=16
dir /d 0755 0 0 0
EOF
mkimage "$T/spec" "$T/vmd.img"
run "$MINIXFS" mkdir "$T/vmd.img" /e
check_err "flex directories cannot be written" "flex directories"

# The lock: a command is refused while another writer holds the image.
rm -f "$T/img" "$T/fifo" "$T/kout"
must "$NEWFS_MINIXFS" -V 2 -s 400 "$T/img"
mkfifo "$T/fifo"
"$MFSOP" "$T/img" <"$T/fifo" >"$T/kout" &
pid=$!
exec 3>"$T/fifo"
echo "mkdir /k 0755" >&3
n=0
while ! grep -q '^1 ok$' "$T/kout" && [ "$n" -lt 30 ]; do
	sleep 1
	n=$((n + 1))
done
run "$MINIXFS" mkdir "$T/img" /d
check_err "a writer is refused while another holds the image" "busy"
exec 3>&-
wait "$pid"
run "$MINIXFS" mkdir "$T/img" /d
check_status "the lock goes with the writer" 0

# -M: the same on an image of one side of a disk.
track=4608
rm -f "$T/img"
must "$NEWFS_MINIXFS" -V 1 -s 360 "$T/img"
: >"$T/two"
t=0
while [ "$t" -lt 80 ]; do
	dd if="$T/img" bs="$track" skip="$t" count=1 2>/dev/null >>"$T/two"
	dd if=/dev/zero bs="$track" count=1 2>/dev/null >>"$T/two"
	t=$((t + 1))
done
run "$MINIXFS" -M "$track:2:0" put "$T/two" "$H/tree/hello" /h
check_status "-M: put writes through the tracks" 0
run "$MINIXFS" -M "$track:2:0" cat "$T/two" /h
check_out "-M: the file reads back" "$H/tree/hello"
run "$FSCK_MINIXFS" -M "$track:2:0" "$T/two"
check_status "-M: fsck finds nothing wrong" 0
check_true "-M: the other side is untouched" \
    test "$(dd if="$T/two" bs="$track" skip=1 count=1 2>/dev/null |
    tr -d '\000' | wc -c)" -eq 0

finish
