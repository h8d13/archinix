#!/bin/sh -e
# Every nixgen command that touches the store needs root: the store is
# 0700 root:root and the work mounts, imports or writes it. Without an
# early guard the failure surfaces wherever the first privileged call
# happens to be, which misreports: nixgen-diffid said "generation
# 'test-sw' not found" when the real problem was that a non-root user
# cannot read the store at all.
# Pins that each guarded command bails immediately, with rc 1 and a
# message naming itself, and that it does so before parsing arguments
# or sourcing /usr/local/lib/nixgen-fs (which does not exist on a build
# host) -- that ordering is what lets this run here instead of in a VM.
# Also pins that nixgen-help stays usable unprivileged.
# Host-only, rootless, no VM, instant.
# usage: arch/tests/root-guard.sh
cd "$(dirname "$0")/../.."

if [ "$(id -u)" = 0 ]; then
	echo "SKIP root-guard: must run as a normal user, not root"
	exit 0
fi

rc=0
T=$(mktemp -d "${TMPDIR:-/tmp}/root-guard.XXXXXX")
trap 'rm -rf "$T"' EXIT

# every command that reaches the store, with arguments plausible enough
# that a missing guard would carry on into real work
check() {	# check <script> <args...>
	name=$1
	shift
	set +e
	sh "arch/nixgen/$name" "$@" > "$T/out" 2> "$T/err"
	st=$?
	set -e
	# 1, not just non-zero: a signal death or a usage path would also
	# be non-zero and would mean the guard did not fire
	if [ "$st" != 1 ]; then
		echo "FAIL $name: rc=$st unprivileged (want 1)"
		rc=1
		return
	fi
	if ! grep -q "^$name: needs root\$" "$T/err"; then
		echo "FAIL $name: no root message, got: $(head -1 "$T/err")"
		rc=1
		return
	fi
	if [ -s "$T/out" ]; then
		echo "FAIL $name: wrote to stdout before bailing"
		rc=1
		return
	fi
	echo "OK   $name: refuses unprivileged"
}

check nixgen-commit some-name
check nixgen-update some-name
check nixgen-switch some-gen
check nixgen-remove some-gen
check nixgen-setup /dev/null some-name
check nixgen-data /dev/null
check nixgen-adopt some-gen
check nixgen-listid
check nixgen-diffid gen-a gen-b
check nixgen-savemeta /
check nixgen-restmeta /

# the doc command is not privileged: guarding it would make the box
# undocumentable for a non-root user
if PAGER=cat sh arch/nixgen/nixgen-help > "$T/help" 2> "$T/help-err"; then
	if grep -q "NIXGEN(7)" "$T/help"; then
		echo "OK   nixgen-help: works unprivileged"
	else
		echo "FAIL nixgen-help: ran but printed no page"
		rc=1
	fi
else
	echo "FAIL nixgen-help: refused unprivileged"
	cat "$T/help-err"
	rc=1
fi

[ "$rc" = 0 ] && echo PASS || echo FAIL
exit "$rc"
