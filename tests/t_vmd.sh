#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Minix-vmd: its super block keeps the zone size in a byte and flags in
# the next, and its flex directories take entries of 8-byte slots.  Names
# of every length up to 60 read back, entries that would cross a block
# start the next one, and the clean flag is its own; what writes entries
# of a fixed size refuses, and the rest works.

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
