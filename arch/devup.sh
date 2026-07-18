#!/bin/sh -e
# In-place nixgen tool refresh on a running box: git pull, run this.
# Lands in the overlay upper (RAM): nixgen-commit to keep.
cd "$(dirname "$0")"
install -m755 nixgen/nixgen-* /usr/local/bin/
