#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# fsck_minixfs: consistent file systems of every format pass; each kind
# of damage is found and reported, and -y repairs it so that a second
# check finds nothing and marks the file system clean.  With -l, trees
# that no directory names go to /lost+found; with -e, the bits past the
# end of the maps must be as given.

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

# The images to damage.  The spec gives /d inode 2, /d/e 3, /f 4, /big 5
# (with a single indirect zone), /g 6, the device /c 7 and the symbolic
# link /l 8; inode 20 and later are free.
spec_small() {
	cat <<EOF
fs $1 blocks=1024 inodes=64
dir  /d 0755 0 0 0
dir  /d/e 0755 0 0 0
file /f 0644 0 0 0 3000 1
file /big 0644 0 0 0 40000 2
file /g 0644 0 0 0 10 3
dev  /c c 1 2 0644 0 0 0
link /l target 0 0 0
EOF
}

# fsck_damaged NAME PATTERN - run fsck_minixfs on $T/img and check that
# it reports PATTERN; then repair a copy, $T/fixed, with -y and check
# that it says so and that nothing is left.
fsck_damaged() {
	run "$FSCK_MINIXFS" "$T/img"
	check_found "$v: $1" "$2"
	cp "$T/img" "$T/fixed"
	run "$FSCK_MINIXFS" -y "$T/fixed"
	check_out_has "$v: -y repairs $1" "$2 (repaired)\$"
	run "$FSCK_MINIXFS" "$T/fixed"
	check_status "$v: nothing is left of $1" 0
}

# check_field NAME IMAGE INO FIELD VALUE - an inode field after a repair.
check_field() {
	check_true "$1" test "$(get_inode "$2" "$3" "$4")" -eq "$5"
}

# damage_super: the super block.
damage_super() {
	max=$("$TUNEFS_MINIXFS" "$T/good" |
	    sed -n 's/.*(MINIX works out \([0-9]*\))$/\1/p')
	cp "$T/good" "$T/img"
	set_super "$T/img" maxsize 0
	fsck_damaged "a maximum file size of 0" \
	    "the super block gives a maximum file size of 0"
	check_info "$v: -y sets the maximum file size MINIX works out" \
	    "$T/fixed" "max file size" "$max"

	# Half of the image is enough to hold the files.
	size=$(($(wc -c <"$T/good")))
	dd if="$T/good" of="$T/img" bs=1024 count=$((size / 2048)) \
	    2>/dev/null
	msg="the image holds $((size / 2)) bytes of the $size that the"
	msg="$msg file system needs"
	run "$FSCK_MINIXFS" "$T/img"
	check_found "$v: a short image" "$msg"
	run "$FSCK_MINIXFS" -y "$T/img"
	check_found "$v: -y leaves a short image" "$msg (not repaired)"
	check_found "$v: -y marks a short image as having errors" \
	    ": marked as having errors\$"
	check_info "$v: a short image is not clean" "$T/img" clean no

	if [ "$version" -eq 3 ]; then
		cp "$T/good" "$T/img"
		set_super "$T/img" flags 0
		run "$FSCK_MINIXFS" "$T/img"
		check_out_has "$v: fsck notes a file system not marked clean" \
		    ": the file system is not marked clean\$"
		check_status "$v: not being marked clean is no problem" 0
		run "$FSCK_MINIXFS" -y "$T/img"
		check_out_has "$v: -y marks it clean" ": marked clean\$"
		check_info "$v: the clean flag is set" "$T/img" clean yes

		cp "$T/good" "$T/img"
		set_super "$T/img" flags 257		# 0x101
		run "$FSCK_MINIXFS" "$T/img"
		check_status "$v: features it does not know stop fsck" 3
		return
	fi

	cp "$T/good" "$T/img"
	set_super "$T/img" state 0
	run "$FSCK_MINIXFS" "$T/img"
	check_out_has "$v: fsck notes a file system not marked clean" \
	    ": the file system is not marked clean\$"
	check_status "$v: not being marked clean is no problem" 0
	run "$FSCK_MINIXFS" -y "$T/img"
	check_out_has "$v: -y marks it clean" ": marked clean\$"
	check_info "$v: the state is valid" "$T/img" clean yes

	cp "$T/good" "$T/img"
	set_super "$T/img" state 3
	fsck_damaged "errors that Linux recorded" \
	    "the super block records errors"
	check_info "$v: -y clears the error state" "$T/fixed" clean yes
}

