#!/bin/bash
# run.sh <id> - run an IGS game's stock binary through the loader.
#
# id: a software name from hash/<machine>.xml (run.sh --list shows them).
# The game's rom set is <id>/, <id>.7z or <id>.zip on the rompath
# (IGS_ROMPATH, or rompath= in igs-loader.ini, or roms/); archives are
# extracted once into ~/.cache/igs-loader/linux/<id> (IGS_CACHE / cachepath=).
#   run.sh --list           the games, and which sets are found
#   run.sh --verify <id>    check a set against the list's hashes
# Env (see docs/IGS_LOADER.md):
#   IGS_FULLSCREEN=1   keep the game's fullscreen mode
#   IGS_FPS_CAP=n      frame-rate cap (default 60, 0 = off)
#   IGS_FPS=1          log the frame rate every 5 s
#   IGS_VERBOSE=1      log every ASIC message
#   IGS_TITLE=text     window title instead of the description (0 = the game's own)
#   IGS_AUDIO_CHUNK=n  audio buffer in samples (default: the game's)
#   IGS_AUDIO_LOG=1    log the game's mixer calls
#   IGS_WRAP="cmd"     run the game under cmd (gdb, strace, ...)
#   IGS_INI=file       settings file instead of igs-loader.ini
# The ini's [<id>] section is exported for that game (the environment wins).
D=$(cd "$(dirname "$0")" && pwd)
ROMSET="$D/build/igs-romset"
[ -x "$ROMSET" ] || { echo "build first: $D/build.sh"; exit 1; }
case "$1" in
    --list|-l|"") exec "$ROMSET" --root "$D" --list ;;
    --verify) exec "$ROMSET" --root "$D" --verify "$2" ;;
esac
# the list entry's features as shell variables (ID NAME MACHINE SET PLUGIN
# DATA BINARY ...), with the set ready in $SET
VARS=$("$ROMSET" --root "$D" --env "$1") || exit 1
eval "$VARS"
# the ini's [<id>] section (the GUI's per-game settings), applied after the
# list's ENV below; variables already in the environment win
SECTION=$("$ROMSET" --root "$D" --section "$1") || exit 1
export IGS_MACHINE="$D/build/$MACHINE.so" IGS_GAME="$D/build/$PLUGIN.so"
export IGS_ASSET_MASK="$ASSET_MASK"
# VARIANT: a build the plugin treats differently (e.g. harpoonlagoon "wa")
export IGS_VARIANT="$VARIANT"
# NAME: the window title (IGS_TITLE overrides it, IGS_TITLE=0 keeps the game's)
export IGS_NAME="$NAME"
# SET: the cabinet's root, for the absolute paths in IGS_SET_DIRS (the list's env)
export IGS_SET="$SET"
# DATA: the game's working directory on the cabinet, inside the set
cd "$SET$DATA" || exit 1
# SDL 1.2 (sdl12-compat) on Wayland/EGL: its GLX path breaks on WSLg's
# Xwayland (GLXBadContextTag from the swap-interval request).  Games with
# SDL built in (E2000) only know x11 and dsp: their entries set
# video_driver / audio_driver.
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-${VIDEO_DRIVER:-wayland}}
# WSLg has a PulseAudio server; pin SDL to it
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-${AUDIO_DRIVER:-pulseaudio}}
export EGL_LOG_LEVEL=${EGL_LOG_LEVEL:-fatal} MESA_LOG_LEVEL=${MESA_LOG_LEVEL:-error}
export LD_LIBRARY_PATH="$D/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# ENV: per-game environment (NAME=value ...), e.g. Mesa driconf workarounds
for e in $ENV; do
    export "$e"
done
eval "$SECTION"
# LIBS: single libraries of the game's own (cabinet paths, or relative to
# DATA), e.g. GLEW, zzip.  Only these are exposed, so the game's bundled SDL
# never shadows the host's.
if [ -n "$LIBS" ]; then
    LIBDIR="${TMPDIR:-/tmp}/igs-libs-$ID"
    rm -rf "$LIBDIR" && mkdir -p "$LIBDIR"
    for l in $LIBS; do
        case "$l" in
            /*) src="$SET$l" ;;
            *)  src="$SET$DATA/$l" ;;
        esac
        ln -s "$src" "$LIBDIR/$(basename "$l")"
    done
    LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$LIBDIR"
fi
export LD_PRELOAD="$D/build/igs_core.so"
# extra libraries (PRELOAD, names in build/)
for p in $PRELOAD; do
    LD_PRELOAD="$LD_PRELOAD:$D/build/$p"
done
# BINARY: relative to DATA (the cabinet runs ./BINARY there) or a cabinet path
case "$BINARY" in
    /*) EXE="$SET$BINARY" ;;
    *)  EXE="./$BINARY" ;;
esac
# LIBC=uclibc (E1000): the executable wants /lib/ld-uClibc.so.0; run it with
# the host's dynamic linker and the uClibc names from build/uclibc
LDSO=
if [ "$LIBC" = uclibc ]; then
    LD_LIBRARY_PATH="$D/build/uclibc:$LD_LIBRARY_PATH"
    LDSO=/lib/ld-linux.so.2
fi
# IGS_WRAP: a command to run the game under, e.g. "gdb -q --args"
exec $IGS_WRAP $LDSO "$EXE"
