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
# the executable: no ASLR (ELF executables want 0x08048000 free), 16 MiB main stack
$CC $CF -o "$OUT/igs-loader.exe" \
    win/main.c win/elf.c win/libc.c win/libc_sys.c win/libc_thread.c win/sdl.c win/gl.c win/core_win.c \
    -static -Wl,--large-address-aware -Wl,--disable-dynamicbase -Wl,--stack,16777216 \
    -lopengl32 -lgdi32 -lwinmm
# plugins: the Linux sources unchanged, with dlfcn.h / sys/mman.h from win/compat
PF="$CF -shared -static -Iwin/compat -include win/compat/igs_plugin.h"
$CC $PF -o "$OUT/e2100.dll" machines/e2100/e2100.c
for g in harpoonlagoon oceanparty scratch heroofrobots; do
    $CC $PF -o "$OUT/$g.dll" "games/$g/$g.c" || echo "($g.dll not built)"
done
ls -l "$OUT"
