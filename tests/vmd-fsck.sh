#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# vmd-fsck.sh SRC.TGZ DIR - build the fsck of Minix-vmd 1.7.0 for flex
# file systems, fsck1f (V1) and fsck2f (V2), into DIR, for t_vmd.sh to
# check what the library writes with (VMD_FSCK=DIR).
#
# SRC.TGZ is 1.7.0/SRC.TGZ of the Minix-vmd distribution.  Nothing of it
# is kept in this tree: the two sources are taken from it and built on
# the host with a header of its own that gives the types and constants
# they use at the widths of the disk, since those of the host differ
# (ino_t, mode_t and uid_t are 16 bits in Minix-vmd).  Two edits: the
# entries come from that header rather than <dirent.h>, and the super
# block that fsck1f.c declares gets fields of the widths of the disk.

set -e

if [ $# -ne 2 ]; then
	echo "usage: vmd-fsck.sh SRC.TGZ DIR" >&2
	exit 2
fi
src=$1
dir=$2
cc=${CC:-cc}

mkdir -p "$dir/inc/minix" "$dir/inc/fs"
tar xzOf "$src" src/sys/cmd/simple/fsck1f.c >"$dir/fsck1f.orig.c"
tar xzOf "$src" src/sys/cmd/simple/fsck2f.c >"$dir/fsck2f.orig.c"
for h in minix/config.h minix/const.h minix/type.h fs/const.h \
    dirent.h; do
	: >"$dir/inc/$h"
done

cat >"$dir/inc/fs/type.h" <<'EOF'
#ifndef VMD_TYPE_H
#define VMD_TYPE_H
#include <stdint.h>
#include <limits.h>
typedef uint16_t bitchunk_t;
typedef int32_t bit_t;
typedef uint32_t block_t;
typedef uint32_t zone_t;
typedef uint16_t zone1_t;
typedef int Ino_t, Mode_t, Uid_t, Gid_t, Dev_t, Zone1_t;
typedef unsigned int Nlink_t;
#define _PROTOTYPE(f, a) f ()
#define PRIVATE static
#define PUBLIC
#define EXTERN extern
#define usizeof(t) ((unsigned) sizeof(t))
#define BLOCK_SIZE 1024
#define MAJOR 8
#define MINOR 0
#define BYTE 0377
#define READING 0
#define WRITING 1
#define I_TYPE 0170000
#define I_SYMBOLIC_LINK 0120000
#define I_REGULAR 0100000
#define I_BLOCK_SPECIAL 0060000
#define I_DIRECTORY 0040000
#define I_CHAR_SPECIAL 0020000
#define I_NAMED_PIPE 0010000
#define I_SET_UID_BIT 0004000
#define I_SET_GID_BIT 0002000
#define R_BIT 0000004
#define W_BIT 0000002
#define X_BIT 0000001
#define I_NOT_ALLOC 0000000
#define MAX_FILE_POS ((uint32_t) 037777777776)
#define NO_BLOCK ((block_t) 0)
#define NO_ENTRY 0
#define NO_ZONE ((zone_t) 0)
#define NO_DEV 0
#define NO_BIT ((bit_t) 0)
#define ROOT_INODE 1
#define SUPER_BLOCK ((block_t) 1)
#define SUPER_MAGIC 0x137F
#define SUPER_V2 0x2468
#define V1_NR_DZONES 7
#define V1_NR_TZONES 9
#define V2_NR_DZONES 7
#define V2_NR_TZONES 10
typedef struct {
	uint16_t d1_mode;
	uint16_t d1_uid;
	uint32_t d1_size;
	uint32_t d1_mtime;
	uint8_t d1_gid;
	uint8_t d1_nlinks;
	uint16_t d1_zone[V1_NR_TZONES];
} d1_inode;
typedef struct {
	uint16_t d2_mode;
	uint16_t d2_nlinks;
	uint16_t d2_uid;
	uint16_t d2_gid;
	uint32_t d2_size;
	uint32_t d2_atime;
	uint32_t d2_mtime;
	uint32_t d2_ctime;
	zone_t d2_zone[V2_NR_TZONES];
} d2_inode;
struct _fl_direct {
	uint16_t d_ino;
	unsigned char d_extent;
	char d_name[5];
};
#define V1_ZONE_NUM_SIZE usizeof(uint16_t)
#define V1_INODE_SIZE usizeof(d1_inode)
#define V1_INDIRECTS (BLOCK_SIZE / V1_ZONE_NUM_SIZE)
#define V1_INODES_PER_BLOCK (BLOCK_SIZE / V1_INODE_SIZE)
#define V1_LINK_MAX CHAR_MAX
#define V2_ZONE_NUM_SIZE usizeof(zone_t)
#define V2_INODE_SIZE usizeof(d2_inode)
#define V2_INDIRECTS (BLOCK_SIZE / V2_ZONE_NUM_SIZE)
#define V2_INODES_PER_BLOCK (BLOCK_SIZE / V2_INODE_SIZE)
#define V2_LINK_MAX SHRT_MAX
#define FL_DIR_ENTRY_SIZE usizeof(struct _fl_direct)
#define FL_NR_DIR_ENTRIES (BLOCK_SIZE / FL_DIR_ENTRY_SIZE)
#define S_FLEX 0x01
#endif
EOF

cat >"$dir/inc/fs/super.h" <<'EOF'
struct super_block {
	uint16_t s_ninodes;
	zone1_t s_nzones;
	int16_t s_imap_blocks;
	int16_t s_zmap_blocks;
	zone1_t s_firstdatazone;
	char s_log_zone_size;
	char s_flags;
	uint32_t s_max_size;
	int16_t s_magic;
	char s_fsck_magic[2];
	zone_t s_zones;
};
EOF

sed -e 's|^#include <dirent.h>|#include <fs/type.h>|' \
    "$dir/fsck2f.orig.c" >"$dir/fsck2f.c"
sed -e 's|^#include <dirent.h>|#include <fs/type.h>|' \
    -e 's|^  ino_t s_ninodes;|  uint16_t s_ninodes;|' \
    -e 's|^  zone_nr s_nzones;|  uint16_t s_nzones;|' \
    -e 's|^  zone_nr s_firstdatazone;|  uint16_t s_firstdatazone;|' \
    -e 's|^  off_t s_maxsize;|  uint32_t s_maxsize;|' \
    "$dir/fsck1f.orig.c" >"$dir/fsck1f.c"
for v in 1 2; do
	"$cc" -std=gnu89 -w -I"$dir/inc" -o "$dir/fsck${v}f" \
	    "$dir/fsck${v}f.c"
done
