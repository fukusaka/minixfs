#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# restore_minixfs: dumps of the test tree in the formats of BSD and Linux,
# written by tests/mkdump, restore into an empty file system as the tree
# that mkimage makes of the same spec, and -t lists them.  Then what it
# refuses: compressed dumps, full dumps into file systems that are not
# empty, names, owners and devices that do not fit, file systems that are
# not clean or have flex directories; -N writes nothing.

. ./tests/lib.sh

TZ=UTC
export TZ

# must CMD ARGS... - a step the checks depend on.
must() {
	if ! "$@" >"$T/must.out" 2>&1; then
		echo "Bail out! failed: $* $(head -3 "$T/must.out")"
		exit 1
	fi
}

# fresh [VERSION] - an empty file system in $T/img, V2 unless given.
fresh() {
	rm -f "$T/img" "$T/symtab"
	must "$NEWFS_MINIXFS" -V "${1:-2}" -s 4096 "$T/img"
}

sed -e "s/@FS@/version=2/" -e "s/@ORDER@/le/" -e "s/@BLOCKS@/4096/" \
    -e "s/@LOGZONE@/0/" tests/tree.spec >"$T/spec"
mkimage "$T/spec" "$T/ref.img"
if ! "$MINIXFS" tar "$T/ref.img" >"$T/ref.tar" 2>/dev/null; then
	echo "Bail out! minixfs tar failed"
	exit 1
fi
# What -t lists: the names of tree.names, from the root.
{
	echo "."
	grep -v '^#' tests/tree.names | sed 's|^|./|'
} | sort >"$T/names"

# variant NAME MKDUMP-OPTIONS... - restore and list a dump of the tree.
variant() {
	_variant_v=$1
	shift
	must "$MKDUMP" "$@" "$T/spec" "$T/dump"
	fresh
	run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
	check_status "$_variant_v: restore succeeds" 0
	cp "$T/err" "$T/restore.err"
	run "$MINIXFS" tar "$T/img"
	check_out "$_variant_v: the tree is that of the spec" "$T/ref.tar"
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$_variant_v: fsck finds nothing wrong" 0
	run "$RESTORE_MINIXFS" -t -f "$T/dump"
	# The lines of names, not those of the dates.
	awk -F '	' 'NF == 2 { print $2 }' "$T/out" | sort >"$T/listed"
	check_same_file "$_variant_v: -t lists every name" "$T/names" \
	    "$T/listed"
}

variant "4.4BSD" -h nfs
variant "4.4BSD, big-endian" -h nfs -B be
variant "4.3BSD" -h nfs -d old
variant "a file system before 4.2BSD" -h ofs -d v7
variant "a file system before 4.2BSD, big-endian" -h ofs -d v7 -B be
variant "UFS2" -h ufs2
variant "Linux runs" -r
variant "UFS2 with attributes, big-endian" -h ufs2 -x -B be
check_true "UFS2: the attributes left out are counted" \
    grep -q "the extended attributes of 16 files left out" "$T/restore.err"
variant "Linux runs with attributes, big-endian" -r -a -B be
check_true "Linux: the attributes left out are counted" \
    grep -q "the extended attributes of 16 files left out" "$T/restore.err"

# -c reads the directories of any dump as those of V7, as the restore of
# NetBSD does; here, those of a dump of a file system before 4.2BSD.
must "$MKDUMP" -h ofs -d v7 "$T/spec" "$T/dump"
fresh
run "$RESTORE_MINIXFS" -r -c -s "$T/symtab" -f "$T/dump" "$T/img"
check_status "-c: restore succeeds" 0
run "$MINIXFS" tar "$T/img"
check_out "-c: the tree is that of the spec" "$T/ref.tar"

# A dump from standard input cannot be read twice, and is checked as the
# files come.
must "$MKDUMP" "$T/spec" "$T/dump"
fresh
run sh -c "\"$RESTORE_MINIXFS\" -r -s \"$T/symtab\" -f - \"$T/img\" \
    <\"$T/dump\""
check_status "standard input: restore succeeds" 0
run "$MINIXFS" tar "$T/img"
check_out "standard input: the tree is that of the spec" "$T/ref.tar"

run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_err "a full dump goes only into an empty file system" \
    "a full dump goes into an empty file system"

