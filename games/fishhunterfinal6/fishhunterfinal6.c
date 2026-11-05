/*
 * Fish Hunter Final 6 (E2100) game plugin: the IGS036's side of the "e2k1"
 * ShareRam protocol, and the cabinet's Sentinel Keys dongle (sentinel.c).
 * docs/FishHunterFinal6.md, docs/IGS_E2100_PROTECTION.md.
 *
 *   - boot: the game reads its channel keys, the running sum's start and
 *     three check words from the dongle (data object 49, decrypted by its
 *     AES feature 43); without them it hangs.
 *   - channel: [running sum, payload, headers], every word XORed with a
 *     32-bit key per direction.  The game announces its next header register
 *     and key in message 5 words 9 / 10; the ASIC its next register in
 *     message 6 word 12 and, once, its key in message 37.
 *   - four or six stations (settings word 48); the chip's firmware is not
 *     dumped: its logic is rebuilt here.  Fish values (message 54), catches
 *     (13 -> 14), the bomb fish (24 -> 43), the extra-fish chain (44 -> 45),
 *     the fish king, settings, the books (total, period, day) and the five
 *     payout modes follow the game's side of the protocol and its operator
 *     pages; the odds are invented (FISH_* tunables).  The chip credits a
 *     catch when it answers; the game's win reports (42, 52) go to the books.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../machines/e2100/e2100.h"
#include "../fishhunter6/fishkeys.h"
#include "sentinel.h"

#define SEATS 6                /* the protocol's arrays */
#define KINDS 19               /* fish kinds; the catch messages carry 20 bytes */

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(...) CORE->log("fishhunterfinal6: " __VA_ARGS__)
#define VLOG(...) do { if (CORE->verbose) LOG(__VA_ARGS__); } while (0)

/* ---- the dongle -------------------------------------------------------------------------- */

/*
 * Data object 49 (80 bytes), as the game reads it (PcShareRam_MainInit,
 * main init, the fish spawner): 0..15 the asset key (the FPGA's; the
 * emulated FPGA takes the list's mask), 16 the game's first key, 20 the
 * chip's, 24 the running sum's start (the machine starts it at 0), 28 / 32 /
 * 36 check words, 40.. three 12-byte rows of which row 2 (64..75) is the
 * number of fish of kinds 0..11 the opening scenes keep on screen.
 */
#define TX_KEY 0x80678267u     /* game -> chip */
#define RX_KEY 0x82675251u     /* chip -> game */
static const uint8_t school[12] = { 10, 10, 8, 8, 6, 6, 5, 5, 4, 4, 3, 3 };

static void dongle(void)
{
    static const uint32_t check[3] = { 0x100002D0, 0x10000300, 0x10000320 };
    struct sentinel_key k;
    uint32_t w;
    int i;

    memset(&k, 0, sizeof(k));
    k.developer_id = 0x5E07A0E1;
    k.raw_feature = 49;
    k.aes_feature = 43;
    memcpy(k.aes_key, "IGS-FHF6-SNTL-43", 16);       /* invented: the key's own */
    k.raw_len = 80;
    memcpy(k.raw, "FishHunterFinal6", 16);
    w = TX_KEY;
    memcpy(k.raw + 16, &w, 4);
    w = RX_KEY;
    memcpy(k.raw + 20, &w, 4);
    for (i = 0; i < 3; i++)
        memcpy(k.raw + 28 + 4 * i, &check[i], 4);
    for (i = 0; i < 3; i++)
        memcpy(k.raw + 40 + 12 * i, school, 12);
    if (sentinel_install(CORE, &k) < 0)
        LOG("cannot emulate the Sentinel dongle");
}

/* ---- the game's tables ------------------------------------------------------------------- */

/*
 * Fish values (message 54 row word 1; the game has none): 0 gurnard ... 12
 * manta ray, 13 shark, 14 killer whale, 15 humpback whale, 16 mermaid, 17
 * treasure turtle, 18 bomb fish.  Kinds 13, 14, 17 get their value with
 * each catch (vmin..vmax); the table holds their middle.
 */
static const uint32_t value[KINDS] = { 2, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 80, 175, 150, 200, 35, 20 };
static const uint32_t vmin[KINDS] = { [13] = 40, [14] = 100, [17] = 10 };
static const uint32_t vmax[KINDS] = { [13] = 120, [14] = 250, [17] = 60 };

