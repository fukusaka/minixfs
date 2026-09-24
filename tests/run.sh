#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# run.sh - run every test script and report the ones that failed.
# Run it from the top of the source tree, usually through "make check".
#
#   TEST_SHELL  shell that runs the scripts (default: sh); it may carry
#               options, such as "bash --posix"
#   JUNIT_XML   if set, also write the results there in JUnit XML, for
#               CI systems that read that format

: "${TEST_SHELL:=sh}"

dir=${TMPDIR:-/tmp}/minixfs-run.$$
rm -rf "$dir"
mkdir "$dir" || exit 1
trap 'rm -rf "$dir"' 0
trap 'exit 1' 1 2 15

failed=
for t in tests/t_*.sh; do
	name=${t#tests/}
	name=${name%.sh}
	echo "# $t"
	# TEST_SHELL is split on purpose, to allow options.
	# shellcheck disable=SC2086
	$TEST_SHELL "$t" >"$dir/$name.tap" 2>&1
	echo $? >"$dir/$name.status"
	cat "$dir/$name.tap"
	if [ "$(cat "$dir/$name.status")" -ne 0 ]; then
		failed="$failed $t"
	fi
done

if [ -n "$JUNIT_XML" ]; then
	{
		echo '<?xml version="1.0" encoding="UTF-8"?>'
		echo '<testsuites name="minixfs">'
		for f in "$dir"/*.tap; do
			name=${f##*/}
			name=${name%.tap}
			awk -v suite="$name" \
			    -v status="$(cat "$dir/$name.status")" \
			    -f tests/tap2junit.awk "$f"
		done
		echo '</testsuites>'
	} >"$JUNIT_XML"
fi

if [ -n "$failed" ]; then
	echo "FAILED:$failed"
	exit 1
fi
echo "all tests passed"
