#!/bin/bash
# win/build-win.sh - build the Windows loader (run in WSL; needs the mingw-w64
# i686 posix toolchain, gcc and g++): build/win/igs-loader.exe, the machine
# plugins, the game plugins and the preloaded libraries as DLLs, the card
# tool and the GUI (igs-loader-gui.exe).  Nothing here is needed at run time
# but those files.
set -e
cd "$(dirname "$0")/.."
OUT=build/win
CC=${CC:-i686-w64-mingw32-gcc-posix}
mkdir -p "$OUT"
[ core/font.h -nt core/gen_font.py ] || python3 core/gen_font.py core/font.h
CF="-O2 -g -Wall -std=gnu11 -fno-strict-aliasing -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32_WINNT=0x0A00"
# rom sets (software lists, .7z/.zip reading): romset/ and the LZMA SDK subset, compiled once
# into build/obj-win/ for the exe, the card tool and the GUI
LZ=third_party/lzma
OBJ=build/obj-win
mkdir -p "$OBJ"
# obj <compiler> <flags> <source>: $OBJ/<name>.o, compiled when the source is newer
obj() {
    local o=$OBJ/$(basename "${3%.*}").o
    [ "$o" -nt "$3" ] || $1 $2 -c -o "$o" "$3"
    echo "$o"
}
[ ! -e "$OBJ/romset.o" ] || [ "$OBJ/romset.o" -nt romset/romset.h ] || rm -f "$OBJ"/*.o
RS=
for s in romset/romset.c romset/softlist.c romset/zip.c romset/sevenzip.c romset/hash.c \
         $LZ/7zArcIn.c $LZ/7zBuf.c $LZ/7zCrc.c $LZ/7zCrcOpt.c $LZ/7zDec.c $LZ/7zStream.c $LZ/Bcj2.c \
         $LZ/Bra.c $LZ/Bra86.c $LZ/BraIA64.c $LZ/CpuArch.c $LZ/Delta.c $LZ/LzmaDec.c $LZ/Lzma2Dec.c; do
    RS="$RS $(obj $CC "$CF -Wno-format-truncation" $s)"
done
# the executable: no ASLR (ELF executables want 0x08048000 free), 16 MiB main stack,
# committed (ELF code has no stack probes: see libc_thread.c)
$CC $CF -Wno-format-truncation -o "$OUT/igs-loader.exe" \
    win/main.c win/elf.c win/libc.c win/libc_sys.c win/libc_thread.c win/sdl.c win/gl.c win/x11.c \
    win/net.c win/alsa.c win/core_win.c $RS \
    -static -Wl,--large-address-aware -Wl,--disable-dynamicbase -Xlinker --stack -Xlinker 16777216,16777216 \
    -lopengl32 -lgdi32 -lwinmm -lws2_32 -lsynchronization
# plugins: the Linux sources unchanged, with dlfcn.h / sys/mman.h from win/compat
PF="$CF -shared -static -Iwin/compat -include win/compat/igs_plugin.h"
for m in e2100 e2000 e1000 e2200 e3000; do
    $CC $PF -o "$OUT/$m.dll" "machines/$m/$m.c"
done
for g in harpoonlagoon fishhunter4 fishhunter6 fishhunterfinal6 oceanparty sangomario scratch heroofrobots goldcaptain piraterevenge gogoball2 speedrider pandafamily speeddriver2 wedanceonline2 percussionkids percussionmaster3 crazyshoot speeddriver3; do
    $CC $PF -o "$OUT/$g.dll" "games/$g/$g.c" $(cat "games/$g/sources" 2>/dev/null) || echo "($g.dll not built)"
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
# the rom-set builder, igs-mkromset.exe (mkromset/): its objects in $OBJ/mkr ($MKR_OBJ, for the GUI too)
. mkromset/sources.sh
MKR_OBJ=$(mkr_objs $CC "$CF -Wno-format-truncation" "$OBJ/mkr")
$CC $CF -Wno-format-truncation -Imkromset -Iromset -o "$OUT/igs-mkromset.exe" mkromset/igs-mkromset.c $MKR_OBJ $RS -static
# the game data tool, igs-assets.exe (assets/)
. assets/sources.sh
$CC $CF -Wno-format-truncation $IGSA_INC -o "$OUT/igs-assets.exe" assets/igs-assets.c $IGSA_SRC $MKR_OBJ $RS \
    -static -Wl,--large-address-aware
# the GUI, igs-loader-gui.exe: Dear ImGui on Win32 + OpenGL 3; 32-bit like the exe, so it links the
# same romset objects; libstdc++ static
CXX=${CXX:-i686-w64-mingw32-g++-posix}
IM=third_party/imgui
GF="-O2 -Wall -std=c++17 -D_WIN32_WINNT=0x0A00 -Iromset -I$IM -Ithird_party/stb"
# the set builder in-process (mkromset/)
[ -n "$MKR_OBJ" ] && GF="$GF -DIGS_GUI_MKROMSET -Imkromset"
GO=
for s in $IM/imgui.cpp $IM/imgui_draw.cpp $IM/imgui_tables.cpp $IM/imgui_widgets.cpp \
         $IM/backends/imgui_impl_win32.cpp $IM/backends/imgui_impl_opengl3.cpp $IM/misc/cpp/imgui_stdlib.cpp; do
    GO="$GO $(obj $CXX "$GF" $s)"
done
$CXX $GF -mwindows -o "$OUT/igs-loader-gui.exe" gui/app.cpp gui/task.cpp gui/ini.cpp gui/setbuild.cpp gui/sys_win.cpp gui/main_win.cpp \
    $GO $MKR_OBJ $RS -static -lopengl32 -lgdi32 -limm32 -ldwmapi -lole32 -luuid -lshell32
ls -l "$OUT"
