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
through the `minixfs` command, and read-only mounts through FUSE with
`mount_minixfs`.

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

Exit status is 0 on success, 1 if anything failed, and 2 for a usage
error.

## Tests

    make check              # the test suite
    make check-sanitize     # the same with AddressSanitizer and UBSan

`SAN_POSTLINK` names a command to run on the sanitizer binary after it
is linked.  On NetBSD/i386, AddressSanitizer does not run with ASLR, so
turn it off for that binary:

    make check-sanitize SAN_POSTLINK="/usr/sbin/paxctl +a"

The base gcc of NetBSD/i386 keeps the stack aligned to 4 bytes only, as
the i386 ABI of NetBSD requires.  The AddressSanitizer stack frames it
lays out can then start at addresses that are not multiples of 8, and
the runtime reports stores to local variables as errors.  Building with
16-byte stack alignment avoids this:

    make check-sanitize SAN_POSTLINK="/usr/sbin/paxctl +a" \
        SANFLAGS="-O1 -g -fno-omit-frame-pointer \
        -mpreferred-stack-boundary=4 -fsanitize=address,undefined \
        -fno-sanitize-recover=all"

The tests are POSIX shell scripts, so that they also run on MINIX 3,
which has no Python 3.  They print TAP.  Variables:

- `TEST_SHELL` - the shell for the scripts (default `sh`)
- `JUNIT_XML=FILE` - also write the results as JUnit XML
- `FUZZ_COUNT`, `FUZZ_SEED` - size and seed of the random damage test

Test images are made by `tests/mkimage`, a separate writer that shares no
code with `src/`, from small text specifications (see the comment at the
top of `tests/mkimage.c`).  The scripts:

- `t_read.sh` - one tree in 24 variants: V1 and V2 with 14- and
  30-character names and V3 with 1024- and 4096-byte blocks, each in both
  byte orders and with one- and two-block zones.  File sizes around the
  block, direct, indirect and double indirect limits; holes; hard and
  symbolic links; devices; `ls`, `cat` and `extract`.
- `t_big.sh` - sparse files that reach the triple indirect zone, and a
  tree deeper than `PATH_MAX`.
- `t_errors.sh` - files that are not file systems, super blocks that do
  not add up, zone numbers outside the data area, impossible sizes, bad
  inode numbers, directory loops, names that would escape `DEST`, and
  truncated images, for every version.
- `t_fuzz.sh` - images with random bytes damaged: every command must end
  with status 0 or 1, without crashing and without writing outside
  `DEST`.
- `t_fsck.sh` - where util-linux is installed: `fsck.minix` accepts the
  test images, and images from `mkfs.minix` are readable.  Skipped
  otherwise.
- `t_fuse.sh` - where `mount_minixfs` is built and mounting is allowed:
  the tree read through the kernel in four formats, with its names,
  contents, modes, owners, inode numbers and device numbers.  Skipped
  otherwise.  `MINIXFS_FUSE` names the program; `FUSE_SUDO` is a
  command to mount and read with where users cannot mount, such as
  `sudo` on NetBSD.

## Layout

    src/mfs.h, src/mfs.c    library: super block, inodes, zone mapping,
                            file data, directories, path lookup
    src/minixfs.c           the minixfs command
    src/mount_minixfs.c     the FUSE file system
    src/compat.h            the differences between systems
    tests/lib.sh            helpers for the test scripts
    tests/run.sh            runs the scripts; tap2junit.awk makes JUnit
    tests/mkimage.c         the independent image writer
    tests/tree.spec         the tree that t_read.sh and t_fuse.sh read
    tests/tree.names        the names in that tree
    tests/t_*.sh            the tests

## References

The on-disk format follows the definitions in the MINIX sources
(`fs/super.h`, `fs/inode.h`, `fs/type.h` and `fs/const.h` of MINIX 2.0.4,
and `minix/fs/mfs` of MINIX 3).  No code is taken from MINIX, Linux or
other MINIX file system implementations.

## Contributing

See `STYLE.md` for the coding rules.

## License

BSD 2-Clause.  See `LICENSE`.
