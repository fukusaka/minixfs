#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# An image on a device: its size comes from the system, as stat(2) gives
# 0 for a block device on Linux, NetBSD and MINIX 3; fsck_minixfs and
# info take it, and tunefs_minixfs -s grows and shrinks the file system
# inside the device, which keeps its size.  The image is put on a loop
# device on Linux and a vnd device on NetBSD and MINIX 3, which takes
# root or DEV_SUDO; the checks are skipped otherwise.
#
#   DEV_SUDO  a command to attach and detach the device and run the
#             commands on it with, such as "sudo" (default: none)
#   DEV_VND   on NetBSD and MINIX 3, the vnd device to use (default: vnd0)

# shellcheck source=tests/lib.sh
. ./tests/lib.sh

: "${DEV_SUDO:=}"
: "${DEV_VND:=vnd0}"

# as_root CMD ARGS... - run a command with the right to the device.
as_root() {
	# DEV_SUDO may be empty or carry options, so it is split on purpose.
	# shellcheck disable=SC2086
	$DEV_SUDO "$@"
}

system=$(uname -s)
case $system in
Linux|NetBSD|Minix)
	;;
*)
	skip "devices" "no way to attach an image on $system"
	finish
	;;
esac
if [ "$(id -u)" -ne 0 ] && [ -z "$DEV_SUDO" ]; then
	skip "devices" "attaching an image takes root; see DEV_SUDO"
	finish
fi

# attach IMAGE - put the image on a device, and set dev to it.
attach() {
	case $system in
	Linux)
		dev=$(as_root losetup -f --show "$1")
		;;
	NetBSD)
		as_root vnconfig "$DEV_VND" "$1" || return 1
		# The block device of the whole disk: d on i386 and amd64.
		case $(/sbin/sysctl -n kern.rawpartition) in
		2)	dev=/dev/${DEV_VND}c ;;
		*)	dev=/dev/${DEV_VND}d ;;
		esac
		;;
	Minix)
		as_root vndconfig "$DEV_VND" "$1" || return 1
		dev=/dev/$DEV_VND
		;;
	esac
	[ -n "$dev" ]
}

detach() {
	case $system in
	Linux)
		as_root losetup -d "$dev"
		;;
	NetBSD)
		as_root vnconfig -u "$DEV_VND"
		;;
	Minix)
		as_root vndconfig -u "$DEV_VND"
		;;
	esac
}

# dev_info NAME FIELD VALUE - a line of "minixfs info" on the device.
dev_info() {
	run as_root "$MINIXFS" info "$dev"
	check_out_has "$1" "^$2: $3\$"
}

"$NEWFS_MINIXFS" -V 3 -b 1024 -s 2880 "$T/img" >/dev/null
if ! attach "$T/img"; then
	fail "the image is put on a device" "$(uname -s): cannot attach"
	finish
fi
dev_info "info gives the size of the device" "image size" 2949120
run as_root "$FSCK_MINIXFS" "$dev"
check_status "fsck finds nothing wrong on the device" 0

run as_root "$TUNEFS_MINIXFS" -s 1440 "$dev"
check_status "tunefs -s shrinks the file system on the device" 0
run as_root "$FSCK_MINIXFS" "$dev"
check_status "fsck passes the shrunk file system" 0
dev_info "the device keeps its size" "image size" 2949120
dev_info "the file system is half the device" "file system size" \
    "1474560 (50% of the image)"
dev_info "info reads the device as far as the file system" \
    "fill sectors" "0 bytes, 0% of the file system, in 0 runs"

run as_root "$TUNEFS_MINIXFS" -s 2880 "$dev"
check_status "tunefs -s grows it back" 0
run as_root "$FSCK_MINIXFS" "$dev"
check_status "fsck passes the grown file system" 0
run as_root "$TUNEFS_MINIXFS" -s 3000 "$dev"
check_err "tunefs -s refuses to grow past the device" \
    "the image holds 2949120 bytes, fewer than the 3072000"
dev_info "the file system is as it was then" zones 2880

detach
check_true "the image file keeps its size" \
    test "$(($(wc -c <"$T/img")))" -eq 2949120
run "$FSCK_MINIXFS" "$T/img"
check_status "the image file holds what was done on the device" 0

finish
