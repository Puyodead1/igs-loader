/*
 * E2100 machine: interface for game plugins.
 *
 * The machine emulates what every E2100 game shares: the /dev/pccard0
 * driver, the FPGA card (BARs, DMA, asset cipher), the ASIC boot handshake
 * and one of three runtime transports: batch framing ([checksum, payload,
 * headers], header register (payload << 7 | n), shared running checksum),
 * the register table, or plain request/response.  Also the PC's serial
 * ports, when the game plugin puts a peripheral on one.  See
 * wiki E2100 Protection.
 *
 * A game plugin supplies what differs per game's ASIC firmware: the channel
 * cipher, the message set, the controller logic, input and its overlay.
 * Every callback runs with the machine lock held.
 */
#ifndef E2100_H
#define E2100_H

#include "../../core/igs.h"

#define E2100_API_VERSION 1
#define E2100_BATCH_MAX   1024             /* payload words per ASIC batch */

struct e2100_batch {
    uint32_t payload[E2100_BATCH_MAX], hdrs[16];
    uint32_t npayload, nhdrs;
};

/* runtime channel state the plugin maintains */
struct e2100_channel {
    uint32_t rx_idx;                       /* BAR1 header register, ASIC -> game */
    uint32_t tx_idx;                       /* BAR1 header register, game -> ASIC */
};

/*
 * Register-table transport (Scratch and other early games, "PCSR"): instead
 * of batches, each side fills the 64 BAR1 header registers.  Word 0 is the
 * bitmask of words 1..25 present, word 32 of words 33..52; words 26 / 53 are
 * the low-byte sums of words 1..25 / 33..52; word 31 is the bitmask of words
 * whose value is a BAR0 byte offset of a data block.  No cipher.
 */
#define E2100_REG_WORDS 64

struct e2100_regs {
    uint32_t w[E2100_REG_WORDS];
    uint32_t block_top;                    /* next free BAR0 offset for replies */
};

/* machine services for game plugins */
struct e2100_api {
    int version;
    const struct igs_core *core;
    void (*batch_add)(struct e2100_batch *b, uint32_t type, const uint32_t *words, uint32_t len);
    /* register-table transport: an immediate word, or a data block in BAR0 */
    void (*reg_put)(struct e2100_regs *r, uint32_t word, uint32_t value);
    void (*reg_block)(struct e2100_regs *r, uint32_t word, const void *data, uint32_t len);
    /*
     * ring transport: queue one ASIC action (id, value, n data words); the
     * machine delivers them in order, one per event.  The game takes the
     * next action as the reply to a command it waits on.
     */
    void (*ring_action)(uint32_t id, uint32_t value, const uint32_t *data, uint32_t n);
    /* the FPGA asset cipher under the runtime mask, in place (len % 16 == 0) */
    void (*asset_encrypt)(void *buf, uint32_t len);
};

enum { E2100_TRANSPORT_BATCH, E2100_TRANSPORT_REGTABLE, E2100_TRANSPORT_REQRESP, E2100_TRANSPORT_RING };

/*
 * Ring transport (E3000 "Real36", Speed Driver 3 and its siblings): no
 * cipher on the registers.  Game -> ASIC: a 5-word slot in a ring of 12 in
 * BAR1 words 0..59 (command, a, n, -, cmd + a + n + a running counter),
 * then a notify; its n data words are in BAR0 at the game's input pointer
 * (words 0..0xBFFF).  ASIC -> game: a slot [id, -, n, value, -] in the read
 * side of the same words, then one event; its n data words at the output
 * pointer (words 0xC000..0x17FFF).  Each block is masked: word k of n ^=
 * ptr + n + 0x5CF9B5E9 + (n - k).  The machine keeps both pointers.
 */
#define E2100_RING_SLOTS 12
#define E2100_RING_KEY   0x5CFA37E9u

/*
 * Request/response transport (Hero of Robots and other Ogre-era games):
 * strictly one reply per game message, no unsolicited batches.  The game
 * puts the byte length in BAR1 word 0 and the bytes at BAR0 0, then
 * notifies; the ASIC answers the same way and raises one event.  Framing
 * and cipher above that are the game plugin's.
 */
#define E2100_REQRESP_MAX 0x2000           /* the games reject longer replies */

/*
 * A serial port with a peripheral on it (card dispenser, QR reader...).
 * The machine opens the path as a port; each write goes to the plugin,
 * whose answer the game then reads back (reads fail with EAGAIN when
 * nothing is pending; tcflush drops it).
 */
struct e2100_serial {
    const char *path;
    /* bytes the game wrote; put the answer in reply, return its length */
    uint32_t (*write)(const uint8_t *p, uint32_t n, uint8_t *reply, uint32_t max);
};

struct e2100_game {
    int version;
    const char *name;

