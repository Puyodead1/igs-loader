/*
 * SangoMario (E2100, recovered) game plugin.  wiki SangoMario.
 *
 * Channel: batch transport, running checksum shared by both directions; a
 * persistent RC4 stream per direction keyed with a 32-bit key as 4
 * little-endian bytes (Ocean Party's).  First keys 0x82675251 (ASIC -> game)
 * and 0x80678267 (game -> ASIC).  The ASIC announces its next header index
 * and 24-bit key in message 3 word 8; the game re-keys before the next batch
 * it reads.  The game announces its own in message 12 (word 2) and switches
 * after sending.
 *
 * As in Ocean Party the ASIC runs the game: the seats' buttons and displays
 * are its own, and the PC shows the board, the lamp run, the wins and the
 * history as the ASIC's (stage, step) says.  This is a re-implementation
 * with made-up odds; one keyboard player.
 *
 *   BET   stage 1 for SANGO_BET_SECONDS; then step 36 with the result as
 *         history entry 0 (the game reads it there)
 *   RUN   stage 2, the lamp run; step 3 asks the game to show it
 *   WIN   stage 4 (stage 3 for the jade seal)
 *   REC   stage 7: the result joins the history; bets are settled
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"

#define RX_KEY_INIT 0x82675251u   /* ASIC -> game */
#define TX_KEY_INIT 0x80678267u   /* game -> ASIC */
#define RX_KEY      0x5A6E31u     /* the 24-bit key the ASIC switches to */
#define SYMS 13                   /* 0-5 heroes, 6-11 weapons, 12 the jade seal */
#define BETS 12
#define HIST 20

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(...) CORE->log("sangomario: " __VA_ARGS__)
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

/* symbol index (the ASIC's) -> name; the game's IgsIo table maps it to its model */
static const char *const names[SYMS] = {
    "GuanYu", "XiahouDun", "MengHuo", "LuBu", "ZhangFei", "HuangZhong",
    "HuaJi", "SheMao", "TianGong", "GuanDao", "LingDao", "YaBang", "YuXi",
};
/* made-up odds; P(symbol) proportional to 1/odds gives every bet the same return */
static const uint32_t odds[BETS] = { 40, 30, 25, 20, 15, 10, 5, 5, 5, 4, 3, 2 };
#define SEAL_ODDS 40

enum { WAIT, DRAW, RESULT, BET, RUN, SEAL, WIN, REC };

static struct {
    struct rc4 rx, tx;
    uint32_t rx_key_live, rx_key_next;
    uint32_t tx_key_live, tx_key;
    int game_state, lamp;

    int started, phase;
    double phase_start, phase_end;
    uint32_t stage, step;
    uint8_t hist[HIST], hist_odds[HIST], hist_seal[HIST];
    uint32_t result, mult, seal;
    long credits, bet[BETS], last_win;
    long bet_seconds, coin_value;
    uint32_t sounds[8], nsounds;
    int panel;                    /* the player panel is shown (Tab toggles) */
} g;

static void reset(struct e2100_channel *ch)
{
    (void)ch;
    g.rx_key_live = g.rx_key_next = RX_KEY_INIT;
    g.tx_key_live = g.tx_key = TX_KEY_INIT;
    rc4_init(&g.rx, g.rx_key_live);
    rc4_init(&g.tx, g.tx_key_live);
}

static void decrypt_from_game(uint32_t *w, uint32_t n)
{
    rc4_words(&g.tx, w, n);
}

/* the game re-keys before decrypting the batch after the one that announced the key */
static void encrypt_to_game(uint32_t *w, uint32_t n)
{
    rc4_words(&g.rx, w, n);
    if (g.rx_key_next != g.rx_key_live) {
        g.rx_key_live = g.rx_key_next;
        rc4_init(&g.rx, g.rx_key_live);
    }
}

