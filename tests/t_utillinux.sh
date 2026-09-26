#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Cross-checks with the MINIX tools of util-linux, where they exist.  They
# are optional: without them the checks are skipped.
#
#  - fsck.minix must accept the images that mkimage writes, which shows
#    that the test images themselves are sound.  It reads little-endian
#    file systems with 1024-byte blocks and one-block zones only.
#  - Images made by mkfs.minix must be readable.
#  - fsck_minixfs and fsck.minix must agree on damaged images, and
#    fsck.minix must accept what fsck_minixfs -y repaired.
#
# FSCK_MINIX and MKFS_MINIX name the tools (default: from PATH).
# UTILLINUX_SRC names a tree of the util-linux sources, whose tests hold
# as hexdumps the images that mkfs.minix makes on a big-endian machine;
# the big-endian V3 is read from there.
#
# fsck.minix calls sync(2) three times each run, which waits for every
# file system of the host, and takes most of the time of this test.
# UTILLINUX_NOSYNC=yes has strace(1) turn those calls into ones that
# succeed at once, where it can (Linux): the images are files of the
# host that the tools here wrote, which fsck.minix reads through the same
# cache, so what it reads is the same.  fsck.minix only checks here; a
# repair by it, which is to reach the disk, is not to go through
# fsck_minix.

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

: "${FSCK_MINIX:=fsck.minix}"
: "${MKFS_MINIX:=mkfs.minix}"
: "${UTILLINUX_NOSYNC:=no}"

have() {
	command -v "$1" >/dev/null 2>&1
}

nosync=no
if [ "$UTILLINUX_NOSYNC" = yes ] && have "$FSCK_MINIX"; then
	if strace -qq -e trace=sync -e inject=sync:retval=0 -o /dev/null \
	    true >/dev/null 2>&1; then
		nosync=yes
	else
		echo "# UTILLINUX_NOSYNC: strace cannot skip sync(2) here;" \
		    "fsck.minix runs as it is"
	fi
fi

# fsck_minix ARGS... - run fsck.minix, without its sync(2) calls if
# UTILLINUX_NOSYNC asks and strace can.  run(), which takes a command,
# calls it.
# shellcheck disable=SC2317
fsck_minix() {
	if [ "$nosync" = yes ]; then
		strace -qq -e trace=sync -e inject=sync:retval=0 \
		    -o /dev/null "$FSCK_MINIX" "$@"
	else
		"$FSCK_MINIX" "$@"
	fi
}

# fsck_accepts: fsck.minix accepts the test tree in every format it
# reads, and notices damage.
fsck_accepts() {
	for fs in "version=1 namelen=14" "version=1 namelen=30" \
	    "version=2 namelen=14" "version=2 namelen=30" "version=3"; do
		sed -e "s/@FS@/$fs/" -e "s/@ORDER@/le/" -e "s/@BLOCKS@/4096/" \
		    -e "s/@LOGZONE@/0/" tests/tree.spec >"$T/spec"
		mkimage "$T/spec" "$T/img"
		run fsck_minix -f "$T/img"
		check_status "fsck.minix accepts the test tree ($fs)" 0
	done

	# fsck.minix must notice damage, or the checks above say nothing.
	cp "$T/img" "$T/bad"
	poke "$T/bad" $((3 * 1024)) 000		# part of the zone map
	run fsck_minix -f "$T/bad"
	check_true "fsck.minix notices a damaged zone map" \
	    test "$status" -ne 0
}

# fsck_triple: a file that reaches the triple indirect zone (see
# t_big.sh).
fsck_triple() {
	for fs in "version=2 namelen=14" "version=3"; do
		cat >"$T/spec" <<EOF
fs $fs order=le blocks=2048 inodes=16
file /tind 0644 0 0 0 67400000 7 0:67380000
EOF
		mkimage "$T/spec" "$T/img"
		run fsck_minix -f "$T/img"
		check_status "fsck.minix accepts a triple indirect file ($fs)" 0

		# Inode 2 is the file.
		cp "$T/img" "$T/bad"
		set_inode "$T/bad" 2 zone9 0
		run fsck_minix -f "$T/bad"
		check_true "fsck.minix notices a lost triple indirect ($fs)" \
		    test "$status" -ne 0
	done
}

