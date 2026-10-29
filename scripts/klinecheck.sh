#!/bin/sh
# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Check that the embedded line blob resolves every kernel text address to the
# source file and line llvm-symbolizer reads from the kernel's DWARF.
#
#   klinecheck.sh <mkline> <blob> <kernel.elf>
#
# This holds the table the kernel actually carries, decoded the way the kernel
# decodes it, against an outside reader of the same DWARF, across every address
# a text symbol names rather than one chosen function. mkline --query resolves
# the blob; llvm-symbolizer prints the function, then file:line:column, then a
# blank line, and expands an inlined call innermost first, so the first of
# those lines is the statement the line program records and the one the blob
# encodes. The comparison is on the base name and the line, since the blob
# keeps base names and no column.

set -u

mkline="$1"
blob="$2"
kernel="$3"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

llvm-nm "$kernel" | awk '$2 ~ /[Tt]/ { print "0x" $1 }' | sort -u > "$work/addr"
if [ ! -s "$work/addr" ]; then
    echo "klinecheck: $kernel has no text symbols"
    exit 1
fi

"$mkline" --query "$blob" < "$work/addr" > "$work/blob"

# The first file:line:column of each blank-line-separated block, reduced to
# base name and line.
llvm-symbolizer --obj="$kernel" < "$work/addr" | awk '
/^$/            { have = 0; next }
have            { next }
/:[0-9]+:[0-9]+$/ || /^\?\?/ {
    s = $0
    sub(/:[0-9]+$/, "", s)
    n = split(s, p, "/")
    print p[n]
    have = 1
}
' > "$work/want"

n=$(wc -l < "$work/addr")
if [ "$(wc -l < "$work/blob")" != "$n" ] || [ "$(wc -l < "$work/want")" != "$n" ]; then
    echo "klinecheck: resolved $(wc -l < "$work/blob") and read $(wc -l < "$work/want") for $n addresses"
    exit 1
fi

paste -d'|' "$work/addr" "$work/blob" "$work/want" > "$work/cmp"
bad=$(awk -F'|' '$2 != $3' "$work/cmp")
if [ -n "$bad" ]; then
    echo "klinecheck: the embedded table disagrees with the DWARF:"
    printf '%s\n' "$bad" | head -10 | sed 's/^/  /'
    echo "  ($(printf '%s\n' "$bad" | wc -l) of $n addresses)"
    exit 1
fi

echo "klinecheck: $n addresses resolve to the line llvm-symbolizer reads"
