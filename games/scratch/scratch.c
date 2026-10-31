/*
 * Scratch m101cn (E2100) game plugin: its ASIC firmware's side of the
 * register-table transport ("PCSR", e2100.h).  docs/Scratch.md.
 *
 *   - ASIC -> game words: 2 inputs (80-bit map), 3 version, 5 credits,
 *     7 settings, 11 payout/tickets, 15 records + player states, 17 card deal,
 *     34 RAM-clear ack, 39/43/44/49-52 bookkeeping, 48 recover data.
 *   - game -> ASIC words: 4 clock, 6 settings save, 12 result report,
 *     16 cursor state, 21 card result, 33/35/36/38/40-42/47 requests.
 *   - The game retries every request until it is answered, so each request
 *     just schedules its answer for the next reply.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"

#define PLAYERS 4

static const struct e2100_api *api;
#define LOG(...) api->core->log("scratch: " __VA_ARGS__)
#define VLOG(...) do { if (api->core->verbose) LOG(__VA_ARGS__); } while (0)

/*
 * Operator settings (word 7, 28 words): [0..10] items, [11..18] password
 * digits, [19..27] IR codes.  Items: gamerate, coinrate, keyinout,
 * keyinrate, keyoutrate, paymode, ticketrate, soundvol, ?, ?, placemode.
 * Maxima 9 5 2 8 8 5 50 9 9 2 2 (table 0x81528C4); these defaults are
 * guesses, not dumped values.
 */
static uint32_t settings[28] = { 2, 0, 1, 4, 0, 0, 0, 4, 4, 0, 0 };

static struct {
    uint8_t keys[10];                  /* word 2: bit 8k+b = dword k bit b */
    uint32_t credit[PLAYERS];
    uint32_t coin_value;
    uint64_t want;                     /* bitmask of words to answer */
    uint8_t recover[64];               /* last cmd 12 report, for word 48 */
    int first;
    uint32_t cursor[12];               /* word 16: [p] 100 * TK + index, [4+p] view, [8+p] */
    int deal[PLAYERS];                 /* red pressed: deal a card if affordable */
    int busy[PLAYERS];                 /* replies left until a dealt card times out */
} g;

/* bet of a ticket kind: TK ids 1, 6, 11/12, 21/22, 31-33, 51-53 */
static uint32_t tk_cost(uint32_t tk)
{
    return tk <= 1 ? 1 : tk < 11 ? 5 : tk / 10 * 10;
}

static void log_block(const char *what, const uint8_t *block, uint32_t avail, uint32_t words)
{
    uint32_t v[16], i;
    char line[16 * 12], *p = line;

    if (!block || avail < words * 4 || words > 16)
        return;
    memcpy(v, block, words * 4);
    for (i = 0; i < words; i++)
        p += sprintf(p, " %u", v[i]);
    LOG("%s:%s", what, line);
}

#define WANT(w) (g.want |= 1ull << (w))
#define WANTED(w) (g.want >> (w) & 1)

static void reg_cmd(uint32_t word, uint32_t value, const uint8_t *block, uint32_t avail)
{
    switch (word) {
    case 4: case 45:                     /* clock, unused: every exchange */
        break;
    case 16:                             /* cursor state, every exchange */
        if (block && avail >= sizeof(g.cursor) && memcmp(g.cursor, block, sizeof(g.cursor))) {
            memcpy(g.cursor, block, sizeof(g.cursor));
            log_block("cursor", block, avail, 12);
        }
        break;
    case 21:                             /* card results: flag[4], RealWin[4], bonus[4], ?[4] */
        if (block && avail >= 64) {
            uint32_t v[16], p;

            memcpy(v, block, sizeof(v));
            for (p = 0; p < PLAYERS; p++) {
                if (v[p] != 1)
                    continue;
                g.busy[p] = 0;
                g.credit[p] += v[4 + p] + v[8 + p];
                LOG("player %u card over: win %u, bonus %u, credit %u", p + 1, v[4 + p], v[8 + p], g.credit[p]);
            }
        }
        break;
    case 6:                              /* settings save (112 bytes) */
        if (block && avail >= sizeof(settings)) {
            memcpy(settings, block, sizeof(settings));
            LOG("settings saved: %u %u %u %u %u %u %u %u %u %u %u",
                settings[0], settings[1], settings[2], settings[3], settings[4], settings[5],
                settings[6], settings[7], settings[8], settings[9], settings[10]);
        }
        break;
    case 12:                             /* result report: kept for recover */
        if (block && avail >= sizeof(g.recover))
            memcpy(g.recover, block, sizeof(g.recover));
        break;
    case 33: WANT(34); break;            /* RAM clear: confirm */
    case 35: WANT(3); break;             /* version */
    case 36: WANT(7); break;             /* settings */
    case 38: WANT(39); break;            /* bookkeeping */
    case 40: WANT(49); WANT(50); WANT(51); WANT(52); break;
    case 41: WANT(43); break;            /* win types for a level */
    case 42: WANT(44); break;            /* win odds for a level */
    case 47: WANT(48); break;            /* recover */
    default:
        VLOG("game word %u = 0x%x%s", word, value, block ? " (block)" : "");
        break;
    }
}

