#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# dump_minixfs: the test tree of each format dumps and restores into a new
# file system as itself; a subtree dumps with the directories above it;
# and dumps of levels 0, 1 and 2, noted in a dumpdates file, restore one
# after the other as the file system they were made of, with files
# removed, added, changed, renamed and turned from a directory into a
# file.

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

TZ=UTC
export TZ

must() {
	if ! "$@" >"$T/must.out" 2>&1; then
		echo "Bail out! failed: $* $(head -3 "$T/must.out")"
		exit 1
	fi
}

# tar_of IMAGE OUT - the archive of the tree of IMAGE, or of a path in it.
tar_of() {
	# The path, if any, is one word or none.
	# shellcheck disable=SC2086
	if ! "$MINIXFS" tar "$1" $3 >"$2" 2>/dev/null; then
		echo "Bail out! minixfs tar failed on $1"
		exit 1
	fi
}

# magic DUMP - the bytes of the magic number of the first header, in hex.
magic() {
	od -A n -t x1 -j 24 -N 4 "$1" | tr -d ' \n'
}

# round FS ORDER NEWFS-OPTIONS... - dump the tree and restore it.
round() {
	_round_v="$1/$2"
	sed -e "s/@FS@/$1/" -e "s/@ORDER@/$2/" -e "s/@BLOCKS@/4096/" \
	    -e "s/@LOGZONE@/${LOGZONE:-0}/" tests/tree.spec >"$T/spec"
	shift 2
	mkimage "$T/spec" "$T/src.img"
	run "$DUMP_MINIXFS" -f "$T/dump" "$T/src.img"
	check_note "$_round_v: dump counts what it wrote" \
	    "DUMP: 8 directories, 20 files,"
	check_true "$_round_v: the dump is whole blocks of 10 records" \
	    test $(($(wc -c <"$T/dump") % 10240)) -eq 0
	rm -f "$T/img" "$T/symtab"
	must "$NEWFS_MINIXFS" "$@" -s 8192 "$T/img"
	run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
	check_status "$_round_v: the dump restores" 0
	tar_of "$T/src.img" "$T/src.tar"
	run "$MINIXFS" tar "$T/img"
	check_out "$_round_v: the tree is that of the image" "$T/src.tar"
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$_round_v: fsck finds nothing wrong" 0
}

round "version=1" le -V 1
round "version=1" be -V 1 -B be
check_true "version=1/be: the dump is big-endian" \
    test "$(magic "$T/dump")" = 0000ea6c
round "version=2 namelen=30" le -V 2 -l 30
check_true "version=2/le: the dump is little-endian" \
    test "$(magic "$T/dump")" = 6cea0000
round "version=2 namelen=30" be -V 3
round "version=3 block=4096" le -V 3
round "version=3 block=4096" be -V 3 -b 4096 -B be
LOGZONE=1 round "version=2" le -V 1

# The flex directories of Minix-vmd dump as any others.
sed -e "s/@FS@/version=2 vmd/" -e "s/@ORDER@/le/" -e "s/@BLOCKS@/4096/" \
    -e "s/@LOGZONE@/0/" tests/tree.spec >"$T/spec"
mkimage "$T/spec" "$T/src.img"
run "$DUMP_MINIXFS" -L flex -f "$T/dump" "$T/src.img"
check_status "Minix-vmd: dump succeeds" 0
rm -f "$T/img" "$T/symtab"
must "$NEWFS_MINIXFS" -V 3 -s 8192 "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_status "Minix-vmd: the dump restores" 0
tar_of "$T/src.img" "$T/src.tar"
run "$MINIXFS" tar "$T/img"
check_out "Minix-vmd: the tree is that of the image" "$T/src.tar"
run "$RESTORE_MINIXFS" -t -f "$T/dump"
check_true "the label is that of -L" grep -q "^Label: flex\$" "$T/err"
check_out_has "the dump is of level 0" "^Dumped from: the epoch\$"

# A subtree, with the directories above it.
sed -e "s/@FS@/version=2/" -e "s/@ORDER@/le/" -e "s/@BLOCKS@/4096/" \
    -e "s/@LOGZONE@/0/" tests/tree.spec >"$T/spec"
mkimage "$T/spec" "$T/src.img"
run "$DUMP_MINIXFS" -f "$T/dump" "$T/src.img" /usr/a
check_status "subtree: dump succeeds" 0
run "$RESTORE_MINIXFS" -t -f "$T/dump"
awk -F '\t' 'NF == 2 { print $2 }' "$T/out" >"$T/listed"
printf '%s\n' . ./usr ./usr/a ./usr/a/b ./usr/a/b/c ./usr/a/b/c/deep \
    >"$T/expected"
check_same_file "subtree: it holds the path and what is below" \
    "$T/expected" "$T/listed"
rm -f "$T/img" "$T/symtab"
must "$NEWFS_MINIXFS" -V 2 -s 4096 "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_note "subtree: the names of the rest are left out" \
    "names of files that the dump does not hold left out"
