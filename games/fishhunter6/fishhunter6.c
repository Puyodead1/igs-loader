/*
 * Fish Hunter 6 (E2100) game plugin: the IGS036's side of the "e2k1"
 * ShareRam protocol (the games' shareram_e2k1/pcshareram.c, M607CN's
 * shareram_e2k1v1.0.5).  docs/FishHunter6.md, docs/IGS_E2100_PROTECTION.md.
 *
 *   - variants "cn" (M601FA..M604CN) and "cn2" (M606CN), Fish-OS: Fish
 *     Hunter 4 M107CN's channel: [payload, headers, sum], RC4 per direction
 *     set once (keys 0x82675251 ASIC -> game, 0x80678267); 12 fish kinds in
 *     the catch messages.  "cn" packs the golden-fish counts (messages 34,
 *     35) as bytes and has no push-to-pay (37).
 *   - variant "6p" (M607CN, Fish6Pv2-OS): Harpoon Lagoon's channel:
 *     [running sum, payload, headers], every word XORed with a 32-bit key
 *     per direction; message 5 words 11/12 announce the game's next header
 *     register and key, message 6 word 12 the ASIC's register, message 42
 *     the ASIC's next key.  13 fish kinds; inputs, version and errors
 *     packed as bytes.
 *   - both boot through the P2A loader, which checks DONE's word 1 against
 *     its upload's CRC (none uploaded: 0).
 *   - six stations; the chip's firmware is not dumped: its game logic is
 *     rebuilt here.  Catches (message 13/25 -> 14) are drawn with
 *     probability RTP / value per fish, wins credited at value x bet; chance
 *     games, the bonus game, settings, the books (total, period, day) and
 *     the five payout modes follow the game's side of the protocol and its
 *     operator pages; the odds are invented (FISH_* tunables).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../machines/e2100/e2100.h"
#include "fishkeys.h"

#define RX_KEY 0x82675251u   /* ASIC -> game (m_rDecodeKey) */
#define TX_KEY 0x80678267u   /* game -> ASIC (m_rEncodeKey) */
#define SEATS     6          /* stations, and the protocol's arrays */
#define KINDS_MAX 13         /* fish kinds the catch messages count */

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(...) CORE->log("fishhunter6: " __VA_ARGS__)
#define VLOG(...) do { if (CORE->verbose) LOG(__VA_ARGS__); } while (0)

/* ---- RC4 ("cn"): 4-byte little-endian key, keystream over LE words ------------------ */

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

/* fish value per kind (FishInfo, the 16-bit value at +10 of each 36-byte row) */
static const uint32_t value[KINDS_MAX] = { 1, 2, 4, 3, 20, 50, 70, 30, 40, 10, 5, 100, 10 };
/* settings menu tables: key-in (开分) and key-out (洗分) credits per press */
static const uint32_t key_in_rate[6] = { 10, 20, 50, 100, 200, 500 };
static const uint32_t key_out_rate[7] = { 1, 10, 20, 50, 100, 200, 500 };
/* "coin rate": coins -> credits (SettingInfoInit's CoinRate pairs) */
static const uint32_t coin_rate[6][2] = { { 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 5 }, { 1, 10 }, { 2, 1 } };

/*
 * SettingPC (37 words; the system page, SettingSysSetting): 0 difficulty
 * 1..10 (..20 with the chance-max flag), 1 level 0..2 (hidden), 2 maximum
 * bet 1..8, 3 minimum bet 1..3, 4 coin rate 0..5, 5 bullet speed 0..1,
 * 6 score times 0..2 (hidden), 11 bet-key coin-out, 12 key-in/key-out on,
 * 13 key-in rate, 14 key-out rate, 15 payout mode, 16 tickets (1..50 =
 * one ticket per n credits, 101..103 = 10/20/30 tickets per credit),
 * 17 music, 18 sound, 19 (an on/off the CN area hides), 20..27 the
 * password digits.  Payout modes: 0 wins out as coins, 1 coins on the
 * payout key, 2 wins out as tickets, 3 tickets on the payout key, 4 wins
 * out as coins then tickets.  The game holds a station in modes 0, 1 and 3
 * while its push-to-pay flag (message 37) is set, and in 1 and 3 while its
 * payout runs.  Defaults: the chip's "ask default" answer (invented, in
 * the menu's ranges).
 */
