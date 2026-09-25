# minixfs

[English](README.md)

MINIX ファイルシステムのイメージを扱う道具です。移植性のある C で、一から書いています。

目標は、Linux、FreeBSD、NetBSD、そして MINIX 3 自身の上で動く、MINIX ファイルシステムの
FUSE ファイルシステムと管理用の道具一式です。

1. MINIX の V1・V2・V3 ファイルシステム
2. リトルエンディアンとビッグエンディアンのイメージを自動で見分ける
3. `mkfs`・`tunefs` のような管理コマンド
4. 結果を信頼できるだけの十分なテスト

## 現状

- `minixfs` コマンドによる、V1・V2・V3 ファイルシステムの読み出し（どちらのバイト順でも）
- `mount_minixfs` による、FUSE でのマウント（読み出し専用、`-w` で書き込み可能）
- `newfs_minixfs` による、どの版でも空のファイルシステムの作成
- `fsck_minixfs` による、整合性の検査と修復
- `tunefs_minixfs` による、設定の変更
- `dump_minixfs` による、BSD の dump の形式での書き出しと、`restore_minixfs` による復元
  （NetBSD、FreeBSD、Linux の dump も復元できます）

| マジック番号 | 版 | 名前 | 出どころ |
|--------|---------|-------|--------|
| 0x137f | V1      | 14    | MINIX |
| 0x138f | V1      | 30    | Linux の拡張 |
| 0x2468 | V2      | 14    | MINIX |
| 0x2478 | V2      | 30    | Linux の拡張 |
| 0x4d5a | V3      | 60    | MINIX 3。ブロックサイズは superblock から |

V2 と V3 の inode には三重間接 zone があります。Linux は使い、MINIX は使いませんが、これも読みます。

Philip Homburg と Kees Bot による MINIX の派生 Minix-vmd は、MINIX のマジック番号を持つ V1 と V2
のファイルシステムを、独自の superblock で書きます。zone の大きさを 1 バイトに、フラグ（flex
ディレクトリ、clean）を次の 1 バイトに持ち、Linux が状態を置く場所に 0x7f と 0x13 のバイトを
持つので、それで見分けます。flex ディレクトリは 8 バイトのスロットでできた項目を持ち、名前は
60 文字までです。どちらも読みます。`info` はこの亜種の名前を示し、clean の印はこの亜種のものを
使います。決まった大きさのディレクトリ項目を書く処理（"." や ".." を戻す、`/lost+found`、
`tunefs_minixfs -B` と `-l`）は、これを拒みます。

バイト順はマジック番号から分かります。PC で作ったイメージはリトルエンディアン、68000 の機械
（Atari ST、Amiga、Macintosh）で作ったものはビッグエンディアンで、PC の Linux カーネルは後者を
読めません。

## ビルド

    make

Makefile は GNU make でも BSD make でも動きます。コードは C99 と POSIX.1-2008 のインターフェース
だけを使い、ライブラリは要りません。

`mount_minixfs` は FUSE のライブラリが要るので、求めたときだけビルドします。

    make fuse                                           # libfuse 3
    make fuse FUSE_LIBS="-lrefuse -lpuffs"              # NetBSD
    make fuse FUSE_LIBS="-lrefuse -lpuffs" FUSE_VERSION=26   # FUSE 2

`FUSE_CFLAGS` と `FUSE_LIBS` の既定は `pkg-config fuse3` の出力です。ソースは FUSE の高水準 API
だけを使い、FUSE 3 の形（`FUSE_VERSION=31`、既定）か FUSE 2 の形（`FUSE_VERSION=26`、古い
librefuse 向け）でビルドできます。

## 使い方

    minixfs [-M SIZE:HEADS:SIDE] info IMAGE
    minixfs [-M ...] ls [-lR] IMAGE [PATH]
    minixfs [-M ...] cat IMAGE PATH
    minixfs [-M ...] extract [-dv] IMAGE DEST [PATH]
    minixfs [-M ...] tar IMAGE [PATH] > ARCHIVE

`info` は、superblock、イメージとファイルシステムの大きさ、イメージのうち 0xe5 か 0xa5 だけの
セクタがどれだけあるかを、その連なりの最も多い長さとともに表示します。これらの値はフォーマットが
残すもので、ほかには誰も書きません。書かれた面より多くの面で読み取ったディスクは、1 トラックずつの
連なりがイメージの半分を占める形で現れます。途中で切れた写しや、ディスクより小さいファイル
システムも現れます。どれもそれ自体は誤りではありません。

