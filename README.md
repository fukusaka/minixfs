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
`newfs_minixfs`, consistency checks and repairs with `fsck_minixfs`,
and changes of settings with `tunefs_minixfs`.

| Magic  | Version | Names | Origin |
|--------|---------|-------|--------|
| 0x137f | V1      | 14    | MINIX |
| 0x138f | V1      | 30    | Linux extension |
| 0x2468 | V2      | 14    | MINIX |
| 0x2478 | V2      | 30    | Linux extension |
| 0x4d5a | V3      | 60    | MINIX 3; block size from the super block |

V2 and V3 inodes have a triple indirect zone, which Linux uses and MINIX
does not; it is read as well.

Minix-vmd, the MINIX derivative of Philip Homburg and Kees Bot, writes
V1 and V2 file systems with the magic numbers of MINIX but a super block
of its own: the zone size in one byte and flags in the next (flex
directories, clean), and the bytes 0x7f, 0x13 where Linux keeps its
state, by which it is known.  Its flex directories hold entries of
8-byte slots with names of up to 60 characters.  Both are read, `info`
names the variant, and the clean flag is its own; what writes directory
entries of a fixed size (putting back "." or "..", `/lost+found`,
`tunefs_minixfs -B` and `-l`) refuses it.

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

    minixfs [-T SIZE:HEADS:SIDE] info IMAGE
    minixfs [-T ...] ls [-lR] IMAGE [PATH]
    minixfs [-T ...] cat IMAGE PATH
    minixfs [-T ...] extract [-dv] IMAGE DEST [PATH]
    minixfs [-T ...] tar IMAGE [PATH] > ARCHIVE

`info` prints the super block, the size of the image against that of
the file system, and how much of the image is sectors of nothing but
0xe5 or 0xa5, which formats leave and nothing else writes, with the
commonest length of such runs.  A disk read with more sides than it was
written on shows as runs of one track, half of the image; a copy cut
short or a file system smaller than its disk shows too.  None of this
is an error in itself.  `ls -l` shows mode, links, owner, group,
size (or major and minor numbers for devices) and modification time in
UTC.  `extract` copies a directory tree out of the image, keeping
permission bits (without set-uid, set-gid and sticky bits) and times.
It makes pipes, and devices only with `-d`, which takes root; devices
left out and sockets, which cannot be copied, are reported in a warning
at the end, and with `-v` one by one.  `tar` writes the tree to standard
output as a POSIX ustar archive instead, devices and pipes included,
with hard links as links and names longer than ustar holds in pax
headers; tar(1) lists it without privileges and makes the devices when
run as root.  Sockets cannot be stored.

`extract` never writes outside `DEST`: directory entries whose names are
empty, `.`, `..` or contain `/` are refused, and so are directory loops.

`-T SIZE:HEADS:SIDE`, which `minixfs`, `fsck_minixfs` and
`mount_minixfs` all take, reads an image that holds the file system in
the tracks of one side only: tracks of SIZE bytes, HEADS of them to a
cylinder, of which side SIDE (from 0) holds the file system.  A
single-sided 360K disk read as a double-sided 720K one, such as Atari
MINIX 1.5 disks, is `-T 4608:2:0`: every other track is empty.  The
super block is then still in place, so `info` looks right, while the
rest reads as damage.  `fsck_minixfs -y -T` repairs such an image in
place and leaves the other side alone.

    mount_minixfs [-T SIZE:HEADS:SIDE] [FUSE options] IMAGE MOUNTPOINT

mounts the image read-only.  Inode numbers, modes, owners, times and
device numbers are those of the image.  Unmount it with `fusermount3 -u
MOUNTPOINT` on Linux or `umount MOUNTPOINT` on the BSDs.

    newfs_minixfs -V version [-N] [-B le|be] [-b block-size]
        [-d directory [-F specfile [-P dbdir] [-x]] [-o uid:gid]]
        [-i inodes] [-l name-length] [-s blocks] [-t time]
        [-z log-zone-size] image

makes a file system, empty or, with `-d`, holding a copy of a
directory.  The version has to be given.  The defaults
are little-endian, 14-character names in V1 and V2, 4096-byte blocks in
V3 (the smallest that MINIX 3 mounts), one inode for about every three
blocks, and one-block zones.  The size comes from `-s`, which also
creates the image file or cuts it to size, or from the size of an
existing file.  `-t` sets the time of the root directory, for images
that must come out the same each time; `-N` prints the layout and writes
nothing.  The first 1024 bytes, where a boot block may be, are left
alone.

With `-d`, the files, directories, symbolic links, devices and pipes of
the directory go in with their modes, owners and times (symbolic links
with 0777, as MINIX and Linux make them), in the order of their names,
so that the same tree makes the same image; hard links stay links and
blocks of zeros stay holes, and the root directory takes the mode, owner
and times of the directory itself.  Sockets are left out with a
warning.  `-o uid:gid` gives every file that owner and group
instead, which V1 needs for a group above 255.

