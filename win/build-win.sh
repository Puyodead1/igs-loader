#!/bin/bash
# win/build-win.sh - build the Windows loader (run in WSL; needs the mingw-w64
# i686 posix toolchain): build/win/igs-loader.exe, the machine plugins, the
# game plugins and the preloaded libraries as DLLs.  Nothing here is needed at
# run time but those files.
set -e
cd "$(dirname "$0")/.."
OUT=build/win
CC=${CC:-i686-w64-mingw32-gcc-posix}
mkdir -p "$OUT"
[ core/font.h -nt core/gen_font.py ] || python3 core/gen_font.py core/font.h
CF="-O2 -g -Wall -std=gnu11 -fno-strict-aliasing -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32_WINNT=0x0A00"
# rom sets (software lists, .7z/.zip reading): romset/ and the LZMA SDK subset
LZ=third_party/lzma
RS="romset/romset.c romset/softlist.c romset/zip.c romset/sevenzip.c romset/hash.c
    $LZ/7zArcIn.c $LZ/7zBuf.c $LZ/7zCrc.c $LZ/7zCrcOpt.c $LZ/7zDec.c $LZ/7zStream.c $LZ/Bcj2.c
    $LZ/Bra.c $LZ/Bra86.c $LZ/BraIA64.c $LZ/CpuArch.c $LZ/Delta.c $LZ/LzmaDec.c $LZ/Lzma2Dec.c"
# the executable: no ASLR (ELF executables want 0x08048000 free), 16 MiB main stack,
# committed (ELF code has no stack probes: see libc_thread.c)
$CC $CF -Wno-format-truncation -o "$OUT/igs-loader.exe" \
    win/main.c win/elf.c win/libc.c win/libc_sys.c win/libc_thread.c win/sdl.c win/gl.c win/x11.c \
    win/core_win.c $RS \
    -static -Wl,--large-address-aware -Wl,--disable-dynamicbase -Xlinker --stack -Xlinker 16777216,16777216 \
    -lopengl32 -lgdi32 -lwinmm
# plugins: the Linux sources unchanged, with dlfcn.h / sys/mman.h from win/compat
PF="$CF -shared -static -Iwin/compat -include win/compat/igs_plugin.h"
for m in e2100 e2000 e1000 e2200; do
    $CC $PF -o "$OUT/$m.dll" "machines/$m/$m.c"
done
for g in harpoonlagoon oceanparty scratch heroofrobots goldcaptain piraterevenge gogoball2 speedrider pandafamily speeddriver2 wedanceonline2 percussionkids percussionmaster3; do
    $CC $PF -o "$OUT/$g.dll" "games/$g/$g.c" || echo "($g.dll not built)"
done
# the preloaded libraries (feature "preload"): build/<name>.so on Linux, <name>.dll here
$CC $PF -o "$OUT/libcasmcard.dll" machines/e2000/libcasmcard.c
$CC $PF -DCASMCARD_SONAME='"libcasmcard.so"' -o "$OUT/libcasmcard-nocard.dll" machines/e2000/libcasmcard.c
$CC $PF -Wno-attributes -o "$OUT/libcasmcard-hero.dll" games/heroofrobots/libcasmcard.c
# the Hero of Robots NFC card tool (winscard.dll loaded at run time)
$CC $CF -Wno-format-truncation -Iromset -o "$OUT/hero-nfc.exe" games/heroofrobots/hero-nfc.c $RS -static -lshell32
# ... and its window (the manifest: common controls 6)
${WINDRES:-i686-w64-mingw32-windres} games/heroofrobots/hero-nfc-gui.rc -O coff -o build/hero-nfc-gui-res.o
$CC $CF -Wno-format-truncation -Wno-unused-function -Iromset -mwindows -o "$OUT/hero-nfc-gui.exe" \
    games/heroofrobots/hero-nfc-gui.c build/hero-nfc-gui-res.o $RS -static -lcomctl32 -lcomdlg32 -lshell32 -lgdi32
ls -l "$OUT"
