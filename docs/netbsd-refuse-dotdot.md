# A lookup of ".." breaks a librefuse mount on NetBSD 10

On NetBSD 10.0 and later, looking up `..` in a directory right below
the root of a file system served through librefuse fails with EPROTO,
and the mount misbehaves from then on.  This is not particular to
`mount_minixfs`: the examples that come with NetBSD in
`share/examples/refuse` do the same, and so does the smallest file
system one can write against the FUSE API.  `mount_minixfs(8)` notes
it under BUGS, and `tests/t_fuse.sh` leaves out the one check that
walks the tree with find(1) on NetBSD.

## What is seen

    $ ls mnt              works
    $ cat mnt/sub/file    works
    $ stat mnt/sub/..     lstat: Protocol error
    $ cat mnt/file        Protocol error
    $ ls mnt              lists only "mnt" itself
    $ stat mnt/sub        No such file or directory

What happens after the failed lookup varies from run to run, since
the file server goes on with a freed node; now and then an operation
still succeeds.  A lookup of a name in a directory does not use `..`,
so a mount works until something walks up, such as `find`, `cd ..`
or a shell completing a path.

## Why

A run of a minimal file system, holding a directory `d` and a file
`f`, with `-o debug` (the operation dump of libpuffs):

    reqid: 0, PUFFS_VN_LOOKUP, cookie: 0xb1548000        (the root)
        puffs_cn: "d" ...
        new 0xb15480c0, type 0x2, size 0x0, dev 0x0
    reqid: 1, PUFFS_VN_LOOKUP, cookie: 0xb15480c0        (d)
        puffs_cn: ".." ...
        new 0xb1548000, type 0x2, size 0x18446744073709551615, dev 0xffffffffffffffff
    reqid: 0, (FAF), PUFFS_ERR_MAKENODE, cookie: 0xb1548000
    reqid: 0, (FAF), PUFFS_VN_RECLAIM, cookie: 0xb1548000

1. `puffs_fuse_node_lookup()` in `lib/librefuse/refuse.c` builds the
   path `/` for `..`, calls the file system's `getattr` on it, which
   succeeds, and then finds the node of the root with
   `puffs_pn_nodewalk()`.  For a node that exists it does not use the
   stat it just got: it answers the kernel with the node's own
   `pn_va`, through `puffs_newinfo_setsize(pni, pn_res->pn_va.va_size)`.

2. The node of the root is made in `fuse_mount()` with
   `puffs_vattr_null()`, and only `va_type` and `va_mode` are set.  Its
   `va_size` stays `VNOVAL`, that is `(u_quad_t)-1`, which the kernel
   knows as `VSIZENOTSET`.  Nothing fills it in later:
   `puffs_fuse_node_getattr()` calls `fuse_getattr()`, which writes the
   result of the file system's `getattr` into the vattr the kernel
   asked for, not into `pn->pn_va`.  A stat of the root before the
   lookup does not help.

3. `puffs_getvnode1()` in `sys/fs/puffs/puffs_node.c` refuses a size of
   `VSIZENOTSET` ("VSIZENOTSET is not a valid size"), sends
   `PUFFS_ERR_MAKENODE` and returns EPROTO.  `puffs_vnop_lookup()` then
   calls `puffs_abortbutton(PUFFS_ABORT_LOOKUP, ...)` on the cookie the
   lookup gave, which sends `PUFFS_VN_RECLAIM` for it.

4. That cookie is the root.  `puffs_fuse_node_reclaim()` frees the
   node of the root, while the kernel keeps its root vnode with the
   same cookie.  Every later operation on the root runs on freed
   memory.

The file system has no say in this: the FUSE API gives it no way to
set the attributes librefuse keeps in a node, and its `getattr` for
`/` is called and answered.

## Where it comes from