#define NSET 37
enum { S_ODDS = 0, S_COIN = 4, S_BETKEY_OUT = 11, S_KEYS = 12, S_KEYIN = 13, S_KEYOUT = 14,
       S_PAYOUT = 15, S_TICKET = 16 };
static const uint32_t set_default[NSET] = { 5, 1, 5, 1, 3, 0, 0, 0, 0, 0, 0, 0, 1, 3, 2, 3, 1, 1, 1, 1 };

/*
 * RecordPC: 18-word rows, [0..5] the total per station (the total book,
 * SettingSysTotalBook), [6..11] this period and [12..17] the last period
 * (the current book, SettingBookNow, summed over the stations).  Rows:
 * 0 credits in, 18 credits out, 36 coins in, 72 coins out, 90 key-in, 108
 * key-out, 126 tickets out; with two keys held (station 2's bet and fire)
 * also 144 played, 162 won, their ratio, 180 shots, and on the total book
 * the 6-word rows of the chance games: 198/204 lucky wheel (tiny game)
 * times / won, 210/216 fish-hunt task (small game), 222/228 depth bomb (big
 * bomb), 234/240 super golden fish (bonus game), with 246/247 beside the
 * last two totals (jackpot fish, their win).  248 and 249 are the play and
 * on times in seconds.  The day book (SettingBookDay): 251 = days kept - 1,
 * 252.. the dates (yyyymmdd, newest first), then three 42-word columns from
 * 294: credits in, credits out, tickets out.
 */
#define NREC 420
#define DAYS 42
#define NROWS 11                      /* the 18-word rows 0..180 */
enum { R_IN = 0, R_OUT = 18, R_COIN_IN = 36, R_COIN_OUT = 72, R_KEY_IN = 90, R_KEY_OUT = 108,
       R_TICKET_OUT = 126, R_BET = 144, R_WIN = 162, R_SHOTS = 180,
       R_CHANCE = 198,                /* + 12 (k - 1): times, + 6: won; k = 1 wheel, 2 task, 3 bomb, 4 golden */
       R_JACKPOTS = 246, R_JACKPOT_WIN = 247,
       R_PLAY_TIME = 248, R_ON_TIME = 249, R_DAY_COUNT = 251, R_DAY_DATE = 252, R_DAY_COL = 294 };
enum { D_IN, D_OUT, D_TICKETS };

/* ---- state ------------------------------------------------------------------------------ */

#define CATCHQ 32

static struct {
    int six;                          /* variant 6p */
    int packed;                       /* variant cn: golden-fish counts as bytes, no message 37 */
    int kinds, fwords;                /* fish kinds in the catch messages, words per kind block */
    struct rc4 rx, tx;                /* cn */
    uint32_t rx_key, tx_key, rx_next; /* 6p */
    int rekey;

    uint32_t input[10];               /* key 8j+b = input[j] bit b */
    uint32_t set[NSET];
    uint32_t rec[NREC];
    long credit[SEATS];
    uint32_t coins[SEATS];            /* coins toward the next credit (coin rate 2:1) */
    uint32_t payout[12];              /* message 18: coins [0..5], tickets [6..11] */
    uint32_t push[SEATS];             /* message 37: push-to-pay */
    uint32_t last_bet[SEATS];
    uint32_t chance[SEATS], chance_win[SEATS];
    uint32_t big_fish[7];
    uint32_t date;                    /* the game's yyyymmdd (message 5) */
    double bet_down[SEATS];           /* bet key held since, for the bet-key coin-out */
    int help;                         /* F1: the key help */

