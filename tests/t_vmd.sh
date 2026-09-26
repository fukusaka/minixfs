#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Minix-vmd: its super block keeps the zone size in a byte and flags in
# the next, and its flex directories take entries of 8-byte slots.  Names
# of every length up to 60 read back, entries that would cross a block
# start the next one, and the clean flag is its own.  The library adds,
# links, renames and removes entries of flex directories as Minix-vmd
# does, and fsck puts back "." and ".." and makes lost+found in them,
# which the fsck of Minix-vmd passes where VMD_FSCK names it.

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

# Names on both sides of each count of extra slots, and the longest.
names="a abcd abcde abcdefghijkl abcdefghijklm abcdefghijklmnopqrst"
names="$names abcdefghijklmnopqrstu"
long=abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz01234567

# free_counts IMAGE INO - the free slots of the flex directory INO, in its
# first zone, that give a count of slots: none, if entries are freed as
# Minix-vmd frees them.
free_counts() {
	_free_counts_off=$(($(get_inode "$1" "$2" zone0) * 1024))
	od -An -tu1 -v -j "$_free_counts_off" \
	    -N "$(get_inode "$1" "$2" size)" "$1" | awk '
	    { for (i = 1; i <= NF; i++) b[n++] = $i }
	    END {
		for (s = 0; s + 8 <= n; s += 8)
			if (b[s] == 0 && b[s + 1] == 0 && b[s + 2] != 0)
				bad++
		print bad + 0
	    }'
}

# vmd_fsck NAME IMAGE - the fsck of Minix-vmd, where VMD_FSCK names the
# directory tests/vmd-fsck.sh built it into, passes IMAGE.  It reads only
# the byte order of the machine, little-endian here.
vmd_fsck() {
	if [ -z "${VMD_FSCK:-}" ]; then
		skip "$1" "VMD_FSCK is not set; see tests/vmd-fsck.sh"
	elif [ "$order" != le ]; then
		skip "$1" "the fsck of Minix-vmd reads its own byte order"
	else
		run "$VMD_FSCK/fsck${version}f" "$2"
		check_status "$1" 0
	fi
}

# size_in IMAGE DIR NAME - the size of NAME in the directory DIR of IMAGE.
size_in() {
	"$MINIXFS" ls -l "$1" "$2" | awk -v n="$3" '$NF == n { print $5 }'
}

# Names of 30 characters, which take 5 slots: 25 fill 125 of the 128
# slots of a block.
thirty() {
	i=0
	while [ "$i" -lt "$1" ]; do
		printf "%s thirty_characters_long_name_%02d\n" "$2" "$i"
		i=$((i + 1))
	done
}

