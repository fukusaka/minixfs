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
which has no Python 3, and follow the rules in `../STYLE.md`; `make
lint-sh` (`shellcheck -x -s sh`) runs shellcheck(1) over them where it
is installed.  They print TAP.  Variables:

- `TEST_SHELL` - the shell for the scripts (default `sh`)
- `JUNIT_XML=FILE` - also write the results as JUnit XML
- `FUZZ_COUNT`, `FUZZ_SEED` - size and seed of the random damage test
- `VMD_FSCK=DIR` - `t_vmd.sh` checks what is written into flex
  directories with the fsck of Minix-vmd 1.7.0 as well, which
  `sh tests/vmd-fsck.sh SRC.TGZ DIR` builds into `DIR` from
  `1.7.0/SRC.TGZ` of the Minix-vmd distribution; nothing of Minix-vmd is
  kept in this tree
- `UTILLINUX_NOSYNC=yes` - `t_utillinux.sh` has strace(1) skip the
  sync(2) calls of `fsck.minix`, three a run, which wait for every file
  system of the host; where strace cannot (it is Linux only), the test
  says so and runs as it is

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
- `t_newfs.sh` - `newfs_minixfs` makes, in every format and the flex
  format of Minix-vmd, the image that `tests/mkimage` makes from an
  empty tree, with the maximum file size MINIX works out, that of Linux
  with `-m linux` or a number, and map tail bits of MINIX, or of Linux
  for names of 30 characters, or as `-e` gives; defaults, sizes, `-N`,
  the boot block, and the options it must refuse.  With `-d`, the test
  tree goes into every format and comes back out with its modes, sizes
  and times; hard and symbolic links, pipes, `-o`, owners of `-o` that
  are no numbers or past their range, the size made to fit, `-N`, sizes
  too small, names too long and files past the maximum file size; a tree
  of many empty files, whose inode table the size made to fit has room
  for; and, where fakeroot(1) is installed, devices.  With `-F`, a
  specification in both forms sets modes, owners (by name, from `-P`)
  and times, adds devices, links, pipes, empty files and escaped names,
  skips optional entries, `-x` leaves out what it does not name, and bad
  entries change nothing.
- `t_fsck.sh` - `fsck_minixfs` passes the test tree in every format and
  an empty file system, and finds each kind of damage in every version
  and byte order: a bad maximum file size, which becomes MINIX's, a
  short image, the clean mark and the errors Linux records, bit maps
  that disagree with the files and a clear bit 0, wrong link counts,
  zones outside the data area or used twice, inodes that no directory
  names or with no valid type, bad directory and file sizes, device
  files with zones, symbolic links that are empty, too long or hold a
  NUL byte, "." that names another inode, missing "." and "..", and
  entries naming inodes past the last, free inodes, names with "/" and
  directories listed twice; `-y` repairs each of them so that a second
  check finds nothing, and leaves a consistent image as it was.  `-y -l`
  links trees that nothing names, also in a loop, into a `/lost+found`
  that it makes or finds; `-e` checks and sets the bits past the end of
  the maps; `-w` notes spare map blocks, a gap before the data zones, a
  maximum file size other than MINIX's, a file past the maximum file
  size and set map tail bits, these not for names of 30 characters, and
  nothing about a layout as MINIX makes it.  A directory holding more
  directories than the walk first has room for is checked too.
- `t_tunefs.sh` - `tunefs_minixfs` prints the settings, `-N` writes
  nothing, and `-c`, `-m` and `-e` each leave a file system that
  `fsck_minixfs` passes and that has what was asked for, in several
  versions and byte orders; `-m` below the largest file is refused and
  changes nothing; `-B` turns the test tree of every format
  and zone size into the image mkimage makes in the other byte order,
  and back; `-l` keeps every name and file of the test tree, with a
  directory that needs an indirect zone, through 14 -> 30 -> 14,
  lists all names too long for 14 characters and changes nothing,
  changes nothing when the directories would not fit, and refuses V3;
  `-s` keeps every file of the test tree through a growth the zone map
  has room for, one that moves the inode table and the zones, and a
  shrink back; it shrinks an image whose files lie past the new end
  (mkimage `skip=`) by moving them down, changes nothing for a size too
  small, and refuses to outgrow V1 and to resize one side of a disk;
  with `-k` the image file keeps its size, the file system shrinks
  inside it and grows back as far as it holds, but no further;
  `-B`, `-l` and `-s` refuse a file system not marked clean without
  `-f`, and work after `fsck_minixfs -y`; a change asked for with one
  that is refused, such as `-B be -s 1`, `-B be -m big` or `-l` on V3,
  does not happen either; bad values are usage errors.
