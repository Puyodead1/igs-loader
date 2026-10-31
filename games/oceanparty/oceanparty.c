/*
 * Ocean Party md105cn (E2100) game plugin.  docs/OceanParty.md.
 *
 * Channel: a persistent RC4 stream per direction, keyed with the 32-bit key
 * as 4 little-endian bytes (the game's sub_806AAF0 / sub_806ABD0).  The game
 * announces its next header index and key in its type-8 heartbeat and
 * switches after sending; the ASIC's own index and key (type 3 word 11) are
 * kept unchanged.
 *
 * In this game the ASIC runs the game and the PC only shows it; the
 * cabinet's per-seat displays and buttons belong to the ASIC.  This is a
 * re-implementation: one keyboard player betting on the 8 fish, made-up odds.
 *
 *   WAIT  until the game first reports state 1 (after loading).
 *   BET   mode 1 (swim) for OCEAN_BET_SECONDS; bets accepted.
 *   SHOW  mode 2 with the result in type 7; the game runs the transition and
 *         the round (heartbeat state 2, 3), then reports state 1.  Mode goes
 *         back to 1 once the round starts so it is not repeated.
 *   then  bets are settled, the result joins the history, BET again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"

#define RX_KEY_INIT 0x364251u   /* ASIC -> game */
#define TX_KEY_INIT 0xA5148Fu   /* game -> ASIC */
#define FISH 8

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(...) CORE->log("oceanparty: " __VA_ARGS__)
#define VLOG(...) do { if (CORE->verbose) LOG(__VA_ARGS__); } while (0)

struct rc4 { uint8_t S[256], i, j; };

static void rc4_init(struct rc4 *r, uint32_t key)
{
    uint8_t k[4] = { key, key >> 8, key >> 16, key >> 24 }, t, j = 0;
    int i;

    for (i = 0; i < 256; i++)
        r->S[i] = i;
    for (i = 0; i < 256; i++) {
        j += r->S[i] + k[i & 3];
        t = r->S[i];
        r->S[i] = r->S[j];
        r->S[j] = t;
    }
    r->i = r->j = 0;
}

static void rc4_words(struct rc4 *r, uint32_t *w, uint32_t n)
{
    uint32_t x, b;
    uint8_t t;

    for (x = 0; x < n; x++) {
        for (b = 0; b < 4; b++) {
            r->i++;
            r->j += r->S[r->i];
            t = r->S[r->i];
            r->S[r->i] = r->S[r->j];
            r->S[r->j] = t;
            w[x] ^= (uint32_t)r->S[(uint8_t)(r->S[r->i] + r->S[r->j])] << (8 * b);
        }
    }
}

enum { WAIT, BET, SHOW };

/*
 * Fish index -> model (the game's model table: Fish_Nemo, Butterfly,
 * SeaSnail, AnglerFish, SeaTurtle, Nautilus, Lobbster, Crab; 8-10 are the
 * shark specials).  Payout multiplier per fish; P(fish) is proportional to
 * 1/odds, which gives every bet the same expected return (about 93%).
 */
static const uint32_t odds[FISH] = { 5, 5, 5, 5, 10, 10, 20, 40 };
static const char *const names[FISH] = {
    "Nemo", "Butterfly", "Snail", "Angler", "Turtle", "Nautilus", "Lobster", "Crab",
};

static struct {
    struct rc4 rx, tx;
    uint32_t rx_key, tx_key, tx_key_live;
    int game_state;               /* heartbeat: 1 swim, 2 transition, 3 round */

    uint32_t mode;                /* type 3 word 0 bits 0-5 */
    int phase, started, seen_round;
    double phase_end;
    uint8_t history[10];          /* newest first */
    uint32_t result, mult;
    long credits, bet[FISH], last_win;
    long bet_seconds, coin_value;
} g;

static void reset(struct e2100_channel *ch)
{
    (void)ch;
    g.rx_key = RX_KEY_INIT;
    g.tx_key = g.tx_key_live = TX_KEY_INIT;
    rc4_init(&g.rx, g.rx_key);
    rc4_init(&g.tx, g.tx_key);
}