`ls -l` は、モード、リンク数、所有者、グループ、大きさ（デバイスならメジャー番号とマイナー番号）、
UTC の更新時刻を表示します。

`extract` はイメージからディレクトリのツリーを写します。アクセス許可（set-user-ID・set-group-ID・
sticky ビットを除く）と時刻を保ち、リンクが 2 つ以上あるファイルの 2 つ目以降の名前はリンクとして
作ります。FIFO は作り、デバイスは root が要る `-d` を付けたときだけ作ります。作らなかったデバイスと、
写せないソケットは、最後に警告として数を示し、`-v` を付ければ 1 件ずつ示します。`DEST` に既にある
名前は、tar(1) と同じく置き換えます。ディレクトリでなければ先に消すので、複数枚組のディスクを順に
1 つのツリーに展開できます。

`tar` は、代わりにツリーを POSIX の ustar アーカイブとして標準出力に書きます。デバイスと FIFO も
入れ、ハードリンクはリンクのまま、ustar に収まらない長い名前は pax のヘッダに入れます。tar(1) は
特権なしでこれを一覧でき、root で実行すればデバイスも作ります。ソケットは入れられません。

`extract` は `DEST` の外には書きません。名前が空の項目、`.`・`..`、`/` を含む項目は拒み、
ディレクトリのループも拒みます。

`-M SIZE:HEADS:SIDE` は、`minixfs`、`fsck_minixfs`、`tunefs_minixfs`、`mount_minixfs` の
どれも受け付けます。ディスクの片面のトラックだけにファイルシステムがあるイメージを読みます。
トラックは SIZE バイトで、1 シリンダに HEADS 個あり、そのうち面 SIDE（0 から数える）に
ファイルシステムがあります。片面 360K のディスクを両面 720K として読んだもの、たとえば Atari
MINIX 1.5 のディスクは `-M 4608:2:0` で、トラックは 1 つおきに空です。こうしたイメージでも
superblock は正しい位置にあるので、`info` は正しく見えますが、残りは壊れているように読めます。
`fsck_minixfs -y -M` は、そうしたイメージをその場で修復し、もう片方の面には手を付けません。

    mount_minixfs [-w [-u always|sync|seconds]] [-M SIZE:HEADS:SIDE]
        [FUSE options] IMAGE MOUNTPOINT

イメージをマウントします。`-w` がなければ読み出し専用です。inode 番号、モード、所有者、時刻、
デバイス番号はイメージのとおりです。`-w` を付けると、ファイル、ディレクトリ、リンクを、作り、消し、
名前を変え、変更できます。アクセス許可はカーネルが検査し、新しいファイルは呼び出し元のものに
なります。ただし、inode が呼び出し元のグループを持てないとき（V1 では 1 バイト）は、ディレクトリの
グループになります。書き込みでマウントしている間、イメージはほかの書き手に対してロックされ、clean の
印を外します。アンマウントで印を付け直します。clean の印のないイメージや flex ディレクトリのものは、
警告を出して読み出し専用でマウントします。`-u` はビットマップをいつイメージに書くかで、fsync と
アンマウントのとき（`sync`、既定）、変わるたび（`always`）、またはそれに加えて指定の秒数ごとです。
アンマウントは、Linux では `fusermount3 -u MOUNTPOINT`、BSD では `umount MOUNTPOINT` で行います。

イメージに書き込むコマンドは、開いている間イメージを fcntl(2) でロックし、ほかのコマンドが
ロックしているイメージは拒みます。

    newfs_minixfs -V version [-N] [-B le|be] [-b block-size]
        [-d directory [-F specfile [-P dbdir] [-x]] [-o uid:gid]]
        [-i inodes] [-l name-length] [-s blocks] [-t time]
        [-z log-zone-size] image

