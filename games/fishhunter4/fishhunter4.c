/*
 * Fish Hunter 4 (E2100) game plugin: the IGS036's side of the "e2k1"
 * ShareRam protocol (the games' shareram_e2k1/pcshareram.c).
 * wiki Fish Hunter 4, wiki E2100 Protection.
 *
 *   - transport: batch framing with the trailing-sum layout ([payload,
 *     headers, sum]), header register 0 both ways, the game as master: one
 *     ASIC batch per game batch.
 *   - variant "cn" (M101-M105CN, Fish-OS): no channel cipher, the older P2A
 *     loader (the DONE word carries the upload checksum, 0).
 *     variant "cnrc4" (M107CN): the same loader, with the RC4 channel and the
 *     4P build's settings and records.
 *     variant "4p" (M400-M402CN, FishHunter4P): RC4 per direction over the
 *     whole body, keys 0x82675251 (ASIC -> game) and 0x80678267, set once.
 *   - the chip's firmware is not dumped: its game logic is rebuilt here.
 *     Catches (message 13/25 -> 14) are drawn with probability RTP / value
 *     per fish, wins credited at value x bet; chance games, the bonus game,
 *     settings, records and the ticket payout follow the game's side of the
 *     protocol; the numbers are invented (FISH_* tunables).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"

#define RX_KEY 0x82675251u   /* ASIC -> game (m_rDecodeKey) */
#define TX_KEY 0x80678267u   /* game -> ASIC (m_rEncodeKey) */
#define SEATS   6            /* the protocol's arrays */
#define PLAYERS 4            /* stations the game runs */
#define KINDS   12           /* fish kinds the catch messages count */

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(...) CORE->log("fishhunter4: " __VA_ARGS__)
#define VLOG(...) do { if (CORE->verbose) LOG(__VA_ARGS__); } while (0)

/* ---- RC4 ("4p"): 4-byte little-endian key, keystream over LE words ------------------ */

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

    for (x = 0; x < n; x++)
        for (b = 0; b < 4; b++) {
            r->i++;
            r->j += r->S[r->i];
            t = r->S[r->i];
            r->S[r->i] = r->S[r->j];
            r->S[r->j] = t;
            w[x] ^= (uint32_t)r->S[(uint8_t)(r->S[r->i] + r->S[r->j])] << (8 * b);
        }
}

/* ---- the game's tables ---------------------------------------------------------------- */

/* fish value per kind (the game's fish table, word +10 of each 36-byte row) */
static const uint32_t value[KINDS] = { 1, 2, 4, 3, 20, 50, 70, 30, 40, 10, 5, 100 };
/* settings menu tables (KeyInRate, KeyOutRate) */
static const uint32_t key_in_rate[6] = { 10, 20, 50, 100, 200, 500 };
static const uint32_t key_out_rate[7] = { 1, 10, 20, 50, 100, 200, 500 };
/* credits per coin by the "coin rate" setting (invented: the table is the chip's) */
static const uint32_t coin_rate[6] = { 1, 2, 5, 10, 20, 50 };

/*
 * The two builds' layouts.  SettingPC defaults are the chip's "ask
 * default" answer (invented, in the menu's ranges; the cn build's own
 * vSetting_DebugSetting values where they fit).
 *
 * cn (19 words): 0 odds level 1..10, 1 level 0..2, 2 maximum bet 1..5,
 * 3 coin rate 0..5, 4 on/off, 5 key-in rate, 6 key-out rate, 7 payout
 * mode 0..4 (1 coins, 3 tickets show the payout panel), 8 tickets: 1..50 =
 * one ticket per n credits, 101..103 = 10/20/30 tickets per credit, 9
 * music and 10 sound effects on/off, 11..18 the password digits.
 *
 * 4p (37 words): 0 odds, 1 level, 2 maximum bet 1..8, 3 minimum bet
 * 1..3, 4 coin rate, 5 weapon type, 6 score times 0..2, 7..10 the system
 * password, 11, 12 on/off, 13 key-in rate, 14 key-out rate, 15 payout
 * mode, 16 tickets, 17 sound effects, 18 music, 19 on/off, 20.. unused.
 *
 * RecordPC: 18 words per row, player p at word p; rows coin in, coin out,
 * key in, tickets out, games, key out, catches, bet, win (the total-book
 * page shows win / bet as the payout rate), then the on and play times in
 * seconds.  4p skips a row after the third.
 */