# damage_maps: bit maps that disagree with the files.
damage_maps() {
	cp "$T/good" "$T/img"
	set_map_bit "$T/img" imap 4 0
	fsck_damaged "an inode in use but free in the map" \
	    "inode 4 is in use but free in the inode map"

	cp "$T/good" "$T/img"
	set_map_bit "$T/img" imap 20 1
	fsck_damaged "a free inode marked in the map" \
	    "inode 20 is free but marked in the inode map"

	first=$(info_field "$T/good" "first data zone")
	zone=$(get_inode "$T/good" 4 zone0)
	cp "$T/good" "$T/img"
	set_map_bit "$T/img" zmap $((zone - first + 1)) 0
	fsck_damaged "a zone in use but free in the map" \
	    "zone $zone is in use but free in the zone map"

	last=$(($(info_field "$T/good" zones) - 1))
	cp "$T/good" "$T/img"
	set_map_bit "$T/img" zmap $((last - first + 1)) 1
	fsck_damaged "a free zone marked in the map" \
	    "zone $last is free but marked in the zone map"

	for map in inode zone; do
		cp "$T/good" "$T/img"
		set_map_bit "$T/img" "$(echo "$map" | cut -c1)map" 0 0
		fsck_damaged "bit 0 of the $map map clear" \
		    "bit 0 of the $map map is clear"
	done

	# mkimage sets the bits past the end of the maps, as Linux does.
	run "$FSCK_MINIXFS" -e 1 "$T/good"
	check_status "$v: -e 1 passes set bits past the end" 0
	run "$FSCK_MINIXFS" -e 0 "$T/good"
	check_found "$v: -e 0 finds set bits past the end" \
	    " bits past the last zone in the zone map are set\$"
	cp "$T/good" "$T/img"
	run "$FSCK_MINIXFS" -y -e 0 "$T/img"
	check_out_has "$v: -y -e 0 clears them" \
	    " bits past the last inode in the inode map are set (repaired)\$"
	run "$FSCK_MINIXFS" -e 0 "$T/img"
	check_status "$v: nothing is left of the set bits" 0
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$v: without -e the bits past the end do not matter" 0
	run "$FSCK_MINIXFS" -e 1 "$T/img"
	check_found "$v: -e 1 finds clear bits past the end" \
	    " bits past the last inode in the inode map are clear\$"
}