ファイルシステムを、空で、または `-d` でディレクトリの写しを入れて作ります。版は必ず指定します。
既定は、リトルエンディアン、V1 と V2 では 14 文字の名前、V3 では 4096 バイトのブロック
（MINIX 3 がマウントできる最小）、およそ 3 ブロックに 1 つの inode、1 ブロックの zone です。
大きさは `-s` で決めます。このときイメージファイルを作るか、その大きさに切り詰めます。`-s` が
なければ、既存のファイルの大きさを使います。`-t` はルートディレクトリの時刻で、毎回同じイメージを
作りたいときに使います。`-N` は配置を表示するだけで、何も書きません。先頭の 1024 バイトは
ブートブロックの場所なので、手を付けません。

`-d` を付けると、ディレクトリのファイル、ディレクトリ、シンボリックリンク、デバイス、FIFO を、
モード・所有者・時刻とともに、名前の順に入れます（シンボリックリンクのモードは、MINIX と Linux と
同じく 0777）。名前の順にするので、同じツリーからは同じイメージができます。ハードリンクはリンクの
まま、すべてゼロのブロックは穴のままにし、ルートディレクトリはそのディレクトリ自身のモード・
所有者・時刻を受け継ぎます。ソケットは警告を出して除きます。`-o uid:gid` はすべてのファイルに
その所有者とグループを与えます。V1 で 255 を超えるグループがあるときに要ります。

`-F` は、NetBSD の makefs の `-F` と同じく mtree(8) の指定を読みます。階層形式と、ビルドが書く
METALOG のようなフルパス形式の両方を読めます。項目は、指す対象の型、モード、所有者、グループ、
時刻、リンク先、デバイス番号を定め、ディレクトリの値を上書きします。ディレクトリにないものは、
項目のとおりに作ります（通常ファイルは空）。ただし `optional` の付いた項目は除きます。型が
ファイルと食い違えば誤りです。ユーザー名とグループ名は、`-P` のディレクトリの `master.passwd`
か `passwd` と `group` で、なければシステムのもので引きます。`-x` を付けると、指定にあるものだけを
入れます。チェックサム、大きさ、フラグは読み飛ばし、パターンを含む名前は扱いません。書き込む前に
すべてを確かめます。長すぎる名前、大きすぎる所有者とデバイス番号、容量（穴もデータとして数える）
です。`-s` もイメージファイルもなければ、その数で足りる大きさのイメージを、`-i` がなければツリーに
足りるだけの inode とともに作ります。

    fsck_minixfs [-lwy] [-e 0|1] [-M SIZE:HEADS:SIDE] image

ファイルシステムを検査します。superblock（最大ファイルサイズ、ファイルシステム全体を持つ
イメージか、Linux が記録した誤り）、ディレクトリ項目（inode 番号、名前、"." と ".."）、inode
（型、大きさ、zone 番号、二重に使われた zone、デバイスファイルの zone、シンボリックリンクの
中身）、リンク数、どのディレクトリも名前を持たない inode、2 つのビットマップです。問題ごとに
1 行と、最後に要約を表示します。`-y` がなければイメージは読むだけです。

`-y` を付けると、直せる問題はそれぞれ直します。悪い項目は消し、"." と ".." は正しい先に向けるか
戻し、悪い zone 番号と二度目に使われた zone は消し、大きさは切り詰め、リンク数は合わせ、どの
ディレクトリも名前を持たない inode は解放し、ビットマップを合わせます。そのあとでもう一度検査し、
clean の印を付けるか、問題が残っていれば誤りありの印を付けます。さらに `-l` を付けると、どの
ディレクトリも名前を持たない inode を、解放せずに、その下のツリーごと `/lost+found` に `#` と
番号の名前で入れます。`/lost+found` がなければ作ります。MINIX と Linux には `/lost+found` が
なく、その fsck はそうした inode を解放するので、`-l` は既定ではありません。

`-e 0` か `-e 1` を付けると、各ビットマップの最後の inode や zone より後ろのビットが 0 か 1 で
なければなりません。MINIX の mkfs は 0 のままにし、Linux の mkfs と `newfs_minixfs` は 1 に
するので、既定では検査しません。`-w` を付けると、MINIX 3 の fsck が警告するものも示します。
ビットマップが要る以上のブロック、inode 表から許されるより後ろの最初のデータ zone、MINIX が
計算するのと違う最大ファイルサイズ（Linux と `newfs_minixfs` は V2 と V3 に 2147483647 を書く）、
とても大きな zone です。これらの配置は動くので、問題ではありません。終了コードは、ファイル
システムに矛盾がなければ（`-y` なら修復後に）0、問題が残れば 1、そもそも検査できなければ 3 です。

