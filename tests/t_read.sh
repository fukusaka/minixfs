#!/bin/sh
# Read the tree of tests/tree.spec from V1 images, little- and big-endian,
# with 14- and 30-character names, and with one- and two-block zones.

. ./tests/lib.sh

# The names "ls -R" prints, in directory order.
cat >"$T/names" <<'EOF'
bin
bin/sh
bin/login
bin/sh2
etc
etc/empty
etc/one
etc/b1023
etc/b1024
etc/b1025
etc/direct
etc/direct1
etc/indirect
etc/dindirect
etc/sh.link
dev
dev/tty0
dev/fd0
dev/fifo
usr
usr/a
usr/a/b
usr/a/b/c
usr/a/b/c/deep
usr/big
usr/holes
usr/sparse
12345678901234
EOF

# The regular files of the tree.
files="bin/sh bin/login bin/sh2 etc/empty etc/one etc/b1023 etc/b1024
etc/b1025 etc/direct etc/direct1 etc/indirect etc/dindirect usr/a/b/c/deep
usr/big usr/holes usr/sparse 12345678901234"

# A name longer than V1 allows.
toolong=1234567890123456789012345678901

touch -t 200001010000 "$T/y2000"

# ls, run on the image of the current variant $v.
read_ls() {
	run "$MINIXFS" ls -R "$img"
	check_out "$v: ls -R lists the tree in directory order" "$T/names"

	run "$MINIXFS" ls "$img" /usr/a/b
	echo c >"$T/want"
	check_out "$v: ls of a subdirectory" "$T/want"

	run "$MINIXFS" ls -l "$img" /bin/login
	echo "-rwsr-xr-x   1     0     0      7675 1990-06-01 00:01" \
	    "/bin/login" >"$T/want"
	check_out "$v: ls -l of a set-uid file" "$T/want"

	run "$MINIXFS" ls -l "$img" /bin
	check_out_has "$v: ls -l counts the links of a file" \
	    '^-rwxr-xr-x   2     2     2     34782 .* sh$'
	check_out_has "$v: ls -l shows a hard link like its file" \
	    '^-rwxr-xr-x   2     2     2     34782 .* sh2$'

	run "$MINIXFS" ls -l "$img" /dev
	cat >"$T/want" <<'EOF'
crw--w----   1     0     0    4,   0 1990-06-01 00:14 tty0
brw-rw-rw-   1     0     0    2,   1 1990-06-01 00:15 fd0
prw-------   1     0     0         0 1990-06-01 00:16 fifo
EOF
	check_out "$v: ls -l of devices and a pipe" "$T/want"

	run "$MINIXFS" ls -l "$img" /etc/sh.link
	check_out_has "$v: ls -l shows the target of a link" \
	    '^lrwxrwxrwx .* /etc/sh.link -> \.\./bin/sh$'

	# /usr/a holds one directory, so it has three links.
	run "$MINIXFS" ls -l "$img" /usr
	check_out_has "$v: ls -l counts the links of a directory" \
	    '^drwx------   3     3     4 .* 1990-06-01 00:18 a$'
}

# cat, run on the image of the current variant $v.
read_cat() {
	# $files is a list of names, split on purpose.
	for f in $files; do
		run "$MINIXFS" cat "$img" "/$f"
		check_out "$v: cat /$f" "$exp/$f"
	done

	run "$MINIXFS" cat "$img" usr//a/./b/c/deep
	check_out "$v: cat with a relative path, // and ." \
	    "$exp/usr/a/b/c/deep"

	run "$MINIXFS" cat "$img" /usr/a/b/c/../c/deep
	check_out "$v: cat through .." "$exp/usr/a/b/c/deep"

	run "$MINIXFS" cat "$img" /usr
	check_err "$v: cat of a directory fails" "Is a directory"

	run "$MINIXFS" cat "$img" /dev/tty0
	check_err "$v: cat of a device fails" "not a regular file"

	run "$MINIXFS" cat "$img" /nonexistent
	check_err "$v: cat of a missing file fails" "No such file"

	run "$MINIXFS" cat "$img" /bin/sh/x
	check_err "$v: a file used as a directory fails" "Not a directory"

	run "$MINIXFS" cat "$img" "/$toolong"
	check_err "$v: a name longer than any entry fails" "too long"
}

