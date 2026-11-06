/*
 * Fish Hunter 6 (E2100) game plugin: the IGS036's side of the "e2k1"
 * ShareRam protocol (the games' shareram_e2k1/pcshareram.c, M607CN's
 * shareram_e2k1v1.0.5).  wiki Fish Hunter 6, wiki E2100 Protection.
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
 *   - variant "plus" (Fish Hunter Plus 6P M602CN, Fish6Pv2-OS): "6p" with
 *     the lobster (kind 12, value 200), push-to-pay as message 38, the
 *     ASIC's next key 43 and the asset key 44, two more 18-word book rows
 *     before the chance games (456-word records), and the game's MAC in
 *     message 45.
 *   - variants "skl" (Fish Hunter SKL M104CN, FishSKL-OS: "6p") and
 *     "sklcn" (M103CN: "cn2"): 14 fish kinds, the golden fish and golden
 *     bream (120, 250) counted with the rest.
 *   - variant "demon" (Demon Hunter M202CN, Hunter_Demon OS): "6p" with its
 *     own keys (0xCD4CABED ASIC -> game, 0xADECB0A7), every message from 24
 *     up one lower in both directions (24 the bomb, 27 chance over, 41 the
 *     next key, 42 the asset key), 14 kinds, four stations in six-station
 *     messages, and chance games 1..3 per station, 4 the bonus for all.
 *     "demon100" (M100CN): the next key is 40, no asset key or push-to-pay,
 *     13 kinds, the reset (20) without a word.
 *   - variant "skl2_8p" (Fish Hunter SKL2 8P M801CN, E2200): SKL2 for eight
 *     stations with a Sentinel HASP key in place of the Sentinel Keys
 *     dongle (Crazy Shoot's layout: read-only memory 0..15 the asset key,
 *     16 / 20 the first keys, 24 the sum's start, 28 / 32 / 36 the words the
 *     chip returns in message 46).  The 8-station arrays: inputs 16 bytes
 *     (Test bit 104), credits 17 words (register 16), payout 16, push 8,
 *     chance kinds 8 nibbles and 8 prizes, golden fish 9, errors 14,
 *     version 7; the game's message 5 is 15 words (date 9, register 13, key
 *     14); a catch is asked as player << 18 | shot << 8 | level and answered
 *     player << 16 | shot << 6; messages 37, 42, 43, 45 become 38, 43, 44,
 *     46.  The book: 24-word rows, 16-word chance blocks, 567 words.
 *   - all boot through the P2A loader, which checks DONE's word 1 against
 *     its upload's CRC (none uploaded: 0).
 *   - six (or eight) stations; the chip's firmware is not dumped: its game logic is
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
#include "../fishhunterfinal6/sentinel.h"

#define RX_KEY 0x82675251u   /* ASIC -> game (m_rDecodeKey) */
#define TX_KEY 0x80678267u   /* game -> ASIC (m_rEncodeKey) */
#define MAXSEATS  8          /* stations: six, eight on SKL2 8P */
#define SEATS     g.seats

/*
 * SKL2's Sentinel dongle, data object 47 (decrypted by AES feature 43): 0..15
 * the asset key (to the FPGA; the loader's cipher takes the list's mask), 16
 * the game's first key, 20 the chip's, 24 the running sum's start, 188 / 192
 * / 196 three words the chip must return in message 45.  The values are
 * the plugin's own; the cabinet's dongle is not dumped.
 */
static uint8_t dongle_raw[208];
/*
 * SKL2 8P's Sentinel HASP key (feature 2): the read-only memory (file
 * 0xFFF5), 40 bytes that THaspKeyPro reads and hasp_decrypts (stored here
 * as the plaintext, the key's encrypt / decrypt are the identity): 0..15
 * the asset key, 16 the game's first key, 20 the chip's, 24 the running
 * sum's start, 28 / 32 / 36 the words the chip returns in message 46.
 * Invented values; the cabinet's key is not dumped.
 */