- `t_utillinux.sh` - where util-linux is installed: `fsck.minix` accepts
  the test images and those of `newfs_minixfs`, images from `mkfs.minix`
  are readable and pass `fsck_minixfs`, a zone map with no tail bits
  grows with `tunefs -s` into those `newfs_minixfs` gives, and
  `fsck_minixfs` and `fsck.minix` find the same damage, and `fsck.minix`
  accepts what `fsck_minixfs -y` repaired, also into `/lost+found` with
  `-l`.  Skipped otherwise.
- `t_tar.sh` - where tar(1) is installed: `minixfs tar` of the test tree
  in several formats is read back by tar(1): contents, modes, owners and
  times, devices with their numbers, pipes, hard and symbolic links, a
  directory as the top, names too long for a plain ustar header, and a
  path of more than 8 KB, where tar(1) reads pax headers (that of
  MINIX 3 does not).
- `t_tracks.sh` - `-M`: a file system spread over the tracks of either
  side of a double-sided image reads with `info`, `ls`, `cat`,
  `extract`, `tar` and `fsck_minixfs` as the file system itself, and
  `fsck_minixfs -y -M` repairs it as `-y` repairs the file system and
  leaves the other side alone; bad `-M` values are usage errors.  `info`
  gives the sizes and the runs of 0xe5 of the spread image, and none
  with `-M`.
- `t_vmd.sh` - Minix-vmd, as `tests/mkimage` writes it with `vmd`: the
  variant and its zone size and clean flag, names of every length up to
  60 in flex directories, entries that start a new block, `fsck` and
  `fsck -y` on them; entries added, linked, renamed and removed through
  the library (`tests/mfsop`) as Minix-vmd does it, a new directory of
  two slots, an entry that does not fit in a block starting the next and
  freed slots taken again, which the fsck of Minix-vmd passes where
  `VMD_FSCK` is set; `fsck -y` putting `.` and `..` back, moving an
  entry out of the slot of `.`, and making `/lost+found` there with
  `-l`, which the fsck of Minix-vmd passes too; `newfs_minixfs -l flex`
  copying a tree with a directory of 17 blocks of 60-character names
  into an image made just large enough, which the fsck of Minix-vmd
  passes; the clean flag set and cleared alone, `tunefs -B` and `-l`
  refused, and `-s` working.  `t_read.sh` reads the test tree
  as Minix-vmd V1 and V2 as well.
- `t_restore.sh` - `restore_minixfs` restores dumps of the test tree
  that `tests/mkdump` writes in the formats of 4.4BSD, 4.3BSD and file
  systems before 4.2BSD (directories of V7), UFS2, and Linux dump with
  runs in c_addr, in either byte order, as the tree mkimage makes of the
  same spec, which `fsck_minixfs` passes; `-t` lists every name, and the
  extended attributes of UFS2 and Linux are counted as left out.  A dump
  from standard input restores; a full dump into a file system that is
  not empty, a compressed dump, a file that is no dump, names too long,
  a group too large for V1 (unless `-o`), a device number that does not
  fit, a file past the maximum file size of the super block and a file
  system not marked clean are refused, with nothing written; the tree
  goes into the flex directories of a file system that `newfs_minixfs -l
  flex` makes, and their dump into a file system of V2, as it was; a
  dump in a file that is cut short is refused before anything is
  written, and leaves no table; `-N` writes nothing; sockets are left
  out.  While it writes the image is not marked clean: a restore killed
  then leaves it so, and the next is refused; one from standard input
  that is cut short leaves it clean again and in order, as does one that
  runs out of room (in `t_dump.sh`); an error of the image, such as a
  zone number outside the data area, leaves it not clean.
- `t_dump.sh` - `dump_minixfs` of the test tree in V1, V2 with 30
  characters, V3 with blocks of 4096 bytes, zones of two blocks and
  Minix-vmd, in either byte order, restores as the image itself, in the
  byte order of the image; a subtree dumps with the directories above
  it; `-L` and `-T` show in `restore_minixfs -t`; and dumps of levels
  0, 1 and 2, noted with `-u` in a dumpdates file, hold what changed
  and restore one after the other as the second image, with files
  removed, added, changed, renamed and turned from a directory into a
  file, while a dump restored twice or out of order is refused; in the
  dumpdates file a blank in the name of the image is written in octal,
  and names that differ in a trailing blank are kept apart.  A level
  1 that runs out of room fails, leaves the file system in order and the
  table following level 0, and restores once the file system has grown;
  and in a file system of 16 inodes, fifteen files give way to fifteen
  others with inodes of their own.
- `t_ops.sh` - the library changes names and files as mount_minixfs -w
  does, driven by `tests/mfsop`: the same commands run on a directory of
  the host (mkdir, mknod, ln, ln -s, mv, rm, rmdir, writes and truncates
  across the indirect zones, chmod), and the image then extracts as that
  directory, with the same modes, link counts and contents, and passes
  `fsck_minixfs`, in every version and byte order, empty and on the test
  tree, and with zones of two blocks.  Each refusal gives the error of
  the system call; a file system that fills up with zones or inodes
  stays consistent; a write that runs out of zones gives back what it
  put past the end of the file, which then grows over zeros; a write
  that meets a zone number outside the data area, in a directory, a
  file or an indirect zone, fails with EIO and leaves the block it names
  alone, and cutting the file to 0 drops the number; a writer killed
  after a change leaves the maps right with `-a` and behind the inodes
  without; and a writer holds a lock that `tunefs_minixfs` and
  `newfs_minixfs` meet, but not a reader.