# damage_lost: inodes that no directory names, with and without -l.
damage_lost() {
	# Take "d", the third entry of the root, away from /d and its tree.
	rzone=$(get_inode "$T/good" 1 zone0)
	cp "$T/good" "$T/img"
	poke_number "$T/img" $(((rzone << lz) * bs + 2 * dsize)) "$width" 0
	set_inode "$T/img" 20 mode 33188		# 0100644
	run "$FSCK_MINIXFS" "$T/img"
	check_found "$v: a tree that no directory names" \
	    "inode 2 is in use (mode 40755) but no directory names it\$"

	cp "$T/img" "$T/fixed"
	run "$FSCK_MINIXFS" -y "$T/fixed"
	check_status "$v: -y frees the tree" 0
	run "$MINIXFS" ls "$T/fixed"
	check_out_has "$v: -y leaves the other files" "^f\$"
	run "$MINIXFS" ls "$T/fixed" /lost+found
	check_status "$v: -y makes no /lost+found" 1

	cp "$T/img" "$T/fixed"
	run "$FSCK_MINIXFS" -y -l "$T/fixed"
	check_out_has "$v: -y -l makes /lost+found" ": made /lost+found\$"
	check_out_has "$v: -y -l points \"..\" of the tree to /lost+found" \
	    ": /lost+found/#2: \"..\" names inode 1, not [0-9]* (repaired)\$"
	check_out_has "$v: -y -l links the tree into /lost+found" \
	    "inode 2 .* it goes to /lost+found/#2 (repaired)\$"
	check_out_has "$v: -y -l links a file into /lost+found" \
	    "inode 20 .* it goes to /lost+found/#20 (repaired)\$"
	run "$FSCK_MINIXFS" "$T/fixed"
	check_status "$v: nothing is left after -y -l" 0
	run "$MINIXFS" ls "$T/fixed" /lost+found
	printf '#2\n#20\n' >"$T/want"
	check_out "$v: /lost+found holds the tree and the file" "$T/want"
	run "$MINIXFS" ls "$T/fixed" "/lost+found/#2"
	echo e >"$T/want"
	check_out "$v: the tree keeps its entries" "$T/want"
	run "$MINIXFS" ls "$T/fixed" "/lost+found/#2/.."
	printf '#2\n#20\n' >"$T/want"
	check_out "$v: \"..\" of the tree names /lost+found" "$T/want"

	# /d/e names /d, and neither has a name: the loop is linked once.
	spec_small "$fs order=$order" >"$T/spec"
	echo "raw /d/e up 2" >>"$T/spec"
	mkimage "$T/spec" "$T/img"
	poke_number "$T/img" $(((rzone << lz) * bs + 2 * dsize)) "$width" 0
	run "$FSCK_MINIXFS" -y -l "$T/img"
	check_status "$v: -y -l links a loop that nothing names" 0
	run "$MINIXFS" ls "$T/img" "/lost+found/#2"
	echo e >"$T/want"
	check_out "$v: the loop is cut at its second name" "$T/want"

	# A /lost+found that is there is used.
	spec_small "$fs order=$order" >"$T/spec"
	echo "dir /lost+found 0700 0 0 0" >>"$T/spec"
	mkimage "$T/spec" "$T/img"
	set_inode "$T/img" 20 mode 33188
	run "$FSCK_MINIXFS" -y -l "$T/img"
	check_status "$v: -y -l uses the /lost+found that is there" 0
	run "$MINIXFS" ls "$T/img" /lost+found
	echo "#20" >"$T/want"
	check_out "$v: the file goes into it" "$T/want"
	run "$MINIXFS" ls "$T/img"
	check_true "$v: the root still has one /lost+found" \
	    test "$(grep -c '^lost+found$' "$T/out")" -eq 1
}

