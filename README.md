# minixfs

[日本語](README.ja.md)

Tools to read, modify, mount and back up MINIX file system images.
Written from scratch in portable C for Linux, FreeBSD, NetBSD and MINIX 3.

## Supported formats

V1, V2, V3 and Minix-vmd images are supported. The byte order is detected
automatically; both little-endian and big-endian images can be read.

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

An image can be a file or a device. Each command below links to its manual.

### Read and modify an image

[minixfs(1)](cat/minixfs.1.txt) lists, extracts and modifies files without mounting:

    minixfs ls -l root.img /etc
    minixfs cat root.img /etc/passwd > passwd
    minixfs extract root.img out
    minixfs put -R usr.img src /usr

`extract` and `put` replace existing names. Use `put -n` to keep them or
`put -i` to confirm replacement. `extract` requires `-d` and root privileges
to create devices. Files created in an image inherit the destination
directory's owner; `-o uid:gid` overrides it.

Other commands include `info`, `blocks`, `tar`, `mkdir`, `rm`, `mv`, `ln`,
`chmod` and `chown`.

### Mount through FUSE

[mount_minixfs(8)](cat/mount_minixfs.8.txt) mounts read-only by default:

    mount_minixfs usr.img /mnt
    fusermount3 -u /mnt

Use `-w` for writing; on BSD, unmount with `umount /mnt`.
Minix-vmd flex directories are always mounted read-only.
Writing on MINIX 3 is unreliable. See the manual's BUGS section for
platform limitations and known data-loss conditions.

### Create an image

[newfs_minixfs(8)](cat/newfs_minixfs.8.txt) creates an empty file system or
copies a directory tree into one:

    newfs_minixfs -V 1 -s 360 floppy.img
    newfs_minixfs -V 2 -d tree -o 0:0 tree.img

`-V` is required. With `-d`, a new image is sized for the tree unless
`-s` specifies its size. `-F` reads an mtree specification such as METALOG.
The first 1024 bytes, reserved for boot code, are preserved.

### Check and repair

[fsck_minixfs(8)](cat/fsck_minixfs.8.txt) checks without writing by default:

    fsck_minixfs root.img
    fsck_minixfs -y -l usr.img

`-y` repairs the image. Inodes with no directory references are freed
unless `-l` saves them under `/lost+found`. After repair, the image is checked again and
marked clean only if no problems remain.

### Change file system settings

[tunefs_minixfs(8)](cat/tunefs_minixfs.8.txt) changes byte order, name length,
size and other settings. With no options it displays the settings;
`-N` previews changes.

    tunefs_minixfs -B le -l 30 atari.img
    tunefs_minixfs -s 2880 floppy.img

Before using `-B`, `-l` or `-s`, unmount the image, check it with
`fsck_minixfs` and keep a backup. Interruption can leave a partial
conversion. With `-l` and `-s` together, resizing can fail after earlier
changes have been applied. `-s` also resizes the image file unless `-k`
is given; devices retain their size.

### Back up and restore

[dump_minixfs(8)](cat/dump_minixfs.8.txt) creates full and incremental BSD dumps.
[restore_minixfs(8)](cat/restore_minixfs.8.txt) lists or restores them, and also
reads NetBSD, FreeBSD, Linux and older BSD dumps.

    dump_minixfs -0u -D dumpdates -f usr.0 usr.img
    newfs_minixfs -V 3 -s 16384 new.img
    restore_minixfs -r -f usr.0 new.img

Choose a destination size large enough for the restored files.
Restore a full dump into an empty file system, then its incremental dumps
in order. Keep `restoresymtable` between restores; it records the previous
restore and inode mapping. Restoring also removes names absent from the dump.

After a recoverable failure such as insufficient space or a truncated
dump, correct the cause and retry the same dump. After an I/O error or
forced termination, first check the image with `fsck_minixfs -y`;
recovery may require starting again from the full dump.
Extended attributes, file flags and sockets are omitted with a warning.
Compressed Linux dumps and multi-volume dumps are unsupported.

## Writing images

Writers lock the image to prevent concurrent changes. `minixfs` write
commands, `mount_minixfs -w` and `restore_minixfs` clear the clean flag
while writing. Interruption or an error that may corrupt the image
leaves it unclean; run `fsck_minixfs -y` before further writes.

For an unclean image, `mount_minixfs` falls back to read-only and
`restore_minixfs` refuses to write. `minixfs` write commands and
`tunefs_minixfs -B`, `-l` and `-s` require `-f` to override the check.
`minixfs -f` preserves the original unclean state after a successful write.

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
