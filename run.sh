#!/bin/bash
# run.sh [game] - run an IGS game's stock binary through the loader.
#
# game: a manifest in ../manifests/<game>.conf (default harpoonlagoon).  The
# manifest names the machine and game plugins, the data dir, the binary and
# the asset mask.  Env (see docs/IGS_LOADER.md):
#   IGS_FULLSCREEN=1   keep the game's fullscreen mode
#   IGS_FPS_CAP=n      frame-rate cap (default 60, 0 = off)
#   IGS_FPS=1          log the frame rate every 5 s
#   IGS_VERBOSE=1      log every ASIC message
#   IGS_TITLE=text     window title instead of NAME (0 = the game's own)
#   IGS_AUDIO_CHUNK=n  audio buffer in samples (default: the game's)
#   IGS_AUDIO_LOG=1    log the game's mixer calls
#   IGS_WRAP="cmd"     run the game under cmd (gdb, strace, ...)
D=$(cd "$(dirname "$0")" && pwd)
ID=${1:-harpoonlagoon}
MANIFEST="$D/../manifests/$ID.conf"
[ -f "$MANIFEST" ] || { echo "no manifest $MANIFEST"; exit 1; }
. "$MANIFEST"
export IGS_MACHINE="$D/build/$MACHINE.so" IGS_GAME="$D/build/$PLUGIN.so"
export IGS_ASSET_MASK="$ASSET_MASK"
# VARIANT: a build the plugin treats differently (e.g. harpoonlagoon "wa")
export IGS_VARIANT="$VARIANT"
# NAME: the window title (IGS_TITLE overrides it, IGS_TITLE=0 keeps the game's)
export IGS_NAME="$NAME"
cd "$DATA" || exit 1
# SDL 1.2 (sdl12-compat) on Wayland/EGL: its GLX path breaks on WSLg's
# Xwayland (GLXBadContextTag from the swap-interval request).  Games with
# SDL built in (E2000) only know x11 and dsp: their manifests set
# VIDEO_DRIVER / AUDIO_DRIVER.
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-${VIDEO_DRIVER:-wayland}}
# WSLg has a PulseAudio server; pin SDL to it
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-${AUDIO_DRIVER:-pulseaudio}}
export EGL_LOG_LEVEL=${EGL_LOG_LEVEL:-fatal} MESA_LOG_LEVEL=${MESA_LOG_LEVEL:-error}
export LD_LIBRARY_PATH="$D/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# ENV: per-game environment (NAME=value ...), e.g. Mesa driconf workarounds
for e in $ENV; do
    export "$e"
done
# LIBS: single libraries of the game's own (relative to DATA), e.g. GLEW,
# zzip.  Only these are exposed, so the game's bundled SDL never shadows
# the host's.
if [ -n "$LIBS" ]; then
    LIBDIR="${TMPDIR:-/tmp}/igs-libs-$ID"
    rm -rf "$LIBDIR" && mkdir -p "$LIBDIR"
    for l in $LIBS; do
        ln -s "$DATA/$l" "$LIBDIR/$(basename "$l")"
    done
    LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$LIBDIR"
fi
export LD_PRELOAD="$D/build/igs_core.so"
# extra libraries from the manifest (PRELOAD, names in build/)
for p in $PRELOAD; do
    LD_PRELOAD="$LD_PRELOAD:$D/build/$p"
done
# IGS_WRAP: a command to run the game under, e.g. "gdb -q --args"
exec $IGS_WRAP "./$BINARY"