#define NSET_MAX 37
#define NREC_MAX 250
struct layout {
    int nset;
    uint32_t def[NSET_MAX];
    int odds, maxbet, coin, keyin, keyout, payout, ticket;
    int bonus_packed;                 /* message 34: counts as bytes of word 1 (cn) or words 1, 2 */
    int row[9];
    int on_time, play_time, nrec;
};
static const struct layout layout_cn = {
    19, { 5, 2, 5, 3, 1, 3, 2, 3, 1, 1, 1 },
    0, 2, 3, 5, 6, 7, 8, 1,
    { 0, 18, 36, 54, 72, 90, 108, 126, 144 }, 231, 230, 232,
};
static const struct layout layout_4p = {
    37, { 5, 1, 5, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 3, 2, 3, 1, 1, 1 },
    0, 2, 4, 13, 14, 15, 16, 0,
    { 0, 18, 36, 72, 90, 108, 126, 144, 162 }, 249, 248, 250,
};
static const struct layout *L = &layout_cn;

enum { R_COIN_IN, R_COIN_OUT, R_KEY_IN, R_TICKET_OUT, R_GAMES, R_KEY_OUT, R_CATCHES, R_BET, R_WIN };
#define REC(r, p) g.rec[L->row[r] + (p)]
/* ---- state ------------------------------------------------------------------------------ */

#define CATCHQ 32

static struct {
    int cipher;                       /* variant 4p */
    struct rc4 rx, tx;

    uint32_t input[10];               /* message 2: word j bit b = key 8j+b */
    uint32_t set[NSET_MAX];
    uint32_t rec[NREC_MAX];
    long credit[SEATS];
    uint32_t payout[12];              /* message 18: coins [0..5], tickets [6..11] */
    uint32_t last_bet[SEATS];
    uint32_t chance[SEATS], chance_win[SEATS];
    uint32_t big_fish[7];

    /* replies queued for the next batch */
    int want_version, want_settings, want_records, want_reset, want_errors;
    int credit_dirty, payout_dirty, chance_dirty, big_dirty;
    uint32_t catchq[CATCHQ][7];
    int ncatch;
    uint32_t bonusq[SEATS][3];
    int nbonus;

    uint32_t batches, state;
    double last_tick, last_sec;
    /* tunables */
    double rtp;
    int chance_every, chance_kind;
} g;

static double frand(void) { return rand() / (RAND_MAX + 1.0); }

static void credit_add(int p, long v)
{
    g.credit[p] += v;
    if (g.credit[p] < 0)
        g.credit[p] = 0;
    g.credit_dirty = 1;
}

static void reset(struct e2100_channel *ch)
{
    const char *v = getenv("IGS_VARIANT");

    (void)ch;
    g.cipher = v && (!strcmp(v, "4p") || !strcmp(v, "cnrc4"));
    rc4_init(&g.rx, RX_KEY);
    rc4_init(&g.tx, TX_KEY);
    g.batches = 0;
}

static void decrypt_from_game(uint32_t *w, uint32_t n)
{
    if (g.cipher)
        rc4_words(&g.tx, w, n);
}

static void encrypt_to_game(uint32_t *w, uint32_t n)
{
    if (g.cipher)
        rc4_words(&g.rx, w, n);
}

/* ---- the chip's game logic ------------------------------------------------------------ */

/* odds level 1..10 (setting 0) shifts the return: level 5 = FISH_RTP */
static double rtp(void)
{
    double r = g.rtp + 0.02 * (5.0 - (double)(g.set[L->odds] ? g.set[L->odds] : 5));

    return r < 0.5 ? 0.5 : r > 1.2 ? 1.2 : r;
}

