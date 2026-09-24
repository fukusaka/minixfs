#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# Damage good images at random and make sure that every command still
# ends in an orderly way: exit status 0 or 1, no crash, no sanitizer
# report, and nothing written outside the extraction directory.
#
# FUZZ_COUNT sets the number of damaged images per format and byte order
# (default 100); FUZZ_SEED changes the sequence.

. ./tests/lib.sh

: "${FUZZ_COUNT:=100}"
: "${FUZZ_SEED:=1}"

# damage IMAGE LIMIT N - overwrite between 1 and 16 random bytes in the
# first LIMIT bytes of IMAGE, the N-th time.
damage() {
	awk -v s=$((FUZZ_SEED * 100003 + $3)) -v lim="$2" 'BEGIN {
		srand(s)
		n = 1 + int(rand() * 16)
		for (k = 0; k < n; k++)
			printf "%d %03o\n", int(rand() * lim), int(rand() * 256)
	}' >"$T/pokes"
	while read -r _damage_off _damage_byte; do
		poke "$1" "$_damage_off" "$_damage_byte"
	done <"$T/pokes"
}

# note_problem WHAT - report a problem with the last command as a comment
# and count it.
note_problem() {
	problems=$((problems + 1))
	echo "# $v #$i: $1: status $status"
	last_err | sed 's/^/#   /'
}

# try_commands - run the reading commands on $T/img.  A damaged size can
# make a file gigabytes long, so the contents go nowhere.
try_commands() {
	for cmd in "info" "ls -lR" "cat /usr/x/big" "cat /bin/b" "cat /usr/l"
	do
		# The command and its arguments are split on purpose.
		# shellcheck disable=SC2086
		set -- $cmd
		_try_commands_cmd=$1
		shift
		"$MINIXFS" "$_try_commands_cmd" "$T/img" "$@" \
		    >/dev/null 2>"$T/err"
		status=$?
		if crashed || [ "$status" -gt 1 ]; then
			note_problem "$cmd"
		fi
	done
	# fsck_minixfs exits with 3 for an image it cannot check.
	"$FSCK_MINIXFS" "$T/img" >/dev/null 2>"$T/err"
	status=$?
	if crashed || [ "$status" -gt 3 ]; then
		note_problem fsck_minixfs
	fi
}

# try_extract - extract $T/img with the size of each file capped at
# 10 MB.  With SIGXFSZ ignored, a larger write fails with EFBIG and is
# reported.
try_extract() {
	chmod -R u+rwx "$T/cage" 2>/dev/null
	rm -rf "$T/cage"
	mkdir "$T/cage"
	run sh -c 'trap "" XFSZ; ulimit -f 20480; exec "$@"' sh \
	    "$MINIXFS" extract "$T/img" "$T/cage/out"
	if crashed || [ "$status" -gt 1 ]; then
		note_problem extract
	fi
	# A damaged mode can make a directory unreadable; find says so.
	if [ -n "$(cd "$T/cage" && find . ! -path . ! -path ./out \
	    ! -path './out/*' 2>/dev/null)" ]; then
		note_problem "extract wrote outside its directory"
	fi
}

for fs in "version=1" "version=2 namelen=30" "version=3" \
    "version=3 block=4096"; do
for order in le be; do
	v="$fs/$order"
	cat >"$T/spec" <<EOF
fs $fs order=$order blocks=2048 inodes=48
dir  /bin 0755 0 0 0
file /bin/a 0755 0 0 0 5000 1
file /bin/b 0755 0 0 0 9000 2 1024:4096
dir  /usr 0755 0 0 0
dir  /usr/x 0755 0 0 0
file /usr/x/big 0644 0 0 0 600000 3
file /usr/x/small 0644 0 0 0 30 4
link /usr/l ../bin/a 0 0 0
dev  /usr/tty c 4 1 0600 0 0 0
hard /usr/a2 /bin/a
EOF
	mkimage "$T/spec" "$T/good"

	# The metadata worth damaging: the super block, the maps, the inode
	# table and the first data zones, which hold the directories.
	bs=$(info_field "$T/good" "block size")
	limit=$((($(info_field "$T/good" "first data zone") + 6) * bs))

	problems=0
	i=0
	while [ "$i" -lt "$FUZZ_COUNT" ]; do
		i=$((i + 1))
		cp "$T/good" "$T/img"
		damage "$T/img" "$limit" "$i"
		try_commands
		try_extract
	done
	check_none "$v: $FUZZ_COUNT damaged images are handled in order" \
	    "$problems"
done
done

finish
