# minixfs

[日本語](README.ja.md)

Tools to read, modify, mount and back up MINIX file system images.
Written from scratch in portable C for Linux, FreeBSD, NetBSD and MINIX 3.

## Supported formats

Supports V1, V2, V3 and Minix-vmd images, with automatic detection of
little-endian and big-endian byte order.

| Magic  | Version | Names | Origin |
|--------|---------|-------|--------|
| 0x137f | V1      | 14    | MINIX |
| 0x137f | V1      | 14; 60 with flex directories | Minix-vmd; super block of its own |
| 0x138f | V1      | 30    | Linux extension |
| 0x2468 | V2      | 14    | MINIX |
| 0x2468 | V2      | 14; 60 with flex directories | Minix-vmd; super block of its own |
| 0x2478 | V2      | 30    | Linux extension |
| 0x4d5a | V3      | 60    | MINIX 3; block size from the super block |

Triple indirect zones in V2 and V3 are supported. See
[minixfs(5)](cat/minixfs.5.txt) for format details and the command manuals
for restrictions on writing Minix-vmd images.

## Build and install

The tools require C99 and POSIX.1-2008, with no additional libraries.
Both GNU make and BSD make are supported.

    make
    make install

`make install` installs commands under `PREFIX` (default `/usr/local`)
and manuals under `MANDIR`.

The FUSE mount tool is built separately:

    make fuse                                            # libfuse 3
    make fuse FUSE_LIBS="-lrefuse -lpuffs"                 # NetBSD
    make fuse FUSE_LIBS="-lrefuse -lpuffs" FUSE_VERSION=26 # older librefuse
    make fuse-minix                                      # MINIX 3

`FUSE_CFLAGS` and `FUSE_LIBS` default to the output of `pkg-config fuse3`.
On FreeBSD, install fusefs-libs3 and pkgconf, load the kernel module with
`kldload fusefs`, and set `vfs.usermount=1` to allow non-root mounts.
For MINIX 3 service setup, see [mount_minixfs(8)](cat/mount_minixfs.8.txt).

## Usage

An image is a file or device. Each manual describes the options and limitations.

| Command | Purpose |
|---|---|
| [minixfs](cat/minixfs.1.txt) | List, extract and edit files without mounting |
| [mount_minixfs](cat/mount_minixfs.8.txt) | FUSE mount; read-only by default, writable with `-w` |
| [newfs_minixfs](cat/newfs_minixfs.8.txt) | Create an empty image or copy a directory into one |
| [fsck_minixfs](cat/fsck_minixfs.8.txt) | Check consistency; `-y` repairs, `-l` saves unreferenced inodes |
| [tunefs_minixfs](cat/tunefs_minixfs.8.txt) | Change byte order, name length, size and other settings; `-N` previews changes |
| [dump_minixfs](cat/dump_minixfs.8.txt) | Create full and incremental BSD dumps |
| [restore_minixfs](cat/restore_minixfs.8.txt) | List or restore dumps, including NetBSD, FreeBSD, Linux and older BSD formats |

### Read and write files

    minixfs ls -l root.img etc
    minixfs extract root.img out
    minixfs put -R usr.img src usr

`extract` and `put` replace existing names. Use `put -n` to keep existing
files or `put -i` to confirm replacement.

### Create and mount

    newfs_minixfs -V 2 -d tree -o 0:0 tree.img
    newfs_minixfs -V 2 -l flex -s 2048 vmd.img
    mount_minixfs tree.img /mnt
    fusermount3 -u /mnt

`-V` is required. With `-d`, new images are sized automatically unless
`-s` specifies a size. `-l flex` creates Minix-vmd format.
On BSD, unmount with `umount /mnt`.
See [mount_minixfs(8), BUGS](cat/mount_minixfs.8.txt) for platform
limitations, including unreliable writes on MINIX 3.

### Check and change settings

    fsck_minixfs root.img
    fsck_minixfs -y -l usr.img
    tunefs_minixfs -N -s 2880 floppy.img

Repair frees unreferenced inodes unless `-l` saves them in `lost+found`.
Before converting with `tunefs`, unmount, check and back up the image.
Interruption or failure with combined options can leave partial changes.

### Back up and restore

    dump_minixfs -0u -D dumpdates -f usr.0 usr.img
    newfs_minixfs -V 3 -s 16384 new.img
    restore_minixfs -r -f usr.0 new.img

Restore the full dump into an empty file system with enough space, then
apply incremental dumps in order. Keep `restoresymtable` for the next
incremental restore. Names absent from the dump are removed.
See [restore_minixfs(8)](cat/restore_minixfs.8.txt) for retry conditions,
omitted attributes and unsupported formats.

## Writing precautions

Writers lock the image. After interruption or an I/O error, run
`fsck_minixfs -y` before further writes.
For an unclean image, `mount_minixfs` falls back to read-only and
`restore_minixfs` refuses to write. The `minixfs` write commands and
`tunefs_minixfs -B`, `-l` and `-s` accept `-f` to override the check.
`minixfs -f` preserves the original unclean state even after success.
## Documentation and development

- [English manuals](man/) / [Japanese manuals](man/ja/): options, defaults,
  exit codes and limitations. [cat/](cat/) holds them formatted as plain
  text, made with `make catman`. `make lint-man` checks their markup.
- [Tests](tests/README.md): run `make check`; sanitizer and portability checks.
- [Implementation](docs/implementation.md): source layout and design notes.
- [Coding style](STYLE.md): contribution rules.

The disk format follows MINIX 2.0.4 (`fs/super.h`, `fs/inode.h`,
`fs/type.h`, `fs/const.h`), MINIX 3 (`minix/fs/mfs`) and Minix-vmd 1.7.0
(`sys/fs/super.h`, `sys/fs/const.h`, `include/dirent.h`).

## License

BSD 2-Clause. See [LICENSE](LICENSE).