must "$MKDUMP" -z "$T/spec" "$T/dump"
run "$RESTORE_MINIXFS" -t -f "$T/dump"
check_err "a compressed dump of Linux is refused" "compressed"

cat tests/tree.spec tests/tree.spec >"$T/dump"
run "$RESTORE_MINIXFS" -t -f "$T/dump"
check_err "a file that is no dump is refused" "not a dump"

must "$MKDUMP" "$T/spec" "$T/dump"
dd if="$T/dump" of="$T/short" bs=1024 count=40 2>/dev/null
fresh
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/short" "$T/img"
check_err "a dump cut short fails" "ends before TS_END"
run "$FSCK_MINIXFS" "$T/img"
check_status "cut short: what was written is in order" 0
check_true "cut short: no table is written" test ! -e "$T/symtab"

# -N checks and writes nothing.
fresh
cp "$T/img" "$T/before"
run "$RESTORE_MINIXFS" -r -N -s "$T/symtab" -f "$T/dump" "$T/img"
check_status "-N succeeds" 0
check_same_file "-N leaves the image as it was" "$T/before" "$T/img"
check_true "-N writes no table" test ! -e "$T/symtab"

# Names too long for V1 with 14 characters: nothing is written.
cat >"$T/long.spec" <<EOF
dir  /bin 0755 2 2 644198400
file /bin/a-name-of-twenty-c 0644 0 0 644198400 10 1
file /bin/short 0644 0 0 644198400 10 2
EOF
must "$MKDUMP" "$T/long.spec" "$T/dump"
fresh 1
cp "$T/img" "$T/before"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_err "V1: a name too long is found" \
    "bin/a-name-of-twenty-c: name longer than 14 characters"
check_same_file "V1: then nothing is written" "$T/before" "$T/img"

# The group of a V1 inode is a byte; -o makes one owner for all.
cat >"$T/gid.spec" <<EOF
dir  /d 0755 0 300 644198400
file /d/f 0644 70 300 644198400 10 1
EOF
must "$MKDUMP" "$T/gid.spec" "$T/dump"
fresh 1
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_err "V1: a group of 300 does not fit" "group 300 does not fit in V1"
run "$RESTORE_MINIXFS" -r -o 5:6 -s "$T/symtab" -f "$T/dump" "$T/img"
check_status "V1: with -o it does" 0
run "$MINIXFS" ls -l "$T/img" /d
check_out_has "V1: -o gives the owner and the group" " 5 *6 .* f\$"

# From standard input, a file that does not fit is left out and the rest
# is restored.
fresh 1
run sh -c "\"$RESTORE_MINIXFS\" -r -s \"$T/symtab\" -f - \"$T/img\" \
    <\"$T/dump\""
check_err "V1, standard input: the group is found as the file comes" \
    "d/f: group 300 does not fit in V1"

cat >"$T/dev.spec" <<EOF
dev  /big c 300 1 0600 0 0 644198400
dev  /ok c 4 1 0600 0 0 644198400
sock /sock 0600 0 0 644198400
EOF
must "$MKDUMP" "$T/dev.spec" "$T/dump"
fresh
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_err "a device number that does not fit is found" \
    "big: device number 0x12c01 does not fit"
sed '/big/d' "$T/dev.spec" >"$T/dev2.spec"
must "$MKDUMP" "$T/dev2.spec" "$T/dump"
fresh
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_note "a socket is left out" "warning: 1 sockets left out"
run "$MINIXFS" ls "$T/img"
check_out_has "the device is there" "^ok\$"
check_true "the socket is not" test "$(grep -c sock "$T/out")" -eq 0

fresh
must "$TUNEFS_MINIXFS" -c dirty "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_err "a file system not marked clean is refused" "not marked clean"

cat >"$T/vmd.spec" <<EOF
fs version=2 order=le blocks=1440 inodes=64 vmd
EOF
mkimage "$T/vmd.spec" "$T/img"
run "$RESTORE_MINIXFS" -r -s "$T/symtab" -f "$T/dump" "$T/img"
check_err "flex directories are refused" "flex directories"

run "$RESTORE_MINIXFS" -r -f "$T/dump"
check_status "-r without an image is a usage error" 2
run "$RESTORE_MINIXFS" -t -r -f "$T/dump" "$T/img"
check_status "-t with -r is a usage error" 2

finish