static void decrypt_from_game(uint32_t *w, uint32_t n)
{
    rc4_words(&g.tx, w, n);
}

static void encrypt_to_game(uint32_t *w, uint32_t n)
{
    rc4_words(&g.rx, w, n);
}

static void message(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *p)
{
    /* type 8 (heartbeat): word 0 bits 0-2 = state, word 2 = next index, key */
    if (type == 8 && len >= 3) {
        if ((int)(p[0] & 7) != g.game_state)
            VLOG("game state %u", p[0] & 7);
        g.game_state = p[0] & 7;
        ch->tx_idx = p[2] & 0x3f;
        g.tx_key = p[2] >> 8;
        return;
    }
    VLOG("rx msg type=%u len=%u w0=0x%08x", type, len, len ? p[0] : 0);
}

/* the game switches to the announced key after sending */
static void batch_done(struct e2100_channel *ch)
{
    (void)ch;
    if (g.tx_key != g.tx_key_live) {
        rc4_init(&g.tx, g.tx_key);
        g.tx_key_live = g.tx_key;
    }
}

static uint32_t draw_result(void)
{
    double w[FISH], sum = 0, r;
    int i;

    for (i = 0; i < FISH; i++)
        sum += w[i] = 1.0 / odds[i];
    r = (double)rand() / RAND_MAX * sum;
    for (i = 0; i < FISH - 1; i++) {
        if (r < w[i])
            break;
        r -= w[i];
    }
    return i;
}

static void step(void)
{
    double now = CORE->now();
    int i;

    if (!g.started) {
        g.started = 1;
        g.bet_seconds = CORE->env_long("OCEAN_BET_SECONDS", 20);
        g.coin_value = CORE->env_long("OCEAN_COIN", 10);
        g.credits = CORE->env_long("OCEAN_CREDITS", 0);
        for (i = 0; i < 10; i++)
            g.history[i] = i % FISH;      /* shows every fish once */
        g.mode = 1;
        g.phase = WAIT;
        srand((unsigned)now);
    }
    switch (g.phase) {
    case WAIT:
        /* the first betting period starts once the game is swimming */
        if (g.game_state == 1) {
            g.phase = BET;
            g.phase_end = now + g.bet_seconds;
        }
        break;
    case BET:
        if (now >= g.phase_end) {
            g.result = draw_result();
            g.mult = odds[g.result];
            g.mode = 2;
            g.phase = SHOW;
            g.seen_round = 0;
            g.phase_end = now + 120;          /* safety net */
            LOG("result fish %u (%s) x%u", g.result, names[g.result], g.mult);
        }
        break;
    case SHOW:
        if (g.game_state == 3 && !g.seen_round) {
            g.seen_round = 1;
            g.mode = 1;
        }
        if ((g.seen_round && g.game_state == 1) || now >= g.phase_end) {
            g.last_win = g.bet[g.result] * (long)g.mult;
            g.credits += g.last_win;
            memmove(g.history + 1, g.history, 9);
            g.history[0] = g.result;
            memset(g.bet, 0, sizeof(g.bet));
            g.mode = 1;
            g.phase = BET;
            g.phase_end = now + g.bet_seconds;
            LOG("round over, won %ld, credit %ld", g.last_win, g.credits);
        }
        break;
    }
}