/* a chance game for the player, now and then after a shot */
static void maybe_chance(int p, uint32_t bet)
{
    double r;

    if (!g.chance_every || g.chance[p] || frand() * g.chance_every >= 1.0)
        return;
    r = g.chance_kind == 1 ? 0.0 : g.chance_kind == 3 ? 0.5 : g.chance_kind == 4 ? 0.8 : g.chance_kind == 5 ? 0.95 : frand();
    if (r < 0.4) {
        g.chance[p] = 1;                                   /* tiny game */
    } else if (r < 0.7) {
        g.chance[p] = 3;                                   /* small game: the win */
        g.chance_win[p] = (20 + rand() % 81) * (bet ? bet : 1);
    } else if (r < 0.9) {
        g.chance[p] = 4;                                   /* big bomb */
    } else {
        int q;                                             /* bonus game: the seats in play */

        for (q = 0; q < PLAYERS; q++)
            if (q == p || g.credit[q] > 0) {
                g.chance[q] = 5;
                g.big_fish[q] = 1 + rand() % 3;
            }
        g.big_fish[6] = 40;
        g.big_dirty = 1;
    }
    g.chance_dirty = 1;
    LOG("player %d: chance game %u", p + 1, g.chance[p]);
}

/*
 * Messages 13 / 25: [player << 16 | weapon << 8 | level, hinted fish per
 * kind (12 bytes, 13 only), fish per kind (12 bytes)].  Reply 14: [star <<
 * 24 | player << 16 | weapon << 8, caught per kind].  25 is a bomb.  The
 * level is the bet (+100 for the boosted weapon).
 */
static void ask_catch(uint32_t type, const uint32_t *w)
{
    uint32_t p = (w[0] >> 16) & 0xFF, weapon = (w[0] >> 8) & 0xFF, level = w[0] & 0xFF;
    uint32_t bet = level % 100, k, *r;
    uint8_t hint[KINDS], have[KINDS], got[KINDS];
    long win = 0;

    if (p >= SEATS || g.ncatch == CATCHQ)
        return;
    if (!bet || bet > 10)
        bet = g.last_bet[p] ? g.last_bet[p] : 1;
    memcpy(hint, &w[1], KINDS);
    memcpy(have, &w[4], KINDS);
    for (k = 0; k < KINDS; k++) {
        uint32_t n;
        double pc = rtp() / value[k];

        if (level > 100)
            pc *= 1.5;
        got[k] = 0;
        for (n = 0; n < have[k]; n++) {
            /* a bomb is a prize: four shots' worth on every fish */
            int catch_it = type == 25 ? frand() < 4 * pc : n < (type == 13 ? hint[k] : 0) ? frand() < 0.5 : frand() < pc;

            if (catch_it)
                got[k]++;
        }
        win += (long)got[k] * value[k] * bet;
    }
    r = g.catchq[g.ncatch++];
    memset(r, 0, 4 * 4);
    r[0] = p << 16 | weapon << 8;
    memcpy(&r[1], got, KINDS);
    if (win) {
        credit_add(p, win);
        REC(R_WIN, p) += (uint32_t)win;
        REC(R_CATCHES, p)++;
    }
    VLOG("player %u weapon %u level %u: win %ld", p + 1, weapon, level, win);
}

