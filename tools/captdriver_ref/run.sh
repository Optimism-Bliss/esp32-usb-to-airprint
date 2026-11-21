#!/bin/bash
# Byte-for-byte comparison of our Hi-SCoA encoder with captdriver's (the
# encoder that has printed on real LBP2900 / LBP3010 printers), and of both
# decoders. Clones captdriver (GPL, used only as a test reference) into
# tools/captdriver_ref/build/ on the first run.
#   CAPTDRIVER=/path/to/captdriver tools/captdriver_ref/run.sh   # use a checkout
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build="$here/build"
mkdir -p "$build"
# ValdikSS's fork at the commit studied for docs/capt.md (same encoder as mounaiban/captdriver)
REPO=${CAPTDRIVER_REPO:-https://github.com/ValdikSS/captdriver.git}
REV=${CAPTDRIVER_REV:-892f320}
src=${CAPTDRIVER:-$build/captdriver}
if [ ! -d "$src/src" ]; then
    git clone -q "$REPO" "$src"
    git -C "$src" checkout -q "$REV"
fi
# hiscoa-decompress.c prints a trace on stderr; it needs <stdio.h> and std.h
cc -O2 -g -Wall -fsanitize=address,undefined -I"$root/components/capt/include" -I"$src/src" -I"$src/tests" \
   -w \
   "$here/compare_hiscoa.c" "$root/components/capt/hiscoa.c" \
   "$src/src/hiscoa-compress.c" "$src/src/hiscoa-common.c" "$src/tests/hiscoa-decompress.c" \
   -o "$build/compare_hiscoa"
"$build/compare_hiscoa" 2>/dev/null