clean の印のないファイルシステムは、そのことを示しますが、問題とはしません。V1 と V2 は Linux と
同じ場所に印を持ち、MINIX はその語を 0 のままにします。V3 は MINIX 3 のフラグに持ち、MINIX 3 は
clean でないファイルシステムを読み出し専用でマウントします。`newfs_minixfs` は新しいファイル
システムに clean の印を付けます。

    tunefs_minixfs [-fN] [-B le|be] [-c clean|dirty] [-e 0|1]
        [-l 14|30] [-m minix|linux|bytes]
        [-M SIZE:HEADS:SIDE] [-s blocks] image

ファイルシステムの設定を変えます。オプションがなければ、または `-N` を付ければ、設定を表示する
だけで何も書きません。`-N` はほかのオプションで何が変わるかを示します。

- `-B` は、ファイルシステムのすべての数値を、PC と同じリトルエンディアンか、Atari ST と Amiga と
  同じビッグエンディアンで書き直します。superblock、ビットマップの語、inode、間接 zone、
  ディレクトリの inode 番号です。
- `-l` は、V1 か V2 のファイルシステムの名前を 14 文字か 30 文字にします。マジック番号が変わり、
  すべてのディレクトリを新しい大きさの項目で書き直します。書き込む前にすべての名前を確かめ、
  14 文字に収まらない名前はすべて示して、何も変えません。大きくなるディレクトリが収まらない
  ときも同じです。
- `-s` は、ファイルシステムとイメージファイルを、そのブロック数に広げるか縮めます。広げるとき、
  zone マップに新しい zone の分の余地がなければブロックを足し、inode 表と使用中のデータ zone を
  すべて後ろへ移して、zone 番号をすべて合わせて書き換えます。縮めるとき、新しい末尾より後ろに
  ある使用中の zone をその前の空き zone へ移し、それを指す zone 番号を書き換えます。zone マップの
  ブロック数は変えないので、`fsck_minixfs -w` がそれを示します。使用中のものが収まらなければ、
  何も変えません。
- `-c` は、Linux と MINIX 3 が印を持つ場所に、clean か dirty の印を付けます。
- `-e` は、ビットマップの最後の inode や zone より後ろのビットを、MINIX の mkfs と同じ 0 か、
  Linux の mkfs と同じ 1 にします。
- `-m` は、superblock の最大ファイルサイズを、MINIX が計算する値、Linux と `newfs_minixfs` が書く
  値、または数値にします。

変更は、古い値と新しい値を並べて示します。superblock とビットマップより多くを書き換える変更は、
`fsck_minixfs` が通り、マウントしていないファイルシステムを前提とします。途中で止まると中途半端な
状態になるので、写しを取っておいてください。Linux と MINIX 3 は、書き込みでマウントしている間は
clean の印を外し、アンマウント時に自分の持つ内容で上書きします。そのため `-B`、`-l`、`-s` は、
clean の印のないファイルシステムを拒みます。`-f` を付ければ変更します。`fsck_minixfs -y` は矛盾の
ないファイルシステムに clean の印を付けるので、印の欄を使わない MINIX で作った V1 と V2 の
ファイルシステムにも、これで印が付きます。どれもマウント中のファイルシステムには使えません。
MINIX も Linux も、マウント中のファイルシステムを広げられません。

    dump_minixfs [-0123456789u] [-D dumpdates] [-L label]
        [-M SIZE:HEADS:SIDE] [-T date] -f file image [path]

ファイルシステムを、NetBSD の dump が UFS1 を書くのと同じ形式で、イメージのバイト順で書き出す
ので、NetBSD の restore で読めます。restore はルートを 2 とするので、inode n は inode n + 1 に
なります。レベル n の dump は、dumpdates ファイル（`-u`、`-D`）にある、それより低いレベルの
最後の dump の後に変わったもの、または `-T` の日時の後に変わったものを持ちます。path を与えると、
そのディレクトリとその下だけを、レベル 0 で書きます。Linux の restore はこれを読みません。
NetBSD が UFS1 について作る dump も同じで、その 64 ビットの日時を自分のレコード数として
受け取るためです。

    restore_minixfs -t [-c] -f file
    restore_minixfs -r [-cNv] [-M SIZE:HEADS:SIDE] [-o uid:gid]
        [-s symtable] -f file image

