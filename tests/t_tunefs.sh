#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# tunefs_minixfs: it prints the settings, -N changes nothing, and each
# change leaves a file system that fsck_minixfs passes and that says what
# was asked for.

. ./tests/lib.sh

# tuned NAME - tunefs succeeded, and fsck passes the image.
tuned() {
	check_status "$v: $1" 0
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$v: fsck passes the image after $1" 0
}

for fs in "1 le" "2 be" "3 le" "3 be"; do
	version=${fs% *}
	order=${fs#* }
	v="V$version/$order"
	rm -f "$T/img"
	"$NEWFS_MINIXFS" -V "$version" -B "$order" -s 2048 -t 0 "$T/img"
	max=$(info_field "$T/img" "max file size")
	run "$FSCK_MINIXFS" -w "$T/img"
	minix=$(sed -n 's/.*where MINIX works out \([0-9]*\)$/\1/p' "$T/out")
	if [ -z "$minix" ]; then
		minix=$max
	fi

	run "$TUNEFS_MINIXFS" "$T/img"
	check_out_has "$v: tunefs prints the state" "^state: clean\$"
	check_out_has "$v: tunefs prints the maximum file size" \
	    "^max file size: $max (MINIX works out $minix)\$"
	check_out_has "$v: tunefs prints the bits past the end" \
	    "^bits past the end of the maps: 1\$"

	cp "$T/img" "$T/before"
	run "$TUNEFS_MINIXFS" -N -c dirty -e 0 -m 1000 "$T/img"
	check_out_has "$v: -N shows a change" "^state: clean -> dirty\$"
	check_same_file "$v: -N writes nothing" "$T/before" "$T/img"

	run "$TUNEFS_MINIXFS" -c dirty "$T/img"
	tuned "-c dirty"
	check_info "$v: -c dirty marks it dirty" "$T/img" clean no
	run "$TUNEFS_MINIXFS" -c clean "$T/img"
	tuned "-c clean"
	check_info "$v: -c clean marks it clean" "$T/img" clean yes
	if [ "$version" -ne 3 ]; then
		set_super "$T/img" state 3
		run "$TUNEFS_MINIXFS" -c clean "$T/img"
		tuned "-c clean on errors"
		check_info "$v: -c clean clears the errors Linux records" \
		    "$T/img" clean yes
	fi

	run "$TUNEFS_MINIXFS" -m minix "$T/img"
	tuned "-m minix"
	check_info "$v: -m minix gives what MINIX works out" "$T/img" \
	    "max file size" "$minix"
	run "$FSCK_MINIXFS" -w "$T/img"
	check_true "$v: fsck -w has nothing to say after -m minix" \
	    test "$(grep -c ': warning: ' "$T/out")" -eq 0
	run "$TUNEFS_MINIXFS" -m linux "$T/img"
	tuned "-m linux"
	check_info "$v: -m linux gives what newfs_minixfs writes" "$T/img" \
	    "max file size" "$max"
	run "$TUNEFS_MINIXFS" -m 123456 "$T/img"
	tuned "-m 123456"
	check_info "$v: -m takes a number" "$T/img" "max file size" 123456

	run "$TUNEFS_MINIXFS" -e 0 "$T/img"
	tuned "-e 0"
	run "$FSCK_MINIXFS" -e 0 "$T/img"
	check_status "$v: -e 0 clears the bits past the end" 0
	run "$TUNEFS_MINIXFS" -e 1 "$T/img"
	tuned "-e 1"
	run "$FSCK_MINIXFS" -e 1 "$T/img"
	check_status "$v: -e 1 sets them" 0
done

# -B: the test tree converted to the other byte order is the image that
# mkimage makes in that order, and converting it back gives the original.
for format in 1/14 1/30 2/14 2/30 3/1024 3/4096; do
for logzone in 0 1; do
	version=${format%/*}
	if [ "$version" -eq 3 ]; then
		fs="version=3 block=${format#*/}"
	else
		fs="version=$version namelen=${format#*/}"
	fi
	v="V$format/$logzone"
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/le/" -e "s/@BLOCKS@/4096/" \
	    -e "s/@LOGZONE@/$logzone/" tests/tree.spec >"$T/spec"
	mkimage "$T/spec" "$T/le"
	sed -e "s/order=le/order=be/" "$T/spec" >"$T/spec.be"
	mkimage "$T/spec.be" "$T/be"

	cp "$T/le" "$T/img"
	run "$TUNEFS_MINIXFS" -N -B be "$T/img"
	check_out_has "$v: -N -B shows the change" \
	    "^byte order: little-endian -> big-endian\$"
	check_same_file "$v: -N -B writes nothing" "$T/le" "$T/img"
	run "$TUNEFS_MINIXFS" -B be "$T/img"
	tuned "-B be"
	check_same_file "$v: -B be gives the big-endian image" "$T/be" \
	    "$T/img"
	run "$TUNEFS_MINIXFS" -B le "$T/img"
	tuned "-B le"
	check_same_file "$v: -B le gives the original back" "$T/le" \
	    "$T/img"
	run "$TUNEFS_MINIXFS" -B le "$T/img"
	check_same_file "$v: -B to the same order changes nothing" "$T/le" \
	    "$T/img"
done
done

# -l: the test tree, with a directory that needs an indirect zone, keeps
# every name and file through 14 -> 30 -> 14.
for fs in "1 le" "1 be" "2 le" "2 be"; do
	version=${fs% *}
	order=${fs#* }
	v="V$version/$order"
	sed -e "s/@FS@/version=$version/" -e "s/@ORDER@/$order/" \
	    -e "s/@BLOCKS@/4096/" -e "s/@LOGZONE@/0/" \
	    -e "s/inodes=64/inodes=700/" tests/tree.spec >"$T/spec"
	echo "dir /many 0755 0 0 0" >>"$T/spec"
	i=0
	while [ "$i" -lt 600 ]; do
		echo "file /many/f$i 0644 0 0 0 1 $i"
		i=$((i + 1))
	done >>"$T/spec"
	mkimage "$T/spec" "$T/img"
	run "$MINIXFS" tar "$T/img"
	cp "$T/out" "$T/want.tar"

	cp "$T/img" "$T/before"
	run "$TUNEFS_MINIXFS" -N -l 30 "$T/img"
	check_out_has "$v: -N -l shows the change" "^name length: 14 -> 30\$"
	check_same_file "$v: -N -l writes nothing" "$T/before" "$T/img"
	for len in 30 14; do
		run "$TUNEFS_MINIXFS" -l "$len" "$T/img"
		tuned "-l $len"
		check_info "$v: -l $len gives $len-character names" "$T/img" \
		    "name length" "$len"
		run "$MINIXFS" tar "$T/img"
		check_out "$v: -l $len keeps every name and file" \
		    "$T/want.tar"
	done
	check_info "$v: the magic is back" "$T/img" magic \
	    "$(info_field "$T/before" magic)"
done

# Names too long for 14 characters are all listed, and nothing changes.
long=a_long_directory_name
cat >"$T/spec" <<EOF
fs version=2 namelen=30 order=le blocks=1024 inodes=64
dir  /$long 0755 0 0 0
file /$long/a_long_file_name 0644 0 0 0 10 1
file /short 0644 0 0 0 10 2
EOF
mkimage "$T/spec" "$T/img"
cp "$T/img" "$T/before"
run "$TUNEFS_MINIXFS" -l 14 "$T/img"
check_status "-l 14 refuses names that are too long" 1
check_true "-l 14 lists a long directory name" grep -q -x -e \
    "name longer than 14 characters: /$long" "$T/out"
check_true "-l 14 lists a long name below it" grep -q -x -e \
    "name longer than 14 characters: /$long/a_long_file_name" "$T/out"
check_same_file "-l 14 changes nothing when names are too long" \
    "$T/before" "$T/img"

# There must be room for the larger directories, or nothing changes.
cat >"$T/spec" <<EOF
fs version=1 order=le blocks=80 inodes=16
file /f 0644 0 0 0 10 1
dir  /d 0755 0 0 0
EOF
i=0
while [ "$i" -lt 200 ]; do
	echo "hard /d/h$i /f"
	i=$((i + 1))
done >>"$T/spec"
mkimage "$T/spec" "$T/img"
free=$(info_field "$T/img" "free zones")
echo "file /filler 0644 0 0 0 $(((free - 2) * 1024)) 2" >>"$T/spec"
mkimage "$T/spec" "$T/img"
check_info "the full image has one free zone" "$T/img" "free zones" 1
cp "$T/img" "$T/before"
run "$TUNEFS_MINIXFS" -l 30 "$T/img"
check_err "-l 30 needs room for the larger directories" \
    "No space left on device"
check_same_file "-l 30 changes nothing without room" "$T/before" "$T/img"

rm -f "$T/img"
"$NEWFS_MINIXFS" -V 3 -s 1024 "$T/img"
run "$TUNEFS_MINIXFS" -l 30 "$T/img"
check_err "V3 has no name length to change" "always 60 characters"

# -s: the test tree keeps every file through a small growth, which the
# zone map has room for, and a large one, for which the zone map needs
# more blocks and the inode table and the zones move up.
for format in 1/14 2/30 3/1024 3/4096; do
for order in le be; do
for logzone in 0 1; do
	version=${format%/*}
	if [ "$version" -eq 3 ]; then
		fs="version=3 block=${format#*/}"
	else
		fs="version=$version namelen=${format#*/}"
	fi
	v="V$format/$order/$logzone"
	blocks=$((8388608 / ${format#*/}))
	if [ "$version" -ne 3 ]; then
		blocks=8192
	fi
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/$order/" \
	    -e "s/@BLOCKS@/$blocks/" -e "s/@LOGZONE@/$logzone/" \
	    tests/tree.spec >"$T/spec"
	mkimage "$T/spec" "$T/img"
	run "$MINIXFS" tar "$T/img"
	cp "$T/out" "$T/want.tar"
	bs=$(info_field "$T/img" "block size")
	zmap=$(info_field "$T/img" "zone map blocks")

	cp "$T/img" "$T/before"
	run "$TUNEFS_MINIXFS" -N -s $((blocks + 64)) "$T/img"
	check_out_has "$v: -N -s shows the change" \
	    "^blocks: $blocks -> $((blocks + 64))\$"
	check_same_file "$v: -N -s writes nothing" "$T/before" "$T/img"

	for size in $((blocks + 64)) $((3 * blocks)); do
		run "$TUNEFS_MINIXFS" -s "$size" "$T/img"
		tuned "-s $size"
		check_true "$v: -s $size grows the image" \
		    test "$(($(wc -c <"$T/img")))" -eq $((size * bs))
		check_info "$v: -s $size gives the zones" "$T/img" zones \
		    $((size >> logzone))
		run "$MINIXFS" tar "$T/img"
		check_out "$v: -s $size keeps every file" "$T/want.tar"
	done
	if [ "$version" -ne 3 ] || [ "${format#*/}" -eq 1024 ]; then
		check_true "$v: the zone map got more blocks" test \
		    "$(info_field "$T/img" "zone map blocks")" -gt "$zmap"
	fi
	run "$TUNEFS_MINIXFS" -s "$blocks" "$T/img"
	tuned "-s $blocks back"
	check_info "$v: -s shrinks back to the zones" "$T/img" zones \
	    $((blocks >> logzone))
	run "$MINIXFS" tar "$T/img"
	check_out "$v: shrinking back keeps every file" "$T/want.tar"
done
done
done

# -s to shrink: with the files far in, past the new end, they move down
# to free zones; with too little room, nothing changes.
for format in 1/14 2/30 3/1024 3/4096; do
for order in le be; do
for logzone in 0 1; do
	version=${format%/*}
	if [ "$version" -eq 3 ]; then
		fs="version=3 block=${format#*/}"
	else
		fs="version=$version namelen=${format#*/}"
	fi
	v="V$format/$order/$logzone"
	blocks=8192
	skip=5000
	if [ "${format#*/}" -eq 4096 ]; then
		blocks=2048
		skip=1200
	fi
	skip=$((skip >> logzone))
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/$order/" \
	    -e "s/@BLOCKS@/$blocks/" -e "s/@LOGZONE@/$logzone/" \
	    -e "1s/\$/ skip=$skip/" tests/tree.spec >"$T/spec"
	mkimage "$T/spec" "$T/img"
	run "$MINIXFS" tar "$T/img"
	cp "$T/out" "$T/want.tar"
	bs=$(info_field "$T/img" "block size")

	cp "$T/img" "$T/before"
	run "$TUNEFS_MINIXFS" -s 64 "$T/img"
	check_err "$v: -s refuses a size too small for the files" \
	    "does not fit in 64 blocks; nothing changed"
	check_same_file "$v: a size too small changes nothing" \
	    "$T/before" "$T/img"

	size=$((blocks - (skip << logzone) + 64))
	run "$TUNEFS_MINIXFS" -s "$size" "$T/img"
	tuned "-s $size, moving the files down"
	check_true "$v: shrinking cuts the image" \
	    test "$(($(wc -c <"$T/img")))" -eq $((size * bs))
	run "$MINIXFS" tar "$T/img"
	check_out "$v: the files that moved down are whole" "$T/want.tar"
done
done
done

rm -f "$T/img"
"$NEWFS_MINIXFS" -V 1 -s 1000 "$T/img"
run "$TUNEFS_MINIXFS" -s 70000 "$T/img"
check_err "V1 cannot count 70000 zones" "too many zones for V1"
run "$TUNEFS_MINIXFS" -M 4608:2:0 -s 2000 "$T/img"
check_err "an image of one side of a disk cannot change size" \
    "cannot change size"

# -B, -l and -s refuse a file system that is not marked clean, since it
# may be mounted, unless -f is given; fsck_minixfs -y marks it clean.
rm -f "$T/img"
"$NEWFS_MINIXFS" -V 2 -s 1000 "$T/img"
run "$TUNEFS_MINIXFS" -c dirty "$T/img"
check_status "-c dirty works on a clean file system" 0
cp "$T/img" "$T/before"
for opt in "-B be" "-l 30" "-s 2000"; do
	# The option and its value are split on purpose.
	# shellcheck disable=SC2086
	run "$TUNEFS_MINIXFS" $opt "$T/img"
	check_err "$opt refuses a file system not marked clean" \
	    "not marked clean, so it may be mounted"
	check_same_file "$opt changes nothing then" "$T/before" "$T/img"
	# shellcheck disable=SC2086
	run "$TUNEFS_MINIXFS" -N $opt "$T/img"
	check_status "-N $opt shows the change all the same" 0
done
for opt in "-c clean" "-m minix" "-e 0"; do
	cp "$T/before" "$T/img"
	# shellcheck disable=SC2086
	run "$TUNEFS_MINIXFS" $opt "$T/img"
	check_status "$opt works on a file system not marked clean" 0
done
cp "$T/before" "$T/img"
run "$TUNEFS_MINIXFS" -f -s 2000 "$T/img"
check_status "-f grows a file system not marked clean" 0
run "$FSCK_MINIXFS" "$T/img"
check_status "fsck passes what -f -s grew" 0
cp "$T/before" "$T/img"
run "$FSCK_MINIXFS" -y "$T/img"
check_out_has "fsck -y marks the consistent file system clean" \
    ": marked clean\$"
run "$TUNEFS_MINIXFS" -s 2000 "$T/img"
check_status "-s works after fsck -y" 0

for bad in "-B middle" "-c maybe" "-e 2" "-m 0" "-m 2147483648" \
    "-m big" "-M 0:2:0" "-l 20" "-s 0" "-s x"; do
	# The option and its value are split on purpose.
	# shellcheck disable=SC2086
	run "$TUNEFS_MINIXFS" $bad "$T/img"
	check_status "tunefs $bad is a usage error" 2
done
run "$TUNEFS_MINIXFS" -c clean "$T/none"
check_status "a missing image fails" 1

finish