static uint8_t hasp_ro[112], hasp_rw[128];
#define KINDS_MAX 17         /* fish kinds the catch messages count */

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
static const uint32_t value_fh6[KINDS_MAX] = { 1, 2, 4, 3, 20, 50, 70, 30, 40, 10, 5, 100, 10 };
/* Plus 6P: kind 12 is the lobster (FishInfo at 0x82CD140; the golden fish follow, not counted) */
static const uint32_t value_plus[KINDS_MAX] = { 1, 2, 4, 3, 20, 50, 70, 30, 40, 10, 5, 100, 200 };
/* SKL: 14 kinds, the golden fish (12) and golden bream (13) among them (FishInfo at 0x82CB060) */
static const uint32_t value_skl[KINDS_MAX] = { 1, 2, 4, 3, 20, 50, 70, 30, 40, 10, 5, 100, 120, 250 };
/* SKL2: 16 fish (FishInfo at 0x832AA00); the catch messages carry a 17th byte, never a fish */
static const uint32_t value_skl2[KINDS_MAX] = { 1, 2, 4, 3, 20, 50, 70, 30, 40, 10, 5, 100, 150, 500, 120, 250, 0 };
/* Demon Hunter: 14 kinds (FishInfo at 0x82CFB80, 28-byte rows, the value at +12) */
static const uint32_t value_demon[KINDS_MAX] = { 1, 2, 4, 3, 20, 50, 70, 30, 40, 10, 5, 100, 200, 120 };
static const uint32_t *value = value_fh6;
/* settings menu tables: key-in (开分) and key-out (洗分) credits per press */
static const uint32_t key_in_rate[6] = { 10, 20, 50, 100, 200, 500 };
static const uint32_t key_out_rate[7] = { 1, 10, 20, 50, 100, 200, 500 };
/* "coin rate": coins -> credits (SettingInfoInit's CoinRate pairs) */
static const uint32_t coin_rate_fh6[7][2] = { { 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 5 }, { 1, 10 }, { 2, 1 } };
/* SKL2 (CoinRate 0x8257660): seven, 1:4 added */
static const uint32_t coin_rate_skl2[7][2] = { { 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 4 }, { 1, 5 }, { 1, 10 }, { 2, 1 } };
#define coin_rate (g.skl2 ? coin_rate_skl2 : coin_rate_fh6)
#define NCOIN (g.skl2 ? 7u : 6u)
#define MAX_BET (g.skl2 ? 30u : 10u)       /* SKL2's maximum bet goes to 20 and 30 */

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
 * 294: credits in, credits out, tickets out.  Plus 6P has two more rows
 * (198, 216; no page shows them) and everything after moves up 36 words
 * (wheel 234, day count 287, records 456 words).
 */
#define NREC_MAX 600
#define DAYS 42
#define NROWS 11                      /* the 18-word rows 0..180 (SKL2 8P: 24-word rows 0..240) */
/* row r starts at r * RW: RW = 3 stations' worth (total, this period, last period) */
#define RW            (3 * SEATS)
#define R_IN          0
#define R_OUT         RW
#define R_COIN_IN     (2 * RW)
#define R_COIN_OUT    (4 * RW)
#define R_KEY_IN      (5 * RW)
#define R_KEY_OUT     (6 * RW)
#define R_TICKET_OUT  (7 * RW)
#define R_BET         (8 * RW)
#define R_WIN         (9 * RW)
#define R_SHOTS       (10 * RW)
/* + 2 SEATS (k - 1): times, + SEATS: won; k = 1 wheel, 2 task, 3 bomb, 4 golden */
#define R_CHANCE      (11 * RW + g.shift)
#define R_JACKPOTS    (R_CHANCE + 8 * SEATS)
#define R_JACKPOT_WIN (R_JACKPOTS + 1)
#define R_LIGHT_GOLD  (R_JACKPOT_WIN + 1 + 2 * SEATS)   /* SKL2: the searchlight's golden fish */
#define R_PLAY_TIME   g.r_play
#define R_ON_TIME     (g.r_play + 1)
#define R_DAY_COUNT   g.r_days
#define R_DAY_DATE    (g.r_days + 1)
#define R_DAY_COL     (g.r_days + 43)
#define NREC          (g.r_days + 169)
enum { D_IN, D_OUT, D_TICKETS };

/* ---- state ------------------------------------------------------------------------------ */

#define CATCHQ 32

