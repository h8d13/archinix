#!/bin/sh -e
# Interleaved A/B against another commit: builds that commit's libs in
# a worktree, compiles bench-ab.cc once per prefix (headers differ), and
# alternates runs so a noise burst cannot land on one side only. Prints
# the min per phase, which is what BASELINE quotes.
# Log: build/tmp/ab.log
# usage: bench/ab.sh <git-ref> [tree] [pairs]
#        tree default: sole *-arch-base in build/archstore
cd "$(dirname "$0")/.."
REPO=$PWD
REF=${1:?usage: bench/ab.sh <git-ref> [tree] [pairs]}
PAIRS=${3:-8}
mkdir -p build/tmp
LOG=$REPO/build/tmp/ab.log
: > "$LOG"

log() { echo "\$ $*" >> "$LOG"; "$@" >> "$LOG" 2>&1; }

TREE=$2
[ -n "$TREE" ] || {
	for g in "$REPO"/build/archstore/nix/store/*-arch-base; do
		[ -d "$g" ] || continue
		[ -z "$TREE" ] || { echo "several arch-base trees, pass one" >&2; exit 1; }
		TREE=$g
	done
}
[ -d "$TREE" ] || { echo "no tree to bench (run arch/bootstrap.sh or pass one)" >&2; exit 1; }
TREE=$(realpath "$TREE")

OLD=$REPO/build/ab-old
# reuse a worktree already built at that commit: repeat runs are the
# point of this harness
if [ "$(git -C "$OLD" rev-parse HEAD 2>/dev/null)" = "$(git rev-parse "$REF")" ] \
	&& [ -d "$OLD/build/prefix" ]; then
	echo "reusing $OLD at $REF"
else
	rm -rf "$OLD"
	log git worktree prune
	log git worktree add --detach "$OLD" "$REF"
	log sh -c "cd '$OLD' && ./build.sh"
fi

# one binary per prefix: the headers are the thing under test
for side in old new; do
	[ "$side" = old ] && P=$OLD/build/prefix || P=$REPO/build/prefix
	log env PKG_CONFIG_PATH=$P/lib/pkgconfig sh -c \
		"g++ -std=c++23 -O2 bench/bench-ab.cc -o build/tmp/bench-ab-$side \
		\$(pkg-config --cflags --libs nix-store nix-util)"
done

: > build/tmp/ab-results
i=0
while [ "$i" -lt "$PAIRS" ]; do
	for side in old new; do
		[ "$side" = old ] && P=$OLD/build/prefix || P=$REPO/build/prefix
		ROOT=$(mktemp -d "$REPO/build/tmp/ab-root.XXXXXX")
		echo "# pair $i $side" >> "$LOG"
		LD_LIBRARY_PATH=$P/lib "build/tmp/bench-ab-$side" "$ROOT" "$TREE" \
			| tee -a "$LOG" | sed "s/^/$side /" >> build/tmp/ab-results
		chmod -R u+w "$ROOT" && rm -rf "$ROOT"
	done
	i=$((i + 1))
done

# min and median: a burst that makes one whole run fast shows up as a
# min far under the median, and that is the case min alone misreads
echo "$PAIRS pairs, $TREE"
sort build/tmp/ab-results \
	| awk '{ v = $(NF-1); k = $1
		for (i = 2; i < NF - 1; i++) k = k " " $i
		n[k]++; val[k, n[k]] = v }
	     END { for (k in n) {
			for (i = 1; i <= n[k]; i++) { s[i] = val[k, i] }
			for (i = 2; i <= n[k]; i++) { x = s[i]; j = i - 1
				while (j > 0 && s[j] > x) { s[j+1] = s[j]; j-- }
				s[j+1] = x }
			printf "%-32s min %6.3f  median %6.3f\n", \
				k, s[1], s[int((n[k]+1)/2)] } }' \
	| sort
echo "full log: $LOG"
