#!/bin/sh
# cc.sh SRC OUT.o [flags]: ${ARC}gcc -c, except that the low word of a MUL64
# product is read from MMID (r58), not MLO (r57): on the Leapster, MUL64 leaves
# r57 alone (as the BIOS's own code shows), and GCC reads r57 ("mlo") only for
# that low word.
set -e
SRC=$1; OUT=$2; shift 2
"${ARC:-arc-elf32-}gcc" "$@" -S "$SRC" -o "$OUT.s"
sed -i -E 's/\b(r57|mlo)\b/mmid/g' "$OUT.s"
"${ARC:-arc-elf32-}gcc" "$@" -c "$OUT.s" -o "$OUT"
rm -f "$OUT.s"