static void message(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *p)
{
    /* 12, heartbeat: byte 0 bits 4-6 state, bit 7 a flag; bytes 1-6 date; 7 lamp; word 2 next index, key */
    if (type == 12 && len >= 3) {
        int st = (p[0] >> 4) & 7;

        if (st != g.game_state)
            VLOG("game state %d", st);
        g.game_state = st;
        g.lamp = p[1] >> 24;
        ch->tx_idx = p[2] & 0x3f;
        g.tx_key = p[2] >> 8;
        return;
    }
    if (type == 13)               /* the PC keyboard as a bitmap (operator tools) */
        return;
    /* 7: the result was read (stage 1 step 36); 10: the lamp run was shown (stage 2 step 3) */
    VLOG("rx msg type=%u len=%u w0=0x%08x", type, len, len ? p[0] : 0);
}

/* the game switches to its announced key after sending; until it first re-keys it announces
   the low 24 bits of its 32-bit first key, which is no change */
static void batch_done(struct e2100_channel *ch)
{
    (void)ch;
    if (g.tx_key != (g.tx_key_live & 0xFFFFFF)) {
        rc4_init(&g.tx, g.tx_key);
        g.tx_key_live = g.tx_key;
    }
}

static uint32_t draw_result(void)
{
    double w[BETS], sum = 0, r;
    int i;

    for (i = 0; i < BETS; i++)
        sum += w[i] = 1.0 / odds[i];
    r = (double)rand() / RAND_MAX * sum;
    for (i = 0; i < BETS - 1; i++) {
        if (r < w[i])
            break;
        r -= w[i];
    }
    return i;
}

static void sound(uint32_t id)
{
    if (g.nsounds < 8)
        g.sounds[g.nsounds++] = id;
}

static void phase(int p, double secs)
{
    g.phase = p;
    g.phase_start = CORE->now();
    g.phase_end = g.phase_start + secs;
}