/* SettingSysSetting's tables, by the stored index */
static const uint32_t key_in_rate[20] = { 10, 20, 30, 40, 50, 100, 150, 200, 250, 300, 350, 400, 450, 500,
                                          1000, 2000, 3000, 4000, 5000, 10000 };
static const uint32_t key_out_rate[21] = { 1, 10, 20, 30, 40, 50, 100, 150, 200, 250, 300, 350, 400, 450, 500,
                                           1000, 2000, 3000, 4000, 5000, 10000 };

/* 1 coin = n credits (index 1..33) */
static uint32_t coin_rate(uint32_t i)
{
    return i < 1 ? 1 : i <= 10 ? i : i <= 14 ? 10 * (i - 9) : i <= 33 ? 100 + 50 * (i - 15) : 1000;
}

/* credits per ticket (stored 101..128) */
static uint32_t ticket_rate(uint32_t i)
{
    return i <= 110 ? (i > 100 ? i - 100 : 1) : i <= 119 ? 10 * (i - 109) : i <= 128 ? 100 * (i - 118) : 1000;
}

/* credit-in limit (进分上限, index 1..9) */
static uint32_t in_limit(uint32_t i)
{
    return 100000 + 50000 * (i >= 1 && i <= 9 ? i - 1 : 0);
}

/*
 * Settings (50 words; the system page, SettingSysSetting): 0 difficulty
 * 0..7 (..9 with the ceiling in 47), 1 maximum bet, 2 minimum bet, 3 bet
 * step, 4 coin rate, 5 key-in/key-out on, 6 key-in rate, 7 key-out rate,
 * 8 business mode, 9 ticket rate, 10 credit-in limit, 11 machine-full limit
 * (the game's warning), 12 payout-key function (Bet held 2 s pays out),
 * 13 music, 14 sound and voice, 15 timed fire, 16 volatility (low, high),
 * 17 a hidden bet bound, 19 weapon type, 20 score times, 26..37 the stored
 * password digits, 47 the difficulty ceiling, 48 stations (low nibble
 * configured, high requested), 49 mode.  Business modes as Fish Hunter
 * 6's: 0 wins out as coins, 1 coins on the payout key, 2 wins out as
 * tickets, 3 tickets on the payout key, 4 wins out as coins then tickets.
 * Defaults: the chip's answer to message 28 (invented, in the menu's
 * ranges); the password is twelve zeros.
 */
#define NSET 50
enum { S_ODDS = 0, S_COIN = 4, S_KEYS = 5, S_KEYIN = 6, S_KEYOUT = 7, S_PAYOUT = 8, S_TICKET = 9,
       S_IN_LIMIT = 10, S_BETKEY_OUT = 12, S_SWING = 16, S_STATIONS = 48 };
static const uint32_t set_default[NSET] = {
    2, 11, 1, 1, 10, 1, 5, 6, 3, 110, 1, 9, 0, 1, 1, 1, 0, 3,
};

/*
 * Records (934 words): every item a pair [lo, hi] = lo + 100000 hi.  Rows
 * of 18 items: station p's total, this period (6 + p), the last period
 * (12 + p).  Rows 0 credits in, 1 credits out, 2 coins in, 4 coins out,
 * 5 key-in, 6 key-out, 7 tickets out, 8 played, 9 won, 10 shots; totals
 * only (6 items) at 540.. for the fish king (???, the whirlwind fish),
 * the extra-fish chain (??, a group the book does not show) and the bomb
 * crab (???) (times, won); 636 / 637 play and on time (seconds); the day
 * book: 639 today's slot (0..41, circular), 640.. dates yyyymmdd, then per
 * day credits in (682..), credits out (766..), tickets out (850..).
 */
#define NREC 934
#define DAYS 42
#define NROWS 11
enum { R_IN = 0, R_OUT = 1, R_COIN_IN = 2, R_COIN_OUT = 4, R_KEY_IN = 5, R_KEY_OUT = 6, R_TICKET_OUT = 7,
       R_BET = 8, R_WIN = 9, R_SHOTS = 10 };
enum { G_KING, G_CHAIN, G_BOMB };   /* the 6-item rows at 540 + 24 g: times, + 12: won */
#define R_PLAY_TIME 636
#define R_ON_TIME   637
#define R_DAY_SLOT  639
#define R_DAY_DATE  640
#define R_DAY_COL   682
enum { D_IN, D_OUT, D_TICKETS };

/* ---- state ------------------------------------------------------------------------------- */

#define CATCHQ 32