static struct {
    int seats;                        /* stations: 6, SKL2 8P 8 */
    int eight;                        /* variant skl2_8p: the HASP key and the 8-station layouts */
    int six;                          /* variants 6p, plus */
    int plus;                         /* variant plus: messages 38, 43, 44; the book's extra rows */
    int skl;                          /* variants skl ("6p"), sklcn ("cn2"): 14 fish kinds */
    int shift;                        /* words the plus book inserts before the chance rows */
    int r_play, r_days;               /* the play time and the day count */
    int skl2;                         /* variant skl2: the Sentinel dongle, LoaderAdv, 17-byte catches, packed 23, 45 */
    int demon;                        /* variants demon, demon100: types >= 24 one lower, its keys, chance 4 the bonus */
    int demon100;                     /* M100CN: next key 40, no asset key or push, 13 kinds, reset 20 empty */
    uint32_t msg_push, msg_key, msg_akey, msg_big, msg_gold;
    uint32_t rx0, tx0;                /* the first keys */
    int packed;                       /* variant cn: golden-fish counts as bytes, no message 37 */
    int kinds, fwords;                /* fish kinds in the catch messages, words per kind block */
    struct rc4 rx, tx;                /* cn */
    uint32_t rx_key, tx_key, rx_next; /* 6p */
    int rekey;

    uint32_t input[16];               /* key 8j+b = input[j] bit b */
    uint32_t set[NSET];
    uint32_t rec[NREC_MAX];
    long credit[MAXSEATS];
    uint32_t coins[MAXSEATS];         /* coins toward the next credit (coin rate 2:1) */
    uint32_t payout[2 * MAXSEATS];    /* message 18: coins [0..n-1], tickets [n..2n-1] */
    uint32_t push[MAXSEATS];          /* message 37: push-to-pay */
    uint32_t last_bet[MAXSEATS];
    uint32_t chance[MAXSEATS], chance_win[MAXSEATS];
    uint32_t big_fish[MAXSEATS + 1];  /* message 26: per station, then the total */
    uint32_t date;                    /* the game's yyyymmdd (message 5) */
    double bet_down[MAXSEATS];        /* bet key held since, for the bet-key coin-out */
    int help;                         /* F1: the key help */