for fs in "1 le" "2 le" "2 be"; do
	version=${fs% *}
	order=${fs#* }
	v="V$version/$order"
	{
		echo "fs version=$version vmd order=$order blocks=2048" \
		    "inodes=256"
		echo "dir /d 0755 0 0 0"
		i=0
		for n in $names $long; do
			i=$((i + 1))
			echo "file /d/$n 0644 0 0 0 $i $i"
		done
		# Enough entries of 4 slots that some would cross a block.
		echo "dir /many 0755 0 0 0"
		i=0
		while [ "$i" -lt 100 ]; do
			echo "file /many/name_of_twenty_$i 0644 0 0 0 1 $i"
			i=$((i + 1))
		done
		echo "raw /d bad_and_long_name 200"
	} >"$T/spec"
	rm -rf "$T/exp"
	mkimage "$T/spec" "$T/good" "$T/exp"

	check_info "$v: info names the variant" "$T/good" variant \
	    "Minix-vmd, flex directories"
	check_info "$v: info reads the zone size from its byte" "$T/good" \
	    "log zone size" 0
	check_info "$v: info reads the clean flag" "$T/good" clean yes
	run "$MINIXFS" ls "$T/good" /d
	for n in $names $long bad_and_long_name; do
		echo "$n"
	done >"$T/want"
	check_out "$v: names of every length read back" "$T/want"
	run "$MINIXFS" ls "$T/good" /many
	check_true "$v: all entries of a directory of blocks read back" \
	    test "$(wc -l <"$T/out")" -eq 100
	run "$MINIXFS" cat "$T/good" "/d/$long"
	check_status "$v: a file of a 60-character name opens" 0

	# The raw entry names a free inode; fsck removes it in place.
	run "$FSCK_MINIXFS" "$T/good"
	check_found "$v: fsck reads flex directories" \
	    "/d/bad_and_long_name: names inode 200, which is free"
	cp "$T/good" "$T/img"
	run "$FSCK_MINIXFS" -y "$T/img"
	check_status "$v: fsck -y removes an entry of a flex directory" 0
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$v: nothing is left after fsck -y" 0
	# As Minix-vmd, fsck frees each slot of the entry it removed.
	check_true "$v: a removed entry leaves no count of slots" \
	    test "$(free_counts "$T/img" 2)" -eq 0
	rm -rf "$T/x"
	run "$MINIXFS" extract "$T/img" "$T/x"
	check_same_tree "$v: the files read back whole" "$T/exp" "$T/x"

	# The clean flag is a bit of the flags byte, and only that changes.
	cp "$T/img" "$T/before"
	run "$TUNEFS_MINIXFS" -c dirty "$T/img"
	check_info "$v: -c dirty clears the clean flag" "$T/img" clean no
	run "$FSCK_MINIXFS" "$T/img"
	check_out_has "$v: fsck notes it" "not marked clean\$"
	run "$FSCK_MINIXFS" -y "$T/img"
	check_out_has "$v: fsck -y sets it again" ": marked clean\$"
	check_same_file "$v: the flag was all that changed" "$T/before" \
	    "$T/img"

	# Entries go into flex directories as Minix-vmd enters them, in an
	# image without the entry of a free inode, whose maps end in clear
	# bits and whose maximum file size is that of MINIX, as the mkfs of
	# Minix-vmd makes it.
	cp "$T/good" "$T/w.img"
	"$FSCK_MINIXFS" -y "$T/w.img" >/dev/null
	"$TUNEFS_MINIXFS" -e 0 -m minix "$T/w.img" >/dev/null
	vmd_fsck "$v: the fsck of Minix-vmd passes the image" "$T/w.img"
	{
		echo "mkdir /w 0755"
		for n in $names $long; do
			echo "mknod /w/$n f 0644"
		done
		echo "link /w/a /w/linked_to_a"
		echo "rename /w/abcd /w/renamed_from_abcd"
		echo "unlink /w/abcde"
		echo "mkdir /w/sub 0755"
		echo "rmdir /w/sub"
		echo "mkdir /w/empty 0755"
	} | "$MFSOP" "$T/w.img" >"$T/ops"
	check_true "$v: names are added, linked, renamed and removed" \
	    test "$(grep -vc ' ok$' "$T/ops")" -eq 0
	run "$MINIXFS" ls "$T/w.img" /w
	for n in $names $long linked_to_a renamed_from_abcd empty; do
		case $n in abcd|abcde) ;; *) echo "$n" ;; esac
	done | sort >"$T/want"
	sort "$T/out" >"$T/got"
	check_same_file "$v: and read back" "$T/want" "$T/got"
	check_true "$v: a new directory holds . and .. in a slot each" \
	    test "$(size_in "$T/w.img" /w empty)" -eq 16
	run "$FSCK_MINIXFS" "$T/w.img"
	check_status "$v: fsck passes the flex directories" 0
	vmd_fsck "$v: so does the fsck of Minix-vmd" "$T/w.img"

	# An entry that does not fit in what is left of a block starts the
	# next, and freed slots are taken again.
	cp "$T/good" "$T/w.img"
	"$FSCK_MINIXFS" -y "$T/w.img" >/dev/null
	"$TUNEFS_MINIXFS" -e 0 -m minix "$T/w.img" >/dev/null
	{
		echo "mkdir /b 0755"
		thirty 26 "mknod /b/"
	} | sed 's|/ |/|; s|$| f 0644|; 1s| f 0644$||' | "$MFSOP" "$T/w.img" \
	    >"$T/ops"
	check_true "$v: the 26th entry of 5 slots starts a new block" \
	    test "$(size_in "$T/w.img" / b)" -eq 1064
	printf 'unlink /b/thirty_characters_long_name_00\n%s\n' \
	    "mknod /b/thirty_characters_long_name_99 f 0644" |
	    "$MFSOP" "$T/w.img" >"$T/ops"
	check_true "$v: a freed entry is taken again" \
	    test "$(size_in "$T/w.img" / b)" -eq 1064
	run "$FSCK_MINIXFS" "$T/w.img"
	check_status "$v: fsck passes the directory of two blocks" 0
	vmd_fsck "$v: so does the fsck of Minix-vmd, again" "$T/w.img"

	# fsck puts "." and ".." back in flex directories, moving an entry
	# that takes the slot of ".", and makes /lost+found in them.  /d is
	# inode 2, and a, its first file, inode 3.
	cp "$T/good" "$T/w.img"
	"$FSCK_MINIXFS" -y "$T/w.img" >/dev/null
	"$TUNEFS_MINIXFS" -e 0 -m minix "$T/w.img" >/dev/null
	cp "$T/w.img" "$T/base.img"
	dz=$(($(get_inode "$T/w.img" 2 zone0) * 1024))
	poke_number "$T/w.img" "$dz" 16 0
	poke_number "$T/w.img" $((dz + 8)) 16 0
	run "$FSCK_MINIXFS" -y "$T/w.img"
	check_out_has "$v: fsck -y puts \".\" back in a flex directory" \
	    '"\." is not entry 1 (repaired)$'
	check_out_has "$v: and \"..\"" '"\.\." is not entry 2 (repaired)$'
	run "$FSCK_MINIXFS" "$T/w.img"
	check_status "$v: nothing is left of the missing dots" 0
	vmd_fsck "$v: the fsck of Minix-vmd passes the dots put back" \
	    "$T/w.img"

	cp "$T/base.img" "$T/w.img"
	poke_number "$T/w.img" "$dz" 16 3
	poke "$T/w.img" $((dz + 3)) 172 172 000 000 000
	run "$FSCK_MINIXFS" -y "$T/w.img"
	check_out_has "$v: fsck -y puts \".\" in place of another entry" \
	    '"\." is not entry 1 (repaired)$'
	run "$MINIXFS" ls "$T/w.img" /d
	check_out_has "$v: which it moves" "^zz\$"
	run "$FSCK_MINIXFS" "$T/w.img"
	check_status "$v: nothing is left of the entry moved" 0
	vmd_fsck "$v: the fsck of Minix-vmd passes the entry moved" \
	    "$T/w.img"

	# Take "d", the third entry of the root, away from /d and its tree.
	cp "$T/base.img" "$T/w.img"
	rz=$(($(get_inode "$T/w.img" 1 zone0) * 1024))
	poke_number "$T/w.img" $((rz + 16)) 16 0
	run "$FSCK_MINIXFS" -y -l "$T/w.img"
	check_out_has "$v: fsck -y -l makes /lost+found in a flex directory" \
	    ": made /lost+found\$"
	run "$MINIXFS" ls "$T/w.img" /lost+found
	echo "#2" >"$T/want"
	check_out "$v: and links the tree into it" "$T/want"
	run "$FSCK_MINIXFS" "$T/w.img"
	check_status "$v: nothing is left after -y -l" 0
	vmd_fsck "$v: the fsck of Minix-vmd passes /lost+found" "$T/w.img"

	run "$TUNEFS_MINIXFS" -B be "$T/img"
	check_err "$v: -B refuses Minix-vmd" "Minix-vmd"
	run "$TUNEFS_MINIXFS" -l 30 "$T/img"
	check_err "$v: -l refuses Minix-vmd" "Minix-vmd"
	check_same_file "$v: the refusals change nothing" "$T/before" \
	    "$T/img"
	run "$TUNEFS_MINIXFS" -s 4096 "$T/img"
	check_status "$v: -s grows Minix-vmd" 0
	run "$FSCK_MINIXFS" "$T/img"
	check_status "$v: fsck passes the grown image" 0
	check_info "$v: growing keeps the variant" "$T/img" variant \
	    "Minix-vmd, flex directories"
done

finish
