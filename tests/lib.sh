# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# lib.sh - helpers shared by the test scripts.
#
# A test script sources this file, keeps its files under $T and ends with
# finish.  Every check goes through one of the check_* helpers, which
# prints one TAP line ("ok N - NAME" or "not ok N - NAME") and, for a
# failure, lines that say what was expected and what came out.
#
# POSIX sh only: the suite has to run on MINIX 3, whose /bin/sh is the
# NetBSD sh.  Variables used inside one function start with "_" and the
# function name, since there is no "local".

: "${MINIXFS:=./minixfs}"
: "${MKIMAGE:=./tests/mkimage}"
: "${NEWFS_MINIXFS:=./newfs_minixfs}"
: "${FSCK_MINIXFS:=./fsck_minixfs}"
: "${TUNEFS_MINIXFS:=./tunefs_minixfs}"
: "${DUMP_MINIXFS:=./dump_minixfs}"
: "${RESTORE_MINIXFS:=./restore_minixfs}"
: "${MKDUMP:=./tests/mkdump}"
: "${MFSOP:=./tests/mfsop}"

# The sanitizers are told to exit with this status, so that a sanitizer
# report can be told apart from an ordinary failure (status 1).
SANITIZER_STATUS=86
asan_base="exitcode=$SANITIZER_STATUS"
ubsan_base="halt_on_error=1:exitcode=$SANITIZER_STATUS"
ASAN_OPTIONS="$asan_base${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
UBSAN_OPTIONS="$ubsan_base${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
export ASAN_OPTIONS UBSAN_OPTIONS

T=${TMPDIR:-/tmp}/minixfs-test.$$
rm -rf "$T"
mkdir "$T" || exit 1
# Extracted trees can hold directories without permissions; give them
# back so that the whole scratch directory can be removed.
trap 'chmod -R u+rwx "$T" 2>/dev/null; rm -rf "$T"' 0
trap 'exit 1' 1 2 15

ntest=0
nfail=0

# TAP output.  Test scripts use the check_* helpers instead.

pass() {
	ntest=$((ntest + 1))
	echo "ok $ntest - $1"
}

# fail NAME [DETAIL...] - one line of detail per argument.
fail() {
	ntest=$((ntest + 1))
	nfail=$((nfail + 1))
	echo "not ok $ntest - $1"
	shift
	# Every line of detail, even inside one argument, is a TAP comment.
	for _fail_line in "$@"; do
		printf '%s\n' "$_fail_line" | sed 's/^/#   /'
	done
}

skip() {
	ntest=$((ntest + 1))
	echo "ok $ntest - $1 # SKIP $2"
}

finish() {
	echo "1..$ntest"
	if [ "$nfail" -eq 0 ]; then
		exit 0
	fi
	exit 1
}

# Running commands.

# run CMD ARGS... - run a command, keeping its standard output in $T/out,
# its standard error in $T/err and its exit status in $status.
run() {
	"$@" >"$T/out" 2>"$T/err"
	status=$?
}

# Did the last command crash or trip a sanitizer?
crashed() {
	[ "$status" -ge 128 ] || [ "$status" -eq "$SANITIZER_STATUS" ]
}

# The first lines of the standard error of the last command.
last_err() {
	head -5 "$T/err"
}

# Checks on the last command.

# check_status NAME EXPECTED - it exited with EXPECTED.
check_status() {
	if [ "$status" -eq "$2" ]; then
		pass "$1"
	else
		fail "$1" "exit status $status, expected $2" \
		    "stderr: $(last_err)"
	fi
}

# check_out NAME FILE - it succeeded and printed exactly FILE.
check_out() {
	if [ "$status" -ne 0 ]; then
		fail "$1" "exit status $status" "stderr: $(last_err)"
	elif cmp -s "$T/out" "$2"; then
		pass "$1"
	else
		fail "$1" "output differs from $2:" \
		    "$(diff "$2" "$T/out" | head -20)"
	fi
}

# check_out_has NAME PATTERN - it succeeded and printed a line matching
# the basic regular expression PATTERN.
check_out_has() {
	if [ "$status" -ne 0 ]; then
		fail "$1" "exit status $status" "stderr: $(last_err)"
	elif grep -q -e "$2" "$T/out"; then
		pass "$1"
	else
		fail "$1" "no line matches \"$2\":" "$(head -20 "$T/out")"
	fi
}