    /* ASIC reset: start a new channel (keys, indices) */
    void (*reset)(struct e2100_channel *ch);
    /* channel cipher over a whole batch body, in place */
    void (*decrypt_from_game)(uint32_t *words, uint32_t n);
    void (*encrypt_to_game)(uint32_t *words, uint32_t n);
    /* one message of a game batch, after the checksum check */
    void (*message)(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *p);
    /* after all messages of a game batch (e.g. switch to an announced key) */
    void (*batch_done)(struct e2100_channel *ch);
    /* the ASIC's next batch to the game, every 16 ms */
    void (*build)(struct e2100_channel *ch, struct e2100_batch *b);

    void (*key)(int sym, int down);        /* optional */
    void (*draw)(int width, int height);   /* optional overlay */

    /*
     * E2100_TRANSPORT_REGTABLE games set these instead of the batch
     * callbacks (cipher, message, build):
     *   reg_cmd    one game word; block = its BAR0 data when word 31 marks
     *              it (avail bytes readable there), else NULL
     *   reg_build  fill the ASIC's reply with api->reg_put / reg_block
     */
    int transport;
    void (*reg_cmd)(uint32_t word, uint32_t value, const uint8_t *block, uint32_t avail);
    void (*reg_build)(struct e2100_regs *r);

    /*
     * E2100_TRANSPORT_REQRESP: the game's message (len bytes, as it put
     * them in BAR0) in, the ASIC's reply out (at most max bytes); returns
     * the reply length.
     */
    uint32_t (*exchange)(const uint8_t *req, uint32_t len, uint8_t *reply, uint32_t max);

    /*
     * E2100_TRANSPORT_RING: one game command (its data unmasked, n words);
     * answer with api->ring_action.  ring_tick runs every 16 ms (inputs,
     * unsolicited actions).
     */
    void (*ring_cmd)(uint32_t cmd, uint32_t a, const uint32_t *data, uint32_t n);
    void (*ring_tick)(void);

    /* optional: serial peripherals, ended by a NULL path */
    const struct e2100_serial *serial;

    /* optional: a short status line for the window title */
    void (*status)(char *buf, size_t n);

    /*
     * optional: the asset-cipher mask for a crypt request's 16-byte key
     * (E2200: one mask per chart key), or NULL for $IGS_ASSET_MASK
     */
    const uint8_t *(*asset_mask)(const uint8_t *key);

    /*
     * Batch transport: the body's layout.  E2100_LAYOUT_RUNNING (0):
     * [running checksum, payload, headers].  E2100_LAYOUT_TRAILING (Fish
     * Hunter 4, "e2k1" ShareRam): [payload, headers, sum of those], no
     * running sum; the cipher covers all of it.
     */
    int layout;
    /*
     * Boot handshake: E2100_HS_SESSION (0), the DONE word carries noise;
     * E2100_HS_NOSUM: the older P2A loader compares the DONE word with the
     * checksum of its upload, so it carries 0 (nothing was uploaded).
     */
    int handshake;

    /* optional: igs_machine.vfile, files and command output ("|cmd") the
       cabinet provides; called without the machine lock, from any fopen */
    const char *(*vfile)(const char *name);

    /*
     * optional: the cipher's CBC mode (ioctl 0x6614 on, 0x6615 off; Speed
     * Driver 4): 64 KiB chunk k >= 1 of a request is under the mask XORed
     * with cbc_delta[k - 1] (measured per game)
     */
    const uint8_t (*cbc_delta)[16];
    unsigned cbc_chunks;
};

enum { E2100_LAYOUT_RUNNING, E2100_LAYOUT_TRAILING };
/*
 * E2100_HS_ADV (Fish Hunter SKL2): the P2A LoaderAdv: after the session keys
 * the ASIC sends "ready" (0x55AA55AA) and "done" (0xAA55AA55, word 1 the
 * upload's sum), each also in plain in BAR1 words 0..7; the host answers
 * 0x5A5A5A5A and 0xFFFF.
 * E2100_HS_LADDER (Speed Driver 3): the plain LoaderAdv: after the reset the
 * host writes 0x08080808 and steps through ASIC commands in the mailbox,
 * unmasked (0xF0.. ready, 0xD0.. upload, 0xB0.. received, 0x90.. sum check,
 * word 1 the upload's IGScrc32); the ASIC goes straight to the check with sum
 * 0, which matches the host's sum of no upload, and the host ends 0x0C0C0C0C.
 */
enum { E2100_HS_SESSION, E2100_HS_NOSUM, E2100_HS_ADV, E2100_HS_LADDER };

/* exported by a game plugin .so */
#define E2100_GAME_ENTRY "e2100_game_entry"
typedef const struct e2100_game *(*e2100_game_entry_fn)(const struct e2100_api *api);

#endif