# damage_inodes: inodes that cannot be right.
damage_inodes() {
	cp "$T/good" "$T/img"
	set_inode "$T/img" 4 nlinks 3
	fsck_damaged "a wrong link count" \
	    "inode 4: link count 3, but 1 entries"
	check_field "$v: -y sets the link count" "$T/fixed" 4 nlinks 1

	cp "$T/good" "$T/img"
	set_inode "$T/img" 4 zone0 60000
	fsck_damaged "a zone outside the data area" \
	    "/f: zone 60000 is outside the data area"
	check_field "$v: -y clears a zone outside the data area" "$T/fixed" \
	    4 zone0 0

	# A bad zone in the indirect zone of /big, before the good ones: only
	# the zone it replaced is left over.
	zwidth=32
	if [ "$version" -eq 1 ]; then
		zwidth=16
	fi
	ind=$(get_inode "$T/good" 5 zone7)
	cp "$T/good" "$T/img"
	poke_number "$T/img" $(((ind << lz) * bs + zwidth / 8)) "$zwidth" \
	    60000
	fsck_damaged "a zone outside the data area in an indirect zone" \
	    "/big: zone 60000 is outside the data area"
	run "$FSCK_MINIXFS" "$T/img"
	check_true "$v: the zones after a bad one in an indirect zone count" \
	    test "$(grep -c 'free but marked' "$T/out")" -eq 1

	cp "$T/good" "$T/img"
	set_inode "$T/img" 6 zone0 "$(get_inode "$T/good" 4 zone0)"
	fsck_damaged "a zone that two files use" "used by another file too"
	check_field "$v: -y clears the second use of a zone" "$T/fixed" \
	    6 zone0 0
	run "$MINIXFS" cat "$T/fixed" /f
	check_out "$v: the first file keeps its zone" "$T/exp/f"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 20 mode 33188		# 0100644
	fsck_damaged "an inode in use that no directory names" \
	    "inode 20 is in use (mode 100644) but no directory names it"
	check_field "$v: -y frees an inode that no directory names" \
	    "$T/fixed" 20 mode 0

	cp "$T/good" "$T/img"
	set_inode "$T/img" 6 mode 62884			# 0172644
	fsck_damaged "an inode with no valid type" "/g: .* no valid type"
	check_field "$v: -y frees an inode with no valid type" "$T/fixed" \
	    6 mode 0

	size=$(get_inode "$T/good" 2 size)
	cp "$T/good" "$T/img"
	set_inode "$T/img" 2 size $((size + 1))
	msg="/d: directory size $((size + 1)) is not a whole number of entries"
	fsck_damaged "a directory size that is not whole entries" "$msg"
	check_field "$v: -y cuts a directory to whole entries" "$T/fixed" \
	    2 size "$size"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 7 zone1 99
	fsck_damaged "a device file with a zone" \
	    "/c: device file has zone numbers besides its device number"
	check_field "$v: -y clears the zone of a device file" "$T/fixed" \
	    7 zone1 0
	check_field "$v: -y keeps the device number" "$T/fixed" 7 zone0 \
	    "$(get_inode "$T/good" 7 zone0)"

	# The text "target" of /l.
	text=$((($(get_inode "$T/good" 8 zone0) << lz) * bs))
	cp "$T/good" "$T/img"
	poke "$T/img" $((text + 3)) 000
	fsck_damaged "a NUL byte in a symbolic link" \
	    "/l: symbolic link of 6 bytes ends at a NUL byte after 3"
	check_field "$v: -y cuts a symbolic link at a NUL byte" \
	    "$T/fixed" 8 size 3

	for bad in "size 0:of 0 bytes" "size 5000:of 5000 bytes" \
	    "nul:that starts with a NUL byte"; do
		cp "$T/good" "$T/img"
		case $bad in
		size*)
			size=${bad%%:*}
			set_inode "$T/img" 8 size "${size#size }"
			;;
		nul:*)
			poke "$T/img" "$text" 000
			;;
		esac
		fsck_damaged "a symbolic link ${bad#*:}" \
		    "/l: names inode 8, which is a symbolic link ${bad#*:}"
		run "$MINIXFS" ls "$T/fixed" /l
		check_status "$v: -y removes a symbolic link ${bad#*:}" 1
	done

	# Only V1 limits the size of a file to less than 4 GiB.
	if [ "$version" -eq 1 ]; then
		cp "$T/good" "$T/img"
		set_inode "$T/img" 6 size 2147483647
		fsck_damaged "a size the zones cannot reach" \
		    "/g: size 2147483647 is more than the zones reach"
		check_field "$v: -y cuts a size to what the zones reach" \
		    "$T/fixed" 6 size 268966912
	fi
}

