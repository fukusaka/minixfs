# minixfs

[English](README.md)

MINIX ファイルシステムのイメージを読み書き・マウント・バックアップする道具です。
Linux、FreeBSD、NetBSD、MINIX 3 向けに、移植性のある C で一から実装しています。

## 対応形式

V1・V2・V3 と Minix-vmd に対応し、リトルエンディアンとビッグエンディアンを自動判別します。

| マジック番号 | 版 | 名前 | 出どころ |
|--------|---------|-------|--------|
| 0x137f | V1      | 14    | MINIX |
| 0x137f | V1      | 14。flex ディレクトリなら 60 | Minix-vmd。独自の superblock |
| 0x138f | V1      | 30    | Linux の拡張 |
| 0x2468 | V2      | 14    | MINIX |
| 0x2468 | V2      | 14。flex ディレクトリなら 60 | Minix-vmd。独自の superblock |
| 0x2478 | V2      | 30    | Linux の拡張 |
| 0x4d5a | V3      | 60    | MINIX 3。ブロックサイズは superblock から |

V2・V3 の三重間接 zone にも対応します。形式の詳細は [minixfs(5)](cat/ja/minixfs.5.txt)、
Minix-vmd への書き込み制限は各コマンドの man を参照してください。

## ビルドとインストール

C99 と POSIX.1-2008 が必要です。追加ライブラリは不要で、GNU make と BSD make に対応します。

    make
    make install

`make install` はコマンドを `PREFIX`（既定は `/usr/local`）、man を `MANDIR` の下に配置します。

FUSE のマウントコマンドは別途ビルドします。

    make fuse                                            # libfuse 3
    make fuse FUSE_LIBS="-lrefuse -lpuffs"                 # NetBSD
    make fuse FUSE_LIBS="-lrefuse -lpuffs" FUSE_VERSION=26 # 古い librefuse
    make fuse-minix                                      # MINIX 3

`FUSE_CFLAGS` と `FUSE_LIBS` の既定は `pkg-config fuse3` の出力です。
FreeBSD では fusefs-libs3 と pkgconf をインストールし、`kldload fusefs` でカーネルモジュールを
読み込みます。一般ユーザーのマウントには `vfs.usermount=1` が必要です。
MINIX 3 のサービス設定は [mount_minixfs(8)](cat/ja/mount_minixfs.8.txt) を参照してください。

## 使い方

イメージにはファイルまたはデバイスを指定します。オプションと制限の詳細は各 man を参照してください。

| コマンド | 用途 |
|---|---|
| [minixfs](cat/ja/minixfs.1.txt) | マウントせずに一覧・抽出・編集 |
| [mount_minixfs](cat/ja/mount_minixfs.8.txt) | FUSE マウント（既定は読み出し専用、`-w` で書き込み） |
| [newfs_minixfs](cat/ja/newfs_minixfs.8.txt) | 空またはディレクトリからイメージを作成 |
| [fsck_minixfs](cat/ja/fsck_minixfs.8.txt) | 整合性検査（`-y` で修復、`-l` で未参照 inode を保存） |
| [tunefs_minixfs](cat/ja/tunefs_minixfs.8.txt) | バイト順・名前長・容量などの変更（`-N` で変更予定を表示） |
| [dump_minixfs](cat/ja/dump_minixfs.8.txt) | BSD dump 形式の全体・増分バックアップ |
| [restore_minixfs](cat/ja/restore_minixfs.8.txt) | dump の一覧・復元（NetBSD・FreeBSD・Linux・旧 BSD にも対応） |

### 読み書き

    minixfs ls -l root.img etc
    minixfs extract root.img out
    minixfs put -R usr.img src usr

`extract` と `put` は既存の名前を置き換えます。`put -n` は既存ファイルを残し、`put -i` は置換を確認します。

### 作成とマウント

    newfs_minixfs -V 2 -d tree -o 0:0 tree.img
    newfs_minixfs -V 2 -l flex -s 2048 vmd.img
    mount_minixfs tree.img /mnt
    fusermount3 -u /mnt

`-V` は必須です。`-d` で新規作成するときは、`-s` がなければ必要容量を自動計算します。
`-l flex` は Minix-vmd 形式です。BSD のアンマウントは `umount /mnt` です。
MINIX 3 の書き込みを含む OS 固有の制限は [mount_minixfs(8) の BUGS](cat/ja/mount_minixfs.8.txt) を参照してください。

### 検査と設定変更

    fsck_minixfs root.img
    fsck_minixfs -y -l usr.img
    tunefs_minixfs -N -s 2880 floppy.img

修復では未参照 inode を解放します。`-l` を併用すると `lost+found` に保存します。
`tunefs` の変換前にはアンマウント・検査・バックアップが必要です。中断や複数オプションの失敗で、変更の一部が残る場合があります。

### バックアップと復元

    dump_minixfs -0u -D dumpdates -f usr.0 usr.img
    newfs_minixfs -V 3 -s 16384 new.img
    restore_minixfs -r -f usr.0 new.img

十分な容量の空のファイルシステムに全体、続いて増分を順に復元します。
次の増分復元まで `restoresymtable` を保持してください。復元時は dump にない名前も削除されます。
失敗後の再実行条件と、復元しない属性・形式は [restore_minixfs(8)](cat/ja/restore_minixfs.8.txt) を参照してください。

## 書き込み時の注意

書き込み中はイメージをロックします。中断や I/O エラー後は、次の書き込み前に `fsck_minixfs -y` で検査してください。
clean でないイメージに対して、`mount_minixfs` は読み出し専用、`restore_minixfs` は書き込み拒否になります。
`minixfs` の書き込みコマンドと `tunefs_minixfs -B`・`-l`・`-s` は `-f` で強制できます。
`minixfs -f` は正常終了しても元の clean でない状態を保持します。
## 資料と開発

- [日本語 man](man/ja/) / [英語 man](man/)：オプション、既定値、終了コード、制限。[cat/](cat/) に、`make catman` で整形したテキスト版があります。`make lint-man` で書式を検査します。
- [テスト](tests/README.md)：`make check` の実行と、サニタイザ・移植性の検証。
- [実装](docs/implementation.md)：ソースの構成と設計資料。
- [コーディング規約](STYLE.md)：変更時の規約。

ディスク形式は MINIX 2.0.4（`fs/super.h`、`fs/inode.h`、`fs/type.h`、`fs/const.h`）、
MINIX 3（`minix/fs/mfs`）、Minix-vmd 1.7.0（`sys/fs/super.h`、`sys/fs/const.h`、`include/dirent.h`）の
定義に従っています。

## ライセンス

BSD 2-Clause。[LICENSE](LICENSE) を参照してください。
