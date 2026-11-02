/*
 * E1000 machine: interface for game plugins.
 *
 * The machine emulates the board: an AMD Geode GX2 (its 2D engine, display
 * controller and frame buffer behind /dev/mem, PCI configuration and VSA
 * registers through port I/O, MSRs through /dev/cpu/0/msr) and the PC card
 * with the IGS027A ("a27") behind /dev/pccard0.  On E1000 the a27 runs the
 * game: each frame it sends the executable a list of commands (screens to
 * set up and run, sounds, scores) and the executable answers with a few
 * words.  A game plugin is that firmware.  Every callback runs with the
 * machine lock held.  Protocol: docs/IGS_E1000.md.
 */
#ifndef E1000_H
#define E1000_H

#include "../../core/igs.h"

#define E1000_API_VERSION 1
#define E1000_DATA_MAX    0x3FC            /* command bytes per frame (the game asserts < 0x400) */

/* machine services for game plugins */
struct e1000_api {
    int version;
    const struct igs_core *core;
    /* the executable's memory (its own tables), or NULL outside its image */
    const void *(*game_ptr)(uint32_t va, uint32_t len);
    /*
     * A fast path for the executable's line-by-line blits from system memory:
     * the 2D engine's result for w x h pixels from src (pitch in bytes) to the
     * frame buffer offset dst (dpitch apart) with the raster mode rm and the current colour
     * key, and a jump written over one of the executable's functions (0 if
     * its first bytes are not expect[0..n)).
     */
    void (*bitmap)(const void *src, int pitch, uint32_t dst, int dpitch, int w, int h, uint32_t rm);
    int (*patch_jump)(uint32_t va, void *target, const uint8_t *expect, int n);
};

struct e1000_game {
    int version;
    const char *name;

    /* the a27 starts (the executable's start-up write, mode 29) */
    void (*reset)(void);
    /*
     * One frame.  in: the executable's last answer (its mode word and n
     * data bytes; n = 0 when it had nothing to say).  Fill out with the next
     * command list (16-bit words, the executable's layout) and return its
     * length in bytes, at most E1000_DATA_MAX.
     */
    int (*frame)(uint32_t mode, const uint8_t *in, int n, uint8_t *out);

    void (*key)(int sym, int down);        /* optional */
    void (*draw)(int width, int height);   /* optional overlay */
    void (*status)(char *buf, size_t n);   /* optional: window title status line */
};

/* exported by a game plugin .so */
#define E1000_GAME_ENTRY "e1000_game_entry"
typedef const struct e1000_game *(*e1000_game_entry_fn)(const struct e1000_api *api);

#endif
