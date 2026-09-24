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

for bad in "-B middle" "-c maybe" "-e 2" "-m 0" "-m 2147483648" \
    "-m big" "-T 0:2:0"; do
	# The option and its value are split on purpose.
	# shellcheck disable=SC2086
	run "$TUNEFS_MINIXFS" $bad "$T/img"
	check_status "tunefs $bad is a usage error" 2
done
run "$TUNEFS_MINIXFS" -c clean "$T/none"
check_status "a missing image fails" 1

finish