tar_of "$T/src.img" "$T/src.tar" usr/a
run "$MINIXFS" tar "$T/img" usr/a
check_out "subtree: it restores as it was" "$T/src.tar"
run "$DUMP_MINIXFS" -1 -f "$T/dump" "$T/src.img" /usr/a
check_note "subtree: a level above 0 is not taken" \
    "a directory is dumped at level 0"

run "$DUMP_MINIXFS" -T "not a date" -f "$T/dump" "$T/src.img"
check_status "-T takes only a date as ctime(3) prints it" 2
run "$DUMP_MINIXFS" "$T/src.img"
check_status "-f is needed" 2
run "$DUMP_MINIXFS" -T "Sat Jan  1 00:00:00 2000" -1 -f "$T/dump" \
    "$T/src.img"
check_status "-T: dump succeeds" 0
run "$RESTORE_MINIXFS" -t -f "$T/dump"
check_out_has "-T: the dump is of what changed since then" \
    "^Dumped from: Sat Jan  1 00:00:00 2000\$"

# Levels.  The files that change get times after the dump of level 0;
# "unused" keeps the inode numbers of the rest the same.
old=644198400
new=$(($(date +%s) + 1000))
cat >"$T/a.spec" <<EOF
fs version=2 order=le blocks=2000 inodes=64
dir  /a 0755 1 1 $old
file /a/f1 0644 1 1 $old 3000 1
file /a/f2 0644 1 1 $old 5000 2
dir  /b 0755 1 1 $old
file /b/g 0644 1 1 $old 100 3
file /c 0644 1 1 $old 2000 4
hard /c2 /c
link /l a/f1 1 1 $old
dir  /d 0755 1 1 $old
file /d/x 0644 1 1 $old 10 5
EOF
cat >"$T/b.spec" <<EOF
fs version=2 order=le blocks=2000 inodes=64
dir  /a 0755 1 1 $new
file /a/f1 0644 1 1 $old 3000 1
unused 1
dir  /bb 0755 1 1 $new
file /bb/g 0644 1 1 $old 100 3
file /c 0600 1 1 $new 2500 6
hard /c2 /c
link /l a/f1 1 1 $old
file /d 0644 1 1 $new 50 7
unused 1
file /a/f3 0644 1 1 $new 700 8
EOF
mkimage "$T/a.spec" "$T/fs.img"
mkimage "$T/b.spec" "$T/b.img"
dates="$T/dumpdates"
run "$DUMP_MINIXFS" -0 -u -D "$dates" -f "$T/d0" "$T/fs.img"
check_status "level 0: dump succeeds" 0
check_true "level 0: -u notes it" grep -q "^$T/fs.img  *0 " "$dates"
cp "$T/b.img" "$T/fs.img"
# Dumps are told apart by their dates, in seconds.
sleep 1
run "$DUMP_MINIXFS" -1 -u -D "$dates" -f "$T/d1" "$T/fs.img"
check_status "level 1: dump succeeds" 0
check_note "level 1: it follows the dump of level 0" \
    "Date of last level 0 dump: [A-Z]"
check_true "level 1: -u notes it too" grep -q "^$T/fs.img  *1 " "$dates"
run "$RESTORE_MINIXFS" -t -f "$T/d1"
awk -F '\t' 'NF == 2 { print $2 }' "$T/out" | sort >"$T/listed"
printf '%s\n' . ./a ./a/f3 ./bb ./c ./c2 ./d | sort >"$T/expected"
check_same_file "level 1: it holds what changed" "$T/expected" \
    "$T/listed"

rm -f "$T/img" "$T/symtab"
must "$NEWFS_MINIXFS" -V 2 -s 2000 "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/d0" "$T/img"
check_status "level 0: it restores" 0
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/d1" "$T/img"
check_status "level 1: it restores on top" 0
check_note "level 1: the files that went are removed" "; 3 files removed\$"
tar_of "$T/b.img" "$T/b.tar"
run "$MINIXFS" tar "$T/img"
check_out "levels 0 and 1: the tree is that of the second image" \
    "$T/b.tar"
run "$FSCK_MINIXFS" "$T/img"
check_status "levels 0 and 1: fsck finds nothing wrong" 0
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/d1" "$T/img"
check_err "level 1: it does not restore twice" "follows a dump of"

run "$DUMP_MINIXFS" -2 -D "$dates" -f "$T/d2" "$T/fs.img"
check_status "level 2: dump succeeds" 0
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/d2" "$T/img"
check_status "level 2: nothing changed, and it restores" 0
run "$MINIXFS" tar "$T/img"
check_out "level 2: the tree stays that of the second image" "$T/b.tar"

rm -f "$T/img" "$T/symtab"
must "$NEWFS_MINIXFS" -V 2 -s 2000 "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/d1" "$T/img"
check_err "level 1: it needs the table of the restore before" \
    "needs the table of the restores before it"


# An image whose path holds a blank is found in the dumpdates file too:
# a dump of level 1 follows that of level 0, and -u once more replaces
# its own line.
cp "$T/fs.img" "$T/a b.img"
rm -f "$T/dumpdates5"
must "$DUMP_MINIXFS" -0 -u -D "$T/dumpdates5" -f "$T/b0" "$T/a b.img"
sleep 1
run "$DUMP_MINIXFS" -1 -u -D "$T/dumpdates5" -f "$T/b1" "$T/a b.img"
check_status "a blank in the path: level 1 is dumped" 0
check_true "a blank in the path: it follows level 0" \
    grep -q "last level 0 dump: [A-Z]" "$T/err"