static void message(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *w)
{
    uint32_t i;

    (void)ch;
    if (type != 5)
        VLOG("rx msg %u len %u w0=0x%08x", type, len, len ? w[0] : 0);
    switch (type) {
    case 3:                                       /* version */
        g.want_version = 1;
        break;
    case 5:                                       /* every batch: state, bets, date, chance-max flag */
        if (len < 9)
            break;
        g.state = w[0];
        for (i = 0; i < SEATS; i++) {
            uint32_t b = w[1 + i], cost;

            if (!b)
                continue;
            cost = b == 99 ? 0 : b % 100;         /* 99: the star bomb, 100+: the auto shot */
            if (cost) {
                g.last_bet[i] = cost <= 10 ? cost : g.last_bet[i];
                credit_add(i, -(long)cost);
                REC(R_BET, i) += cost;
                REC(R_GAMES, i)++;
                if (i < PLAYERS)
                    maybe_chance(i, cost);
            }
        }
        break;
    case 7:                                       /* settings, please */
        g.want_settings = 1;
        break;
    case 8:                                       /* records (day book) */
    case 11:                                      /* records (total book) */
    case 32:                                      /* records (current) */
        g.want_records = 1;
        break;
    case 9:                                       /* new settings */
        for (i = 0; i < len && i < (uint32_t)L->nset; i++)
            g.set[i] = w[i];
        LOG("settings stored");
        break;
    case 13:
    case 25:
        if (len >= 7)
            ask_catch(type, w);
        break;
    case 16:                                      /* lamps and counters: nothing to do */
        break;
    case 19:                                      /* reset all: bookkeeping and credits */
        memset(g.rec, 0, sizeof(g.rec));
        memset(g.credit, 0, sizeof(g.credit));
        memset(g.payout, 0, sizeof(g.payout));
        g.credit_dirty = g.payout_dirty = 1;
        g.want_reset = 1;
        LOG("reset all");
        break;
    case 24:                                      /* the swell fish was hit: a tiny game */
        if (len >= 1 && w[0] < PLAYERS && !g.chance[w[0]] && frand() < 0.3) {
            g.chance[w[0]] = 1;
            g.chance_dirty = 1;
        }
        break;
    case 28:                                      /* chance game over: player << 16 | v */
        if (len >= 1) {
            uint32_t p = w[0] >> 16, v = w[0] & 0xFFFF;

            if (v == 0xFF0F) {                    /* the bonus game is over for everyone */
                for (i = 0; i < SEATS; i++)
                    if (g.chance[i] == 5)
                        g.chance[i] = 0;
                memset(g.big_fish, 0, sizeof(g.big_fish));
            } else if (p < SEATS) {
                if (v) {                          /* the tiny game's win, in fish values */
                    long win = (long)v * (g.last_bet[p] ? g.last_bet[p] : 1);

                    credit_add(p, win);
                    REC(R_WIN, p) += (uint32_t)win;
                }
                if (g.chance[p] == 3 && g.chance_win[p]) {
                    credit_add(p, g.chance_win[p]);
                    REC(R_WIN, p) += g.chance_win[p];
                }
                g.chance[p] = 0;
                g.chance_win[p] = 0;
            }
            g.chance_dirty = 1;
        }
        break;
    case 31:                                      /* default settings */
        memcpy(g.set, L->def, sizeof(g.set));
        g.want_settings = 1;
        break;
    case 34:                                      /* bonus game: [player, kind-0 hits | big fish hits << 8] */
        if (len >= 2 && w[0] < SEATS && g.nbonus < SEATS) {
            uint32_t p = w[0], n0 = L->bonus_packed ? w[1] & 0xFF : w[1];
            uint32_t n1 = L->bonus_packed ? (w[1] >> 8) & 0xFF : len >= 3 ? w[2] : 0, c0 = 0, c1 = 0, b;

            for (i = 0; i < n0; i++)
                c0 += frand() < 0.5;
            if (n1 && frand() < 0.15)
                c1 = 1 + rand() % 3;
            g.bonusq[g.nbonus][0] = p;
            g.bonusq[g.nbonus][1] = c0;
            g.bonusq[g.nbonus][2] = c1;
            g.nbonus++;
            b = g.last_bet[p] ? g.last_bet[p] : 1;
            credit_add(p, (long)(10 * c0 + 100 * c1) * b);
            REC(R_WIN, p) += (10 * c0 + 100 * c1) * b;
        }
        break;
    case 21: case 29: case 17: case 27:           /* debug, test data, payout setup, big-game win */
        break;
    }
}

/* ---- the ASIC's batch -------------------------------------------------------------------- */