# extract, run on the image of the current variant $v.
read_extract() {
	rm -rf "$T/x"
	run "$MINIXFS" extract "$img" "$T/x"
	check_note "$v: extract counts what it did" \
	    "^17 files, 7 directories, 1 symbolic links; 3 special"
	check_same_tree "$v: extract reproduces the tree" "$exp" "$T/x"
	check_mode "$v: extract drops set-uid" "$T/x/bin/login" -rwxr-xr-x
	check_mode "$v: extract keeps the mode of a file" "$T/x/etc/one" \
	    -rw-------
	check_mode "$v: extract keeps the mode of a directory" "$T/x/usr/a" \
	    drwx------
	check_older "$v: extract sets the time of a file" "$T/x/bin/sh" \
	    "$T/y2000"
	check_older "$v: extract sets the time of a large file" \
	    "$T/x/etc/indirect" "$T/y2000"
	check_older "$v: extract sets the time of a directory" "$T/x/usr/a" \
	    "$T/y2000"
	check_link "$v: extract makes the symbolic link" "$T/x/etc/sh.link" \
	    ../bin/sh

	rm -rf "$T/x"
	run "$MINIXFS" extract "$img" "$T/x" /usr/a
	check_status "$v: extract of a subdirectory succeeds" 0
	check_same_tree "$v: extract of a subdirectory" "$exp/usr/a" "$T/x"

	run "$MINIXFS" extract "$img" "$T/x" /bin/sh
	check_err "$v: extract of a file fails" "Not a directory"
}

# format: version/name length/block size
for format in 1/14/1024 1/30/1024; do
for order in le be; do
for logzone in 0 1; do
	version=${format%%/*}
	namelen=${format#*/}
	namelen=${namelen%/*}
	bsize=${format##*/}
	fs="namelen=$namelen"
	# MINIX uses 14-character names; the 30-character form is the Linux
	# extension, with a magic number of its own.
	case $format in
	1/14/*)	magic=0x137f ;;
	*)	magic=0x138f ;;
	esac
	if [ "$order" = be ]; then
		byteorder=big-endian
	else
		byteorder=little-endian
	fi

	v="V$format/$order/$logzone"
	img="$T/img"
	exp="$T/expect"
	rm -rf "$exp"
	sed -e "s/@FS@/$fs/" -e "s/@ORDER@/$order/" \
	    -e "s/@BLOCKS@/$((4194304 / bsize))/" \
	    -e "s/@LOGZONE@/$logzone/" tests/tree.spec >"$T/spec"
	mkimage "$T/spec" "$img" "$exp"

	check_info "$v: info shows the version" "$img" version "$version"
	check_info "$v: info shows the byte order" "$img" "byte order" \
	    "$byteorder"
	check_info "$v: info shows the magic number" "$img" magic "$magic"
	check_info "$v: info shows the name length" "$img" "name length" \
	    "$namelen"
	check_info "$v: info shows the block size" "$img" "block size" \
	    "$bsize"
	check_info "$v: info shows the zone size" "$img" "log zone size" \
	    "$logzone"

	read_ls
	read_cat
	read_extract
done
done
done

# Names that fill the whole entry have no terminating NUL.
name=123456789012345678901234567890
v="30-character names"
cat >"$T/spec" <<EOF
fs namelen=30 order=be blocks=256 inodes=16
file /$name 0644 0 0 0 5 1
file /abcdefghijklmn 0644 0 0 0 5 2
EOF
mkimage "$T/spec" "$T/img" "$T/full"
run "$MINIXFS" ls "$T/img"
printf '%s\n' "$name" abcdefghijklmn >"$T/want"
check_out "$v: a name that fills its entry is listed" "$T/want"
run "$MINIXFS" cat "$T/img" "/$name"
check_out "$v: a name that fills its entry is found" "$T/full/$name"

finish