`refuse.c` 1.107 (2022-01-22, "Do not call fuse_operations.getattr()
before initializing filesystem") removed from `fuse_mount()`

    if (fuse->op.getattr)
        if (fuse->op.getattr(po_root->po_path, &st) == 0)
            puffs_stat2vattr(&pn_root->pn_va, &st);

which up to 1.106 gave the root its attributes.  Calling an operation
of the file system before its `init` was wrong, and the removal is
right; but nothing took its place to fill in the attributes of the
root afterwards.  The change is in NetBSD 10.0 and later, and the code
is the same in -current (`refuse.c` 1.114, as of September 2026).

## Reproducing it

With `share/examples/refuse/fanoutfs` of NetBSD, unmodified (built
with `-DFUSE_USE_VERSION=26`, as it is written against the FUSE 2
API), and `/etc/fanoutfs.conf` naming a directory that has a
subdirectory with a file in it:

    # fanoutfs /mnt
    # cat /mnt/Sub/F
    # stat /mnt/Sub/..     -> lstat: Protocol error

Or with the smallest file system, which gives the dump above when run
with `-f -o debug`:

    #define FUSE_USE_VERSION 31
    #include <errno.h>
    #include <fuse.h>
    #include <string.h>
    #include <sys/stat.h>

    static const char text[] = "hello\n";

    static int
    tiny_getattr(const char *path, struct stat *st,
        struct fuse_file_info *fi)
    {
        memset(st, 0, sizeof(*st));
        if (strcmp(path, "/") == 0 || strcmp(path, "/d") == 0) {
            st->st_mode = S_IFDIR | 0755;
            st->st_nlink = 2;
            return 0;
        }
        if (strcmp(path, "/f") == 0) {
            st->st_mode = S_IFREG | 0644;
            st->st_nlink = 1;
            st->st_size = sizeof(text) - 1;
            return 0;
        }
        return -ENOENT;
    }

    static int
    tiny_readdir(const char *path, void *buf, fuse_fill_dir_t fill,
        off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl)
    {
        fill(buf, ".", NULL, 0, 0);
        fill(buf, "..", NULL, 0, 0);
        if (strcmp(path, "/") == 0) {
            fill(buf, "d", NULL, 0, 0);
            fill(buf, "f", NULL, 0, 0);
        }
        return 0;
    }

    static int
    tiny_open(const char *path, struct fuse_file_info *fi)
    {
        return strcmp(path, "/f") == 0 ? 0 : -ENOENT;
    }

    static int
    tiny_read(const char *path, char *buf, size_t size, off_t off,
        struct fuse_file_info *fi)
    {
        size_t len = sizeof(text) - 1;

        if ((size_t)off >= len)
            return 0;
        if (size > len - (size_t)off)
            size = len - (size_t)off;
        memcpy(buf, text + off, size);
        return (int)size;
    }

    int
    main(int argc, char **argv)
    {
        static struct fuse_operations ops = {
            .getattr = tiny_getattr,
            .readdir = tiny_readdir,
            .open = tiny_open,
            .read = tiny_read,
        };

        return fuse_main(argc, argv, &ops, NULL);
    }

    $ cc -o tinyfs tinyfs.c -lrefuse -lpuffs
    # mkdir m && ./tinyfs -f m &
    # stat m/d/..           -> lstat: Protocol error

## A fix

Have the lookup store the attributes it just got from `getattr` in the
node it answers with, whether the node is new or already known.  A
lookup only ever comes from the main loop, after `init`, so this keeps
what 1.107 set out to do; it also stops a known node from answering
with the size it had at its first lookup.

    --- lib/librefuse/refuse.c
    +++ lib/librefuse/refuse.c
    @@ -450,8 +450,16 @@
     		pn_res = newrn(pu);
     		if (pn_res == NULL)
     			return errno;
    -		puffs_stat2vattr(&pn_res->pn_va, &st);
     	}
    +	/*
    +	 * Store the attributes in the node even if it already exists:
    +	 * the root node is created in fuse_mount() without calling
    +	 * op.getattr(), so its va_size is still VNOVAL, which the kernel
    +	 * rejects (VSIZENOTSET) when a lookup of ".." returns the root.
    +	 * Nodes found this way also get fresh attributes instead of
    +	 * the ones from their first lookup.
    +	 */
    +	puffs_stat2vattr(&pn_res->pn_va, &st);
     
     	puffs_newinfo_setcookie(pni, pn_res);
     	puffs_newinfo_setvtype(pni, pn_res->pn_va.va_type);

With librefuse built so, on NetBSD 10.1, the minimal file system and
fanoutfs work across `..`, and `mount_minixfs` passes all of
`tests/t_fuse.sh` with the check left out on NetBSD put back.

## Status

No report of this was found, as of September 2026: not in GNATS,
searched over every state for "librefuse", "refuse", "puffs" (lib and
kern), "fuse" (lib), "dotdot", "VSIZENOTSET" and "EPROTO" (kern); not
in the netbsd-bugs list from 2022 on, where every report arrives; and
not in the source history.  A problem report with the above has been
drafted but not sent.