    /* replies queued for the next batch */
    int want_version, want_settings, want_records, want_reset, want_errors;
    int credit_dirty, payout_dirty, chance_dirty, big_dirty, push_dirty;
    uint32_t catchq[CATCHQ][1 + (KINDS_MAX + 3) / 4];
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

/* ---- the books ------------------------------------------------------------------------------ */

/* today's row (0): a new date pushes the older days down */
static void day_roll(void)
{
    uint32_t *date = &g.rec[R_DAY_DATE];
    int c;

    if (!g.date || date[0] == g.date)
        return;
    memmove(date + 1, date, (DAYS - 1) * 4);
    date[0] = g.date;
    for (c = 0; c < 3; c++) {
        uint32_t *col = &g.rec[R_DAY_COL + DAYS * c];

        memmove(col + 1, col, (DAYS - 1) * 4);
        col[0] = 0;
    }
    if (date[1] && g.rec[R_DAY_COUNT] < DAYS - 1)
        g.rec[R_DAY_COUNT]++;
}

static void day_add(int col, uint32_t v)
{
    day_roll();
    g.rec[R_DAY_COL + DAYS * col] += v;
}

/* an 18-word row: the station's total and this period */
static void rec_add(int row, int p, uint32_t v)
{
    g.rec[row + p] += v;
    g.rec[row + 6 + p] += v;
}

/* message 32 (the current book's Bet key held): this period becomes the last */
static void close_period(void)
{
    int r, p;

    for (r = 0; r < NROWS; r++)
        for (p = 0; p < SEATS; p++) {
            g.rec[18 * r + 12 + p] = g.rec[18 * r + 6 + p];
            g.rec[18 * r + 6 + p] = 0;
        }
    LOG("period closed");
}

/* chance game k (1 tiny, 3 small, 4 big bomb, 5 bonus) -> its 12-word block */
static int chance_row(uint32_t k)
{
    return R_CHANCE + 12 * (k == 1 ? 0 : k == 3 ? 1 : k == 4 ? 2 : 3);
}

/* ---- credits and the payout ----------------------------------------------------------- */

static void credit_add(int p, long v)
{
    g.credit[p] += v;
    if (g.credit[p] < 0)
        g.credit[p] = 0;
    g.credit_dirty = 1;
}

/* up to c credits of station p out as coins (at the coin rate) or tickets; the credits used */
static long dispense(int p, long c, int tickets)
{
    long used;
    uint32_t n;

    if (c <= 0)
        return 0;
    if (!tickets) {
        const uint32_t *r = coin_rate[g.set[S_COIN] < 6 ? g.set[S_COIN] : 0];

        n = (uint32_t)(c * r[0] / r[1]);
        used = (long)n * r[1] / r[0];
        g.payout[p] += n;
    } else {
        uint32_t t = g.set[S_TICKET] ? g.set[S_TICKET] : 1;

        n = t > 100 ? (uint32_t)c * (10 * t - 1000) : (uint32_t)c / t;
        used = t > 100 ? c : (long)n * t;
        g.payout[6 + p] += n;
    }
    if (!n)
        return 0;
    g.credit[p] -= used;
    rec_add(R_OUT, p, (uint32_t)used);
    day_add(D_OUT, (uint32_t)used);
    /* the station holds while its payout runs */
    g.push[p] = 1;
    g.credit_dirty = g.payout_dirty = g.push_dirty = 1;
    return used;
}

/* the payout key (or the bet key held, with bet-key coin-out): all credits out */
static void pay_out(int p)
{
    uint32_t mode = g.set[S_PAYOUT];

    if (mode != 2 && mode != 3)
        dispense(p, g.credit[p], 0);
    if (mode >= 2)
        dispense(p, g.credit[p], 1);
    LOG("station %d pays out, %ld credits left", p + 1, g.credit[p]);
}

/* a win: credited; the "win" modes (0, 2, 4) pay it straight out */
static void win(int p, long v)
{
    uint32_t mode = g.set[S_PAYOUT];

    if (v <= 0)
        return;
    credit_add(p, v);
    rec_add(R_WIN, p, (uint32_t)v);
    if (mode == 0 || mode == 4)
        v -= dispense(p, v, 0);
    if (mode == 2 || mode == 4)
        dispense(p, v, 1);
}

/* ---- channel ------------------------------------------------------------------------------- */

static void reset(struct e2100_channel *ch)
{
    (void)ch;
    rc4_init(&g.rx, RX_KEY);
    rc4_init(&g.tx, TX_KEY);
    g.rx_key = RX_KEY;
    g.tx_key = TX_KEY;
    g.rekey = 0;
    g.batches = 0;
}

static void decrypt_from_game(uint32_t *w, uint32_t n)
{
    if (!g.six) {
        rc4_words(&g.tx, w, n);
        return;
    }
    while (n--)
        *w++ ^= g.tx_key;
}

static void encrypt_to_game(uint32_t *w, uint32_t n)
{
    if (!g.six) {
        rc4_words(&g.rx, w, n);
        return;
    }
    while (n--)
        *w++ ^= g.rx_key;
    /* message 42 went out under the old key: the next batch uses the new one */
    if (g.rekey) {
        g.rx_key = g.rx_next;
        g.rekey = 0;
    }
}

/* ---- the chip's game logic ------------------------------------------------------------ */

/* difficulty 1..20 (setting 0) shifts the return: 5 = FISH_RTP */
static double rtp(void)
{
    double r = g.rtp + 0.02 * (5.0 - (double)(g.set[S_ODDS] ? g.set[S_ODDS] : 5));

    return r < 0.5 ? 0.5 : r > 1.2 ? 1.2 : r;
}

static void chance_start(int p, uint32_t k)
{
    g.chance[p] = k;
    g.rec[chance_row(k) + p]++;
}

/* a chance game for the station, now and then after a shot */
static void maybe_chance(int p, uint32_t bet)
{
    double r;

    if (!g.chance_every || g.chance[p] || frand() * g.chance_every >= 1.0)
        return;
    r = g.chance_kind == 1 ? 0.0 : g.chance_kind == 3 ? 0.5 : g.chance_kind == 4 ? 0.8 : g.chance_kind == 5 ? 0.95 : frand();
    if (r < 0.4) {
        chance_start(p, 1);                                /* lucky wheel (tiny game) */
    } else if (r < 0.7) {
        chance_start(p, 3);                                /* fish-hunt task (small game): its prize */
        g.chance_win[p] = (20 + rand() % 81) * (bet ? bet : 1);
    } else if (r < 0.9) {
        chance_start(p, 4);                                /* depth bomb */
    } else {
        int q;                                             /* super golden fish: the stations in play */

        for (q = 0; q < SEATS; q++)
            if (q == p || g.credit[q] > 0) {
                chance_start(q, 5);
                g.big_fish[q] = 1 + rand() % 3;
            }
        g.big_fish[6] = 40;
        g.big_dirty = 1;
    }
    g.chance_dirty = 1;
    LOG("station %d: chance game %u", p + 1, g.chance[p]);
}

/*
 * Messages 13 / 25: [player << 16 | weapon << 8 | level, hinted fish per
 * kind (13 only), fish per kind], kinds as bytes in fwords words each.
 * Reply 14: [player << 16 | weapon << 8, caught per kind].  25 is a bomb.
 * The level is the bet (+100 for the boosted weapon).
 */
static void ask_catch(uint32_t type, const uint32_t *w)
{
    uint32_t p = (w[0] >> 16) & 0xFF, weapon = (w[0] >> 8) & 0xFF, level = w[0] & 0xFF;
    uint32_t bet = level % 100, k, *r;
    uint8_t hint[16], have[16], got[16];
    long v = 0;

    if (p >= SEATS || g.ncatch == CATCHQ)
        return;
    if (!bet || bet > 10)
        bet = g.last_bet[p] ? g.last_bet[p] : 1;
    memcpy(hint, &w[1], 4 * g.fwords);
    memcpy(have, &w[1 + g.fwords], 4 * g.fwords);
    memset(got, 0, sizeof(got));
    for (k = 0; k < (uint32_t)g.kinds; k++) {
        uint32_t n;
        double pc = rtp() / value[k];

        if (level > 100)
            pc *= 1.5;
        for (n = 0; n < have[k]; n++) {
            /* a bomb is a prize: four shots' worth on every fish */
            int catch_it = type == 25 ? frand() < 4 * pc : n < (type == 13 ? hint[k] : 0) ? frand() < 0.5 : frand() < pc;

            if (catch_it)
                got[k]++;
        }
        v += (long)got[k] * value[k] * bet;
    }
    r = g.catchq[g.ncatch++];
    memset(r, 0, sizeof(g.catchq[0]));
    r[0] = p << 16 | weapon << 8;
    memcpy(&r[1], got, 4 * g.fwords);
    if (type == 25 && g.chance[p] == 4)            /* the depth bomb's catch */
        g.rec[chance_row(4) + 6 + p] += (uint32_t)v;
    win(p, v);
    VLOG("player %u weapon %u level %u: win %ld", p + 1, weapon, level, v);
}

static void message(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *w)
{
    uint32_t i;

    if (type != 5)
        VLOG("rx msg %u len %u w0=0x%08x", type, len, len ? w[0] : 0);
    switch (type) {
    case 3:                                       /* version */
        g.want_version = 1;
        break;
    case 5:                                       /* every batch: state, bets, date, flags (6p: next register, key) */
        if (len < 11)
            break;
        g.state = w[0];
        g.date = w[7];
        for (i = 0; i < SEATS; i++) {
            uint32_t b = w[1 + i], cost;

            if (!b)
                continue;
            cost = b == 99 ? 0 : b % 100;         /* 99: the star bomb, 100+: the auto shot */
            if (cost) {
                g.last_bet[i] = cost <= 10 ? cost : g.last_bet[i];
                credit_add(i, -(long)cost);
                rec_add(R_BET, i, cost);
                rec_add(R_SHOTS, i, 1);
                maybe_chance(i, cost);
            }
        }
        if (g.six && len >= 13) {
            ch->tx_idx = w[11] & 0x3F;
            g.tx_key = w[12];
        }
        break;
    case 7:                                       /* settings, please */
        g.want_settings = 1;
        break;
    case 8:                                       /* records, please */
    case 11:
        g.want_records = 1;
        break;
    case 32:                                      /* close the period, then the records */
        close_period();
        g.want_records = 1;
        break;
    case 9:                                       /* new settings */
        for (i = 0; i < len && i < NSET; i++)
            g.set[i] = w[i];
        LOG("settings stored");
        break;
    case 13:
    case 25:
        if (len >= 1 + 2 * (uint32_t)g.fwords)
            ask_catch(type, w);
        break;
    case 16:                                      /* lamps and counters: nothing to do */
    case 17:                                      /* payout set-up: the panel follows message 18 */
        break;
    case 19:                                      /* reset all: the books and credits */
        memset(g.rec, 0, sizeof(g.rec));
        memset(g.credit, 0, sizeof(g.credit));
        memset(g.coins, 0, sizeof(g.coins));
        memset(g.payout, 0, sizeof(g.payout));
        memset(g.push, 0, sizeof(g.push));
        g.credit_dirty = g.payout_dirty = g.push_dirty = 1;
        g.want_reset = 1;
        LOG("reset all");
        break;
    case 24:                                      /* the swell fish was hit [player, level]: a lucky wheel */
        if (len >= 1 && w[0] < SEATS && !g.chance[w[0]] && frand() < 0.3) {
            chance_start(w[0], 1);
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
                long wv = (long)v * (g.last_bet[p] ? g.last_bet[p] : 1);  /* the wheel's win, in fish values */

                if (g.chance[p] == 3)
                    wv += g.chance_win[p];
                if (g.chance[p] && g.chance[p] != 5)
                    g.rec[chance_row(g.chance[p]) + 6 + p] += (uint32_t)wv;
                win(p, wv);
                g.chance[p] = 0;
                g.chance_win[p] = 0;
            }
            g.chance_dirty = 1;
        }
        break;
    case 31:                                      /* default settings */
        memcpy(g.set, set_default, sizeof(g.set));
        g.want_settings = 1;
        break;
    case 34:                                      /* golden-fish hits [player, small fish, jackpot fish] (cn: [player, small | jackpot << 8, 0, 0]) */
        if (len >= 2 && w[0] < SEATS && g.nbonus < SEATS) {
            uint32_t p = w[0], c0 = 0, c1 = 0, b;
            uint32_t n0 = g.packed ? w[1] & 0xFF : w[1], n1 = g.packed ? (w[1] >> 8) & 0xFF : len >= 3 ? w[2] : 0;
            long v;

            for (i = 0; i < n0 && i < 255; i++)
                c0 += frand() < 0.5;
            if (n1 && frand() < 0.15)
                c1 = 1 + rand() % 3;
            g.bonusq[g.nbonus][0] = p;
            g.bonusq[g.nbonus][1] = c0;
            g.bonusq[g.nbonus][2] = c1;
            g.nbonus++;
            b = g.last_bet[p] ? g.last_bet[p] : 1;
            v = (long)(10 * c0 + 100 * c1) * b;
            g.rec[chance_row(5) + 6 + p] += (uint32_t)v;
            if (c1) {
                g.rec[R_JACKPOTS]++;
                g.rec[R_JACKPOT_WIN] += 100 * c1 * b;
            }
            win(p, v);
        }
        break;
    case 21: case 27: case 29:                    /* debug, big-game win report, test data */
        break;
    }
}

