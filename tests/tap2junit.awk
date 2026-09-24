# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Shoichi Fukusaka
#
# tap2junit.awk - turn the TAP output of one test script into a JUnit
# <testsuite> element.
#
#	awk -v suite=NAME -v status=STATUS -f tap2junit.awk FILE.tap
#
# STATUS is the exit status of the script; a script that failed without
# failing a check (a "Bail out!" or a crash) becomes one failed test case.
# Only POSIX awk is used.

function esc(s) {
	gsub(/&/, "\\&amp;", s)
	gsub(/</, "\\&lt;", s)
	gsub(/>/, "\\&gt;", s)
	gsub(/"/, "\\&quot;", s)
	return s
}

/^ok [0-9]+ - / || /^not ok [0-9]+ - / {
	n++
	line = $0
	if (line ~ /^not ok/) {
		kind[n] = "fail"
		sub(/^not ok [0-9]+ - /, "", line)
	} else {
		kind[n] = "pass"
		sub(/^ok [0-9]+ - /, "", line)
	}
	if (line ~ / # SKIP /) {
		kind[n] = "skip"
		detail[n] = line
		sub(/^.* # SKIP /, "", detail[n])
		sub(/ # SKIP .*$/, "", line)
	}
	title[n] = line
	next
}

/^#   / && n > 0 && kind[n] == "fail" {
	d = $0
	sub(/^#   /, "", d)
	detail[n] = detail[n] d "\n"
	next
}

/^Bail out!/ {
	bail = $0
}

END {
	failures = 0
	skips = 0
	for (i = 1; i <= n; i++) {
		if (kind[i] == "fail")
			failures++
		if (kind[i] == "skip")
			skips++
	}
	if (status != 0 && failures == 0) {
		n++
		kind[n] = "fail"
		title[n] = "script finished"
		detail[n] = "exit status " status
		if (bail != "")
			detail[n] = detail[n] ": " bail
		failures++
	}

	printf "  <testsuite name=\"%s\" tests=\"%d\"", esc(suite), n
	printf " failures=\"%d\" skipped=\"%d\">\n", failures, skips
	for (i = 1; i <= n; i++) {
		printf "    <testcase classname=\"%s\"", esc(suite)
		printf " name=\"%s\"", esc(title[i])
		if (kind[i] == "pass") {
			print "/>"
			continue
		}
		print ">"
		if (kind[i] == "skip") {
			printf "      <skipped message=\"%s\"/>\n", \
			    esc(detail[i])
		} else {
			first = detail[i]
			sub(/\n.*$/, "", first)
			printf "      <failure message=\"%s\">", esc(first)
			printf "%s</failure>\n", esc(detail[i])
		}
		print "    </testcase>"
	}
	print "  </testsuite>"
}