# check_err NAME PATTERN - it failed with status 1 and its standard error
# matches PATTERN.
check_err() {
	if [ "$status" -ne 1 ]; then
		fail "$1" "exit status $status, expected 1" \
		    "stderr: $(last_err)"
	elif grep -q -e "$2" "$T/err"; then
		pass "$1"
	else
		fail "$1" "stderr does not match \"$2\":" "$(last_err)"
	fi
}

# check_note NAME PATTERN - it succeeded and its standard error matches
# PATTERN.
check_note() {
	if [ "$status" -ne 0 ]; then
		fail "$1" "exit status $status" "stderr: $(last_err)"
	elif grep -q -e "$2" "$T/err"; then
		pass "$1"
	else
		fail "$1" "stderr does not match \"$2\":" "$(last_err)"
	fi
}

# check_found NAME PATTERN - it exited with 1, the status for problems
# found, and printed a line matching PATTERN.
check_found() {
	if [ "$status" -ne 1 ]; then
		fail "$1" "exit status $status, expected 1" \
		    "stdout: $(head -5 "$T/out")"
	elif grep -q -e "$2" "$T/out"; then
		pass "$1"
	else
		fail "$1" "no line matches \"$2\":" "$(head -10 "$T/out")"
	fi
}

# check_info NAME IMAGE FIELD VALUE - "minixfs info" shows VALUE.
check_info() {
	run "$MINIXFS" info "$2"
	check_out_has "$1" "^$3: $4\$"
}

# Checks on files.

# check_same_file NAME EXPECTED ACTUAL
check_same_file() {
	if cmp -s "$2" "$3"; then
		pass "$1"
	else
		fail "$1" "$3 differs from $2"
	fi
}

# check_same_tree NAME EXPECTED ACTUAL - same names, types and contents.
check_same_tree() {
	if diff -r "$2" "$3" >"$T/diff" 2>&1; then
		pass "$1"
	else
		fail "$1" "$(head -20 "$T/diff")"
	fi
}

# check_mode NAME PATH MODE - "ls -ld" shows MODE (such as drwxr-xr-x).
check_mode() {
	# ls is the portable way to the mode; the names are the tests' own.
	# shellcheck disable=SC2012
	_check_mode_got=$(ls -ld "$2" | cut -c1-10)
	if [ "$_check_mode_got" = "$3" ]; then
		pass "$1"
	else
		fail "$1" "mode $_check_mode_got, expected $3"
	fi
}

# check_older NAME PATH REF - PATH was modified before REF.
check_older() {
	if [ -n "$(find "$2" -prune ! -newer "$3")" ]; then
		pass "$1"
	else
		fail "$1" "$2 is newer than $3"
	fi
}

# check_link NAME PATH TARGET - PATH is a symbolic link to TARGET.
check_link() {
	# ls is the portable way to the target; the names are the tests' own.
	# shellcheck disable=SC2012
	_check_link_got=$(ls -l "$2")
	case $_check_link_got in
	*" -> $3")
		pass "$1"
		;;
	*)
		fail "$1" "$_check_link_got"
		;;
	esac
}

# check_contents NAME DIR NAMES - DIR holds exactly NAMES (paths relative
# to DIR, sorted, separated by blanks).
check_contents() {
	_check_contents_got=$(cd "$2" && find . ! -name . | sed 's|^\./||' |
	    sort | tr '\n' ' ')
	if [ "$_check_contents_got" = "$3 " ]; then
		pass "$1"
	else
		fail "$1" "found: $_check_contents_got" "expected: $3"
	fi
}

# check_true NAME CMD ARGS... - the command succeeds.
check_true() {
	_check_true_name=$1
	shift
	if "$@"; then
		pass "$_check_true_name"
	else
		fail "$_check_true_name" "failed: $*"
	fi
}

# check_none NAME COUNT - COUNT problems were found, and there should be
# none.  The problems themselves were reported as comments.
check_none() {
	if [ "$2" -eq 0 ]; then
		pass "$1"
	else
		fail "$1" "$2 problems; see the lines above"
	fi
}