static struct {
    int hooked;
    uint32_t rx_key, tx_key, rx_next_key, rx_next_idx;
    int rekey, reidx;

    uint32_t input[10];
    uint32_t set[NSET];
    uint32_t rec[NREC];
    long credit[SEATS];
    uint32_t payout[12];              /* message 18: coins [0..5], tickets [6..11] */
    uint32_t push[SEATS];             /* message 32: push-to-pay */
    uint32_t bomb_bet[SEATS];         /* the bet that caught the bomb fish */
    uint32_t chain_bet[SEATS], chain_n[SEATS];
    uint32_t date, state;
    double bet_down[SEATS];           /* Bet held since, for the payout-key function */
    int help, stations;

    int want_version, want_settings, want_records, want_reset, want_values, want_errors;
    int credit_dirty, payout_dirty, push_dirty;
    uint32_t q14[CATCHQ][8], q43[CATCHQ][8], q45[CATCHQ][2];
    int n14, n43, n45;

    uint32_t batches;
    double last_tick, last_sec;
    /* tunables */
    double rtp;
    double chain_go;
    int force;                        /* [Testing] 1 lightning fish, 2 fish king: always caught */
} g;

static double frand(void) { return rand() / (RAND_MAX + 1.0); }

/* ---- the books ---------------------------------------------------------------------------- */

static void pair_add(uint32_t *p, uint32_t v)
{
    uint64_t t = (uint64_t)p[0] + 100000ull * p[1] + v;

    p[0] = (uint32_t)(t % 100000);
    p[1] = (uint32_t)(t / 100000);
}

/* a row's item: the station's total and this period */
static void rec_add(int row, int p, uint32_t v)
{
    pair_add(&g.rec[36 * row + 2 * p], v);
    pair_add(&g.rec[36 * row + 2 * (6 + p)], v);
}

static void game_add(int game, int won, int p, uint32_t v)
{
    pair_add(&g.rec[540 + 24 * game + 12 * won + 2 * p], v);
}

/* today's slot: a new date takes the next one */
static void day_roll(void)
{
    uint32_t s = g.rec[R_DAY_SLOT] % DAYS;
    int c;

    if (!g.date || g.rec[R_DAY_DATE + s] == g.date)
        return;
    if (g.rec[R_DAY_DATE + s])
        s = (s + 1) % DAYS;
    g.rec[R_DAY_SLOT] = s;
    g.rec[R_DAY_DATE + s] = g.date;
    for (c = 0; c < 3; c++)
        g.rec[R_DAY_COL + 2 * DAYS * c + 2 * s] = g.rec[R_DAY_COL + 2 * DAYS * c + 2 * s + 1] = 0;
}

static void day_add(int col, uint32_t v)
{
    day_roll();
    pair_add(&g.rec[R_DAY_COL + 2 * DAYS * col + 2 * (g.rec[R_DAY_SLOT] % DAYS)], v);
}

/* message 29 (the current book's Fire held 3 s): this period becomes the last */
static void close_period(void)
{
    int r, p;

    for (r = 0; r < NROWS; r++)
        for (p = 0; p < SEATS; p++) {
            memcpy(&g.rec[36 * r + 2 * (12 + p)], &g.rec[36 * r + 2 * (6 + p)], 8);
            g.rec[36 * r + 2 * (6 + p)] = g.rec[36 * r + 2 * (6 + p) + 1] = 0;
        }
    LOG("period closed");
}

/* ---- credits and the payout ------------------------------------------------------------- */

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
    uint32_t r = tickets ? ticket_rate(g.set[S_TICKET]) : coin_rate(g.set[S_COIN]);
    uint32_t n;
    long used;

    if (c <= 0 || !r)
        return 0;
    n = (uint32_t)(c / r);
    if (!n)
        return 0;
    used = (long)n * r;
    g.payout[(tickets ? 6 : 0) + p] += n;
    g.credit[p] -= used;
    rec_add(R_OUT, p, (uint32_t)used);
    day_add(D_OUT, (uint32_t)used);
    /* the station holds while its payout runs */
    g.push[p] = 1;
    g.credit_dirty = g.payout_dirty = g.push_dirty = 1;
    return used;
}

/* the payout key: all credits out */
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
    if (!g.hooked) {
        g.hooked = 1;
        dongle();
    }
    g.rx_key = RX_KEY;
    g.tx_key = TX_KEY;
    g.rekey = g.reidx = 0;
    g.batches = 0;
}

