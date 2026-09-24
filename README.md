# minixfs

Tools for MINIX file system images, written from scratch in portable C.

The aim is a FUSE file system and a set of management tools for the MINIX
file systems, on Linux, FreeBSD, NetBSD and MINIX 3 itself:

1. MINIX V1, V2 and V3 file systems
2. little- and big-endian images, recognised automatically
3. `mkfs`, `tunefs`-style management commands
4. a test suite thorough enough to trust the results

## Status

Read-only access to V1 file systems in either byte order, through the
`minixfs` command.  V2 and V3 file systems are recognised but not read
yet.

| Magic  | Version | Names | Origin |
|--------|---------|-------|--------|
| 0x137f | V1      | 14    | MINIX |
| 0x138f | V1      | 30    | Linux extension |
| 0x2468 | V2      | 14    | MINIX (recognised only) |
| 0x2478 | V2      | 30    | Linux extension (recognised only) |
| 0x4d5a | V3      | 60    | MINIX 3 (recognised only) |

The byte order is taken from the magic number.  Images made on a PC are
little-endian; those made on 68000 machines (Atari ST, Amiga, Macintosh)
are big-endian, and the Linux kernel on a PC cannot read them.

## Building

    make

The Makefile works with both GNU make and BSD make.  The code is C99 with
POSIX.1-2008 interfaces and needs no libraries.

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

Exit status is 0 on success, 1 if anything failed, and 2 for a usage
error.

## Tests

    make check              # the test suite
    make check-sanitize     # the same with AddressSanitizer and UBSan

The tests are POSIX shell scripts, so that they also run on MINIX 3,
which has no Python 3.  They print TAP.  Variables:

- `TEST_SHELL` - the shell for the scripts (default `sh`)
- `JUNIT_XML=FILE` - also write the results as JUnit XML
- `FUZZ_COUNT`, `FUZZ_SEED` - size and seed of the random damage test

Test images are made by `tests/mkimage`, a separate writer that shares no
code with `src/`, from small text specifications (see the comment at the
top of `tests/mkimage.c`).  The scripts:

- `t_read.sh` - one tree in 8 variants: 14- and 30-character names,
  both byte orders, and one- and two-block zones.  File sizes around the
  block, direct, indirect and double indirect limits; holes; hard and
  symbolic links; devices; `ls`, `cat` and `extract`.
- `t_big.sh` - a tree deeper than `PATH_MAX`.
- `t_errors.sh` - files that are not file systems, V2 and V3 magic
  numbers, super blocks that do not add up, zone numbers outside the data
  area, impossible sizes, bad inode numbers, directory loops, names that
  would escape `DEST`, and truncated images.
- `t_fuzz.sh` - images with random bytes damaged: every command must end
  with status 0 or 1, without crashing and without writing outside
  `DEST`.
- `t_fsck.sh` - where util-linux is installed: `fsck.minix` accepts the
  test images, and images from `mkfs.minix` are readable.  Skipped
  otherwise.

## Layout

    src/mfs.h, src/mfs.c    library: super block, inodes, zone mapping,
                            file data, directories, path lookup
    src/minixfs.c           the minixfs command
    tests/lib.sh            helpers for the test scripts
    tests/run.sh            runs the scripts; tap2junit.awk makes JUnit
    tests/mkimage.c         the independent image writer
    tests/tree.spec         the tree that t_read.sh reads
    tests/t_*.sh            the tests

## References

The on-disk format follows the definitions in the MINIX sources
(`fs/super.h`, `fs/inode.h`, `fs/type.h` and `fs/const.h` of MINIX
2.0.4).  No code is taken from MINIX, Linux or
other MINIX file system implementations.

## Contributing

See `STYLE.md` for the coding rules.

## License

BSD 2-Clause.  See `LICENSE`.