    /* replies queued for the next batch */
    int want_version, want_settings, want_records, want_reset, want_errors;
    int credit_dirty, payout_dirty, chance_dirty, big_dirty, push_dirty;
    uint32_t catchq[CATCHQ][1 + (KINDS_MAX + 3) / 4];
    int ncatch;
    uint32_t bonusq[MAXSEATS][3];
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

/* a row: the station's total and this period */
static void rec_add(int row, int p, uint32_t v)
{
    g.rec[row + p] += v;
    g.rec[row + SEATS + p] += v;
}

/* message 32 (the current book's Bet key held): this period becomes the last */
static void close_period(void)
{
    int r, p;

    for (r = 0; r < NROWS; r++)
        for (p = 0; p < SEATS; p++) {
            g.rec[RW * r + 2 * SEATS + p] = g.rec[RW * r + SEATS + p];
            g.rec[RW * r + SEATS + p] = 0;
        }
    LOG("period closed");
}

/* chance game k (1 tiny, 3 small, 4 big bomb, 5 bonus; SKL2 6 the all-station event) -> its block (times, won) */
static int chance_row(uint32_t k)
{
    if (k == 6)                       /* SKL2: after the jackpot pair, 248 (8P 378) */
        return R_JACKPOT_WIN + 1;
    if (g.demon)                      /* Demon Hunter: 1, 2, 3 per station, 4 the bonus */
        return R_CHANCE + 2 * SEATS * (k >= 1 && k <= 4 ? k - 1 : 3);
    return R_CHANCE + 2 * SEATS * (k == 1 ? 0 : k == 3 ? 1 : k == 4 ? 2 : 3);
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
        const uint32_t *r = coin_rate[g.set[S_COIN] < NCOIN ? g.set[S_COIN] : 0];

        n = (uint32_t)(c * r[0] / r[1]);
        used = (long)n * r[1] / r[0];
        g.payout[p] += n;
    } else {
        uint32_t t = g.set[S_TICKET] ? g.set[S_TICKET] : 1;

        n = t > 100 ? (uint32_t)c * (10 * t - 1000) : (uint32_t)c / t;
        used = t > 100 ? c : (long)n * t;
        g.payout[SEATS + p] += n;
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
    g.rx_key = g.rx0;
    g.tx_key = g.tx0;
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
    if (g.demon) {
        int q, busy = 0, bonus = 0;

        for (q = 0; q < SEATS; q++) {
            busy |= g.chance[q] != 0;
            bonus |= g.chance[q] == 4;
        }
        r = g.chance_kind == 1 ? 0.0 : g.chance_kind == 2 ? 0.3 : g.chance_kind == 3 ? 0.6 : g.chance_kind == 4 ? 0.95 : frand();
        if (bonus)                                         /* nothing new while the bonus waits */
            return;
        if (r < 0.9) {                                     /* 1, 2, 3: the station's own game */
            chance_start(p, r < 0.3 ? 1 : r < 0.6 ? 2 : 3);
        } else if (!busy) {                                /* 4 the bonus: every station in play (the game has four) */
            for (q = 0; q < 4; q++)
                if (q == p || g.credit[q] > 0) {
                    chance_start(q, 4);
                    g.big_fish[q] = 1 + rand() % 3;            /* message 25: the station's jackpot pumpkins */
                }
            g.big_fish[SEATS] = 40;                            /* and the pumpkins in all */
            g.big_dirty = 1;
        } else {
            return;
        }
        g.chance_dirty = 1;
        LOG("station %d: chance game %u", p + 1, g.chance[p]);
        return;
    }
    r = g.chance_kind == 1 ? 0.0 : g.chance_kind == 3 ? 0.5 : g.chance_kind == 4 ? 0.8 : g.chance_kind == 5 ? 0.95 : g.chance_kind == 6 ? 1.0 : frand();
    if (g.skl2 && (r >= 1.0 || (!g.chance_kind && frand() < 0.1))) {
        int q;                                             /* SKL2: the all-station event, every station in play */

        for (q = 0; q < SEATS; q++)
            if ((q == p || g.credit[q] > 0) && !g.chance[q])
                chance_start(q, 6);
    } else if (r < 0.4) {
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
        g.big_fish[SEATS] = 40;
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
    /* SKL2 8P: player << 18 | shot << 8 (10 bits) | level, answered player << 16 | shot << 6 */
    uint32_t p = g.eight ? w[0] >> 18 : (w[0] >> 16) & 0xFF;
    uint32_t weapon = g.eight ? (w[0] >> 8) & 0x3FF : (w[0] >> 8) & 0xFF, level = w[0] & 0xFF;
    uint32_t bet = level % 100, k, *r;
    uint8_t hint[20], have[20], got[20];
    long v = 0;

    if (p >= SEATS || g.ncatch == CATCHQ)
        return;
    if (!bet || bet > MAX_BET)
        bet = g.last_bet[p] ? g.last_bet[p] : 1;
    memcpy(hint, &w[1], 4 * g.fwords);
    memcpy(have, &w[1 + g.fwords], 4 * g.fwords);
    memset(got, 0, sizeof(got));
    for (k = 0; k < (uint32_t)g.kinds; k++) {
        uint32_t n;
        double pc;

        if (!value[k])                /* SKL2's 17th byte: no fish */
            continue;
        pc = rtp() / value[k];

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
    r[0] = p << 16 | weapon << (g.eight ? 6 : 8);
    memcpy(&r[1], got, 4 * g.fwords);
    if (type == 25 && g.chance[p] == 4 && !g.demon) /* the depth bomb's catch */
        g.rec[chance_row(4) + SEATS + p] += (uint32_t)v;
    win(p, v);
    VLOG("player %u weapon %u level %u: win %ld", p + 1, weapon, level, v);
}

static void message(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *w)
{
    uint32_t i;

    if (type != 5)
        VLOG("rx msg %u len %u w0=0x%08x", type, len, len ? w[0] : 0);
    if (g.demon && type >= 24)                    /* Demon Hunter has no 24 (the swell fish): the rest move down */
        type++;
    switch (type) {
    case 3:                                       /* version */
        g.want_version = 1;
        break;
    case 5:                                       /* every batch: state, bets, date, flags (6p: next register, key) */
        if (len < (g.eight ? 15u : 11u))
            break;
        g.state = w[0];
        g.date = w[1 + SEATS];                    /* 8P: bets 1..8, the date 9 */
        for (i = 0; i < SEATS; i++) {
            uint32_t b = w[1 + i], cost;

            if (!b)
                continue;
            cost = b == 99 ? 0 : b % 100;         /* 99: the star bomb, 100+: the auto shot */
            if (cost) {
                g.last_bet[i] = cost <= MAX_BET ? cost : g.last_bet[i];
                credit_add(i, -(long)cost);
                rec_add(R_BET, i, cost);
                rec_add(R_SHOTS, i, 1);
                maybe_chance(i, cost);
            }
        }
        if (g.eight) {                            /* 8P: the next register 13, key 14 */
            ch->tx_idx = w[13] & 0x3F;
            g.tx_key = w[14];
        } else if (g.six && len >= 13) {
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

            if (v == 0xFF0F) {                    /* the bonus game (SKL2: or the searchlight; Demon Hunter: after every game) is over for everyone */
                for (i = 0; i < SEATS; i++)
                    if (g.demon ? g.chance[i] == 4 : g.chance[i] == 5 || g.chance[i] == 6)
                        g.chance[i] = 0;
                memset(g.big_fish, 0, sizeof(g.big_fish));
            } else if (v == 0xFF0E) {             /* the station's assigned game timed out (SKL2): no prize */
                if (p < SEATS)
                    g.chance[p] = g.chance_win[p] = 0;
            } else if (p < SEATS) {
                long wv = (long)v * (g.last_bet[p] ? g.last_bet[p] : 1);  /* the wheel's win, in fish values */

                if (g.chance[p] == 3)
                    wv += g.chance_win[p];
                if (g.chance[p] && (g.demon || (g.chance[p] != 5 && g.chance[p] != 6)))
                    g.rec[chance_row(g.chance[p]) + SEATS + p] += (uint32_t)wv;
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
            /* SKL2's searchlight (chance 6): its golden fish pay 500..1000 */
            int light = g.skl2 && g.chance[p] == 6;
            long v;

            for (i = 0; i < n0 && i < 255; i++)
                c0 += frand() < 0.5;
            /* Demon Hunter's big pumpkin pays 300..1000 */
            if (n1 && frand() < (light ? 0.3 : 0.15))
                c1 = light ? 5 + rand() % 6 : g.demon ? 3 + rand() % 8 : 1 + rand() % 3;
            g.bonusq[g.nbonus][0] = p;
            g.bonusq[g.nbonus][1] = c0;
            g.bonusq[g.nbonus][2] = c1;
            g.nbonus++;
            b = g.last_bet[p] ? g.last_bet[p] : 1;
            v = (long)(10 * c0 + 100 * c1) * b;
            g.rec[chance_row(light ? 6 : g.demon ? 4 : 5) + SEATS + p] += (uint32_t)v;
            if (c1 && light) {
                g.rec[R_LIGHT_GOLD]++;                /* beside the searchlight's total */
            } else if (c1) {
                g.rec[R_JACKPOTS]++;
                g.rec[R_JACKPOT_WIN] += 100 * c1 * b;
            }
            win(p, v);
        }
        break;
    case 21: case 27: case 29:                    /* debug, big-game win report, test data */
        break;
    case 37:                                      /* plus: a request no code path sends */
        break;
    case 46:                                      /* SKL2: the dongle's words 188, 192, 196 (the chip answers them in 45) */
        break;
    case 44:                                      /* SKL2, at start: the MAC, as plus's 45 */
    case 45:                                      /* plus, SKL2 8P, at start: eth0's (else eth1's) MAC, bytes 0..3 and 4..5 */
        if ((type == 45) == (!g.skl2 || g.eight) && len >= 2)
            VLOG("game MAC %02x:%02x:%02x:%02x:%02x:%02x", w[0] & 0xFF, w[0] >> 8 & 0xFF, w[0] >> 16 & 0xFF,
                w[0] >> 24, w[1] & 0xFF, w[1] >> 8 & 0xFF);
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
    uint32_t w[16];
    uint8_t keys[16];
    int i, n = g.eight ? 16 : 10;                 /* 8P: 16 bytes, 112 keys */

    if (!g.six) {
        api->batch_add(b, 2, g.input, 10);
        return;
    }
    for (i = 0; i < n; i++)
        keys[i] = (uint8_t)g.input[i];
    api->batch_add(b, 2, w, pack(w, keys, n));
}

static void add_version(struct e2100_batch *b)
{
    static const char ver[8] = "FH6-036", rom[8] = "HLE";
    uint32_t w[19], i;

    if (g.six) {                                  /* [fuse << 16, version, rom] (8P: two words more, unused) */
        w[0] = 0;
        memcpy(&w[1], ver, 8);
        memcpy(&w[3], rom, 8);
        w[5] = w[6] = 0;
        api->batch_add(b, 4, w, g.eight ? 7 : 5);
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
            static const uint8_t akey[16] = "Fish6PHunterV2.0";   /* Demon Hunter keys its FPGA AES with it */

            g.rx_next = (uint32_t)rand() << 16 ^ (uint32_t)rand();
            g.rekey = 1;
            api->batch_add(b, g.msg_key, &g.rx_next, 1);
            for (i = 0; i < 16; i++)
                w[i] = g.eight ? hasp_ro[i] : g.skl2 ? dongle_raw[i] : akey[i];
            if (g.msg_akey)                       /* M100CN has none: its FPGA key is its own */
                api->batch_add(b, g.msg_akey, w, 16);
            /* SKL2: the dongle's words 188, 192, 196 (8P: the HASP key's 28, 32, 36);
               the game hangs unless they match */
            if (g.eight)
                api->batch_add(b, 46, (const uint32_t *)(hasp_ro + 28), 3);
            else if (g.skl2)
                api->batch_add(b, 45, (const uint32_t *)(dongle_raw + 188), 3);
        }
    }
    /* the payout runs down, one ticket or coin every 100 ms */
    if (now - g.last_tick >= 0.1) {
        g.last_tick = now;
        for (i = 0; i < 2u * SEATS; i++)
            if (g.payout[i]) {
                uint32_t p = i % SEATS;

                g.payout[i]--;
                if (i < (uint32_t)SEATS) {
                    rec_add(R_COIN_OUT, p, 1);
                } else {
                    rec_add(R_TICKET_OUT, p, 1);
                    day_add(D_TICKETS, 1);
                }
                /* the station is free again when its payout is through */
                if (!g.payout[p] && !g.payout[SEATS + p] && g.push[p]) {
                    g.push[p] = 0;
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
        api->batch_add(b, 15, w, g.eight ? 14 : g.six ? 12 : 40);
        g.want_errors = 0;
    }
    if (g.want_reset) {
        w[0] = 1;
        api->batch_add(b, 20, w, g.demon100 ? 0 : 1);   /* M100CN reads no word */
        g.want_reset = 0;
    }
    if (g.credit_dirty) {                         /* per station a coin flag and the credits (6p: + register) */
        for (i = 0; i < SEATS; i++) {
            w[i] = 0;
            w[SEATS + i] = (uint32_t)g.credit[i];
        }
        w[2 * SEATS] = ch->rx_idx;
        api->batch_add(b, 6, w, 2 * SEATS + (g.six ? 1 : 0));
        g.credit_dirty = 0;
    }
    if (g.payout_dirty) {
        api->batch_add(b, 18, g.payout, 2 * SEATS);
        g.payout_dirty = 0;
    }
    if (g.push_dirty && !g.packed && g.msg_push) {
        api->batch_add(b, g.msg_push, g.push, SEATS);
        g.push_dirty = 0;
    }
    if (g.big_dirty) {
        api->batch_add(b, g.msg_big, g.big_fish, SEATS + 1);
        g.big_dirty = 0;
    }
    if (g.chance_dirty && g.skl2) {               /* SKL2: kinds as nibbles, prizes as halfwords */
        memset(w, 0, 5 * 4);
        for (i = 0; i < SEATS; i++) {
            w[0] |= (g.chance[i] & 0xF) << (4 * i);
            w[1 + i / 2] |= (g.chance_win[i] & 0xFFFF) << (16 * (i & 1));
        }
        api->batch_add(b, 23, w, 1 + SEATS / 2);
        g.chance_dirty = 0;
    }
    if (g.chance_dirty) {
        for (i = 0; i < SEATS; i++) {
            w[i] = g.chance[i];
            w[SEATS + i] = g.chance_win[i];
        }
        api->batch_add(b, 23, w, 2 * SEATS);
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
            api->batch_add(b, g.msg_gold, g.bonusq[i], 3);
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

static void key_bit(int bit, int down);

static void key(int sym, int down)
{
    unsigned i;

    if (sym == 282) {                             /* F1 */
        if (down)
            g.help = !g.help;
        return;
    }
    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
        if (keymap[i].sym == sym)
            key_bit(g.eight && keymap[i].bit == 78 ? 104 : keymap[i].bit, down);   /* 8P: Test is 104 */
    if (g.eight)
        for (i = 0; i < sizeof(fish_keymap8) / sizeof(fish_keymap8[0]); i++)
            if (fish_keymap8[i].sym == sym)
                key_bit(fish_keymap8[i].bit, down);
}

static void key_bit(int bit, int down)
{
    /* SKL2 8P: the cabinet's station labels 1..8 (the key help's rows) are
       the protocol's stations 0, 6, 1, 5, 3, 7, 4, 2 (the I/O test page, the
       books' columns) */
    static const int label8[8] = { 0, 6, 1, 5, 3, 7, 4, 2 };
    int p;

    if (g.eight) {
        if (bit & (COIN | KEYIN | KEYOUT | PAYOUT))
            bit = (bit & ~0xFF) | label8[bit & 7];
        else if (bit < 64)
            bit = 8 * label8[bit >> 3] + (bit & 7);
    }
    p = bit & 0xFF;

    {                                             /* a station's key, or a chip action */
        if (bit & COIN) {
            if (down) {
                const uint32_t *r = coin_rate[g.set[S_COIN] < NCOIN ? g.set[S_COIN] : 0];

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
        fish_keys_draw(api->core, h, g.demon ? 4 : SEATS);
}

static void status(char *buf, size_t n)
{
    size_t o;
    int i;

    o = (size_t)snprintf(buf, n, "credits");
    for (i = 0; i < SEATS && o < n; i++)
        o += (size_t)snprintf(buf + o, n - o, "%s%ld", i ? " / " : " ", g.credit[i]);
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

static void dongle(void)
{
    static const uint32_t words[3] = { 0x534B4C32, 0x46484E54, 0x00000105 };   /* invented */
    struct sentinel_key k;
    uint32_t v;

    memcpy(dongle_raw, "FishHunterSKLv2!", 16);
    v = TX_KEY;
    memcpy(dongle_raw + 16, &v, 4);
    v = RX_KEY;
    memcpy(dongle_raw + 20, &v, 4);
    memcpy(dongle_raw + 188, words, 12);
    memset(&k, 0, sizeof(k));
    k.developer_id = 0x5E07A0E1;
    k.raw_feature = 47;
    k.aes_feature = 43;
    memcpy(k.aes_key, "IGS-FHSK2-SNT-43", 16);           /* invented: the key's own */
    memcpy(k.raw, dongle_raw, sizeof(dongle_raw));
    k.raw_len = sizeof(dongle_raw);
    if (sentinel_install(CORE, &k) < 0)
        LOG("cannot emulate the Sentinel dongle");
}

/* ---- SKL2 8P's HASP key, at the HASP client's API (statically linked) ------------------- */

#define HASP_FILEID_RW 0xFFF4
#define HASP_FILEID_RO 0xFFF5

static int hasp_login(int feature, const void *vendor, int *handle)
{
    (void)vendor;
    VLOG("hasp_login feature %d", feature);
    if (handle)
        *handle = 1;
    return 0;
}

static int hasp_logout(int handle) { (void)handle; return 0; }

static int hasp_read(int handle, int file, unsigned off, unsigned len, void *buf)
{
    const uint8_t *m = file == HASP_FILEID_RW ? hasp_rw : hasp_ro;
    size_t n = file == HASP_FILEID_RW ? sizeof(hasp_rw) : sizeof(hasp_ro);

    (void)handle;
    if (file != HASP_FILEID_RW && file != HASP_FILEID_RO)
        return 9;                                 /* HASP_INV_FILEID */
    if ((size_t)off + len > n)
        return 8;                                 /* HASP_MEM_RANGE */
    memcpy(buf, m + off, len);
    return 0;
}

static int hasp_write(int handle, int file, unsigned off, unsigned len, const void *buf)
{
    (void)handle;
    if (file != HASP_FILEID_RW)
        return 9;
    if ((size_t)off + len > sizeof(hasp_rw))
        return 8;
    memcpy(hasp_rw + off, buf, len);
    return 0;
}

static int hasp_crypt(int handle, void *buf, int len) { (void)handle; (void)buf; (void)len; return 0; }

static void hasp_install(void)
{
    /* the client's entries in M801CN's executable (THaspKeyPro's callers, 0x806A750..) */
    static const struct { uint32_t addr; void *fn; } api_[] = {
        { 0x8214880, (void *)hasp_login }, { 0x8214820, (void *)hasp_logout },
        { 0x8213750, (void *)hasp_read }, { 0x82136A0, (void *)hasp_write },
        { 0x8214780, (void *)hasp_crypt }, { 0x8214700, (void *)hasp_crypt },
    };
    static const uint32_t words[3] = { 0x534B4C38, 0x46485038, 0x00000801 };   /* invented */
    uint32_t v;
    unsigned i;

    memcpy(hasp_ro, "FishHunterSKL2-8", 16);
    v = TX_KEY;
    memcpy(hasp_ro + 16, &v, 4);
    v = RX_KEY;
    memcpy(hasp_ro + 20, &v, 4);
    memcpy(hasp_ro + 28, words, 12);
    for (i = 0; i < sizeof(api_) / sizeof(api_[0]); i++)
        if (!CORE->hook || CORE->hook(api_[i].addr, api_[i].fn))
            LOG("cannot hook the HASP client at 0x%08x", api_[i].addr);
}

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    const char *v = getenv("IGS_VARIANT");
    long credits;
    int i;

    api = a;
    g.eight = v && !strcmp(v, "skl2_8p");
    g.seats = g.eight ? 8 : 6;
    g.plus = v && !strcmp(v, "plus");
    g.skl = v && (!strcmp(v, "skl") || !strcmp(v, "sklcn"));
    g.skl2 = v && (!strcmp(v, "skl2") || g.eight);
    g.demon = v && (!strcmp(v, "demon") || !strcmp(v, "demon100"));
    g.demon100 = v && !strcmp(v, "demon100");
    g.six = g.plus || g.skl2 || g.demon || (v && (!strcmp(v, "6p") || !strcmp(v, "skl")));
    g.packed = !v || !strcmp(v, "cn");
    g.shift = g.plus || g.eight ? 2 * RW : 0;    /* two more rows before the chance games */
    g.msg_push = g.plus || g.eight ? 38 : g.demon100 ? 0 : g.demon ? 36 : 37;
    g.msg_key = g.plus || g.eight ? 43 : g.demon100 ? 40 : g.demon ? 41 : 42;
    g.msg_akey = g.plus || g.eight ? 44 : g.demon100 ? 0 : g.demon ? 42 : 43;
    g.msg_big = g.demon ? 25 : 26;
    g.msg_gold = g.demon ? 34 : 35;
    g.rx0 = g.demon ? 0xCD4CABEDu : RX_KEY;
    g.tx0 = g.demon ? 0xADECB0A7u : TX_KEY;
    /* Demon Hunter M100CN: 13 kinds, Plus 6P's values (FishInfo at 0x82C6320, 24-byte rows, the value at +12) */
    value = g.plus || g.demon100 ? value_plus : g.skl ? value_skl : g.skl2 ? value_skl2 : g.demon ? value_demon : value_fh6;
    g.r_play = g.skl2 ? R_LIGHT_GOLD + 1 : R_JACKPOT_WIN + 1;   /* SKL2 261 (8P 395), else 248 + shift */
    g.r_days = g.r_play + 3;
    if (g.eight) {
        /* the HASP key is read before the card is opened; the executable is mapped */
        hasp_install();
        game.name = "Fish Hunter SKL2 8P";
        game.handshake = E2100_HS_ADV;
    } else if (g.skl2) {
        /* the game reads the dongle before it opens the card; the executable is mapped */
        dongle();
        game.name = "Fish Hunter SKL2";
        game.handshake = E2100_HS_ADV;
    }
    if (g.plus)
        game.name = "Fish Hunter Plus 6P";
    if (g.skl)
        game.name = "Fish Hunter SKL";
    if (g.demon)
        game.name = "Demon Hunter";
    g.help = 1;
    g.kinds = g.skl2 ? 17 : g.skl || (g.demon && !g.demon100) ? 14 : g.six ? 13 : 12;
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