# fsck_newfs: fsck.minix accepts what newfs_minixfs makes, in every format
# it reads.
fsck_newfs() {
	for opts in "-V 1 -l 14" "-V 1 -l 30" "-V 2 -l 14" "-V 2 -l 30" \
	    "-V 3 -b 1024"; do
		rm -f "$T/img"
		# The options are split on purpose.
		# shellcheck disable=SC2086
		"$NEWFS_MINIXFS" $opts -s 2048 "$T/img"
		run fsck_minix -f "$T/img"
		check_status "fsck.minix accepts newfs_minixfs $opts" 0
	done
}

# fsck_agrees: both checkers find each kind of damage, and neither
# complains about the undamaged image.  The spec is that of t_fsck.sh.
fsck_agrees() {
	for fs in "version=1 namelen=14" "version=2 namelen=30" "version=3"; do
		cat >"$T/spec" <<EOF
fs $fs order=le blocks=1024 inodes=64
dir  /d 0755 0 0 0
dir  /d/e 0755 0 0 0
file /f 0644 0 0 0 3000 1
file /big 0644 0 0 0 40000 2
file /g 0644 0 0 0 10 3
EOF
		mkimage "$T/spec" "$T/good"
		run "$FSCK_MINIXFS" "$T/good"
		check_status "$fs: fsck_minixfs passes the undamaged image" 0
		first=$(info_field "$T/good" "first data zone")
		zone=$(get_inode "$T/good" 4 zone0)
		zbit=$((zone - first + 1))
		for damage in "imap 4 0" "imap 20 1" "zmap $zbit 0" \
		    "nlinks 3" "zone0 60000" "no entries in /d/e"; do
			cp "$T/good" "$T/img"
			# The words of each damage are split on purpose.
			# shellcheck disable=SC2086
			set -- $damage
			case $1 in
			imap|zmap)
				set_map_bit "$T/img" "$1" "$2" "$3"
				;;
			no)
				set_inode "$T/img" 3 size 0
				set_inode "$T/img" 3 zone0 0
				;;
			*)
				set_inode "$T/img" 4 "$1" "$2"
				;;
			esac
			run fsck_minix -f "$T/img"
			check_true "$fs: fsck.minix finds \"$damage\"" \
			    test "$status" -ne 0
			run "$FSCK_MINIXFS" "$T/img"
			check_status "$fs: fsck_minixfs finds \"$damage\"" 1
			run "$FSCK_MINIXFS" -y "$T/img"
			run fsck_minix -f "$T/img"
			what="fsck.minix accepts the repair of \"$damage\""
			check_status "$fs: $what" 0
		done

		# /d and its tree lose their name, and go to /lost+found.
		width=16
		if [ "${fs%% *}" = version=3 ]; then
			width=32
		fi
		dsize=$((width / 8 + $(info_field "$T/good" "name length")))
		cp "$T/good" "$T/img"
		poke_number "$T/img" \
		    $(($(get_inode "$T/good" 1 zone0) * 1024 + 2 * dsize)) \
		    "$width" 0
		run "$FSCK_MINIXFS" -y -l "$T/img"
		run fsck_minix -f "$T/img"
		check_status "$fs: fsck.minix accepts /lost+found from -y -l" 0
	done
}