# tree_listing DIR - each name below DIR with its mode; for what is not a
# directory, its link count; for a regular file, its checksum; for a
# symbolic link, its target.  Pipes and dangling links are compared this
# way, which diff -r cannot.
tree_listing() {
	(cd "$1" && find . ! -name . | sort | while read -r _tree_listing_f; do
		_tree_listing_l=$(ls -ld "$_tree_listing_f")
		# The fields are split on purpose.
		# shellcheck disable=SC2086
		set -- $_tree_listing_l
		printf '%s %s' "$_tree_listing_f" "$1"
		if [ -d "$_tree_listing_f" ]; then
			echo
		elif [ -h "$_tree_listing_f" ]; then
			echo " $2 -> ${_tree_listing_l##* -> }"
		elif [ -f "$_tree_listing_f" ]; then
			echo " $2 $(cksum <"$_tree_listing_f")"
		else
			echo " $2"
		fi
	done)
}

# Images.

# mkimage SPEC IMAGE [EXPECTDIR] - build an image from a spec file.
mkimage() {
	_mkimage_ok=yes
	if [ -n "$3" ]; then
		"$MKIMAGE" -e "$3" "$1" "$2" || _mkimage_ok=no
	else
		"$MKIMAGE" "$1" "$2" || _mkimage_ok=no
	fi
	if [ "$_mkimage_ok" = no ]; then
		echo "Bail out! mkimage failed on $1"
		exit 1
	fi
}

# inode_of PATH - the inode number of a file of the host.
inode_of() {
	# ls -i is the portable way to it; the names are the tests' own.
	# shellcheck disable=SC2012
	ls -i "$1" | awk '{ print $1 }'
}

# link_target PATH - what a symbolic link of the host points to.
link_target() {
	# ls -l is the portable way to it; the names are the tests' own.
	# shellcheck disable=SC2012
	ls -l "$1" | sed 's/.* -> //'
}

# info_field IMAGE FIELD - one value from "minixfs info".
info_field() {
	"$MINIXFS" info "$1" | sed -n "s/^$2: //p"
}

# poke IMAGE OFFSET OCTAL... - overwrite bytes of an image, given as
# octal numbers (printf takes only octal escapes portably).
poke() {
	_poke_image=$1
	_poke_off=$2
	shift 2
	_poke_bytes=
	for _poke_b in "$@"; do
		_poke_bytes="$_poke_bytes\\$_poke_b"
	done
	# The escapes are the format.
	# shellcheck disable=SC2059
	printf "$_poke_bytes" |
	    dd of="$_poke_image" bs=1 seek="$_poke_off" conv=notrunc 2>/dev/null
}

# poke_number IMAGE OFFSET BITS VALUE - store a 16- or 32-bit number in
# the byte order of the image.
poke_number() {
	_poke_number_n=$(($3 / 8))
	_poke_number_be=0
	if [ "$(info_field "$1" "byte order")" = big-endian ]; then
		_poke_number_be=1
	fi
	_poke_number_bytes=
	_poke_number_i=0
	while [ "$_poke_number_i" -lt "$_poke_number_n" ]; do
		if [ "$_poke_number_be" -eq 1 ]; then
			_poke_number_shift=$(((_poke_number_n - 1 - \
			    _poke_number_i) * 8))
		else
			_poke_number_shift=$((_poke_number_i * 8))
		fi
		_poke_number_oct=$(printf '%03o' \
		    $(($4 >> _poke_number_shift & 255)))
		_poke_number_bytes="$_poke_number_bytes $_poke_number_oct"
		_poke_number_i=$((_poke_number_i + 1))
	done
	# The list of bytes is split on purpose.
	# shellcheck disable=SC2086
	poke "$1" "$2" $_poke_number_bytes
}

# The layout of each version: "OFFSET BITS" of a field of the super block
# (from its start) or of an inode (from the start of the inode).  See
# src/mfs.c for the full layout.
#
# layout VERSION FIELD
layout() {
	case $1:$2 in
	[12]:ninodes)	echo "0 16" ;;
	3:ninodes)	echo "0 32" ;;
	1:zones)	echo "2 16" ;;
	[23]:zones)	echo "20 32" ;;
	[12]:firstdata)	echo "8 16" ;;
	3:firstdata)	echo "10 16" ;;
	[12]:logzone)	echo "10 16" ;;
	3:logzone)	echo "12 16" ;;
	3:blocksize)	echo "28 16" ;;
	[12]:maxsize)	echo "12 32" ;;
	3:maxsize)	echo "16 32" ;;
	[12]:state)	echo "18 16" ;;
	3:flags)	echo "14 16" ;;
	[123]:mode)	echo "0 16" ;;
	1:nlinks)	echo "13 8" ;;
	[23]:nlinks)	echo "2 16" ;;
	1:size)		echo "4 32" ;;
	[23]:size)	echo "8 32" ;;
	1:zone0)	echo "14 16" ;;
	[23]:zone0)	echo "24 32" ;;
	1:zone1)	echo "16 16" ;;
	[23]:zone1)	echo "28 32" ;;
	1:zone7)	echo "28 16" ;;
	[23]:zone7)	echo "52 32" ;;
	[23]:zone9)	echo "60 32" ;;
	1:mtime)	echo "8 32" ;;
	[23]:atime)	echo "12 32" ;;
	[23]:mtime)	echo "16 32" ;;
	[23]:ctime)	echo "20 32" ;;
	*)
		echo "Bail out! no field $2 in V$1" >&2
		exit 1
		;;
	esac
}