static void decrypt_from_game(uint32_t *w, uint32_t n)
{
    while (n--)
        *w++ ^= g.tx_key;
}

/* after this batch: the key it announced (message 37) */
static void encrypt_to_game(uint32_t *w, uint32_t n)
{
    while (n--)
        *w++ ^= g.rx_key;
    if (g.rekey) {
        g.rx_key = g.rx_next_key;
        g.rekey = 0;
    }
}

/* ---- the chip's game logic -------------------------------------------------------------- */

/* difficulty 0..9 (setting 0) shifts the return: 2 (the default) = FISH_RTP */
static double rtp(void)
{
    double r = g.rtp + 0.02 * (2.0 - (double)g.set[S_ODDS]);

    return r < 0.5 ? 0.5 : r > 1.2 ? 1.2 : r;
}

/* volatility (setting 16): high swings each shot's odds to 0.25 or 1.75 times, the same on average */
static double swing(void)
{
    return !g.set[S_SWING] ? 1.0 : frand() < 0.5 ? 0.25 : 1.75;
}

/* the value of one fish of kind k: drawn for the shark, killer whale and treasure turtle */
static uint32_t fish_value(int k)
{
    if (vmax[k])
        return vmin[k] + (uint32_t)rand() % (vmax[k] - vmin[k] + 1);
    return value[k];
}

static void bytes_get(uint8_t *b, const uint32_t *w, int n)
{
    int i;

    for (i = 0; i < n; i++)
        b[i] = (uint8_t)(w[i / 4] >> (8 * (i % 4)));
}