/* ---- the ASIC's batch -------------------------------------------------------------------- */

/* bytes, 4 per word, little-endian */
static uint32_t pack(uint32_t *w, const uint8_t *b, uint32_t n)
{
    uint32_t i;

    memset(w, 0, (n + 3) / 4 * 4);
    for (i = 0; i < n; i++)
        w[i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
    return (n + 3) / 4;
}

static void add_input(struct e2100_batch *b)
{
    uint32_t w[10];
    uint8_t keys[10];
    int i;

    if (!g.six) {
        api->batch_add(b, 2, g.input, 10);
        return;
    }
    for (i = 0; i < 10; i++)
        keys[i] = (uint8_t)g.input[i];
    api->batch_add(b, 2, w, pack(w, keys, 10));
}

static void add_version(struct e2100_batch *b)
{
    static const char ver[8] = "FH6-036", rom[8] = "HLE";
    uint32_t w[19], i;

    if (g.six) {                                  /* [fuse << 16, version, rom] */
        w[0] = 0;
        memcpy(&w[1], ver, 8);
        memcpy(&w[3], rom, 8);
        api->batch_add(b, 4, w, 5);
        return;
    }
    memset(w, 0, sizeof(w));                      /* 8 chars, 8 chars, fuse, two words */
    for (i = 0; i < 8; i++) {
        w[i] = (uint8_t)ver[i];
        w[8 + i] = (uint8_t)rom[i];
    }
    api->batch_add(b, 4, w, 19);
}

static void build(struct e2100_channel *ch, struct e2100_batch *b)
{
    uint32_t w[48], i;
    double now = CORE->now();

    if (g.batches++ == 0) {
        g.credit_dirty = g.payout_dirty = g.chance_dirty = g.push_dirty = 1;
        g.want_errors = 1;
        g.last_tick = g.last_sec = now;
        if (g.six) {
            /* the next ASIC -> game key, and the asset key (the FPGA mask
               comes from the software list) */
            static const uint8_t akey[16] = "Fish6PHunterV2.0";

            g.rx_next = (uint32_t)rand() << 16 ^ (uint32_t)rand();
            g.rekey = 1;
            api->batch_add(b, 42, &g.rx_next, 1);
            for (i = 0; i < 16; i++)
                w[i] = akey[i];
            api->batch_add(b, 43, w, 16);
        }
    }
    /* the payout runs down, one ticket or coin every 100 ms */
    if (now - g.last_tick >= 0.1) {
        g.last_tick = now;
        for (i = 0; i < 12; i++)
            if (g.payout[i]) {
                g.payout[i]--;
                if (i < 6) {
                    rec_add(R_COIN_OUT, i, 1);
                } else {
                    rec_add(R_TICKET_OUT, i - 6, 1);
                    day_add(D_TICKETS, 1);
                }
                /* the station is free again when its payout is through */
                if (!g.payout[i % 6] && !g.payout[6 + i % 6] && g.push[i % 6]) {
                    g.push[i % 6] = 0;
                    g.push_dirty = 1;
                }
                g.payout_dirty = 1;
            }
    }
    /* bet-key coin-out: the bet key held 2 s pays the station out */
    if (g.set[S_BETKEY_OUT])
        for (i = 0; i < SEATS; i++)
            if (g.bet_down[i] && now - g.bet_down[i] >= 2.0) {
                g.bet_down[i] = 0;
                pay_out(i);
            }
    if (now - g.last_sec >= 1.0) {
        g.last_sec += 1.0;
        g.rec[R_ON_TIME]++;
        if (g.state)
            g.rec[R_PLAY_TIME]++;
    }
    if (g.want_records) {                         /* 420 words: alone with the inputs */
        day_roll();
        add_input(b);
        api->batch_add(b, 12, g.rec, NREC);
        g.want_records = 0;
        return;
    }
    add_input(b);
    if (g.want_version) {
        add_version(b);
        g.want_version = 0;
    }
    if (g.want_settings) {
        api->batch_add(b, 10, g.set, NSET);
        g.want_settings = 0;
    }
    if (g.want_errors) {                          /* ErrorPC: no master or station errors */
        memset(w, 0, 40 * 4);
        api->batch_add(b, 15, w, g.six ? 12 : 40);
        g.want_errors = 0;
    }
    if (g.want_reset) {
        w[0] = 1;
        api->batch_add(b, 20, w, 1);
        g.want_reset = 0;
    }
    if (g.credit_dirty) {                         /* per station a coin flag and the credits (6p: + register) */
        for (i = 0; i < SEATS; i++) {
            w[i] = 0;
            w[6 + i] = (uint32_t)g.credit[i];
        }
        w[12] = ch->rx_idx;
        api->batch_add(b, 6, w, g.six ? 13 : 12);
        g.credit_dirty = 0;
    }
    if (g.payout_dirty) {
        api->batch_add(b, 18, g.payout, 12);
        g.payout_dirty = 0;
    }
    if (g.push_dirty && !g.packed) {
        api->batch_add(b, 37, g.push, SEATS);
        g.push_dirty = 0;
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
    while (g.ncatch && b->npayload + 1 + g.fwords <= E2100_BATCH_MAX && b->nhdrs < 16) {
        api->batch_add(b, 14, g.catchq[0], 1 + g.fwords);
        memmove(g.catchq[0], g.catchq[1], sizeof(g.catchq[0]) * --g.ncatch);
    }
    for (i = 0; i < (uint32_t)g.nbonus && b->nhdrs < 16; i++) {
        if (g.packed) {                           /* [player, small | multiplier << 8, 0, 0] */
            w[0] = g.bonusq[i][0];
            w[1] = g.bonusq[i][1] | g.bonusq[i][2] << 8;
            w[2] = w[3] = 0;
            api->batch_add(b, 35, w, 4);
        } else {
            api->batch_add(b, 35, g.bonusq[i], 3);
        }
    }
    g.nbonus = 0;
}

/* ---- input ---------------------------------------------------------------------------------- */

/* the keys: fishkeys.h (shared with Fish Hunter Final 6) */
#define COIN   FK_COIN
#define KEYIN  FK_KEYIN
#define KEYOUT FK_KEYOUT
#define PAYOUT FK_PAYOUT
#define keymap fish_keymap

static void key(int sym, int down)
{
    unsigned i;

    if (sym == 282) {                             /* F1 */
        if (down)
            g.help = !g.help;
        return;
    }
    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        int bit = keymap[i].bit, p = bit & 0xFF;

        if (keymap[i].sym != sym)
            continue;
        if (bit & COIN) {
            if (down) {
                const uint32_t *r = coin_rate[g.set[S_COIN] < 6 ? g.set[S_COIN] : 0];

                rec_add(R_COIN_IN, p, 1);
                if (++g.coins[p] >= r[0]) {
                    g.coins[p] = 0;
                    credit_add(p, r[1]);
                    rec_add(R_IN, p, r[1]);
                    day_add(D_IN, r[1]);
                }
                LOG("coin at station %d: %ld credits", p + 1, g.credit[p]);
            }
        } else if (bit & KEYIN) {                 /* the attendant's key-in, when switched on */
            if (down && g.set[S_KEYS]) {
                uint32_t r = key_in_rate[g.set[S_KEYIN] < 6 ? g.set[S_KEYIN] : 0];

                credit_add(p, r);
                rec_add(R_KEY_IN, p, r);
                rec_add(R_IN, p, r);
                day_add(D_IN, r);
                LOG("key-in at station %d: %ld credits", p + 1, g.credit[p]);
            }
        } else if (bit & KEYOUT) {                /* the attendant's key-out: the rate's credits, or what is left */
            if (down && g.set[S_KEYS] && g.credit[p] > 0) {
                long r = key_out_rate[g.set[S_KEYOUT] < 7 ? g.set[S_KEYOUT] : 0];

                if (r > g.credit[p])
                    r = g.credit[p];
                credit_add(p, -r);
                rec_add(R_KEY_OUT, p, (uint32_t)r);
                rec_add(R_OUT, p, (uint32_t)r);
                day_add(D_OUT, (uint32_t)r);
                LOG("key-out at station %d: %ld credits", p + 1, g.credit[p]);
            }
        } else if (bit & PAYOUT) {
            if (down)
                pay_out(p);
        } else {
            if (down)
                g.input[bit >> 3] |= 1u << (bit & 7);
            else
                g.input[bit >> 3] &= ~(1u << (bit & 7));
            if ((bit & 7) == 4 && bit < 8 * SEATS)
                g.bet_down[bit >> 3] = down ? CORE->now() : 0;
        }
    }
}

/* the key help, bottom left */
static void draw(int w, int h)
{
    (void)w;
    if (g.help)
        fish_keys_draw(api->core, h, SEATS);
}

static void status(char *buf, size_t n){
    snprintf(buf, n, "credits %ld / %ld / %ld / %ld / %ld / %ld",
             g.credit[0], g.credit[1], g.credit[2], g.credit[3], g.credit[4], g.credit[5]);
}

static struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Fish Hunter 6",
    .status = status,
    .reset = reset,
    .decrypt_from_game = decrypt_from_game,
    .encrypt_to_game = encrypt_to_game,
    .message = message,
    .build = build,
    .key = key,
    .draw = draw,
    /* both loaders check DONE's word 1 against the upload's CRC: none, 0 */
    .handshake = E2100_HS_NOSUM,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    const char *v = getenv("IGS_VARIANT");
    long credits;
    int i;

    api = a;
    g.six = v && !strcmp(v, "6p");
    g.packed = !v || !strcmp(v, "cn");
    g.help = 1;
    g.kinds = g.six ? 13 : 12;
    g.fwords = (g.kinds + 3) / 4;
    game.layout = g.six ? E2100_LAYOUT_RUNNING : E2100_LAYOUT_TRAILING;
    memcpy(g.set, set_default, sizeof(g.set));
    g.rtp = atof(CORE->env("FISH_RTP", "0.95"));
    g.chance_every = atoi(CORE->env("FISH_CHANCE", "300"));
    g.chance_kind = atoi(CORE->env("FISH_CHANCE_KIND", "0"));
    credits = atol(CORE->env("FISH_CREDITS", "0"));
    for (i = 0; i < SEATS; i++)
        g.credit[i] = credits;
    srand((unsigned)(CORE->now() * 1000.0) ^ (unsigned)time(NULL));
    return &game;
}