static void build(struct e2100_channel *ch, struct e2100_batch *b)
{
    uint32_t w[NREC_MAX], i;
    double now = CORE->now();

    (void)ch;
    if (g.batches++ == 0) {
        g.credit_dirty = g.payout_dirty = g.chance_dirty = 1;
        g.want_errors = 1;
        g.last_tick = g.last_sec = now;
    }
    /* the payout runs down, one ticket or coin every 100 ms */
    if (now - g.last_tick >= 0.1) {
        g.last_tick = now;
        for (i = 0; i < 12; i++)
            if (g.payout[i]) {
                g.payout[i]--;
                REC(i < 6 ? R_COIN_OUT : R_TICKET_OUT, i % 6)++;
                g.payout_dirty = 1;
            }
    }
    if (now - g.last_sec >= 1.0) {
        g.last_sec += 1.0;
        g.rec[L->on_time]++;
        if (g.state)
            g.rec[L->play_time]++;
    }
    if (g.want_records) {                         /* 232 / 250 words: alone */
        if (L->nrec + 10 <= E2100_BATCH_MAX)
            api->batch_add(b, 2, g.input, 10);
        api->batch_add(b, 12, g.rec, L->nrec);
        g.want_records = 0;
        return;
    }
    api->batch_add(b, 2, g.input, 10);
    if (g.want_version) {
        static const char ver[8] = "FH4-036", rom[8] = "HLE";

        memset(w, 0, 19 * 4);
        for (i = 0; i < 8; i++) {
            w[i] = (uint8_t)ver[i];
            w[8 + i] = (uint8_t)rom[i];
        }
        api->batch_add(b, 4, w, 19);
        g.want_version = 0;
    }
    if (g.want_settings) {
        api->batch_add(b, 10, g.set, L->nset);
        g.want_settings = 0;
    }
    if (g.want_errors) {                          /* ErrorPC: no master or slave errors */
        memset(w, 0, 40 * 4);
        api->batch_add(b, 15, w, 40);
        g.want_errors = 0;
    }
    if (g.want_reset) {
        w[0] = 1;
        api->batch_add(b, 20, w, 1);
        g.want_reset = 0;
    }
    if (g.credit_dirty) {
        for (i = 0; i < SEATS; i++) {
            w[i] = 0;
            w[6 + i] = (uint32_t)g.credit[i];
        }
        api->batch_add(b, 6, w, 12);
        g.credit_dirty = 0;
    }
    if (g.payout_dirty) {
        api->batch_add(b, 18, g.payout, 12);
        g.payout_dirty = 0;
    }
    if (g.big_dirty) {
        api->batch_add(b, 26, g.big_fish, 7);
        g.big_dirty = 0;
    }
    if (g.chance_dirty) {
        for (i = 0; i < SEATS; i++) {
            w[i] = g.chance[i];
            w[6 + i] = g.chance_win[i];
        }
        api->batch_add(b, 23, w, 12);
        g.chance_dirty = 0;
    }
    while (g.ncatch && b->npayload + 4 <= E2100_BATCH_MAX && b->nhdrs < 16) {
        api->batch_add(b, 14, g.catchq[0], 4);
        memmove(g.catchq[0], g.catchq[1], sizeof(g.catchq[0]) * --g.ncatch);
    }
    for (i = 0; i < (uint32_t)g.nbonus && b->nhdrs < 16; i++)
        api->batch_add(b, 35, g.bonusq[i], 3);
    g.nbonus = 0;
}

/* ---- input ---------------------------------------------------------------------------------- */

/*
 * SDL 1.2 key symbols -> input bits (station p: bits 8p+0..5 = up, down,
 * right, left, bet, fire; 78 = Test), or a chip action: coin, key-in,
 * payout (key-out) for a station.
 */
#define COIN   0x100
#define KEYIN  0x200
#define KEYOUT 0x400
static const struct { int sym; int bit; } keymap[] = {
    { 273, 0 }, { 274, 1 }, { 275, 2 }, { 276, 3 },          /* arrows */
    { 308, 4 }, { 307, 4 }, { 306, 5 }, { 305, 5 },           /* Alt bet, Ctrl fire */
    { 'r', 8 }, { 'f', 9 }, { 'g', 10 }, { 'd', 11 }, { 's', 12 }, { 'a', 13 },
    { 'i', 16 }, { 'k', 17 }, { 'l', 18 }, { 'j', 19 }, { 'v', 20 }, { 'b', 21 },
    { 264, 24 }, { 258, 25 }, { 262, 26 }, { 260, 27 }, { 266, 28 }, { 256, 29 },
    { 283, 78 },                                               /* F2 = Test */
    { '5', COIN | 0 }, { '6', COIN | 1 }, { '7', COIN | 2 }, { '8', COIN | 3 },
    { 286, KEYIN | 0 }, { 287, KEYIN | 1 }, { 288, KEYIN | 2 }, { 289, KEYIN | 3 },     /* F5..F8 */
    { 290, KEYOUT | 0 }, { 291, KEYOUT | 1 }, { 292, KEYOUT | 2 }, { 293, KEYOUT | 3 }, /* F9..F12 */
};

