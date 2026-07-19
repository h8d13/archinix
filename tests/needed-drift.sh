#!/bin/sh -e
# Slim-libstore guard: the local-store extraction dropped curl (remote
# stores) and seccomp (build sandbox) with ~half the compiled code. A
# future upstream rebase that re-adds a source file can silently drag
# them back; compare the NEEDED sets so that growth fails loudly.
cd "$(dirname "$0")/.."
P=$PWD/build/prefix/lib

n=0 fail=0
ok() {	# ok <status> <name> <detail-on-fail>
	n=$((n + 1))
	if [ "$1" -eq 0 ]; then
		echo "ok $n - $2"
	else
		echo "not ok $n - $2: $3"
		fail=1
	fi
}

# version-independent: libfoo.so.1.2.3 -> libfoo
needed() {
	objdump -p "$1" | awk '/NEEDED/ { print $2 }' \
		| sed 's/\.so.*//' | sort | tr '\n' ' '
}

STORE_WANT="ld-linux-x86-64 libc libgcc_s libnixutil libsqlite3 libstdc++ "
UTIL_WANT="ld-linux-x86-64 libblake3 libboost_context libboost_iostreams libc libcrypto libgcc_s libm libstdc++ "

# r=0-or-1 dance keeps `sh -e` from aborting before ok() can report
got=$(needed "$P"/libnixstore.so.[0-9]*)
r=0; [ "$got" = "$STORE_WANT" ] || r=1
ok $r "libnixstore NEEDED set unchanged" "got: $got want: $STORE_WANT"

got=$(needed "$P"/libnixutil.so.[0-9]*)
r=0; [ "$got" = "$UTIL_WANT" ] || r=1
ok $r "libnixutil NEEDED set unchanged" "got: $got want: $UTIL_WANT"

echo "1..$n"
exit $fail
