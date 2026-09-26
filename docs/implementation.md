# The implementation

What each source file holds, and where the details of the
implementation are written down.  The manuals say what the commands
do; this is for reading and changing the code.

## The source files

The library, which every command links:

    src/layout.h            the on-disk layout
    src/mfs.h, src/mfs.c    super block, inodes, zone mapping, file
                            data, directories, path lookup
    src/mfs_write.c         taking and freeing inodes and zones,
                            writing and resizing files, adding
                            directory entries
    src/mfs_ops.c           making, linking, removing and renaming
                            files and directories
    src/mfs_format.c        laying out and writing a new file system
    src/mfs_tune.c          changing a file system in place: byte
                            order, name length, size
    src/compat.h            the differences between systems

The commands:

    src/minixfs.h, src/minixfs.c
                            minixfs: what its commands share, and the
                            commands that read
    src/minixfs_write.c     the commands of minixfs that write
    src/mount_minixfs.c     mount_minixfs, the FUSE file system
    src/newfs_minixfs.c     newfs_minixfs
    src/tree.h, src/tree.c  newfs_minixfs -d: copying a directory tree
    src/spec.h, src/spec.c  newfs_minixfs -F: reading an mtree spec
    src/fsck_minixfs.c      fsck_minixfs
    src/tunefs_minixfs.c    tunefs_minixfs
    src/dump_minixfs.c      dump_minixfs
    src/restore_minixfs.c   restore_minixfs
    src/dumpfmt.h, src/dumpfmt.c
                            the dump format of BSD: headers, maps,
                            runs of c_addr, directory entries

## Where the details are

- `src/mfs.h` gives each function of the library with what it does,
  what it returns and what it leaves behind when it fails; for
  instance, how `tunefs_minixfs -s` moves the inode table and the
  zones in use is under `mfs_grow()` and `mfs_shrink()`.
- The comment at the top of each command says how it goes about its
  work, such as the inode numbers that `dump_minixfs` gives in the
  dump and why.
- `src/compat.h` says why each difference between the systems is
  there, such as the ioctl that gives the size of a device.
- `tests/README.md` says what each test script covers.
- `docs/netbsd-refuse-dotdot.md` is a note on a fault of librefuse on
  NetBSD 10 that the tests work around.