# damage_dirs: directory entries that point to the wrong places.
damage_dirs() {
	# "." of /d is the first entry in its first zone.
	zone=$(get_inode "$T/good" 2 zone0)
	cp "$T/good" "$T/img"
	poke_number "$T/img" $(((zone << lz) * bs)) "$width" 5
	fsck_damaged "\".\" naming another inode" \
	    "/d: \".\" names inode 5, not 2"

	# "x" in place of "." names /d a second time, and goes.
	cp "$T/good" "$T/img"
	poke "$T/img" $(((zone << lz) * bs + width / 8)) 170	# "x"
	fsck_damaged "a missing \".\"" "/d: \".\" is not entry 1"
	run "$MINIXFS" ls "$T/fixed" /d
	echo e >"$T/want"
	check_out "$v: -y puts \".\" back" "$T/want"

	# "m", naming /f, in place of ".." of /d/e moves out of the way.
	ezone=$(get_inode "$T/good" 3 zone0)
	cp "$T/good" "$T/img"
	poke_number "$T/img" $(((ezone << lz) * bs + dsize)) "$width" 4
	poke "$T/img" $(((ezone << lz) * bs + dsize + width / 8)) 155 000
	fsck_damaged "an entry in place of \"..\"" \
	    "/d/e: \"..\" is not entry 2"
	run "$MINIXFS" ls "$T/fixed" /d/e
	echo m >"$T/want"
	check_out "$v: -y moves the entry in place of \"..\"" "$T/want"
	run "$MINIXFS" cat "$T/fixed" /d/e/m
	check_out "$v: the moved entry names the same file" "$T/exp/f"
	run "$MINIXFS" ls "$T/fixed" /d/e/..
	echo e >"$T/want"
	check_out "$v: the new \"..\" names the parent" "$T/want"

	# A directory with no zone gets one for "." and "..".
	cp "$T/good" "$T/img"
	set_inode "$T/img" 3 size 0
	set_inode "$T/img" 3 zone0 0
	fsck_damaged "a directory with no entries" \
	    "/d/e: \"\.\.\" is not entry 2"
	check_field "$v: -y gives the directory two entries" "$T/fixed" 3 \
	    size $((2 * dsize))
	run "$MINIXFS" ls "$T/fixed" /d/e/..
	echo e >"$T/want"
	check_out "$v: -y gives the directory a \"..\"" "$T/want"

	for raw in "bad 999:names inode 999, past the last" \
	    "free 30:names inode 30, which is free" \
	    "a/b 4:bad name \"a/b\"" \
	    "up 2:directory inode 2 is listed in more than one directory"; do
		spec_small "$fs order=$order" >"$T/spec"
		echo "raw /d/e ${raw%%:*}" >>"$T/spec"
		mkimage "$T/spec" "$T/img"
		fsck_damaged "an entry \"${raw%%:*}\"" "${raw#*:}"
		run "$MINIXFS" ls "$T/fixed" /d/e
		: >"$T/none"
		check_out "$v: -y removes the entry \"${raw%%:*}\"" "$T/none"
	done
}

run "$FSCK_MINIXFS"
check_status "no image is a usage error" 2
run "$FSCK_MINIXFS" -e 2 "$T/none"
check_status "-e takes only 0 and 1" 2

dd if=/dev/zero of="$T/zero" bs=1024 count=64 2>/dev/null
run "$FSCK_MINIXFS" "$T/zero"
check_status "a file that is not a file system cannot be checked" 3

# Consistent images of every format pass.
for format in 1/14/1024 1/30/1024 2/14/1024 2/30/1024 3/60/1024 3/60/4096; do
for order in le be; do
for logzone in 0 1; do
	version=${format%%/*}
	namelen=${format#*/}
	namelen=${namelen%/*}
	bsize=${format##*/}
	if [ "$version" -eq 3 ]; then
		fs="version=3 block=$bsize"
	else
		fs="version=$version namelen=$namelen"
	fi
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/$order/" \
	    -e "s/@BLOCKS@/$((4194304 / bsize))/" \
	    -e "s/@LOGZONE@/$logzone/" tests/tree.spec >"$T/spec"
	mkimage "$T/spec" "$T/img"
	run "$FSCK_MINIXFS" "$T/img"
	check_out_has "V$format/$order/$logzone: the test tree is consistent" \
	    " 0 problems\$"
done
done
done

# More directories in one than the queue of the walk first has room for.
{
	echo "fs version=2 order=le blocks=4096 inodes=400"
	i=0
	while [ "$i" -lt 100 ]; do
		echo "dir /d$i 0755 0 0 0"
		i=$((i + 1))
	done
} >"$T/spec"
mkimage "$T/spec" "$T/img"
run "$FSCK_MINIXFS" "$T/img"
check_status "a directory holding a hundred directories is consistent" 0