run "$DUMP_MINIXFS" -1 -u -D "$T/dumpdates5" -f "$T/b1" "$T/a b.img"
check_true "a blank in the path: -u replaces the line of the level" \
    test "$(wc -l <"$T/dumpdates5")" -eq 2
check_true "a blank in the path: is written in octal" \
    grep -q "a\\\\040b.img " "$T/dumpdates5"
# A name that ends in a blank is not that without it.
cp "$T/fs.img" "$T/c.img "
cp "$T/fs.img" "$T/c.img"
must "$DUMP_MINIXFS" -0 -u -D "$T/dumpdates5" -f "$T/c0" "$T/c.img "
run "$DUMP_MINIXFS" -1 -D "$T/dumpdates5" -f "$T/c1" "$T/c.img"
check_true "a blank at the end: the name without it has no dump of its own" \
    grep -q "last level -1 dump: the epoch" "$T/err"

# A level 1 that runs out of room fails, but leaves the file system in
# order and the table following level 0, and restores once there is room.
cat >"$T/a4.spec" <<EOF
fs version=2 order=le blocks=400 inodes=64
file /f 0644 1 1 $old 1000 1
EOF
cat >"$T/b4.spec" <<EOF
fs version=2 order=le blocks=400 inodes=64
file /f 0644 1 1 $old 1000 1
file /big 0644 1 1 $new 20000 2
EOF
mkimage "$T/a4.spec" "$T/fs4.img"
mkimage "$T/b4.spec" "$T/b4.img"
rm -f "$T/dumpdates4"
must "$DUMP_MINIXFS" -0 -u -D "$T/dumpdates4" -f "$T/e0" "$T/fs4.img"
cp "$T/b4.img" "$T/fs4.img"
sleep 1
must "$DUMP_MINIXFS" -1 -u -D "$T/dumpdates4" -f "$T/e1" "$T/fs4.img"
rm -f "$T/img" "$T/symtab"
must "$NEWFS_MINIXFS" -V 2 -s 400 -i 16 "$T/img"
must "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/e0" "$T/img"
date0=$(sed -n 's/^date //p' "$T/symtab")
zones=$(info_field "$T/img" zones)
free=$(info_field "$T/img" "free zones")
must "$TUNEFS_MINIXFS" -s $((zones - free + 1)) "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/e1" "$T/img"
check_err "out of room: level 1 fails" "No space left on device"
check_true "out of room: it says to restore it again" \
    grep -q "restore it again" "$T/err"
run "$FSCK_MINIXFS" "$T/img"
check_status "out of room: fsck finds nothing wrong" 0
check_info "out of room: the image is marked clean again" "$T/img" clean \
    yes
check_true "out of room: the table still follows level 0" \
    test "$(sed -n 's/^date //p' "$T/symtab")" = "$date0"
must "$TUNEFS_MINIXFS" -s 400 "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/e1" "$T/img"
check_status "out of room: level 1 restores once there is room" 0
tar_of "$T/b4.img" "$T/b4.tar"
run "$MINIXFS" tar "$T/img"
check_out "out of room: the tree is then that of the second image" \
    "$T/b4.tar"

# The files that went make room for the new ones: in a file system of 16
# inodes, fifteen files give way to fifteen others with inodes of their
# own.
{
	echo "fs version=2 order=le blocks=400 inodes=64"
	i=1
	while [ "$i" -le 15 ]; do
		echo "file /f$i 0644 1 1 $old 10 $i"
		i=$((i + 1))
	done
} >"$T/a3.spec"
{
	echo "fs version=2 order=le blocks=400 inodes=64"
	echo "unused 15"
	i=1
	while [ "$i" -le 15 ]; do
		echo "file /g$i 0644 1 1 $new 10 $i"
		i=$((i + 1))
	done
} >"$T/b3.spec"
mkimage "$T/a3.spec" "$T/fs3.img"
mkimage "$T/b3.spec" "$T/b3.img"
rm -f "$T/dumpdates3"
must "$DUMP_MINIXFS" -0 -u -D "$T/dumpdates3" -f "$T/e0" "$T/fs3.img"
cp "$T/b3.img" "$T/fs3.img"
sleep 1
must "$DUMP_MINIXFS" -1 -u -D "$T/dumpdates3" -f "$T/e1" "$T/fs3.img"
rm -f "$T/img" "$T/symtab"
must "$NEWFS_MINIXFS" -V 2 -s 400 -i 16 "$T/img"
must "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/e0" "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/e1" "$T/img"
check_status "inodes that go are free for new files" 0
check_note "the fifteen files that went are removed" \
    "; 15 files removed\$"
tar_of "$T/b3.img" "$T/b3.tar"
run "$MINIXFS" tar "$T/img"
check_out "the tree is that of the second image" "$T/b3.tar"

finish
