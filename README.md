# minixfs

Tools for MINIX file system images, written from scratch in portable C.

The aim is a FUSE file system and a set of management tools for the MINIX
file systems, on Linux, FreeBSD, NetBSD and MINIX 3 itself:

1. MINIX V1, V2 and V3 file systems
2. little- and big-endian images, recognised automatically
3. `mkfs`, `tunefs`-style management commands
4. a test suite thorough enough to trust the results

## Status

Read-only access to V1, V2 and V3 file systems in either byte order,
through the `minixfs` command, read-only mounts through FUSE with
`mount_minixfs`, empty file systems of any of them with
`newfs_minixfs`, and consistency checks and repairs with `fsck_minixfs`.

| Magic  | Version | Names | Origin |
|--------|---------|-------|--------|
| 0x137f | V1      | 14    | MINIX |
| 0x138f | V1      | 30    | Linux extension |
| 0x2468 | V2      | 14    | MINIX |
| 0x2478 | V2      | 30    | Linux extension |
| 0x4d5a | V3      | 60    | MINIX 3; block size from the super block |

V2 and V3 inodes have a triple indirect zone, which Linux uses and MINIX
does not; it is read as well.

The byte order is taken from the magic number.  Images made on a PC are
little-endian; those made on 68000 machines (Atari ST, Amiga, Macintosh)
are big-endian, and the Linux kernel on a PC cannot read them.

## Building

    make

The Makefile works with both GNU make and BSD make.  The code is C99 with
POSIX.1-2008 interfaces and needs no libraries.

`mount_minixfs` needs a FUSE library and is built on request:

    make fuse                                           # libfuse 3
    make fuse FUSE_LIBS="-lrefuse -lpuffs"              # NetBSD
    make fuse FUSE_LIBS="-lrefuse -lpuffs" FUSE_VERSION=26   # FUSE 2

`FUSE_CFLAGS` and `FUSE_LIBS` default to what `pkg-config fuse3` says.
The source uses only the high-level FUSE API, in its FUSE 3 form
(`FUSE_VERSION=31`, the default) or its FUSE 2 form (`FUSE_VERSION=26`,
for older librefuse).

## Usage

    minixfs info IMAGE
    minixfs ls [-lR] IMAGE [PATH]
    minixfs cat IMAGE PATH
    minixfs extract [-v] IMAGE DEST [PATH]

`info` prints the super block.  `ls -l` shows mode, links, owner, group,
size (or major and minor numbers for devices) and modification time in
UTC.  `extract` copies a directory tree out of the image, keeping
permission bits (without set-uid, set-gid and sticky bits) and times.
Devices, pipes and sockets are counted but not created.

`extract` never writes outside `DEST`: directory entries whose names are
empty, `.`, `..` or contain `/` are refused, and so are directory loops.

    mount_minixfs [FUSE options] IMAGE MOUNTPOINT

mounts the image read-only.  Inode numbers, modes, owners, times and
device numbers are those of the image.  Unmount it with `fusermount3 -u
MOUNTPOINT` on Linux or `umount MOUNTPOINT` on the BSDs.

    newfs_minixfs -V version [-N] [-B le|be] [-b block-size]
        [-i inodes] [-l name-length] [-s blocks] [-t time]
        [-z log-zone-size] image

makes an empty file system.  The version has to be given.  The defaults
are little-endian, 14-character names in V1 and V2, 4096-byte blocks in
V3 (the smallest that MINIX 3 mounts), one inode for about every three
blocks, and one-block zones.  The size comes from `-s`, which also
creates the image file or cuts it to size, or from the size of an
existing file.  `-t` sets the time of the root directory, for images
that must come out the same each time; `-N` prints the layout and writes
nothing.  The first 1024 bytes, where a boot block may be, are left
alone.

    fsck_minixfs [-y] image

checks a file system: the super block (maximum file size, an image that
holds the whole file system, errors that Linux recorded), directory
entries (inode numbers, names, "." and ".."), inodes (types, sizes, zone
numbers, zones used twice, zones of device files, the text of symbolic
links), link counts, inodes that no directory names, and both bit maps.
It prints one line per problem and a summary.  Without `-y` the image is
only read.  With `-y` each problem is repaired where it can be: bad
entries are removed, "." and ".." are pointed where they belong or put
back, bad zone numbers and the second use of a zone are cleared, sizes
are cut, link counts are set, inodes that no directory names are freed,
and the bit maps are made to match; the image is then checked again and
marked clean, or as having errors if problems remain.  It exits with 0
if the file system is consistent (after the repairs, with `-y`), 1 if
problems remain and 3 if the image cannot be checked at all.

A file system that is not marked clean is noted but is not a problem.
V1 and V2 keep the mark where Linux does; MINIX leaves that word zero.
V3 keeps it in the flags of MINIX 3, which mounts a file system that is
not clean read-only; `newfs_minixfs` marks new file systems clean.

The other commands exit with 0 on success, 1 if anything failed; all
exit with 2 for a usage error.

## Tests

    make check

See `tests/README.md` for what the tests cover and how to run them with
sanitizers, other shells and JUnit output.

## Layout

    src/mfs.h, src/mfs.c    library: super block, inodes, zone mapping,
                            file data, directories, path lookup
    src/minixfs.c           the minixfs command
    src/mount_minixfs.c     the FUSE file system
    src/newfs_minixfs.c     the newfs_minixfs command
    src/fsck_minixfs.c      the fsck_minixfs command
    src/mfs_format.c        library: laying out and writing a new
                            file system
    src/layout.h            the on-disk layout
    src/compat.h            the differences between systems
    tests/                  the test suite; see tests/README.md

## References

The on-disk format follows the definitions in the MINIX sources
(`fs/super.h`, `fs/inode.h`, `fs/type.h` and `fs/const.h` of MINIX 2.0.4,
and `minix/fs/mfs` of MINIX 3).  No code is taken from MINIX, Linux or
other MINIX file system implementations.

## Contributing

See `STYLE.md` for the coding rules.

## License

BSD 2-Clause.  See `LICENSE`.
