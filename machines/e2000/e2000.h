/*
 * E2000 machine: interface for game plugins.
 *
 * The machine emulates what E2000 games share: the PLX SDK driver
 * (/dev/plx/Pci9030*) for the PCI9030 bridge in front of the protection CPU,
 * its transport, the /dev/roio BIOS fingerprint and the unused /dev/pccard0.
 * Three transports (docs/IGS_E2000.md):
 *   - E2000_IGS036: shared-RAM message words, BAR3 status registers,
 *     RC4-wrapped message keys (Gold Captain, Pirate Revenge);
 *   - E2000_IGS027A: RC4-encrypted packets in BAR2 behind a sequenced
 *     mailbox byte (Go Go Ball 2);
 *   - E2000_IGS036_MSG: shared-RAM header words and a stream of short
 *     sub-messages, one RC4 stream for both directions (Speed Rider).
 *
 * A game plugin supplies what the protection firmware does for its game:
 * the reply to each message or packet (inputs, coins, acks), input and its
 * overlay.  Every callback runs with the machine lock held.
 */
#ifndef E2000_H
#define E2000_H

#include "../../core/igs.h"

#define E2000_API_VERSION 1
#define E2000_MSG_MAX     2048             /* words per message, header included */

enum { E2000_IGS036, E2000_IGS027A, E2000_IGS036_MSG };

/*
 * IGS036 message transport: the chip's header words.  The first
 * E2000_MSG_HDR_RC4 go through the RC4 stream (BAR2 +20, +24, +28), the
 * rest are XOR the message key (+32 .. +60).
 */
#define E2000_MSG_HDR      11
#define E2000_MSG_HDR_RC4  3
#define E2000_SUB_MAX      12288           /* sub-message bytes per message, either way (the game's buffers) */

/*
 * IGS027A packet (32-bit words).  The machine decrypts words PKT_PR..
 * PKT_PR + 39 (160 bytes) and checks PKT_SUM; data run on from PKT_DATA.
 */
#define E2000_PKT_WORDS   2081             /* the game's buffer: 8324 bytes */
#define E2000_PKT_LEN     16               /* reply: data bytes from PKT_DATA */
#define E2000_PKT_SUM     17               /* sum of the 40 encrypted words */
#define E2000_PKT_PR      18               /* type, function, -, param 0 (data length), params 1.. */
#define E2000_PKT_RET     29               /* reply: result (PR + 11) */
#define E2000_PKT_RET2    30               /* reply: second result */
#define E2000_PKT_ERROR   31               /* reply: nonzero = "a27 error", the game exits */
#define E2000_PKT_DATA    33               /* bulk data (byte 132) */

/* machine services for game plugins */
struct e2000_api {
    int version;
    const struct igs_core *core;
    /*
     * IGS027A: restart a packet RC4 stream with a new key (from the next
     * packet on).  IGS036 message transport: restart the one stream once the
     * message being built is written (to_game ignored).
     */
    void (*rekey)(int to_game, const void *key, int len);
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
     * IGS036: one exchange.  req: the game's message, decrypted (req[0] =
     * word count, req[1] = key, payload from req[2]); NULL for the start-up
     * doorbell.  Fill reply[2..] and return the reply's word count (reply[0]).
     */
    uint32_t (*exchange)(const uint32_t *req, uint32_t nreq, uint32_t *reply);

    void (*key)(int sym, int down);        /* optional */
    void (*draw)(int width, int height);   /* optional overlay */
    void (*file_opened)(const char *path); /* optional: the game fopen'ed a file */
    void (*status)(char *buf, size_t n);   /* optional: window title status line */

    int transport;                         /* E2000_IGS036 (default) or E2000_IGS027A */
    /* IGS027A: RC4 keys of the packet streams, pkt_key_len bytes each */
    const char *pkt_key_to_game, *pkt_key_from_game;
    int pkt_key_len;
    /*
     * IGS027A: one packet.  port: the mailbox port nibble (0x20, 0x30; 0x50
     * is the start-up packet, sent in clear).  msg: E2000_PKT_WORDS words,
     * decrypted; write the reply over it and return its data length in
     * bytes (from E2000_PKT_DATA).
     */
    uint32_t (*packet)(int port, uint32_t *msg);

    /* IGS036 message transport: the stream's RC4 key, msg_key_len bytes */
    const char *msg_key;
    int msg_key_len;
    /*
     * IGS036 message transport: one exchange.  in: the game's sub-message
     * bytes (nin of them; NULL at power-on).  Fill hdr[E2000_MSG_HDR] and up
     * to max bytes of sub-messages into out; return their length (the
     * machine adds the ending 0).
     */
    uint32_t (*message)(const uint8_t *in, uint32_t nin, uint32_t *hdr, uint8_t *out, uint32_t max);
};

/* exported by a game plugin .so */
#define E2000_GAME_ENTRY "e2000_game_entry"
typedef const struct e2000_game *(*e2000_game_entry_fn)(const struct e2000_api *api);

#endif
