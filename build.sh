#!/bin/bash
# build.sh - build the loader core, the machine plugins and the game plugins
# (all 32-bit) into build/, the native tools (igs-romset, hero-nfc, and the
# GUI igs-loader-gui when libsdl2-dev is installed), and unpack the libraries
# Ubuntu 24.04 no longer ships (ncurses 5) into lib/.
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
# E1000 executables are linked against uClibc: libc.so.0 and friends on glibc
mkdir -p build/uclibc
gcc $CF -Wl,-soname,libc.so.0 -o build/uclibc/libc.so.0 core/uclibc.c -ldl
gcc $CF -DSHIM_ONLY -Wl,-soname,libm.so.0 -Wl,--no-as-needed -o build/uclibc/libm.so.0 core/uclibc.c -lm
gcc $CF -DSHIM_ONLY -Wl,-soname,libdl.so.0 -Wl,--no-as-needed -o build/uclibc/libdl.so.0 core/uclibc.c -ldl
for g in games/*/; do
    n=$(basename "$g")
    gcc $CF -fvisibility=hidden -o "build/$n.so" "$g$n.c"
done
# native 64-bit tools share the rom-set objects (software lists, rompath, .7z/.zip sets)
LZ=third_party/lzma
NF="-O2 -Wall -Wno-format-truncation -D_FILE_OFFSET_BITS=64"
mkdir -p build/obj
RS=
# obj <compiler> <flags> <source>: build/obj/<name>.o, compiled when the source is newer
obj() {
    local o=build/obj/$(basename "${3%.*}").o
    [ "$o" -nt "$3" ] || $1 $2 -c -o "$o" "$3"
    echo "$o"
}
# romset.h is in every one of them
[ ! -e build/obj/romset.o ] || [ build/obj/romset.o -nt romset/romset.h ] || rm -f build/obj/*.o
for s in romset/romset.c romset/softlist.c romset/zip.c romset/sevenzip.c romset/hash.c \
         $LZ/7zArcIn.c $LZ/7zBuf.c $LZ/7zCrc.c $LZ/7zCrcOpt.c $LZ/7zDec.c $LZ/7zStream.c $LZ/Bcj2.c \
         $LZ/Bra.c $LZ/Bra86.c $LZ/BraIA64.c $LZ/CpuArch.c $LZ/Delta.c $LZ/LzmaDec.c $LZ/Lzma2Dec.c; do
    RS="$RS $(obj gcc "$NF" $s)"
done
# the rom-set helper run.sh uses
gcc $NF -o build/igs-romset romset/igs-romset.c $RS
# the Hero of Robots NFC card tool (libpcsclite loaded at run time)
gcc $NF -Iromset -o build/hero-nfc games/heroofrobots/hero-nfc.c $RS -ldl
# the rom-set builder (mkromset/, the C port of romtool/mkromset.py): its own objects in
# build/obj/mkr ($MKR_OBJ, for anything else that links it), then the CLI
. mkromset/sources.sh
MKR_OBJ=$(mkr_objs gcc "$NF" build/obj/mkr)
gcc $NF -Imkromset -Iromset -o build/igs-mkromset mkromset/igs-mkromset.c $MKR_OBJ $RS -lpthread
# the GUI, igs-loader-gui (Dear ImGui on SDL2 + OpenGL 3): needs libsdl2-dev
if command -v sdl2-config >/dev/null; then
    IM=third_party/imgui
    GF="-O2 -Wall -std=c++17 -Iromset -I$IM -Ithird_party/stb $(sdl2-config --cflags)"
    # the set builder in-process (mkromset/)
    [ -n "$MKR_OBJ" ] && GF="$GF -DIGS_GUI_MKROMSET -Imkromset"
    GO=
    for s in $IM/imgui.cpp $IM/imgui_draw.cpp $IM/imgui_tables.cpp $IM/imgui_widgets.cpp \
             $IM/backends/imgui_impl_sdl2.cpp $IM/backends/imgui_impl_opengl3.cpp $IM/misc/cpp/imgui_stdlib.cpp; do
        GO="$GO $(obj g++ "$GF" $s)"
    done
    g++ $GF -o build/igs-loader-gui gui/app.cpp gui/task.cpp gui/ini.cpp gui/setbuild.cpp gui/sys_posix.cpp gui/main_sdl.cpp \
        $GO $MKR_OBJ $RS $(sdl2-config --libs) -lGL -lpthread
else
    echo "(igs-loader-gui not built: no sdl2-config; install libsdl2-dev)"
fi
if [ ! -e lib/libncurses.so.5 ]; then
    for d in debs/*.deb; do
        dpkg -x "$d" /tmp/igs-debs
    done
    cp -a /tmp/igs-debs/lib/i386-linux-gnu/* lib/ 2>/dev/null || true
    cp -a /tmp/igs-debs/usr/lib/i386-linux-gnu/* lib/ 2>/dev/null || true
    rm -rf /tmp/igs-debs
fi
ls build
