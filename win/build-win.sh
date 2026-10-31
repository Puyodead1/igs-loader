#!/bin/bash
# win/build-win.sh - build the Windows loader (run in WSL; needs the mingw-w64
# i686 posix toolchain): build/win/igs-loader.exe, the machine plugins and the
# game plugins as DLLs.  Nothing here is needed at run time but those files.
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
# the executable: no ASLR (ELF executables want 0x08048000 free), 16 MiB main stack
$CC $CF -Wno-format-truncation -o "$OUT/igs-loader.exe" \
    win/main.c win/elf.c win/libc.c win/libc_sys.c win/libc_thread.c win/sdl.c win/gl.c win/core_win.c $RS \
    -static -Wl,--large-address-aware -Wl,--disable-dynamicbase -Wl,--stack,16777216 \
    -lopengl32 -lgdi32 -lwinmm
# plugins: the Linux sources unchanged, with dlfcn.h / sys/mman.h from win/compat
PF="$CF -shared -static -Iwin/compat -include win/compat/igs_plugin.h"
$CC $PF -o "$OUT/e2100.dll" machines/e2100/e2100.c
for g in harpoonlagoon oceanparty scratch heroofrobots; do
    $CC $PF -o "$OUT/$g.dll" "games/$g/$g.c" || echo "($g.dll not built)"
done
ls -l "$OUT"