static void bytes_put(uint32_t *w, const uint8_t *b, int n)
{
    int i;

    memset(w, 0, (size_t)(n + 3) / 4 * 4);
    for (i = 0; i < n; i++)
        w[i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
}

/* b0, b1, b2: the drawn values of kinds 13, 14, 17 (M = 1) */
static uint32_t special_word(const uint32_t *v)
{
    return v[13] | v[14] << 8 | v[17] << 16;
}

/*
 * Message 13: [bet, player << 16 | bullet, 20 bytes hit per kind (bit 7: a
 * fish king of that kind among them; byte 19: 1 + the kind of an extra-fish
 * starter), 20 bytes chain size per king kind].  Reply 14: [player << 17 |
 * bullet << 1, 20 bytes caught per kind (bit 6: the first starts the
 * extra-fish chain; bit 7: the king's chain catches its whole kind; byte
 * 19: the starter starts the chain), M, b0 | b1 << 8 | b2 << 16].
 */
static void ask_catch(const uint32_t *w)
{
    uint32_t bet = w[0], p = (w[1] >> 16) & 0xFF, bullet = w[1] & 0xFFFF, v[KINDS], k, *r;
    uint8_t hit[20], chain[20], got[20];
    double odds = swing();
    long won = 0;

    if (p >= SEATS || g.n14 == CATCHQ)
        return;
    if (!bet || bet > 1000)
        bet = 1;
    bytes_get(hit, &w[2], 20);
    bytes_get(chain, &w[7], 20);
    memset(got, 0, sizeof(got));
    memset(v, 0, sizeof(v));
    for (k = 0; k < KINDS; k++) {
        uint32_t n = hit[k] & 0x7F, i, c = 0;
        double pc;

        if (!hit[k])
            continue;
        v[k] = fish_value((int)k);
        if (hit[k] & 0x80) {
            /* the fish king: its whole kind, or nothing */
            uint32_t size = chain[k] ? chain[k] : n;

            VLOG("station %u: fish king of kind %u hit, %u fish", p + 1, k, size);
            if ((g.force & 2) || frand() < odds * rtp() / ((double)v[k] * size)) {
                got[k] = 0x80;
                won += (long)size * v[k] * bet;
                game_add(G_KING, 0, (int)p, 1);
                LOG("station %u: fish king %u, %u fish", p + 1, k, size);
            }
            continue;
        }
        pc = odds * rtp() / v[k];
        for (i = 0; i < n; i++)
            c += frand() < pc;
        if (!c)
            continue;
        got[k] = (uint8_t)c;
        if (k == 18)
            g.bomb_bet[p] = bet;              /* the bomb follows */
        won += (long)c * v[k] * bet;
    }
    /*
     * The lightning fish (byte 19: 1 + its kind) caught on its own roll
     * starts the extra-fish chain (送燈); the chain pays.  Bit 6 would make the
     * first hit fish of a kind the starter: the game stops ("(nP)非閃電魚啟動
     * 送燈") unless that fish is the lightning fish, so it is not used.
     */
    if (hit[19])
        VLOG("station %u: lightning fish of kind %u hit", p + 1, hit[19] - 1u);
    if (hit[19] && hit[19] <= KINDS && !g.chain_n[p] &&
        ((g.force & 1) || frand() < odds * rtp() / value[hit[19] - 1])) {
        got[19] = hit[19];
        g.chain_bet[p] = bet;
        g.chain_n[p] = 1;
        game_add(G_CHAIN, 0, (int)p, 1);
        LOG("station %u: the lightning fish starts the extra-fish chain", p + 1);
    }
    r = g.q14[g.n14++];
    r[0] = p << 17 | bullet << 1;
    bytes_put(&r[1], got, 20);
    r[6] = 1;
    r[7] = special_word(v);
    win((int)p, won);
    VLOG("station %u bullet %u bet %u: win %ld", p + 1, bullet, bet, won);
}

/* message 24: the bomb's blast [player, 20 bytes hit per kind]; reply 43 */
static void ask_bomb(const uint32_t *w)
{
    uint32_t p = w[0], bet, v[KINDS], k, *r;
    uint8_t hit[20], got[20];
    long won = 0;

    if (p >= SEATS || g.n43 == CATCHQ)
        return;
    bet = g.bomb_bet[p] ? g.bomb_bet[p] : 1;
    bytes_get(hit, &w[1], 20);
    memset(got, 0, sizeof(got));
    memset(v, 0, sizeof(v));
    for (k = 0; k < KINDS; k++) {
        uint32_t i, c = 0;
        double pc;

        if (!hit[k])
            continue;
        v[k] = fish_value((int)k);
        pc = 4 * rtp() / v[k];                /* a prize: four shots' worth on every fish */
        for (i = 0; i < hit[k]; i++)
            c += frand() < pc;
        got[k] = (uint8_t)c;
        won += (long)c * v[k] * bet;
    }
    r = g.q43[g.n43++];
    r[0] = p << 1;
    bytes_put(&r[1], got, 20);
    r[6] = 1;
    r[7] = special_word(v);
    game_add(G_BOMB, 0, (int)p, 1);
    win((int)p, won);
    LOG("station %u: the bomb fish wins %ld", p + 1, won);
}

/*
 * Message 44: the next fish for the extra-fish chain [player, 20 bytes
 * candidate kinds]; reply 45 [player | kind << 8 (255: the end), the value
 * of a shark or killer whale].  Each round goes on with FISH_CHAIN's
 * probability; the game stops after 20 fish.  The chain counts as running
 * (chain_n 1..21) until the game's report (message 42 type 3): a lightning
 * fish caught while the game still runs one would stop it ("送燈魚重覆啟動").
 */
static void ask_chain(const uint32_t *w)
{
    uint32_t p = w[0], k, n = 0, pick = 255, val = 0, *r;
    uint8_t cand[20];

    if (p >= SEATS || g.n45 == CATCHQ)
        return;
    bytes_get(cand, &w[1], 20);
    for (k = 0; k < KINDS; k++)
        n += cand[k] != 0;
    if (n && g.chain_n[p] && g.chain_n[p] <= 20 && frand() < g.chain_go) {
        uint32_t i = (uint32_t)rand() % n;

        for (k = 0; k < KINDS; k++)
            if (cand[k] && !i--)
                break;
        pick = k;
        val = fish_value((int)k);
        win((int)p, (long)val * (g.chain_bet[p] ? g.chain_bet[p] : 1));
        g.chain_n[p]++;
    } else if (g.chain_n[p]) {
        g.chain_n[p] = 21;                        /* over: waiting for the report */
    }
    r = g.q45[g.n45++];
    r[0] = p | pick << 8;
    r[1] = val;
}

static void message(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *w)
{
    uint32_t i;

    if (type != 5 && type != 16)
        VLOG("rx msg %u len %u w0=0x%08x", type, len, len ? w[0] : 0);
    switch (type) {
    case 3:                                       /* version, please */
        g.want_version = 1;
        break;
    case 5:                                       /* every batch: state, a shot's bet per station, date, next register, key */
        if (len < 11)
            break;
        g.state = w[0];
        g.date = w[7];
        for (i = 0; i < SEATS; i++)
            if (w[1 + i]) {
                credit_add((int)i, -(long)w[1 + i]);
                rec_add(R_BET, (int)i, w[1 + i]);
                rec_add(R_SHOTS, (int)i, 1);
            }
        ch->tx_idx = w[9] & 0x3F;
        g.tx_key = w[10];
        break;
    case 7:                                       /* settings, please */
        g.want_settings = 1;
        break;
    case 8:                                       /* records, please */
    case 11:
        g.want_records = 1;
        break;
    case 29:                                      /* close the period, then the records */
        close_period();
        g.want_records = 1;
        break;
    case 9:                                       /* new settings (7 follows) */
        for (i = 0; i < len && i < NSET; i++)
            g.set[i] = w[i];
        LOG("settings stored");
        break;
    case 28:                                      /* default settings */
        memcpy(g.set, set_default, sizeof(g.set));
        g.set[S_STATIONS] = 0x11 * (uint32_t)g.stations;
        g.want_settings = 1;
        break;
    case 13:
        if (len >= 12)
            ask_catch(w);
        break;
    case 24:
        if (len >= 6)
            ask_bomb(w);
        break;
    case 44:
        if (len >= 6)
            ask_chain(w);
        break;
    case 19:                                      /* reset all: the books and credits */
        memset(g.rec, 0, sizeof(g.rec));
        memset(g.credit, 0, sizeof(g.credit));
        memset(g.payout, 0, sizeof(g.payout));
        memset(g.push, 0, sizeof(g.push));
        g.credit_dirty = g.payout_dirty = g.push_dirty = 1;
        g.want_reset = 1;
        LOG("reset all");
        break;
    case 42:                                      /* a win report [player << 8 | type, bet, win] */
        if (len >= 3 && (w[0] >> 8) < SEATS) {
            uint32_t t = w[0] & 0xFF;

            if (t == 1)
                game_add(G_BOMB, 1, (int)(w[0] >> 8), w[2]);
            else if (t == 3) {
                game_add(G_CHAIN, 1, (int)(w[0] >> 8), w[2]);
                g.chain_n[w[0] >> 8] = 0;         /* the chain is over */
            }
        }
        break;
    case 52:                                      /* fish-king wins per station */
        for (i = 0; i < len && i < SEATS; i++)
            if (w[i])
                game_add(G_KING, 1, (int)i, w[i]);
        break;
    case 39:                                      /* the PC's MAC address */
        if (len >= 2)
            VLOG("MAC %02x:%02x:%02x:%02x:%02x:%02x", w[0] & 0xFF, w[0] >> 8 & 0xFF, w[0] >> 16 & 0xFF,
                 w[0] >> 24, w[1] & 0xFF, w[1] >> 8 & 0xFF);
        break;
    case 16:                                      /* lamps, counter pulses */
    case 26:                                      /* output test */
    case 46:                                      /* debug */
        break;
    }
}

/* ---- the chip's batch --------------------------------------------------------------------- */

static void add_input(struct e2100_batch *b)
{
    uint32_t w[4];
    uint8_t keys[12];
    int i;

    memset(keys, 0, sizeof(keys));
    for (i = 0; i < 10; i++)
        keys[i] = (uint8_t)g.input[i];
    bytes_put(w, keys, 12);
    w[3] = 0;                                     /* DIP switches */
    api->batch_add(b, 2, w, 4);
}

static void add_version(struct e2100_batch *b)
{
    uint32_t w[7];

    memset(w, 0, sizeof(w));                      /* fuses 0, "inter", "rom": 7 chars and a 0 each */
    memcpy(&w[1], "FHF6036", 7);
    memcpy(&w[3], "HLE", 3);
    api->batch_add(b, 4, w, 7);
}

static void add_values(struct e2100_batch *b)
{
    uint32_t w[KINDS * 6];
    int k;

    memset(w, 0, sizeof(w));
    for (k = 0; k < KINDS; k++) {
        w[6 * k] = 1;                             /* the big ones may spawn */
        w[6 * k + 1] = value[k];
    }
    api->batch_add(b, 54, w, KINDS * 6);
}

static void build(struct e2100_channel *ch, struct e2100_batch *b)
{
    uint32_t w[16], i;
    double now = CORE->now();

    if (g.batches++ == 0) {
        g.credit_dirty = g.payout_dirty = g.push_dirty = 1;
        g.want_errors = g.want_values = 1;
        g.last_tick = g.last_sec = now;
        /* the chip's key, taken once; this batch still goes out under the old one */
        g.rx_next_key = (uint32_t)rand() << 16 ^ (uint32_t)rand();
        g.rekey = 1;
        api->batch_add(b, 37, &g.rx_next_key, 1);
    }
    /* the header register announced in message 6 is the chip's from this batch on */
    if (g.reidx) {
        ch->rx_idx = g.rx_next_idx;
        g.reidx = 0;
    }
    /* the payout runs down, one ticket or coin every 100 ms */
    if (now - g.last_tick >= 0.1) {
        g.last_tick = now;
        for (i = 0; i < 12; i++)
            if (g.payout[i]) {
                g.payout[i]--;
                if (i < 6) {
                    rec_add(R_COIN_OUT, (int)i, 1);
                } else {
                    rec_add(R_TICKET_OUT, (int)i - 6, 1);
                    day_add(D_TICKETS, 1);
                }
                if (!g.payout[i % 6] && !g.payout[6 + i % 6] && g.push[i % 6]) {
                    g.push[i % 6] = 0;
                    g.push_dirty = 1;
                }
                g.payout_dirty = 1;
            }
    }
    /* the payout-key function: Bet held 2 s pays the station out */
    if (g.set[S_BETKEY_OUT])
        for (i = 0; i < SEATS; i++)
            if (g.bet_down[i] && now - g.bet_down[i] >= 2.0) {
                g.bet_down[i] = 0;
                pay_out((int)i);
            }
    if (now - g.last_sec >= 1.0) {
        int playing = 0;

        g.last_sec += 1.0;
        g.rec[R_ON_TIME]++;
        for (i = 0; i < SEATS; i++)
            playing |= g.credit[i] > 0;
        if (g.state == 0 && playing)
            g.rec[R_PLAY_TIME]++;
    }
    if (g.want_records) {                         /* 934 words: alone with the inputs */
        day_roll();
        add_input(b);
        api->batch_add(b, 12, g.rec, NREC);
        g.want_records = 0;
        return;
    }
    add_input(b);
    if (g.want_values) {
        add_values(b);
        g.want_values = 0;
    }
    if (g.want_version) {
        add_version(b);
        g.want_version = 0;
    }
    if (g.want_settings) {
        /* the stations this cabinet has: the requested count (high nibble) */
        g.set[S_STATIONS] = (g.set[S_STATIONS] & ~0xF0u) | (uint32_t)g.stations << 4;
        if (!(g.set[S_STATIONS] & 0xF))
            g.set[S_STATIONS] |= (uint32_t)g.stations;
        api->batch_add(b, 10, g.set, NSET);
        g.want_settings = 0;
    }
    if (g.want_errors) {                          /* no master or station errors */
        memset(w, 0, 12 * 4);
        api->batch_add(b, 15, w, 12);
        g.want_errors = 0;
    }
    if (g.want_reset) {
        w[0] = 1;
        api->batch_add(b, 20, w, 1);
        g.want_reset = 0;
    }
    if (g.credit_dirty) {                         /* per station 0 and the credits, the next register */
        for (i = 0; i < SEATS; i++) {
            w[i] = 0;
            w[6 + i] = (uint32_t)g.credit[i];
        }
        g.rx_next_idx = (uint32_t)rand() % 64;
        g.reidx = 1;
        w[12] = g.rx_next_idx;
        api->batch_add(b, 6, w, 13);
        g.credit_dirty = 0;
    }
    if (g.payout_dirty) {
        api->batch_add(b, 18, g.payout, 12);
        g.payout_dirty = 0;
    }
    if (g.push_dirty) {
        api->batch_add(b, 32, g.push, SEATS);
        g.push_dirty = 0;
    }
    while (g.n14 && b->npayload + 8 <= E2100_BATCH_MAX && b->nhdrs < 16) {
        api->batch_add(b, 14, g.q14[0], 8);
        memmove(g.q14[0], g.q14[1], sizeof(g.q14[0]) * --g.n14);
    }
    while (g.n43 && b->npayload + 8 <= E2100_BATCH_MAX && b->nhdrs < 16) {
        api->batch_add(b, 43, g.q43[0], 8);
        memmove(g.q43[0], g.q43[1], sizeof(g.q43[0]) * --g.n43);
    }
    while (g.n45 && b->npayload + 2 <= E2100_BATCH_MAX && b->nhdrs < 16) {
        api->batch_add(b, 45, g.q45[0], 2);
        memmove(g.q45[0], g.q45[1], sizeof(g.q45[0]) * --g.n45);
    }
}

/* ---- input ---------------------------------------------------------------------------------- */

static void key(int sym, int down)
{
    unsigned i;

    if (sym == 282) {                             /* F1 */
        if (down)
            g.help = !g.help;
        return;
    }
    for (i = 0; i < sizeof(fish_keymap) / sizeof(fish_keymap[0]); i++) {
        int bit = fish_keymap[i].bit, p = bit & 0xFF;

        if (fish_keymap[i].sym != sym)
            continue;
        if (bit & (FK_COIN | FK_KEYIN | FK_KEYOUT | FK_PAYOUT)) {
            if (!down || p >= g.stations)
                continue;
            /* coins and key-in stop at the credit-in limit; key-in/out need their switch */
            if ((bit & (FK_COIN | FK_KEYIN)) && g.credit[p] >= (long)in_limit(g.set[S_IN_LIMIT]))
                continue;
            if ((bit & (FK_KEYIN | FK_KEYOUT)) && !g.set[S_KEYS])
                continue;
            if (bit & FK_COIN) {
                uint32_t r = coin_rate(g.set[S_COIN]);

                credit_add(p, r);
                rec_add(R_COIN_IN, p, 1);
                rec_add(R_IN, p, r);
                day_add(D_IN, r);
                LOG("coin at station %d: %ld credits", p + 1, g.credit[p]);
            } else if (bit & FK_KEYIN) {          /* the attendant's key-in */
                uint32_t r = key_in_rate[g.set[S_KEYIN] < 20 ? g.set[S_KEYIN] : 0];

                credit_add(p, r);
                rec_add(R_KEY_IN, p, r);
                rec_add(R_IN, p, r);
                day_add(D_IN, r);
                LOG("key-in at station %d: %ld credits", p + 1, g.credit[p]);
            } else if (bit & FK_KEYOUT) {         /* the attendant's key-out: the rate's credits, or what is left */
                long r = key_out_rate[g.set[S_KEYOUT] < 21 ? g.set[S_KEYOUT] : 0];

                if (r > g.credit[p])
                    r = g.credit[p];
                if (r <= 0)
                    continue;
                credit_add(p, -r);
                rec_add(R_KEY_OUT, p, (uint32_t)r);
                rec_add(R_OUT, p, (uint32_t)r);
                day_add(D_OUT, (uint32_t)r);
                LOG("key-out at station %d: %ld credits", p + 1, g.credit[p]);
            } else {
                pay_out(p);
            }
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

static void draw(int w, int h)
{
    (void)w;
    if (g.help)
        fish_keys_draw(CORE, h, g.stations);
}

static void status(char *buf, size_t n)
{
    if (g.stations == 4)
        snprintf(buf, n, "credits %ld / %ld / %ld / %ld", g.credit[0], g.credit[1], g.credit[2], g.credit[3]);
    else
        snprintf(buf, n, "credits %ld / %ld / %ld / %ld / %ld / %ld", g.credit[0], g.credit[1], g.credit[2],
                 g.credit[3], g.credit[4], g.credit[5]);
}

static struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Fish Hunter Final 6",
    .status = status,
    .reset = reset,
    .decrypt_from_game = decrypt_from_game,
    .encrypt_to_game = encrypt_to_game,
    .message = message,
    .build = build,
    .key = key,
    .draw = draw,
    .layout = E2100_LAYOUT_RUNNING,
    /* the loader checks DONE's word 1 against the upload's CRC: none, 0 */
    .handshake = E2100_HS_NOSUM,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    const char *v = getenv("IGS_VARIANT");
    long credits;
    int i;

    api = a;
    g.help = 1;
    g.stations = atoi(CORE->env("FISH_STATIONS", v && !strcmp(v, "4") ? "4" : "6")) == 4 ? 4 : 6;
    memcpy(g.set, set_default, sizeof(g.set));
    g.set[S_STATIONS] = 0x11 * (uint32_t)g.stations;
    g.rtp = atof(CORE->env("FISH_RTP", "0.95"));
    g.chain_go = atof(CORE->env("FISH_CHAIN", "0.85"));
    g.force = atoi(CORE->env("FISH_FORCE", "0"));
    credits = atol(CORE->env("FISH_CREDITS", "0"));
    for (i = 0; i < SEATS; i++)
        g.credit[i] = i < g.stations ? credits : 0;
    srand((unsigned)(CORE->now() * 1000.0) ^ (unsigned)time(NULL));
    return &game;
}