rm -f "$T/img"
"$NEWFS_MINIXFS" -V 3 -s 2048 "$T/img"
run "$FSCK_MINIXFS" "$T/img"
check_out_has "an empty file system from newfs_minixfs is consistent" \
    "1 of 704 inodes and 1 of 2033 zones in use, 0 problems\$"

# -w notes what the fsck of MINIX 3 warns about, and none of it is a
# problem.
spec_small "version=3 block=4096 order=le" >"$T/spec"
mkimage "$T/spec" "$T/img"
run "$FSCK_MINIXFS" -w "$T/img"
check_true "-w notes nothing about a layout as MINIX makes it" \
    test "$(grep -c ': warning: ' "$T/out")" -eq 0
spec_small "version=3 block=4096 order=le spare=1 gap=2" >"$T/spec"
mkimage "$T/spec" "$T/img"
run "$FSCK_MINIXFS" "$T/img"
check_status "spare map blocks and a gap are no problem" 0
check_true "without -w they are not noted" \
    test "$(grep -c ': warning: ' "$T/out")" -eq 0
run "$FSCK_MINIXFS" -w "$T/img"
check_status "-w does not make them a problem" 0
check_out_has "-w notes a spare inode map block" \
    ": warning: 2 inode map blocks, where 1 are enough\$"
check_out_has "-w notes a spare zone map block" \
    ": warning: 2 zone map blocks, where 1 are enough\$"
first=$(info_field "$T/img" "first data zone")
check_out_has "-w notes a gap before the first data zone" \
    ": warning: the first data zone is $first, where the inode table \
leaves room from $((first - 2))\$"
set_super "$T/img" maxsize 1000000
run "$FSCK_MINIXFS" -w "$T/img"
check_out_has "-w notes a maximum file size other than MINIX's" \
    ": warning: the maximum file size is 1000000, where MINIX works out \
2147483647\$"

# A file past the maximum file size is whole, but Linux maps no block
# past it and none of the systems lets it grow: -w notes it, and it is no
# problem.
rm -f "$T/img" "$T/f"
"$NEWFS_MINIXFS" -V 2 -s 400 "$T/img" >/dev/null
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "x" }' >"$T/f"
"$MINIXFS" put "$T/img" "$T/f" /f
set_super "$T/img" maxsize 10000
run "$FSCK_MINIXFS" "$T/img"
check_status "a file past the maximum file size is no problem" 0
check_true "without -w it is not noted" \
    test "$(grep -c ': warning: ' "$T/out")" -eq 0
run "$FSCK_MINIXFS" -w "$T/img"
check_out_has "-w notes a file past the maximum file size" \
    ": warning: /f: size 20000 is more than the maximum file size, 10000\$"
spec_small "version=2 order=be" >"$T/spec"
mkimage "$T/spec" "$T/img"
run "$FSCK_MINIXFS" -w "$T/img"
check_out_has "-w notes the maximum file size that Linux writes in V2" \
    ": warning: the maximum file size is 2147483647, where MINIX works \
out 67378176\$"

# Each kind of damage, in every version and both byte orders.
for fs in "version=1" "version=2 namelen=30" "version=3" \
    "version=3 block=4096"; do
for order in le be; do
	v="$fs/$order"
	version=${fs#version=}
	version=${version%% *}
	spec_small "$fs order=$order" >"$T/spec"
	rm -rf "$T/exp"
	mkimage "$T/spec" "$T/good" "$T/exp"
	bs=$(info_field "$T/good" "block size")
	lz=$(info_field "$T/good" "log zone size")
	width=16
	if [ "$version" -eq 3 ]; then
		width=32
	fi
	dsize=$((width / 8 + $(info_field "$T/good" "name length")))
	run "$FSCK_MINIXFS" "$T/good"
	check_status "$v: the image to damage is consistent" 0
	cp "$T/good" "$T/img"
	run "$FSCK_MINIXFS" -y "$T/img"
	check_status "$v: -y passes a consistent image" 0
	check_same_file "$v: -y leaves a consistent image as it was" \
	    "$T/good" "$T/img"
	damage_super
	damage_maps
	damage_inodes
	damage_dirs
	damage_lost
done
done

finish
