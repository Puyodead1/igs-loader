/*
 * E2000 machine: interface for game plugins.
 *
 * The machine emulates what E2000 games share: the PLX SDK driver
 * (/dev/plx/Pci9030*) for the PCI9030 bridge in front of the IGS036
 * protection CPU, the shared-RAM transport (BAR2 message words, BAR3 mailbox
 * registers, RC4-wrapped message keys), the /dev/roio BIOS fingerprint and
 * the unused /dev/pccard0.  See docs/IGS_E2000.md.
 *
 * A game plugin supplies what the IGS036 firmware does for its game: the
 * reply to each message (inputs, coins, acks), input and its overlay.
 * Every callback runs with the machine lock held.
 */
#ifndef E2000_H
#define E2000_H

#include "../../core/igs.h"

#define E2000_API_VERSION 1
#define E2000_MSG_MAX     2048             /* words per message, header included */

/* machine services for game plugins */
struct e2000_api {
    int version;
    const struct igs_core *core;
};

struct e2000_game {
    int version;
    const char *name;

    /* RC4 keys of the two message-key streams (sent with the trailing NUL) */
    const char *key_to_game;               /* IGS036 -> game, e.g. "PRIGS036" */
    const char *key_from_game;             /* game -> IGS036, e.g. "PR_LINUX" */
    /* BIOS strings the game's /dev/roio check expects: {address, text}, 0-ended */
    const struct e2000_bios { uint32_t addr; const char *text; } *bios;

    void (*reset)(void);
    /*
     * One exchange.  req: the game's message, decrypted (req[0] = word count,
     * req[1] = key, payload from req[2]); NULL for the start-up doorbell.
     * Fill reply[2..] and return the reply's word count (reply[0]).
     */
    uint32_t (*exchange)(const uint32_t *req, uint32_t nreq, uint32_t *reply);

    void (*key)(int sym, int down);        /* optional */
    void (*draw)(int width, int height);   /* optional overlay */
    void (*file_opened)(const char *path); /* optional: the game fopen'ed a file */
    void (*status)(char *buf, size_t n);   /* optional: window title status line */
};

/* exported by a game plugin .so */
#define E2000_GAME_ENTRY "e2000_game_entry"
typedef const struct e2000_game *(*e2000_game_entry_fn)(const struct e2000_api *api);

#endif