static void pay_out(int p)
{
    long c = g.credit[p];
    uint32_t mode = g.set[L->payout], t = g.set[L->ticket] ? g.set[L->ticket] : 1;

    if (c <= 0)
        return;
    REC(R_KEY_OUT, p) += (uint32_t)c;
    if (mode == 1) {                              /* coins: KeyOutRate credits per coin */
        uint32_t r = key_out_rate[g.set[L->keyout] < 7 ? g.set[L->keyout] : 0], n = (uint32_t)c / r;

        if (!n)
            return;
        g.payout[p] += n;
        c -= (long)n * r;
    } else {                                      /* tickets */
        uint32_t n = t > 100 ? (uint32_t)c * (10 * t - 1000) : (uint32_t)c / t;

        if (!n)
            return;
        g.payout[6 + p] += n;
        c = t > 100 ? 0 : c - (long)n * t;
    }
    LOG("station %d pays out, %ld credits left", p + 1, c);
    g.credit[p] = c;
    g.credit_dirty = g.payout_dirty = 1;
}

static void key(int sym, int down)
{
    unsigned i;

    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        int bit = keymap[i].bit, p = bit & 0xFF;

        if (keymap[i].sym != sym)
            continue;
        if (bit & COIN) {
            if (down) {
                uint32_t r = coin_rate[g.set[L->coin] < 6 ? g.set[L->coin] : 0];

                credit_add(p, r);
                REC(R_COIN_IN, p)++;
                LOG("coin at station %d: %ld credits", p + 1, g.credit[p]);
            }
        } else if (bit & KEYIN) {
            if (down) {
                uint32_t r = key_in_rate[g.set[L->keyin] < 6 ? g.set[L->keyin] : 0];

                credit_add(p, r);
                REC(R_KEY_IN, p) += r;
                LOG("key-in at station %d: %ld credits", p + 1, g.credit[p]);
            }
        } else if (bit & KEYOUT) {
            if (down)
                pay_out(p);
        } else if (down) {
            g.input[bit >> 3] |= 1u << (bit & 7);
        } else {
            g.input[bit >> 3] &= ~(1u << (bit & 7));
        }
    }
}

static void status(char *buf, size_t n)
{
    snprintf(buf, n, "credits %ld / %ld / %ld / %ld", g.credit[0], g.credit[1], g.credit[2], g.credit[3]);
}

static const struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Fish Hunter 4",
    .status = status,
    .reset = reset,
    .decrypt_from_game = decrypt_from_game,
    .encrypt_to_game = encrypt_to_game,
    .message = message,
    .build = build,
    .key = key,
    .layout = E2100_LAYOUT_TRAILING,
};

static struct e2100_game game_cn;

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    const char *v = getenv("IGS_VARIANT");
    long credits;
    int i;

    api = a;
    L = v && (!strcmp(v, "4p") || !strcmp(v, "cnrc4")) ? &layout_4p : &layout_cn;
    memcpy(g.set, L->def, sizeof(g.set));
    g.rtp = atof(CORE->env("FISH_RTP", "0.95"));
    g.chance_every = atoi(CORE->env("FISH_CHANCE", "300"));
    g.chance_kind = atoi(CORE->env("FISH_CHANCE_KIND", "0"));
    credits = atol(CORE->env("FISH_CREDITS", "0"));
    for (i = 0; i < PLAYERS; i++)
        g.credit[i] = credits;
    srand((unsigned)(CORE->now() * 1000.0));
    /* the older P2A loader: no session keys, DONE checked against the upload */
    if (!v || strcmp(v, "4p")) {
        game_cn = game;
        game_cn.handshake = E2100_HS_NOSUM;
        return &game_cn;
    }
    return &game;
}