# mkfs_readable: images made by mkfs.minix can be read.  Each item is
# the options, then the version and name length they give.
mkfs_readable() {
	for item in "-1 -n 14:1:14" "-1 -n 30:1:30" "-2 -n 14:2:14" \
	    "-2 -n 30:2:30" "-3:3:60"; do
		opts=${item%%:*}
		ver=${item#*:}
		n=${ver#*:}
		ver=${ver%:*}
		dd if=/dev/zero of="$T/img" bs=1024 count=2048 2>/dev/null
		# The options are split on purpose.
		# shellcheck disable=SC2086
		run "$MKFS_MINIX" $opts "$T/img"
		check_status "mkfs.minix $opts succeeds" 0
		what="an image from mkfs.minix $opts"
		check_info "$what has version $ver" "$T/img" version "$ver"
		check_info "$what has $n-character names" "$T/img" \
		    "name length" "$n"
		run "$MINIXFS" ls -lR "$T/img"
		check_status "$what can be listed" 0
		run "$FSCK_MINIXFS" "$T/img"
		check_status "$what passes fsck_minixfs" 0
	done
}

if have "$FSCK_MINIX"; then
	fsck_accepts
	fsck_triple
	fsck_newfs
	fsck_agrees
else
	skip "fsck.minix accepts the test images" "no $FSCK_MINIX"
fi

# mkfs_full_map: mkfs.minix sizes the zone map by the data zones, so that
# 8196 blocks with 16 inodes leave no bit past the last zone.  When
# tunefs_minixfs -s grows such an image, the new bits past the end are
# what newfs_minixfs gives: clear for names of 14, set for names of 30.
# tunefs -e sets those of the inode map to match first.
mkfs_full_map() {
	for n in 14:0 30:1; do
		end=${n#*:}
		n=${n%:*}
		dd if=/dev/zero of="$T/img" bs=1024 count=8196 2>/dev/null
		run "$MKFS_MINIX" -2 -n "$n" -i 16 "$T/img"
		check_status "mkfs.minix -2 -n $n -i 16 succeeds" 0
		"$TUNEFS_MINIXFS" -e "$end" "$T/img" >/dev/null
		run "$TUNEFS_MINIXFS" -s 9000 "$T/img"
		check_status "names of $n: a full zone map grows" 0
		run "$FSCK_MINIXFS" -e "$end" "$T/img"
		check_status "names of $n: the new bits past the end are $end" 0
	done
}

# be_images: the big-endian file systems that mkfs.minix makes, from the
# hexdump -C in the tests of util-linux: numbers big-endian, maps as
# bytes, which -W 8 takes.
be_images() {
	_be_dir=$UTILLINUX_SRC/tests/expected/minix
	for _be_name in v1c14 v1c30 v2c14 v2c30 v3c60; do
		_be_dump=$_be_dir/fsck-images-$_be_name.BE
		if [ ! -f "$_be_dump" ]; then
			skip "the big-endian $_be_name of mkfs.minix is read" \
			    "no $_be_dump"
			continue
		fi
		# Each line gives 16 bytes at an offset, "*" repeats the
		# line before up to the next offset, and the last offset is
		# the size.
		awk '
		function hex(s,    i, v) {
			v = 0
			for (i = 1; i <= length(s); i++)
				v = v * 16 + index("0123456789abcdef",
				    substr(s, i, 1)) - 1
			return v
		}
		function put(    i) {
			printf "printf \047"
			for (i = 0; i < 16; i++)
				printf "\\%03o", b[i]
			printf "\047\n"
		}
		/^\*$/ { rep = 1; next }
		$1 ~ /^[0-9a-f]+$/ && length($1) == 8 {
			off = hex($1)
			if (rep)
				for (o = last + 16; o < off; o += 16)
					put()
			rep = 0
			if (NF < 17)
				exit
			for (i = 0; i < 16; i++)
				b[i] = hex($(i + 2))
			put()
			last = off
		}' "$_be_dump" >"$T/be.sh"
		sh "$T/be.sh" >"$T/img"
		check_info "the big-endian $_be_name of mkfs.minix is read as \
such" "$T/img" "byte order" big-endian
		run "$FSCK_MINIXFS" -W 8 "$T/img"
		check_status "and passes fsck_minixfs -W 8, maps and all" 0
	done
}

if [ -n "${UTILLINUX_SRC:-}" ]; then
	be_images
else
	skip "the big-endian images of mkfs.minix are read" \
	    "UTILLINUX_SRC is not set"
fi

if have "$MKFS_MINIX"; then
	mkfs_readable
	mkfs_full_map
else
	skip "images from mkfs.minix are readable" "no $MKFS_MINIX"
fi

finish
