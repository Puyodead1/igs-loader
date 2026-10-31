/*
 * Harpoon Lagoon (E2100) game plugin: its ASIC firmware's side of the
 * runtime channel.  docs/IGS_E2100_PROTECTION.md, docs/HarpoonLagoon.md.
 *
 *   - channel cipher: every word XORed with a 32-bit key per direction;
 *     the game announces its next index and key in message 5 (words 11/12).
 *   - ASIC -> game: operator settings (55, 10) once, then every batch the
 *     button bitmap (2), coin pop-ups (59), credits (6), player status (52).
 *   - game -> ASIC: 50 = a station starts a game (spend a credit),
 *     58 = game events (round over, ticket payout).
 *   - controls: 4 stations on the keyboard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"

#define RX_KEY_INIT 0x82675251u   /* ASIC -> game */
#define TX_KEY_INIT 0x80678267u   /* game -> ASIC */
#define STATIONS 6                /* message 6 has 6 coin/credit slots */
#define PLAYERS  4

static const struct e2100_api *api;
#define LOG(...) api->core->log("harpoonlagoon: " __VA_ARGS__)
#define VLOG(...) do { if (api->core->verbose) LOG(__VA_ARGS__); } while (0)

static struct {
    uint32_t rx_key, tx_key;
    uint32_t batches_tx;
    uint8_t keys[10];             /* 80-bit button bitmap (message 2) */
    uint32_t coins[STATIONS], credits[STATIONS], coin_events[STATIONS];
    int credits_dirty, status_dirty;
} g;

/*
 * Operator settings the real ASIC keeps in NVRAM.  Message 55 -> 0x8405A00:
 * word 10 is a divisor and must not be 0.  Message 10 -> 0x8406620: word 17
 * is Attract Sound (>3 = always).  Defaults, not dumped values.
 *
 * The US-WA builds (manifest VARIANT=wa) keep message 10 at 0x8406BA0 with
 * another layout: word 17 is the language (0..3), which also picks the cell
 * bank of the ticket panel, so 4 runs off the end of it at start-up.
 */
static const uint32_t settings55[15] = {
    [0] = 10, [7] = 1, [8] = 1, [9] = 1, [10] = 1,
    [11] = 1, [12] = 1, [13] = 1, [14] = 1,
};
static uint32_t settings10[50] = { [17] = 4 };

static void reset(struct e2100_channel *ch)
{
    const char *variant = getenv("IGS_VARIANT");

    (void)ch;
    if (variant && !strcmp(variant, "wa"))
        settings10[17] = 0;
    g.rx_key = RX_KEY_INIT;
    g.tx_key = TX_KEY_INIT;
    g.batches_tx = 0;
}

static void decrypt_from_game(uint32_t *w, uint32_t n)
{
    while (n--)
        *w++ ^= g.tx_key;
}

static void encrypt_to_game(uint32_t *w, uint32_t n)
{
    while (n--)
        *w++ ^= g.rx_key;
}

static void message(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *p)
{
    if (type != 5 && type != 64)
        VLOG("rx msg type=%u len=%u w0=0x%08x", type, len, len ? p[0] : 0);
    /* type 5: words 11/12 announce the next header index and key */
    if (type == 5 && len >= 13) {
        ch->tx_idx = p[11] & 0x3f;
        g.tx_key = p[12];
    }
    /* type 50: station p starts a game -> spend a credit */
    if (type == 50 && len >= 1 && p[0] < STATIONS) {
        if (g.credits[p[0]])
            g.credits[p[0]]--;
        g.credits_dirty = g.status_dirty = 1;
        LOG("station %u starts a game, %u credits left", p[0] + 1, g.credits[p[0]]);
    }
    /* type 58: game events [station, action, a, b]: 1 round over, 3 tickets */
    if (type == 58 && len >= 4 && p[1] != 0)
        LOG("station %u event %u (%u, %u)", p[0] + 1, p[1], p[2], p[3]);
}

