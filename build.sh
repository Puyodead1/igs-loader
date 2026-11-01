#!/bin/bash
# build.sh - build the loader core, the machine plugins and the game plugins
# (all 32-bit) into build/, and unpack the libraries Ubuntu 24.04 no longer
# ships (ncurses 5) into lib/.
set -e
cd "$(dirname "$0")"
mkdir -p build lib
[ core/font.h -nt core/gen_font.py ] || python3 core/gen_font.py core/font.h
CF="-m32 -O2 -Wall -fPIC -shared"
# the core interposes libc/SDL symbols, so it keeps default visibility
gcc $CF -o build/igs_core.so core/core.c -ldl -lpthread
for m in machines/*/; do
    n=$(basename "$m")
    gcc $CF -fvisibility=hidden -o "build/$n.so" "$m$n.c" -ldl -lpthread
done
# E2000 executables need /lib/libcasmcard.so by absolute path (see the stub)
gcc $CF -Wl,-soname,/lib/libcasmcard.so -o build/libcasmcard.so machines/e2000/libcasmcard.c
# Speed Rider links it as plain libcasmcard.so
gcc $CF -DCASMCARD_SONAME='"libcasmcard.so"' -Wl,-soname,libcasmcard.so -o build/libcasmcard-nocard.so \
    machines/e2000/libcasmcard.c
# Hero of Robots: its smart-card reader and card (preloaded, SONAME libcasmcard.so)
gcc $CF -fvisibility=hidden -Wl,-soname,libcasmcard.so -o build/libcasmcard-hero.so \
    games/heroofrobots/libcasmcard.c -ldl
for g in games/*/; do
    n=$(basename "$g")
    gcc $CF -fvisibility=hidden -o "build/$n.so" "$g$n.c"
done
# the rom-set helper (native 64-bit): software lists, rompath, .7z/.zip sets
LZ=third_party/lzma
gcc -O2 -Wall -Wno-format-truncation -D_FILE_OFFSET_BITS=64 -o build/igs-romset \
    romset/igs-romset.c romset/romset.c romset/softlist.c romset/zip.c romset/sevenzip.c romset/hash.c \
    $LZ/7zArcIn.c $LZ/7zBuf.c $LZ/7zCrc.c $LZ/7zCrcOpt.c $LZ/7zDec.c $LZ/7zStream.c $LZ/Bcj2.c \
    $LZ/Bra.c $LZ/Bra86.c $LZ/BraIA64.c $LZ/CpuArch.c $LZ/Delta.c $LZ/LzmaDec.c $LZ/Lzma2Dec.c
if [ ! -e lib/libncurses.so.5 ]; then
    for d in debs/*.deb; do
        dpkg -x "$d" /tmp/igs-debs
    done
    cp -a /tmp/igs-debs/lib/i386-linux-gnu/* lib/ 2>/dev/null || true
    cp -a /tmp/igs-debs/usr/lib/i386-linux-gnu/* lib/ 2>/dev/null || true
    rm -rf /tmp/igs-debs
fi
ls build
