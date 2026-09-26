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

イメージにはファイルまたはデバイスを指定します。各コマンドのリンク先に詳しい説明があります。

### マウントせずに読み書きする

[minixfs(1)](cat/ja/minixfs.1.txt) でファイルを一覧・抽出・変更できます。

    minixfs ls -l root.img etc
    minixfs cat root.img etc/passwd > passwd
    minixfs extract root.img out
    minixfs put -R usr.img src usr

`extract` と `put` は既存の名前を置き換えます。`put -n` は既存のものを残し、`put -i` は
置き換える前に確認します。`extract` でデバイスを作るには `-d` と root 権限が必要です。
イメージ内に作るファイルの所有者は格納先のディレクトリから引き継ぎ、`-o uid:gid` で変更できます。

ほかに `info`、`blocks`、`tar`、`mkdir`、`rm`、`mv`、`ln`、`chmod`、`chown` があります。

### FUSE でマウントする

[mount_minixfs(8)](cat/ja/mount_minixfs.8.txt) は、既定では読み出し専用でマウントします。

    mount_minixfs usr.img /mnt
    fusermount3 -u /mnt

書き込む場合は `-w` を付けます。BSD では `umount /mnt` でアンマウントします。
MINIX 3 での書き込みは不安定です。各 OS の制限と既知のデータ消失条件は man の BUGS を参照してください。

### イメージを作る

[newfs_minixfs(8)](cat/ja/newfs_minixfs.8.txt) で、空のファイルシステム、または
ディレクトリツリーを取り込んだファイルシステムを作れます。

    newfs_minixfs -V 1 -s 360 floppy.img
    newfs_minixfs -V 2 -d tree -o 0:0 tree.img
    newfs_minixfs -V 2 -l flex -s 2048 vmd.img

`-V` は必須です。`-l flex` で Minix-vmd のファイルシステムを作ります。`-d` で新規イメージを作る場合、`-s` がなければツリーに必要な容量を確保します。
`-F` は METALOG などの mtree 指定を読み込みます。ブートコード用の先頭 1024 バイトは保持します。
最大ファイルサイズとビットマップ末尾のビットは MINIX に、30 文字の名前では Linux に合わせます。`-m` と `-e` で変えられます。

### 検査・修復する

[fsck_minixfs(8)](cat/ja/fsck_minixfs.8.txt) は、既定では書き込まずに検査します。

    fsck_minixfs root.img
    fsck_minixfs -y -l usr.img

`-y` で修復します。未参照 inode（ディレクトリから参照されていない inode）は解放します。
`-l` を併用すると、解放せずに `lost+found` に保存します。
修復後に再検査し、問題が残っていなければ clean の印を付けます。

### 設定を変更する

[tunefs_minixfs(8)](cat/ja/tunefs_minixfs.8.txt) は、バイト順、名前の長さ、容量などを変更します。
オプションなしでは現在の設定を表示し、`-N` では変更予定を表示します。

    tunefs_minixfs -B le -l 30 atari.img
    tunefs_minixfs -s 2880 floppy.img

`-B`・`-l`・`-s` を使う前に、アンマウントし、`fsck_minixfs` で検査してバックアップを取ってください。
中断すると変換途中の状態が残ります。`-l` と `-s` の併用では、先行する変更の適用後に容量変更が
失敗する場合があります。`-s` はイメージファイルの容量も変更しますが、`-k` 指定時とデバイスでは保持します。

### バックアップ・復元する

[dump_minixfs(8)](cat/ja/dump_minixfs.8.txt) は、BSD dump 形式で全体・増分バックアップを作ります。
[restore_minixfs(8)](cat/ja/restore_minixfs.8.txt) は、その一覧表示・復元に加え、
NetBSD・FreeBSD・Linux・旧 BSD の dump も読み込みます。

    dump_minixfs -0u -D dumpdates -f usr.0 usr.img
    newfs_minixfs -V 3 -s 16384 new.img
    restore_minixfs -r -f usr.0 new.img

復元先には十分な容量を確保し、空のファイルシステムに全体、続いて増分を順に復元します。
次の増分復元に必要な `restoresymtable` は保持してください。dump にない名前は削除されます。

容量不足や dump の途中切れなど、回復可能な失敗では、原因を解消して同じ dump を再実行できます。
I/O エラーや強制終了後は、先に `fsck_minixfs -y` で検査してください。
全体の dump から復元をやり直す必要がある場合もあります。
拡張属性・ファイルフラグ・ソケットは警告して除外します。Linux の圧縮 dump と複数ボリュームの dump は未対応です。

## イメージへの書き込み

書き込み中はイメージをロックし、同時変更を防ぎます。`minixfs` の書き込みコマンド、
`mount_minixfs -w`、`restore_minixfs` は書き込み中に clean の印を外します。
中断や整合性を損なうエラーの後は印が戻らないため、次の書き込み前に `fsck_minixfs -y` で検査してください。

clean でないイメージは、`mount_minixfs` では読み出し専用になり、`restore_minixfs` では書き込めません。
`minixfs` の書き込みコマンドと `tunefs_minixfs -B`・`-l`・`-s` は、`-f` でこの検査を省略できます。
`minixfs -f` は、書き込みに成功しても元の clean でない状態を保持します。

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