static void step(void)
{
    double now = CORE->now();
    int i;

    if (!g.started) {
        g.started = 1;
        g.bet_seconds = CORE->env_long("SANGO_BET_SECONDS", 20);
        g.coin_value = CORE->env_long("SANGO_COIN", 10);
        g.credits = CORE->env_long("SANGO_CREDITS", 0);
        g.panel = CORE->env_long("SANGO_PANEL", 1) != 0;
        for (i = 0; i < HIST; i++) {
            g.hist[i] = i % BETS;
            g.hist_odds[i] = odds[i % BETS];
        }
        srand((unsigned)(now * 1000));
        phase(WAIT, CORE->env_long("SANGO_START_SECONDS", 3));
    }
    switch (g.phase) {
    case WAIT:                    /* attract, until the game has loaded */
        g.stage = 7; g.step = 0;
        if (now >= g.phase_end)
            phase(DRAW, 0.5);
        break;
    case DRAW:                    /* stage 1: the ASIC draws; step 36 hands the result over */
        g.stage = 1; g.step = 0;
        if (now >= g.phase_end) {
            long force = CORE->env_long("SANGO_RESULT", -1);   /* testing: 0-11, 12 the seal */

            /* the seal's bonus scene waits on animations in the lost Award banks: off unless asked */
            long rate = CORE->env_long("SANGO_SEAL_RATE", 0);

            g.seal = force >= 0 ? force == 12 : rate > 0 && rand() % rate == 0;
            g.result = g.seal ? 12 : force >= 0 && force < BETS ? (uint32_t)force : draw_result();
            g.mult = g.seal ? SEAL_ODDS : odds[g.result];
            memmove(g.hist + 1, g.hist, HIST - 1);
            memmove(g.hist_odds + 1, g.hist_odds, HIST - 1);
            memmove(g.hist_seal + 1, g.hist_seal, HIST - 1);
            /* the seal: main symbol and odds stay a weapon/hero draw, byte 2 = 40 + extra */
            g.hist[0] = g.seal ? draw_result() : g.result;
            g.hist_odds[0] = g.seal ? odds[g.hist[0]] : g.mult;
            g.hist_seal[0] = g.seal ? 41 : 0;
            LOG("result %s x%u", names[g.result], g.mult);
            phase(RESULT, CORE->env_long("SANGO_RESULT_SECONDS", 1));
        }
        break;
    case RESULT:
        g.stage = 1; g.step = 36;
        if (now >= g.phase_end)
            phase(BET, g.bet_seconds);
        break;
    case BET:                     /* stage 2: step 1 runs the countdown (word 1); step 3 closes bets */
        g.stage = 2; g.step = now < g.phase_end ? 1 : 3;
        if (now >= g.phase_end + 1)
            phase(RUN, CORE->env_long("SANGO_RUN_SECONDS", 12));
        break;
    case RUN:                     /* stage 3: the lamp runs to the result */
        g.stage = 3; g.step = 0;
        if (now >= g.phase_end)
            phase(g.seal ? SEAL : WIN, g.seal ? CORE->env_long("SANGO_SEAL_SECONDS", 35) :
                                                CORE->env_long("SANGO_WIN_SECONDS", 6));
        break;
    case SEAL:                    /* the jade seal's bonus: stage 3 shows it (stage 4 waits on lost banks) */
        g.stage = (uint32_t)CORE->env_long("SANGO_SEAL_STAGE", 3); g.step = 0;
        if (now >= g.phase_end)
            phase(WIN, 0);
        break;
    case WIN:
        if (now >= g.phase_end) {
            if (g.seal) {         /* the seal pays every bet at its odds */
                g.last_win = 0;
                for (i = 0; i < BETS; i++)
                    g.last_win += g.bet[i] * SEAL_ODDS;
            } else {
                g.last_win = g.bet[g.result] * (long)g.mult;
            }
            g.credits += g.last_win;
            memset(g.bet, 0, sizeof(g.bet));
            LOG("round over, won %ld, credit %ld", g.last_win, g.credits);
            phase(REC, CORE->env_long("SANGO_REC_SECONDS", 4));
        }
        break;
    case REC:                     /* stage 7: the result, then the history */
        g.stage = 7; g.step = 0;
        if (now >= g.phase_end)
            phase(DRAW, 0.5);
        break;
    }
    if (CORE->env_long("SANGO_STAGE", -1) >= 0) {   /* testing: force (stage, step) */
        g.stage = (uint32_t)CORE->env_long("SANGO_STAGE", 1);
        g.step = (uint32_t)CORE->env_long("SANGO_STEP", 0);
    }
}

static void build(struct e2100_channel *ch, struct e2100_batch *b)
{
    uint32_t w[25], i;
    double left;

    step();
    /*
     * 3, status: word 0 bits 0-5 step, 6-9 stage, 10 sound effects, 11 music, 12 reset;
     * word 1 the betting countdown in frames (shown / 60 % 100); words 2-7 per seat (nibbles:
     * error, pay mode; >> 8 a value; 0 = fine); word 8 next index | key << 8.
     */
    memset(w, 0, sizeof(w));
    /* bit 12 releases the game: its scene follows (stage, step) only while its state is 0 */
    w[0] = (g.step & 0x3f) | (g.stage & 0xf) << 6 | 1u << 10 | 1u << 11 | 1u << 12;
    left = g.phase == BET ? g.phase_end - CORE->now() : 0;
    w[1] = left > 0 ? (uint32_t)(left * 60) : 0;
    w[8] = (ch->rx_idx & 0x3f) | RX_KEY << 8;
    g.rx_key_next = RX_KEY;
    api->batch_add(b, 3, w, 9);
    /* 7, history: 20 words [symbol, odds, seal (40 + n, 255 none), 0]; 5 words of flags */
    memset(w, 0, sizeof(w));
    for (i = 0; i < HIST; i++)
        w[i] = g.hist[i] | (uint32_t)g.hist_odds[i] << 8 | (uint32_t)(g.hist_seal[i] ? g.hist_seal[i] : 255) << 16;
    api->batch_add(b, 7, w, 25);
    /* 9, sound events: 0x02000000 | id */
    for (i = 0; i < g.nsounds; i++) {
        w[0] = 0x02000000 | g.sounds[i];
        api->batch_add(b, 9, w, 1);
    }
    g.nsounds = 0;
}

