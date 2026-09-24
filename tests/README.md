# Tests

Run the suite from the top of the source tree:

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
which has no Python 3, and follow the rules in `../STYLE.md`.  They
print TAP.  Variables:

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
  with status 0 or 1 (`fsck_minixfs`: up to 3), without crashing and
  without writing outside `DEST`.
- `t_newfs.sh` - `newfs_minixfs` makes, in every format, the image that
  `tests/mkimage` makes from an empty tree; defaults, sizes, `-N`, the
  boot block, and the options it must refuse.
- `t_fsck.sh` - `fsck_minixfs` passes the test tree in every format and
  an empty file system, and finds each kind of damage in every version
  and byte order: bit maps that disagree with the files, wrong link
  counts, zones outside the data area or used twice, inodes that no
  directory names or with no valid type, bad directory sizes, "." that
  names another inode, and entries naming inodes past the last, free
  inodes, names with "/" and directories listed twice.
- `t_utillinux.sh` - where util-linux is installed: `fsck.minix` accepts
  the test images and those of `newfs_minixfs`, images from `mkfs.minix`
  are readable and pass `fsck_minixfs`, and `fsck_minixfs` and
  `fsck.minix` find the same damage.  Skipped otherwise.
- `t_fuse.sh` - where `mount_minixfs` is built and mounting is allowed:
  the tree read through the kernel in four formats, with its names,
  contents, modes, owners, inode numbers and device numbers.  Skipped
  otherwise.  `MINIXFS_FUSE` names the program; `FUSE_SUDO` is a
  command to mount and read with where users cannot mount, such as
  `sudo` on NetBSD.

## Files

    lib.sh          helpers: running commands, the check_* helpers, the
                    layout of each version, changing fields of images
    run.sh          runs every t_*.sh; tap2junit.awk turns TAP into JUnit
    mkimage.c       the independent image writer
    tree.spec       the tree that t_read.sh and t_fuse.sh read
    tree.names      the names in that tree