`-F` reads an mtree(8) specification as NetBSD's makefs `-F` does, in
the hierarchical form and in the form with full paths of the METALOG
that a build writes: an entry sets the type, mode, owner, group, time,
link target and device number of what it names, overriding the
directory; what the directory does not have is made as the entry says
(a regular file empty), `optional` entries excepted; and a type that
differs from that of the file is an error.  User and group names are
looked up in `master.passwd` or `passwd` and `group` of `-P`, or else
in those of the system.  With `-x`, only what the specification names
goes in.  Checksums, sizes and flags in it are ignored, and names with
patterns are not supported.  Everything is checked
before anything is written: names too long, owners and device numbers
too large, and room, counting holes as data.  Without `-s` and an image
file, the image is made large enough by that count, with inodes enough
for the tree unless `-i` gives them.

    fsck_minixfs [-lwy] [-e 0|1] [-T SIZE:HEADS:SIDE] image

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
marked clean, or as having errors if problems remain.  With `-l` as
well, inodes that no directory names are linked into `/lost+found` as
`#` and their number, trees and all, instead of being freed;
`/lost+found` is made if it is not there.  MINIX and Linux have no
`/lost+found`, and their fsck frees such inodes, so `-l` is not the
default.  With `-e 0` or `-e 1`, the bits of each map past the last
inode or zone must be 0 or 1: the mkfs of MINIX leaves them clear while
that of Linux and `newfs_minixfs` set them, so they are not checked by
default.  With `-w`, what the fsck of MINIX 3 warns about is noted too:
map blocks beyond what the maps need, a first data zone later than the
inode table allows, a maximum file size other than the one MINIX works
out (Linux and `newfs_minixfs` write 2147483647 in V2 and V3), and very
large zones; these layouts work, so they are not problems.  It exits
with 0 if the file system is consistent (after the repairs, with `-y`),
1 if problems remain and 3 if the image cannot be checked at all.

A file system that is not marked clean is noted but is not a problem.
V1 and V2 keep the mark where Linux does; MINIX leaves that word zero.
V3 keeps it in the flags of MINIX 3, which mounts a file system that is
not clean read-only; `newfs_minixfs` marks new file systems clean.

    tunefs_minixfs [-fN] [-B le|be] [-c clean|dirty] [-e 0|1]
        [-l 14|30] [-m minix|linux|bytes] [-s blocks]
        [-T SIZE:HEADS:SIDE] image

changes the settings of a file system; without options, or with `-N`,
it prints them and writes nothing, and `-N` shows what the options
would change.  `-B` stores every number of the file system in the
other byte order, little-endian as on the PC or big-endian as on the
Atari ST and the Amiga: the super block, the words of the maps, the
inodes, the indirect zones and the inode numbers in directories.  `-l`
gives a V1 or V2 file system names of 14 or 30 characters: the magic
number changes and every directory is written anew with entries of the
new size.  Before anything is written, every name is checked, and names
too long for 14 characters are all listed and nothing changes; so it
does when the larger directories would not fit.  `-s` grows or shrinks
the file system, and the image file with it, to so many blocks.  To
grow, if the zone map has no room for the new zones, it gets more
blocks, and the inode table and every data zone in use move up to make
room, with every zone number changed to match.  To shrink, the zones in
use past the new end move to free zones before it, with the zone
numbers that name them; the zone map keeps its blocks, which
`fsck_minixfs -w` notes.  What is in use has to fit, or nothing
changes.  `-c` marks the file
system clean or dirty, where Linux and MINIX 3 keep the mark.  `-e`
sets the bits of the maps past the last inode or zone to 0, as the mkfs
of MINIX leaves them, or 1, as that of Linux.  `-m` sets the maximum
file size in the super block to what MINIX works out, to what Linux and
`newfs_minixfs` write, or to a number.  Each change is printed as the
old and the new value.
Changes that rewrite more than the super block and the maps expect a
file system that `fsck_minixfs` passes and that is not mounted, and one
cut short leaves it half changed, so keep a copy.  Linux and MINIX 3
take the clean mark away while they have a file system mounted for
writing, and would write their own idea of it back over the change, so
`-B`, `-l` and `-s` refuse a file system that is not marked clean; `-f`
changes it all the same.  `fsck_minixfs -y` marks a file system clean
that it finds consistent, which also serves for V1 and V2 file systems
made by MINIX, where the mark is never set.  None of this works on a
mounted file system: neither MINIX nor Linux can grow one.

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
    src/tunefs_minixfs.c    the tunefs_minixfs command
    src/tree.h, src/tree.c  newfs_minixfs -d: copying a directory tree
    src/spec.h, src/spec.c  newfs_minixfs -F: reading an mtree spec
    src/mfs_format.c        library: laying out and writing a new
                            file system
    src/mfs_tune.c          library: changing a file system in place
    src/mfs_write.c         library: taking inodes and zones, writing
                            files and directory entries
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