/*
 * SDL 1.2 keys: '5' coin; Q W E R T Y bet on the heroes, A S D F G H on the weapons; X clear.
 * Sound ids (message 9): 0-4 ButtonSound, 5 Take, 6 CoinIn, 7 KeyIn, 8 KeyOut, 9-17 BetSound_01-09,
 * 18-19 AwardMusic_07, 20 AwardMusic_08, 21 AwardSound_01b, 22 AwardMusic_09, 23 BonusMusic,
 * 24 ErrorSound.
 */
static void key(int sym, int down)
{
    static const char betkeys[] = "qwertyasdfgh";
    const char *k;
    int i;

    if (!down)
        return;
    if (sym == '\t') {
        g.panel = !g.panel;
    } else if (sym == '5') {
        g.credits += g.coin_value;
        sound(6);                 /* CoinIn */
    } else if (sym == 'x' && g.phase == BET) {
        for (i = 0; i < BETS; i++)
            g.credits += g.bet[i];
        memset(g.bet, 0, sizeof(g.bet));
    } else if (sym > 0 && sym < 128 && (k = strchr(betkeys, sym)) &&
               g.phase == BET && g.credits > 0) {
        g.credits--;
        g.bet[k - betkeys]++;
        sound(9 + (k - betkeys) % 9);   /* BetSound_01..09 */
    }
}

/* the player panel: the cabinet's seat display, on screen */
static void draw(int width, int height)
{
    char line[5][120], st[64];
    int i, f, w = 0, x, y, left = (int)(g.phase_end - CORE->now() + 0.999);
    size_t n;

    if (!g.started || !g.panel)
        return;
    if (g.phase == BET)
        snprintf(st, sizeof(st), "BETS OPEN %2ds", left > 0 ? left : 0);
    else if (g.phase == WAIT)
        snprintf(st, sizeof(st), "STARTING...");
    else if (g.phase == DRAW || g.phase == RESULT)
        snprintf(st, sizeof(st), "NEXT ROUND");
    else if (g.phase == RUN || g.phase == SEAL)
        snprintf(st, sizeof(st), "RUNNING");
    else
        snprintf(st, sizeof(st), "RESULT %s x%u", names[g.result], g.mult);
    snprintf(line[0], sizeof(line[0]), "CREDIT %-6ld WIN %-6ld %s  [5]coin [X]clear",
             g.credits, g.last_win, st);
    for (i = 0; i < 4; i++) {
        n = 0;
        for (f = 3 * i; f < 3 * i + 3; f++)
            n += snprintf(line[1 + i] + n, sizeof(line[0]) - n, "[%c]%-10s x%-2u %-3ld ",
                          "QWERTYASDFGH"[f], names[f], odds[f], g.bet[f]);
    }
    for (i = 0; i < 5; i++)
        if ((int)strlen(line[i]) * CORE->font_w > w)
            w = strlen(line[i]) * CORE->font_w;
    /* in the middle of the board, inside the ring */
    x = (width - w) / 2;
    y = height / 2 - 2 * (CORE->font_h + 6);
    CORE->fill(x - 8, y - 8, w + 16, 5 * (CORE->font_h + 6) + 10, 0, 0, 0, 0.6f);
    for (i = 0; i < 5; i++)
        CORE->text(x, y + (4 - i) * (CORE->font_h + 6), line[i],
                   1.0f, i ? 1.0f : 0.85f, i ? 1.0f : 0.2f);
}

static void status(char *buf, size_t n)
{
    long bets = 0;
    int f;

    for (f = 0; f < BETS; f++)
        bets += g.bet[f];
    snprintf(buf, n, "credit %ld | bets %ld | last win %ld | %u-%u", g.credits, bets, g.last_win,
             g.stage, g.step);
}

static const struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "SangoMario",
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
