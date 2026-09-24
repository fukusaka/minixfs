# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
# The tree used by t_read.sh.  @FS@ (the name length), @ORDER@, @BLOCKS@
# and @LOGZONE@ are filled in by the test.  File sizes sit on either side
# of the block, direct-zone and indirect-zone boundaries of a file system
# with 1024-byte zones; with 2048-byte zones they land elsewhere, which is
# also useful.
fs @FS@ order=@ORDER@ blocks=@BLOCKS@ inodes=64 logzone=@LOGZONE@
dir  /bin 0755 2 2 644198400
file /bin/sh 0755 2 2 644198400 34782 1
file /bin/login 04755 0 0 644198460 7675 2
hard /bin/sh2 /bin/sh
dir  /etc 0755 0 0 644198520
file /etc/empty 0644 0 0 644198580 0 3
file /etc/one 0600 0 0 644198640 1 4
file /etc/b1023 0644 0 0 644198700 1023 5
file /etc/b1024 0644 0 0 644198760 1024 6
file /etc/b1025 0644 0 0 644198820 1025 7
file /etc/direct 0644 0 0 644198880 7168 8
file /etc/direct1 0644 0 0 644198940 7169 9
file /etc/indirect 0644 0 0 644199000 531456 10
file /etc/dindirect 0644 0 0 644199060 531457 11
link /etc/sh.link ../bin/sh 0 0 644199120
dir  /dev 0755 0 0 644199180
dev  /dev/tty0 c 4 0 0620 0 0 644199240
dev  /dev/fd0 b 2 1 0666 0 0 644199300
fifo /dev/fifo 0600 0 0 644199360
dir  /usr 0755 3 4 644199420
dir  /usr/a 0700 3 4 644199480
dir  /usr/a/b 0755 3 4 644199540
dir  /usr/a/b/c 0755 3 4 644199600
file /usr/a/b/c/deep 0644 3 4 644199660 100 12
file /usr/big 0644 3 4 644199720 700000 13
file /usr/holes 0644 3 4 644199780 600000 14 0:20000 100000:450000
file /usr/sparse 0644 3 4 644199840 300000 15 0:300000
file /12345678901234 0644 0 0 644199900 14 16
