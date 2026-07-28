#!/bin/sh -e
# In-place nixgen tool refresh on a running box: git pull, run this.
# Lands in the overlay upper (RAM): nixgen-commit to keep.
cd "$(dirname "$0")"
# nixgen-fs is a sourced table, not a command: it belongs in lib, and a
# stale copy in bin would show up as an undocumented command
rm -f /usr/local/bin/nixgen-fs
install -Dm644 nixgen/nixgen-fs /usr/local/lib/nixgen-fs
for t in nixgen/nixgen-*; do
	case ${t#nixgen/} in
	nixgen-fs) ;;
	# lib pieces stay out of the drift-checked command surface
	nixgen-entry|nixgen-seedstate|nixgen-statepaths)
		install -m755 "$t" /usr/local/lib/ ;;
	nixgen-data-generator)
		install -m755 "$t" /etc/systemd/system-generators/ ;;
	*)
		install -m755 "$t" /usr/local/bin/ ;;
	esac
done