static void build(struct e2100_channel *ch, struct e2100_batch *b)
{
    uint32_t w[12], i;

    step();
    /*
     * Type 3 status (sub_8067C30 case 3): word 0 bits 0-5 = game mode
     * (0 idle, 1 swim, 2 transition, 3 round, 4 settings), bit 6 = music on,
     * bit 7 = sound effects on (both muted otherwise), bits 8-23 = speed
     * (frame scale (v-1)/60+1); words 1-2 = last 10 results (nibbles,
     * newest first); word 11 = next RX index (low byte) and key (>>8).
     */
    memset(w, 0, sizeof(w));
    w[0] = (uint32_t)CORE->env_long("OCEAN_MODE", g.mode) | 0x40 | 0x80 | (1u << 8);
    for (i = 0; i < 8; i++)
        w[1] |= (uint32_t)(g.history[i] & 0xf) << (4 * i);
    w[2] = (g.history[8] & 0xf) | (uint32_t)(g.history[9] & 0xf) << 4;
    w[11] = (ch->rx_idx & 0x3f) | (g.rx_key << 8);
    api->batch_add(b, 3, w, 12);
    /* type 7: the round result (fish, multiplier) */
    w[0] = (g.result & 0xff) | (g.mult & 0xff) << 8;
    api->batch_add(b, 7, w, 1);
}

/* SDL 1.2 key symbols: '5' coin, Q W E R T Y U I bet on fish 0-7, X clear */
static void key(int sym, int down)
{
    static const char betkeys[] = "qwertyui";
    const char *k;
    int i;

    if (!down)
        return;
    if (sym == '5') {
        g.credits += g.coin_value;
    } else if (sym == 'x' && g.phase == BET) {
        for (i = 0; i < FISH; i++)
            g.credits += g.bet[i];
        memset(g.bet, 0, sizeof(g.bet));
    } else if (sym > 0 && sym < 128 && (k = strchr(betkeys, sym)) &&
               g.phase == BET && g.credits > 0) {
        g.credits--;
        g.bet[k - betkeys]++;
    }
}

/* the player panel: the cabinet's per-seat display, on screen */
static void draw(int width, int height)
{
    char line[3][192], status[64];
    int i, f, w = 0, x, y, left = (int)(g.phase_end - CORE->now() + 0.999);
    size_t n;

    (void)height;
    if (!g.started)
        return;
    if (g.phase == WAIT)
        snprintf(status, sizeof(status), "STARTING...");
    else if (g.phase == BET)
        snprintf(status, sizeof(status), "BETS OPEN %2ds", left > 0 ? left : 0);
    else
        snprintf(status, sizeof(status), "RESULT %s x%u", names[g.result], g.mult);
    snprintf(line[0], sizeof(line[0]),
             "CREDIT %-7ld  LAST WIN %-6ld  %-22s  [5] coin  [X] clear bets",
             g.credits, g.last_win, status);
    for (i = 0; i < 2; i++) {
        n = 0;
        for (f = 4 * i; f < 4 * i + 4; f++)
            n += snprintf(line[1 + i] + n, sizeof(line[0]) - n, "[%c] %-9s x%-2u bet %-4ld   ",
                          "QWERTYUI"[f], names[f], odds[f], g.bet[f]);
    }
    for (i = 0; i < 3; i++)
        if ((int)strlen(line[i]) * CORE->font_w > w)
            w = strlen(line[i]) * CORE->font_w;
    /* above the bottom paytable bar */
    x = (width - w) / 2;
    y = 150;
    CORE->fill(x - 12, y - 12, w + 24, 3 * (CORE->font_h + 6) + 16, 0, 0, 0, 0.6f);
    for (i = 0; i < 3; i++)
        CORE->text(x, y + (2 - i) * (CORE->font_h + 6), line[i],
                   1.0f, i ? 1.0f : 0.85f, i ? 1.0f : 0.2f);
}

static void status(char *buf, size_t n)
{
    long bets = 0;
    int f;

    for (f = 0; f < FISH; f++)
        bets += g.bet[f];
    snprintf(buf, n, "credit %ld | bets %ld | last win %ld | %s", g.credits, bets, g.last_win,
             g.phase == BET ? "bets open" : g.phase == SHOW ? names[g.result] : "starting");
}

static const struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Ocean Party",
    .status = status,
    .reset = reset,
    .decrypt_from_game = decrypt_from_game,
    .encrypt_to_game = encrypt_to_game,
    .message = message,
    .batch_done = batch_done,
    .build = build,
    .key = key,
    .draw = draw,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    api = a;
    return &game;
}