static void reg_build(struct e2100_regs *r)
{
    static const uint32_t zero[600];
    uint32_t w[24], i;

    if (!g.first) {
        g.first = 1;
        WANT(3); WANT(7); WANT(15);
    }
    /* word 2: inputs, every reply */
    for (i = 0; i < 10; i++)
        w[i] = g.keys[i];
    api->reg_block(r, 2, w, 10 * 4);
    /* word 5: credits [4 + p] */
    memset(w, 0, 8 * 4);
    for (i = 0; i < PLAYERS; i++)
        w[4 + i] = g.credit[i];
    api->reg_block(r, 5, w, 8 * 4);

    /* word 17: deal a card [p] flag, [4+p] can-win, [8+p] bet view, [12+p] catch id */
    for (i = 0; i < PLAYERS; i++) {
        uint32_t deal[16] = { 0 }, tk = g.cursor[i] / 100, cost = tk_cost(tk);

        if (g.busy[i])
            g.busy[i]--;
        if (!g.deal[i])
            continue;
        g.deal[i] = 0;
        if (g.busy[i])
            continue;                    /* a card is still being scratched */
        if (!tk || g.credit[i] < cost) {
            LOG("player %u: no deal (tk %u, cost %u, credit %u)", i + 1, tk, cost, g.credit[i]);
            continue;
        }
        g.credit[i] -= cost;
        g.busy[i] = 60 * 60;             /* about a minute of replies */
        deal[i] = 1;
        deal[4 + i] = rand() % 3 ? 0 : cost * (1 + rand() % 5);
        deal[8 + i] = g.cursor[4 + i];
        deal[12 + i] = g.cursor[i];
        api->reg_block(r, 17, deal, sizeof(deal));
        LOG("player %u: deal catch %u, can win %u, credit %u", i + 1, deal[12 + i], deal[4 + i], g.credit[i]);
        break;                           /* one deal per reply */
    }
    if (WANTED(3)) {              /* version: "I:" / "O:" text, fuses, FPGA 1.0 */
        static const char text[16] = "SCRATCH LOADER  ";
        uint32_t v[19] = { 0 };

        for (i = 0; i < 16; i++)
            v[i] = (uint8_t)text[i];
        v[17] = 1;
        api->reg_block(r, 3, v, sizeof(v));
    }
    if (WANTED(7))
        api->reg_block(r, 7, settings, sizeof(settings));
    if (WANTED(15))               /* records, all players idle */
        api->reg_block(r, 15, zero, 220 * 4);
    if (WANTED(34))
        api->reg_put(r, 34, 1);
    if (WANTED(39))
        api->reg_block(r, 39, zero, 598 * 4);
    if (WANTED(43))
        api->reg_block(r, 43, zero, 292);
    if (WANTED(44))
        api->reg_block(r, 44, zero, 676);
    if (WANTED(48))
        api->reg_block(r, 48, g.recover, sizeof(g.recover));
    for (i = 49; i <= 52; i++)
        if (WANTED(i))
            api->reg_block(r, i, zero, 88);
    g.want = 0;
}

/*
 * SDL 1.2 key symbols -> input bits.  Player p (bits +8p): up 0, down 1,
 * right 2, left 3, yellow 4, red 5, keyin 7; keyout 0x30 + 3p, scratch
 * left 0x48 + p, scratch right 0x42 + p.  Test is bit 0x4E.  COIN adds
 * credit for player 1.
 */
#define COIN 0xFF
static const struct { int sym; uint8_t bit; } keymap[] = {
    { 273, 0 }, { 274, 1 }, { 275, 2 }, { 276, 3 },          /* arrows */
    { 308, 4 }, { 307, 4 },                                   /* Alt: yellow */
    { 306, 5 }, { 305, 5 }, { ' ', 5 },                       /* Ctrl, Space: red */
    { 'z', 0x48 }, { 'x', 0x42 },                             /* scratch left / right */
    { '5', COIN },
    { 283, 0x4E },                                            /* F2: Test */
};

static void key(int sym, int down)
{
    unsigned i;

    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        uint8_t bit = keymap[i].bit;

        if (keymap[i].sym != sym)
            continue;
        if (bit == COIN) {
            if (down) {
                g.credit[0] += g.coin_value;
                LOG("coin: credit %u", g.credit[0]);
            }
        } else if (down) {
            if ((bit & 7) == 5 && bit < 8 * PLAYERS && !(g.keys[bit >> 3] & 1 << 5))
                g.deal[bit >> 3] = 1;        /* red: take a ticket */
            g.keys[bit >> 3] |= 1 << (bit & 7);
        } else {
            g.keys[bit >> 3] &= ~(1 << (bit & 7));
        }
    }
}

static void reset(struct e2100_channel *ch)
{
    const char *v = getenv("SCRATCH_COIN");

    (void)ch;
    g.coin_value = v && atoi(v) > 0 ? (uint32_t)atoi(v) : 100;
    g.first = 0;
}

static void status(char *buf, size_t n)
{
    snprintf(buf, n, "credit %u%s", g.credit[0], g.busy[0] ? " | scratching" : "");
}

static const struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Scratch",
    .status = status,
    .reset = reset,
    .key = key,
    .transport = E2100_TRANSPORT_REGTABLE,
    .reg_cmd = reg_cmd,
    .reg_build = reg_build,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    api = a;
    return &game;
}
