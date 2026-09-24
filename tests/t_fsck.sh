#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# fsck_minixfs: consistent file systems of every format pass, and each
# kind of damage is found and reported.

. ./tests/lib.sh

# The images to damage.  The spec gives /d inode 2, /d/e 3, /f 4, /big 5
# (with a single indirect zone) and /g 6; inode 20 and later are free.
spec_small() {
	cat <<EOF
fs $1 blocks=1024 inodes=64
dir  /d 0755 0 0 0
dir  /d/e 0755 0 0 0
file /f 0644 0 0 0 3000 1
file /big 0644 0 0 0 40000 2
file /g 0644 0 0 0 10 3
EOF
}

# fsck_damaged NAME PATTERN - run fsck_minixfs on $T/img and check that
# it reports PATTERN.
fsck_damaged() {
	run "$FSCK_MINIXFS" "$T/img"
	check_found "$v: $1" "$2"
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
}

# damage_inodes: inodes that cannot be right.
damage_inodes() {
	cp "$T/good" "$T/img"
	set_inode "$T/img" 4 nlinks 3
	fsck_damaged "a wrong link count" \
	    "inode 4: link count 3, but 1 entries"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 4 zone0 60000
	fsck_damaged "a zone outside the data area" \
	    "/f: zone 60000 is outside the data area"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 6 zone0 "$(get_inode "$T/good" 4 zone0)"
	fsck_damaged "a zone that two files use" "used by another file too"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 20 mode 33188		# 0100644
	fsck_damaged "an inode in use that no directory names" \
	    "inode 20 is in use (mode 100644) but no directory names it"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 6 mode 62884			# 0172644
	fsck_damaged "an inode with no valid type" "/g: .* no valid type"

	cp "$T/good" "$T/img"
	set_inode "$T/img" 2 size 33
	fsck_damaged "a directory size that is not whole entries" \
	    "/d: directory size 33 is not a whole number of entries"

	# Only V1 limits the size of a file to less than 4 GiB.
	if [ "$version" -eq 1 ]; then
		cp "$T/good" "$T/img"
		set_inode "$T/img" 6 size 2147483647
		fsck_damaged "a size the zones cannot reach" \
		    "/g: size 2147483647 is more than the zones reach"
	fi
}

# damage_dirs: directory entries that point to the wrong places.
damage_dirs() {
	# "." of /d is the first entry in its first zone.
	bs=$(info_field "$T/good" "block size")
	lz=$(info_field "$T/good" "log zone size")
	zone=$(get_inode "$T/good" 2 zone0)
	width=16
	if [ "$version" -eq 3 ]; then
		width=32
	fi
	cp "$T/good" "$T/img"
	poke_number "$T/img" $(((zone << lz) * bs)) "$width" 5
	fsck_damaged "\".\" naming another inode" \
	    "/d: \".\" names inode 5, not 2"

	for raw in "bad 999:names inode 999, past the last" \
	    "free 30:names inode 30, which is free" \
	    "a/b 4:bad name \"a/b\"" \
	    "up 2:directory inode 2 is listed in more than one"; do
		spec_small "$fs order=$order" >"$T/spec"
		echo "raw /d/e ${raw%%:*}" >>"$T/spec"
		mkimage "$T/spec" "$T/img"
		fsck_damaged "an entry \"${raw%%:*}\"" "${raw#*:}"
	done
}

run "$FSCK_MINIXFS"
check_status "no image is a usage error" 2

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

rm -f "$T/img"
"$NEWFS_MINIXFS" -V 3 -s 2048 "$T/img"
run "$FSCK_MINIXFS" "$T/img"
check_out_has "an empty file system from newfs_minixfs is consistent" \
    "1 of 704 inodes and 1 of 2033 zones in use, 0 problems\$"

# Each kind of damage, in every version and both byte orders.
for fs in "version=1" "version=2 namelen=30" "version=3" \
    "version=3 block=4096"; do
for order in le be; do
	v="$fs/$order"
	version=${fs#version=}
	version=${version%% *}
	spec_small "$fs order=$order" >"$T/spec"
	mkimage "$T/spec" "$T/good"
	run "$FSCK_MINIXFS" "$T/good"
	check_status "$v: the image to damage is consistent" 0
	damage_maps
	damage_inodes
	damage_dirs
done
done

finish