# set_super IMAGE FIELD VALUE - change a field of the super block.
set_super() {
	# layout prints two words.
	# shellcheck disable=SC2046
	set -- "$1" "$3" $(layout "$(info_field "$1" version)" "$2")
	poke_number "$1" $((1024 + $3)) "$4" "$2"
}

# inode_offset IMAGE INO - byte offset of an inode in the image.
inode_offset() {
	_inode_offset_imap=$(info_field "$1" "inode map blocks")
	_inode_offset_zmap=$(info_field "$1" "zone map blocks")
	_inode_offset_bs=$(info_field "$1" "block size")
	_inode_offset_size=64
	if [ "$(info_field "$1" version)" -eq 1 ]; then
		_inode_offset_size=32
	fi
	echo $(((2 + _inode_offset_imap + _inode_offset_zmap) * \
	    _inode_offset_bs + ($2 - 1) * _inode_offset_size))
}

# set_inode IMAGE INO FIELD VALUE - change a field of an inode.
set_inode() {
	_set_inode_base=$(inode_offset "$1" "$2")
	# layout prints two words.
	# shellcheck disable=SC2046
	set -- "$1" "$4" $(layout "$(info_field "$1" version)" "$3")
	poke_number "$1" $((_set_inode_base + $3)) "$4" "$2"
}

# get_number IMAGE OFFSET BITS - read a number stored in the byte order
# of the image.
get_number() {
	od -An -tu1 -j "$2" -N $(($3 / 8)) "$1" |
	    awk -v be="$(info_field "$1" "byte order")" '{
		m = 1
		for (i = 1; i <= NF; i++) {
			if (be == "big-endian") {
				v = v * 256 + $i
			} else {
				v = v + $i * m
				m = m * 256
			}
		}
	} END { printf "%.0f\n", v }'
}

# get_inode IMAGE INO FIELD - read a field of an inode.
get_inode() {
	_get_inode_base=$(inode_offset "$1" "$2")
	# layout prints two words.
	# shellcheck disable=SC2046
	set -- "$1" $(layout "$(info_field "$1" version)" "$3")
	get_number "$1" $((_get_inode_base + $2)) "$3"
}

# set_map_bit IMAGE imap|zmap BIT 0|1 - clear or set a bit of a bit map.
# The maps are arrays of words, 16 bits wide in V1 and V2 and 32 bits in
# V3, in the byte order of the image.
set_map_bit() {
	_set_map_bit_bs=$(info_field "$1" "block size")
	_set_map_bit_start=$((2 * _set_map_bit_bs))
	if [ "$2" = zmap ]; then
		_set_map_bit_start=$((_set_map_bit_start + \
		    $(info_field "$1" "inode map blocks") * _set_map_bit_bs))
	fi
	_set_map_bit_byte=$(($3 / 8))
	if [ "$(info_field "$1" "byte order")" = big-endian ]; then
		if [ "$(info_field "$1" version)" -eq 3 ]; then
			_set_map_bit_byte=$((_set_map_bit_byte ^ 3))
		else
			_set_map_bit_byte=$((_set_map_bit_byte ^ 1))
		fi
	fi
	_set_map_bit_off=$((_set_map_bit_start + _set_map_bit_byte))
	_set_map_bit_old=$(od -An -tu1 -j "$_set_map_bit_off" -N 1 "$1")
	_set_map_bit_mask=$((1 << ($3 % 8)))
	if [ "$4" -eq 1 ]; then
		_set_map_bit_new=$((_set_map_bit_old | _set_map_bit_mask))
	else
		_set_map_bit_new=$((_set_map_bit_old & ~_set_map_bit_mask))
	fi
	poke "$1" "$_set_map_bit_off" "$(printf '%03o' "$_set_map_bit_new")"
}
