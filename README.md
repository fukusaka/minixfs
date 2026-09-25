# minixfs

[日本語](README.ja.md)

Tools for MINIX file system images, written from scratch in portable C.

The aim is a FUSE file system and a set of management tools for the MINIX
file systems, on Linux, FreeBSD, NetBSD and MINIX 3 itself:

1. MINIX V1, V2 and V3 file systems
2. little- and big-endian images, recognised automatically
3. `mkfs`, `tunefs`-style management commands
4. a test suite thorough enough to trust the results

## Status

Read-only access to V1, V2 and V3 file systems in either byte order,
through the `minixfs` command, mounts through FUSE with
`mount_minixfs`, read-only or, with `-w`, read-write, empty file systems of any of them with
`newfs_minixfs`, consistency checks and repairs with `fsck_minixfs`,
changes of settings with `tunefs_minixfs`, and dumps in the format of
BSD dump with `dump_minixfs`, which `restore_minixfs` restores, as it
does the dumps of NetBSD, FreeBSD and Linux.

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
    make fuse-minix                                     # MINIX 3

`FUSE_CFLAGS` and `FUSE_LIBS` default to what `pkg-config fuse3` says.
The source uses only the high-level FUSE API, in its FUSE 3 form
(`FUSE_VERSION=31`, the default) or its FUSE 2 form (`FUSE_VERSION=26`,
for older librefuse).

## Usage

    minixfs [-M SIZE:HEADS:SIDE] info IMAGE
    minixfs [-M ...] ls [-lR] IMAGE [PATH]
    minixfs [-M ...] cat IMAGE PATH
    minixfs [-M ...] extract [-dv] IMAGE DEST [PATH]
    minixfs [-M ...] tar IMAGE [PATH] > ARCHIVE

`info` prints the super block, the size of the image against that of
the file system, and how much of the image is sectors of nothing but
0xe5 or 0xa5, which formats leave and nothing else writes, with the
commonest length of such runs.  A disk read with more sides than it was
written on shows as runs of one track, half of the image; a copy cut
short or a file system smaller than its disk shows too.  None of this
is an error in itself.  `ls -l` shows mode, links, owner, group,
size (or major and minor numbers for devices) and modification time in
UTC.  `extract` copies a directory tree out of the image, keeping
permission bits (without set-uid, set-gid and sticky bits) and times,
and the other names of a file with more than one link as links.
It makes pipes, and devices only with `-d`, which takes root; devices
left out and sockets, which cannot be copied, are reported in a warning
at the end, and with `-v` one by one.  A name already in `DEST` is
replaced as tar(1) replaces it, removed first unless it is a directory,
so that the disks of a set go one after the other into one tree.
`tar` writes the tree to standard
output as a POSIX ustar archive instead, devices and pipes included,
with hard links as links and names longer than ustar holds in pax
headers; tar(1) lists it without privileges and makes the devices when
run as root.  Sockets cannot be stored.

`extract` never writes outside `DEST`: directory entries whose names are
empty, `.`, `..` or contain `/` are refused, and so are directory loops.

`-M SIZE:HEADS:SIDE`, which `minixfs`, `fsck_minixfs`, `tunefs_minixfs`
and `mount_minixfs` all take, reads an image that holds the file system in
the tracks of one side only: tracks of SIZE bytes, HEADS of them to a
cylinder, of which side SIDE (from 0) holds the file system.  A
single-sided 360K disk read as a double-sided 720K one, such as Atari
MINIX 1.5 disks, is `-M 4608:2:0`: every other track is empty.  The
super block is then still in place, so `info` looks right, while the
rest reads as damage.  `fsck_minixfs -y -M` repairs such an image in
place and leaves the other side alone.

    mount_minixfs [-w [-u always|sync|seconds]] [-M SIZE:HEADS:SIDE]
        [FUSE options] IMAGE MOUNTPOINT

