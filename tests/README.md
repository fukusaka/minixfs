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
  symbolic links; devices; `ls`, `cat` and `extract`, which makes the
  pipe, warns of the devices it leaves out, refuses `-d` without root
  and, where fakeroot(1) is installed, makes the devices with `-d`.
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
  and byte order: a bad maximum file size, a short image, the clean mark
  and the errors Linux records, bit maps that disagree with the files
  and a clear bit 0, wrong link counts, zones outside the data area or
  used twice, inodes that no directory names or with no valid type, bad
  directory and file sizes, device files with zones, symbolic links that
  are empty, too long or hold a NUL byte, "." that names another inode,
  missing "." and "..", and entries naming inodes past the last, free
  inodes, names with "/" and directories listed twice; `-y` repairs
  each of them so that a second check finds nothing, and leaves a
  consistent image as it was.  `-y -l` links trees that nothing names,
  also in a loop, into a `/lost+found` that it makes or finds; `-e`
  checks and sets the bits past the end of the maps; `-w` notes spare
  map blocks, a gap before the data zones and a maximum file size
  other than MINIX's, and nothing about a layout as MINIX makes it.  A directory
  holding more directories than the walk first has room for is
  checked too.
- `t_tunefs.sh` - `tunefs_minixfs` prints the settings, `-N` writes
  nothing, and `-c`, `-m` and `-e` each leave a file system that
  `fsck_minixfs` passes and that has what was asked for, in several
  versions and byte orders; `-B` turns the test tree of every format
  and zone size into the image mkimage makes in the other byte order,
  and back; `-l` keeps every name and file of the test tree, with a
  directory that needs an indirect zone, through 14 -> 30 -> 14,
  lists all names too long for 14 characters and changes nothing,
  changes nothing when the directories would not fit, and refuses V3;
  bad values are usage errors.
- `t_utillinux.sh` - where util-linux is installed: `fsck.minix` accepts
  the test images and those of `newfs_minixfs`, images from `mkfs.minix`
  are readable and pass `fsck_minixfs`, and `fsck_minixfs` and
  `fsck.minix` find the same damage, and `fsck.minix` accepts what
  `fsck_minixfs -y` repaired, also into `/lost+found` with `-l`.
  Skipped otherwise.
- `t_tar.sh` - where tar(1) is installed: `minixfs tar` of the test tree
  in several formats is read back by tar(1): contents, modes, owners and
  times, devices with their numbers, pipes, hard and symbolic links, a
  directory as the top, and names too long for a plain ustar header.
- `t_tracks.sh` - `-T`: a file system spread over the tracks of either
  side of a double-sided image reads with `info`, `ls`, `cat`,
  `extract`, `tar` and `fsck_minixfs` as the file system itself, and
  `fsck_minixfs -y -T` repairs it as `-y` repairs the file system and
  leaves the other side alone; bad `-T` values are usage errors.  `info`
  gives the sizes and the runs of 0xe5 of the spread image, and none
  with `-T`.
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