BSD の dump の形式の dump を読みます。`dump_minixfs` のもの、NetBSD と FreeBSD の UFS1 と UFS2
のもの、Linux の dump の ext2・ext3・ext4 のもの（0.4b49 からヘッダに書く連続の符号化も）、
4.2BSD と 4.3BSD のものを、4.4BSD・4.2BSD・V7 のどのディレクトリでも、どちらのバイト順でも
読みます。`-t` は名前を一覧します。`-r` は、全体の dump を空のファイルシステムに戻し、その後で
差分の dump を順に上に重ねます。dump の各 inode をどの inode にしたかは `-s`（既定は
`restoresymtable`）に記録します。dump の各ディレクトリは dump のとおりの中身になり、最後の名前が
なくなったファイルは解放します。名前と inode の数は、書き込む前に確かめます。パイプではなく
ファイルから読むなら、所有者、デバイス番号、大きさも確かめます。`-o` はすべてのファイルに 1 つの
所有者を与えるもので、収まらない所有者があるときに使います。拡張属性、ファイルフラグ、ソケットは
警告を出して除きます。Linux の圧縮した dump と、複数のボリュームにわたる dump は読みません。

ほかのコマンドは、成功すれば 0、何かに失敗すれば 1 で終了します。どのコマンドも、使い方の誤りなら
2 で終了します。

## マニュアル

マニュアルは、英語のものが `man/` に、日本語のものが `man/ja/` にあります。コマンドについては
minixfs(1)、mount_minixfs(8)、newfs_minixfs(8)、fsck_minixfs(8)、tunefs_minixfs(8)、
dump_minixfs(8)、restore_minixfs(8)、ファイルシステムの形式については minixfs(5) です。
`make install` はこれらを `MANDIR` の下に、コマンドを `PREFIX` の下に入れます。`make lint-man`
はマニュアルを検査します。

## テスト

    make check

テストが何を確かめるか、サニタイザ・ほかのシェル・JUnit の出力で流す方法は、`tests/README.md`
を参照してください。

## ファイルの構成

    src/mfs.h, src/mfs.c    ライブラリ: superblock、inode、zone の対応、
                            ファイルのデータ、ディレクトリ、パスの検索
    src/minixfs.c           minixfs コマンド
    src/mount_minixfs.c     FUSE ファイルシステム
    src/newfs_minixfs.c     newfs_minixfs コマンド
    src/fsck_minixfs.c      fsck_minixfs コマンド
    src/tunefs_minixfs.c    tunefs_minixfs コマンド
    src/dump_minixfs.c      dump_minixfs コマンド
    src/restore_minixfs.c   restore_minixfs コマンド
    src/dumpfmt.h, src/dumpfmt.c
                            BSD の dump の形式: ヘッダ、ビットマップ、
                            c_addr の連続の符号化、ディレクトリ項目
    src/tree.h, src/tree.c  newfs_minixfs -d: ディレクトリのツリーの写し
    src/spec.h, src/spec.c  newfs_minixfs -F: mtree の指定の読み取り
    src/mfs_format.c        ライブラリ: 新しいファイルシステムの配置と書き込み
    src/mfs_tune.c          ライブラリ: ファイルシステムのその場での変更
    src/mfs_write.c         ライブラリ: inode と zone の確保と解放、
                            ファイルの書き込みと大きさの変更、
                            ディレクトリ項目の追加
    src/mfs_ops.c           ライブラリ: ファイルとディレクトリの作成、
                            リンク、削除、名前の変更
    src/layout.h            ディスク上の配置
    src/compat.h            システムごとの違い
    tests/                  テスト。tests/README.md を参照

## 参考

ディスク上の形式は、MINIX のソースの定義（MINIX 2.0.4 の `fs/super.h`、`fs/inode.h`、
`fs/type.h`、`fs/const.h` と、MINIX 3 の `minix/fs/mfs`）に従っています。MINIX、Linux、ほかの
MINIX ファイルシステムの実装のコードは使っていません。

## 貢献

コードの決まりは `STYLE.md` を参照してください。

## ライセンス

BSD 2-Clause。`LICENSE` を参照してください。