mounts the image, read-only unless `-w` is given.  Inode numbers,
modes, owners, times and device numbers are those of the image.  With
`-w`, files, directories and links can be made, removed, renamed and
changed; the kernel checks permissions, and new files belong to the
caller, with the group of their directory where the inode cannot hold
that of the caller (a byte in V1).  While mounted for writing, the
image is locked against other writers and not marked clean; unmounting
marks it clean again.  An image that is not marked clean, or has flex
directories, is mounted read-only with a warning.  `-u` says when the
bit maps go to the image: at fsync and unmount (`sync`, the default),
as they change (`always`), or also after so many seconds.  Unmount it
with `fusermount3 -u MOUNTPOINT` on Linux or `umount MOUNTPOINT` on the
BSDs.  `-o rw,ro,update=X,tracks=X` stand for `-w`, read-only, `-u X`
and `-M X`, as mount(8) gives options.  On MINIX 3 it is a service that
mount(8) starts on a vnd device (`mount -t minixfs -o rw /dev/vnd0
/mnt`), installed as mount_minixfs(8) says; writing is not reliable
there, as libpuffs gets new symbolic links, pipes and character devices
wrong.

The commands that write an image lock it with fcntl(2) while they have
it open, and refuse an image that another holds.

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

    fsck_minixfs [-lwy] [-e 0|1] [-M SIZE:HEADS:SIDE] image

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
        [-l 14|30] [-m minix|linux|bytes]
        [-M SIZE:HEADS:SIDE] [-s blocks] image

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

    dump_minixfs [-0123456789u] [-D dumpdates] [-L label]
        [-M SIZE:HEADS:SIDE] [-T date] -f file image [path]

writes the file system as NetBSD dump writes one of UFS1, in the byte
order of the image, so that the restore of NetBSD reads it; inode n
becomes inode n + 1, as restore takes 2 for the root.  A dump of level
n holds what changed since the last dump of a lower level that the
dumpdates file notes (`-u`, `-D`), or since the date of `-T`; with a
path, only that directory and what is below it go in, at level 0.  The
restore of Linux does not read it, as it does not read the dumps NetBSD
makes of UFS1: it takes their 64-bit date for a count of its own.

    restore_minixfs -t [-c] -f file
    restore_minixfs -r [-cNv] [-M SIZE:HEADS:SIDE] [-o uid:gid]
        [-s symtable] -f file image

reads dumps of BSD dump: those of `dump_minixfs`, of NetBSD and FreeBSD
for UFS1 and UFS2, of Linux dump for ext2, 3 and 4 (also the runs that
it writes into the headers since 0.4b49), and of 4.2BSD and 4.3BSD,
with directories of 4.4BSD, 4.2BSD or V7, in either byte order.  `-t`
lists the names; `-r` restores a full dump into an empty file system,
then each incremental one on top, keeping in `-s` (`restoresymtable`
by default) which inode each inode of the dumps became.  Each directory
of the dump comes to hold what the dump says it holds, and files whose
last name went away are freed.  The names, and the number of inodes,
are checked before anything is written; from a file rather than a
pipe, so are owners, device numbers and sizes.  `-o` gives every file
one owner, for owners that do not fit.  Extended attributes, file
flags and sockets are left out with a warning; compressed dumps of
Linux and dumps on more than one volume are not read.

The other commands exit with 0 on success, 1 if anything failed; all
exit with 2 for a usage error.

## Manuals

The manuals are in `man/`, in English, and in `man/ja/`, in Japanese:
minixfs(1), mount_minixfs(8), newfs_minixfs(8), fsck_minixfs(8),
tunefs_minixfs(8), dump_minixfs(8) and restore_minixfs(8) for the
commands, and minixfs(5) for the formats of
the file systems.  `make install` puts them below `MANDIR`, with the
commands below `PREFIX`, and `make lint-man` checks them.

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
    src/dump_minixfs.c      the dump_minixfs command
    src/restore_minixfs.c   the restore_minixfs command
    src/dumpfmt.h, src/dumpfmt.c
                            the dump format of BSD: headers, maps,
                            runs of c_addr, directory entries
    src/tree.h, src/tree.c  newfs_minixfs -d: copying a directory tree
    src/spec.h, src/spec.c  newfs_minixfs -F: reading an mtree spec
    src/mfs_format.c        library: laying out and writing a new
                            file system
    src/mfs_tune.c          library: changing a file system in place
    src/mfs_write.c         library: taking and freeing inodes and
                            zones, writing and resizing files, adding
                            directory entries
    src/mfs_ops.c           library: making, linking, removing and
                            renaming files and directories
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