- `t_put.sh` - the commands of `minixfs` that write an image without
  mounting it, `put`, `mkdir`, `rm`, `mv`, `ln`, `chmod` and `chown`,
  make the same changes as their namesakes make on a directory of the
  host, and the image then extracts as that directory and passes
  `fsck_minixfs`, in every version and byte order and with zones of two
  blocks; `put` keeps mtimes and `chmod` sets the set-user-ID bit; what
  is made belongs to the directory it is made in, or to `-o`, and a
  group V1 cannot hold is refused; a name in the way is replaced, kept
  with `-n` or asked about with `-i`, a directory needs `-R` and does
  not replace a file; `put -R` and `ln` take a name without the slashes
  after it; names too long, the root, a directory below
  itself, a link to a directory, bad modes and owners are refused; a
  file that runs out of room, also after the first 64 KiB it copies, or
  that is larger than the maximum file size of the super block, leaves
  nothing; a refusal leaves the image
  marked clean, and a write that fails on a zone number outside the
  data area, or whose fsync(2) fails (made to by strace(1) on Linux),
  does not; a file system not marked clean is refused without
  `-f` and keeps its mark with it, flex directories of Minix-vmd are
  written, a writer holding the lock is met, and `-M` writes through
  the tracks.
- `t_fuse.sh` - where `mount_minixfs` is built and mounting is allowed:
  the tree read through the kernel in four formats, with its names,
  contents, modes, owners, inode numbers and device numbers; and with
  `-w`, in three formats, the tree changed through the kernel as a copy
  on the host is changed, the image not marked clean and locked while
  mounted, the maps behind with `-u sync` and written with `always` or
  seconds, and afterwards marked clean, passing `fsck_minixfs` and
  holding what the host does; mounted in the background, without `-f`,
  the image stays locked, and in a set-group-ID directory new files and
  directories take its group, and new directories the bit (not on
  NetBSD, whose librefuse takes it away); a file removed while open
  keeps its inode until it is closed, so that writing to it changes no
  file that comes after it, and gives it back then; through its
  descriptor (`tests/fdops`) it is written, read, stat, cut, given a
  time, a mode and an owner, with names of 14 and 30 characters, with
  `-o hide=memory`, with another name, open and removed as well or not,
  when another file is renamed over it, and when its directory is
  removed or replaced, which libfuse refuses; the name libfuse hides it
  under is not in the directory but where it is kept there, and a file
  renamed to such a name is renamed as to any other if it is not open,
  and kept till the unmount if it is; a file system not marked clean is
  mounted read-only, and flex directories of Minix-vmd written; writing
  or cutting a file to grow past the maximum
  file size of the super block fails with EFBIG; and a change that
  fails on a zone number outside
  the data area, or a sync whose fsync(2) fails (made to by strace(1)
  on Linux), keeps the mark away at the unmount.  On FreeBSD no hard
  link is made through the mount.  On NetBSD the list of every name,
  which looks up "..", after which librefuse has freed the root, is
  left out.  On MINIX 3, where it is a service of mount(8) on a vnd
  device, the reading, run by hand it shows how to mount, and writing
  is left out.  Skipped otherwise.  `MINIXFS_FUSE` names the program;
  `FUSE_SUDO` is a command to mount and read with where users cannot
  mount, such as `sudo` on NetBSD.
- `t_device.sh` - where an image can be put on a device, a loop device
  on Linux, an md device on FreeBSD and a vnd device on NetBSD and
  MINIX 3, as root or through `DEV_SUDO` (such as `sudo`): `info` and
  `fsck_minixfs` take the size of the device from the system, not from
  stat(2); `tunefs_minixfs -s` shrinks the file system on the device
  and grows it back, the device keeping its size, and refuses to grow
  it past the end of the device.
  `DEV_VND` names the vnd device (default `vnd0`).  Skipped otherwise.

## Files

    lib.sh          helpers: running commands, the check_* helpers, the
                    layout of each version, changing fields of images
    run.sh          runs every t_*.sh; tap2junit.awk turns TAP into JUnit
    mkimage.c       the independent image writer
    mkdump.c        the independent writer of dumps of BSD and Linux
    mfsop.c         a driver of the library for t_ops.sh
    fdops.c         work through the descriptor of a removed file, for
                    t_fuse.sh
    tree.spec       the tree that t_read.sh and t_fuse.sh read
    tree.names      the names in that tree
