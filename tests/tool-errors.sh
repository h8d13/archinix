#!/bin/sh -e
# Bad input must be an error, not a crash. Every tool wraps main in a
# handler so a short basename, an unreadable store or a full disk
# reports and exits 1; without it the exception escapes into
# std::terminate, which is SIGABRT (rc 134) and a core dump. That is
# user-facing: nixgen-remove shells out to rm-path, so a mistyped
# generation name used to dump core in the box.
# Pins, per tool: rc is 1 (never a signal), the message names the tool,
# and stdout stays clean so the callers that capture it (nixgen-commit
# reads import-dir's stdout) never see half a path.
# usage: tests/tool-errors.sh <tmpdir>
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/prefix

[ -n "$1" ] || { echo "usage: $0 <tmpdir>" >&2; exit 1; }
mkdir -p "$1"
ROOT=$(realpath "$1")

for t in import-dir rm-path export-path import-path store-paths; do
	[ -x "build/$t" ] || g++ -std=c++23 -O2 "arch/$t.cc" -o "build/$t" \
		$(PKG_CONFIG_PATH=$P/lib/pkgconfig pkg-config --cflags --libs nix-store nix-util)
done

N=0 FAIL=0
ok() {	# ok <cond-exit-status> <desc>
	N=$((N + 1))
	if [ "$1" = 0 ]; then echo "ok $N - $2"
	else echo "not ok $N - $2"; FAIL=1; fi
}

# `[ ... ]; ok $?` under sh -e aborts the run on the first failure,
# before ok can print anything. Everything here is a failure mode, so
# report them all instead of dying on the first one.
try() {	# try <desc> <cond...>
	# _d, not desc: check() below holds its own desc across these calls
	_d=$1
	shift
	if "$@"; then ok 0 "$_d"; else ok 1 "$_d"; fi
}

RUN="env LD_LIBRARY_PATH=$P/lib"

# a real store to aim the basename cases at, so the failure under test
# is the argument and not a missing store
mkdir -p "$ROOT/src"
echo payload > "$ROOT/src/f"
$RUN build/import-dir "$ROOT/store" gen "$ROOT/src" > /dev/null 2> "$ROOT/log-seed"

# check <tool> <desc> <argv...>: rc must be 1, stderr must name the
# tool, stdout must be empty. rc >= 128 means a signal killed it.
check() {
	tool=$1 desc=$2
	shift 2
	set +e
	$RUN "build/$tool" "$@" > "$ROOT/out-$tool" 2> "$ROOT/err-$tool"
	rc=$?
	set -e
	try "$tool: $desc does not die by signal (rc=$rc)" [ "$rc" -lt 128 ]
	try "$tool: $desc exits 1" [ "$rc" = 1 ]
	try "$tool: $desc reports with the tool name" \
		grep -q "^$tool: " "$ROOT/err-$tool"
	try "$tool: $desc leaves stdout empty" [ ! -s "$ROOT/out-$tool" ]
}

# too short to be a store path: parseStorePath throws before any work
check rm-path "short basename" "$ROOT/store" shortid
check export-path "short basename" "$ROOT/store" shortid

# store root that cannot be created: openStore throws
check store-paths "unopenable store" /proc/nonexistent-store
check import-dir "unopenable store" /proc/nonexistent-store gen "$ROOT/src"

# import-path reads the bundle from stdin; an unopenable store must
# fail the same way before it touches the stream
set +e
echo garbage | $RUN build/import-path /proc/nonexistent-store \
	> "$ROOT/out-import-path" 2> "$ROOT/err-import-path"
s=$?
set -e
try "import-path: unopenable store does not die by signal (rc=$s)" \
	[ "$s" -lt 128 ]
try "import-path: unopenable store exits 1" [ "$s" = 1 ]
try "import-path: unopenable store reports with the tool name" \
	grep -q "^import-path: " "$ROOT/err-import-path"

# a source tree that does not exist: the dump throws mid-stream, after
# the store is open, so this exercises the unwind path rather than the
# early argument checks
check import-dir "missing source tree" "$ROOT/store2" gen /nonexistent-tree

echo "1..$N"
exit $FAIL