static void build(struct e2100_channel *ch, struct e2100_batch *b)
{
    uint32_t w[16], i;

    if (g.batches_tx++ == 0) {
        api->batch_add(b, 55, settings55, 15);
        api->batch_add(b, 10, settings10, 50);
        g.credits_dirty = g.status_dirty = 1;
    }
    /* message 2: the 80-bit button bitmap, every batch */
    memset(w, 0, sizeof(w));
    for (i = 0; i < 10; i++)
        w[i / 4] |= (uint32_t)g.keys[i] << (8 * (i % 4));
    api->batch_add(b, 2, w, 3);
    /* message 59: one coin pop-up per station per batch */
    for (i = 0; i < STATIONS; i++) {
        if (g.coin_events[i]) {
            g.coin_events[i]--;
            w[0] = i;
            api->batch_add(b, 59, w, 1);
        }
    }
    /* message 6: coins [0..5], credits [6..11], next header index [12] */
    if (g.credits_dirty) {
        for (i = 0; i < STATIONS; i++) {
            w[i] = g.coins[i];
            w[6 + i] = g.credits[i];
        }
        w[12] = ch->rx_idx;
        api->batch_add(b, 6, w, 13);
        g.credits_dirty = 0;
    }
    /* message 52: per player [amount, status (0 = normal), value] */
    if (g.status_dirty) {
        for (i = 0; i < PLAYERS; i++) {
            w[3 * i] = 0;
            w[3 * i + 1] = 0;
            w[3 * i + 2] = 1;
        }
        api->batch_add(b, 52, w, 3 * PLAYERS);
        g.status_dirty = 0;
    }
}

/*
 * SDL 1.2 key symbols -> button bits (station p: bits 8p+0..5 = up, down,
 * right, left, bet, fire; 78 = Test) or coin (0x80 | station).
 */
#define COIN 0x80
static const struct { int sym; uint8_t bit; } keymap[] = {
    { 273, 0 }, { 274, 1 }, { 275, 2 }, { 276, 3 },          /* arrows */
    { 308, 4 }, { 307, 4 }, { 306, 5 }, { 305, 5 },           /* Alt bet, Ctrl fire */
    { '5', COIN | 0 },
    { 'r', 8 }, { 'f', 9 }, { 'g', 10 }, { 'd', 11 }, { 's', 12 }, { 'a', 13 }, { '6', COIN | 1 },
    { 'i', 16 }, { 'k', 17 }, { 'l', 18 }, { 'j', 19 }, { 'v', 20 }, { 'b', 21 }, { '7', COIN | 2 },
    { 264, 24 }, { 258, 25 }, { 262, 26 }, { 260, 27 }, { 266, 28 }, { 256, 29 }, { '8', COIN | 3 },
    { 283, 78 },                                               /* F2 = Test */
};

static void key(int sym, int down)
{
    unsigned i;

    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        uint8_t bit = keymap[i].bit;

        if (keymap[i].sym != sym)
            continue;
        if (bit & COIN) {
            if (down) {
                unsigned st = bit & 7;

                /* coins to play = settings block 10, word 1, + 1 */
                g.coin_events[st]++;
                if (++g.coins[st] >= settings10[1] + 1) {
                    g.coins[st] = 0;
                    g.credits[st]++;
                }
                g.credits_dirty = 1;
                LOG("coin at station %u: %u credits", st + 1, g.credits[st]);
            }
        } else if (down) {
            g.keys[bit >> 3] |= 1 << (bit & 7);
        } else {
            g.keys[bit >> 3] &= ~(1 << (bit & 7));
        }
    }
}

static void status(char *buf, size_t n)
{
    snprintf(buf, n, "credits %u / %u / %u / %u",
             g.credits[0], g.credits[1], g.credits[2], g.credits[3]);
}

static const struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Harpoon Lagoon",
    .status = status,
    .reset = reset,
    .decrypt_from_game = decrypt_from_game,
    .encrypt_to_game = encrypt_to_game,
    .message = message,
    .build = build,
    .key = key,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    api = a;
    return &game;
}
