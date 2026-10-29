#!/bin/sh
# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Check that nycsym names an address with the source line DWARF records for
# it, by agreeing with llvm-symbolizer on a stable kernel function.
#
#   symcheck.sh <nycsym> <kernel.elf>
#
# The address is read from the symbol table rather than hard-coded, and the
# expected file and line come from llvm-symbolizer, so the check follows the
# source as it moves. It compares the file's base name and the line, which
# llvm-symbolizer prints as an absolute path with a column nycsym does not.

set -u

nycsym="$1"
kernel="$2"
func="${SYMCHECK_FUNC:-panic}"

addr=$(llvm-nm "$kernel" | awk -v n="$func" '$3==n { print "0x" $1 }')
if [ -z "$addr" ]; then
    echo "symcheck: $kernel has no $func symbol"
    exit 1
fi

# llvm-symbolizer prints the function on one line and file:line:column on the
# next. Drop the column, then reduce to base name and line.
want=$(llvm-symbolizer --obj="$kernel" "$addr" | sed -n '2p' | sed 's/:[0-9]*$//')
case "$want" in
/*) ;;
*)  echo "symcheck: llvm-symbolizer gave no source line for $func at $addr"
    exit 1 ;;
esac
want=$(basename "$want")

# nycsym prints "ADDR name+off file:line"; the last field is the source line.
got=$("$nycsym" "$kernel" "$addr" | awk '{ print $NF }')
case "$got" in
*.*:[0-9]*) ;;
*)  echo "symcheck: nycsym gave no source line for $func at $addr: '$got'"
    exit 1 ;;
esac
got=$(basename "$got")

if [ "$want" != "$got" ]; then
    echo "symcheck: $func at $addr: nycsym says $got, llvm-symbolizer says $want"
    exit 1
fi

echo "symcheck: $func at $addr -> $got"
