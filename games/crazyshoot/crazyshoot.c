/*
 * Crazy Shoot (E2100, CrazyShoot-OS) game plugin: the IGS036's side of the
 * game's Unity plugin libPcShareram.so ("e2k1" ShareRam, as Fish Hunter 6
 * M607CN), and the Sentinel HASP key that plugin links in.  wiki Crazy Shoot.
 *
 *   - channel: [running sum, payload, headers], every word XORed with a
 *     32-bit key per direction; message 5 words 11 / 12 announce the game's
 *     next header register and key, message 6 word 12 the chip's register,
 *     message 0x1C (once) the chip's next key.  The first keys and the sum's
 *     start come from the HASP key's memory (below).
 *   - three stations.  The chip's firmware is not dumped; its game logic is
 *     the developers' own stand-in, shipped in Assembly-CSharp.dll
 *     (FakeChanceGenerator, Buffer, SystemSetting), ported here: the payout
 *     buffers, rope breaks (normal and gold bullet), balloon odds, the lucky
 *     wheel and bonus, the collection toys and gold-bullet awards.  Coins,
 *     bullets, tickets, settings and the books follow the game's side of the
 *     protocol.
 *   - the HASP key (feature 18): hasp_login / logout / read / write /
 *     encrypt / decrypt are hooked in libPcShareram.so.  Its read-only memory
 *     holds the channel keys and three words the game sends in message 0x30
 *     (bytes 16..39, which the game hasp_decrypts: stored here as the
 *     result) and the game's parameters from byte 40 (byte 0 = 4: the
 *     balloon round lasts 30 s, as the game requires).  The values are the
 *     plugin's own; the cabinet's key is not dumped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../machines/e2100/e2100.h"

#define P 3                       /* stations */
#define SLOTS 6                   /* the protocol's per-station arrays */

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(...) CORE->log("crazyshoot: " __VA_ARGS__)
#define VLOG(...) do { if (CORE->verbose) LOG(__VA_ARGS__); } while (0)

/* ---- the HASP key -------------------------------------------------------------------- */

#define RX_KEY 0x43534B31u        /* chip -> game, RO 20 (invented) */
#define TX_KEY 0x31534B43u        /* game -> chip, RO 16 (invented) */
static uint8_t hasp_ro[112], hasp_rw[128];

static void hasp_init(void)
{
    static const uint32_t words[3] = { 0x43524153, 0x5A595348, 0x00000100 };   /* invented */
    uint32_t v;

    memset(hasp_ro, 0, sizeof(hasp_ro));
    memcpy(hasp_ro, "CrazyShootHASP18", 16);
    v = TX_KEY;
    memcpy(hasp_ro + 16, &v, 4);
    v = RX_KEY;
    memcpy(hasp_ro + 20, &v, 4);
    /* 24: the running sum's start, the machine's 0 */
    memcpy(hasp_ro + 28, words, 12);
    hasp_ro[40] = 4;              /* balloon round: 4 * 10 - 10 = 30 s (BalloonManager.MaxTime) */
}

#define HASP_FILEID_RW 0xFFF4
#define HASP_FILEID_RO 0xFFF5

static int hasp_login(int feature, const void *vendor, int *handle)
{
    (void)vendor;
    VLOG("hasp_login feature %d", feature);
    *handle = 1;
    return 0;
}

static int hasp_logout(int handle) { (void)handle; return 0; }

static int hasp_read(int handle, int file, unsigned off, unsigned len, void *buf)
{
    const uint8_t *m = file == HASP_FILEID_RW ? hasp_rw : hasp_ro;
    size_t n = file == HASP_FILEID_RW ? sizeof(hasp_rw) : sizeof(hasp_ro);

    (void)handle;
    memset(buf, 0, len);
    if (off < n)
        memcpy(buf, m + off, len < n - off ? len : n - off);
    return 0;
}

static int hasp_write(int handle, int file, unsigned off, unsigned len, const void *buf)
{
    (void)handle;
    if (file == HASP_FILEID_RW && off < sizeof(hasp_rw))
        memcpy(hasp_rw + off, buf, len < sizeof(hasp_rw) - off ? len : sizeof(hasp_rw) - off);
    return 0;
}

/* the memory holds what the game's decrypt would produce: both are the identity */
static int hasp_crypt(int handle, void *buf, int len) { (void)handle; (void)buf; (void)len; return 0; }

/* ---- the game's settings (SystemSetting) ------------------------------------------- */

/*
 * SettingPC (29 words): 0 free play, 1 prize option (0 score, 1 tickets),
 * 2 coin rate, 3 ticket rate, 4 bullets per credit, 5 bonus top, 6 console
 * rate, 7 shoot time limit, 8 game volume, 9 demo volume (indices into the
 * tables below), 13..20 and 21..28 the two passwords' digits.
 */
#define NSET 29
static const uint32_t set_default[NSET] = { 0, 1, 0, 12, 5, 1, 0, 0, 4, 4 };
static const uint32_t con_coin[3] = { 1, 2, 3 };
static const uint32_t con_ticket[17] = { 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 20, 25, 30, 35, 40, 45, 50 };
static const uint32_t con_bullet[9] = { 3, 4, 5, 6, 8, 10, 12, 15, 20 };
static const uint32_t buf_rate[6] = { 700, 50, 160, 60, 30, 0 };
static const uint32_t gift_init_hits[10] = { 200, 300, 500, 800, 1000, 1500, 2000, 2500, 3000, 3500 };

/* RecordPC (338 words): 18-word rows (total, this period, last period, per
   station) 0 coins in, 18 tickets out, 36 service, 54 mercy; the day book
   (75 = today's slot, 40 days: 76 dates, 116 in, 156 out, 196 service, 236
   mercy) and the month book (277 = this month's slot, 12: 278 months, 290..) */
#define NREC 338
enum { R_IN = 0, R_OUT = 18, R_SERVICE = 36, R_MERCY = 54, R_DAY = 75, R_MONTH = 277 };

/* ---- state ------------------------------------------------------------------------------ */

struct buffer {
    int all[P][6], small[P][6], add_count[P];
    uint32_t shoot_cycle[P];
    int rec_add_count[P], rec[P], rec_small[P];
    uint32_t win[P][6], win_small[P][6], win_times[P][6], shift_times[P];
    uint32_t wheel_cycle[P], bonus_cycle[P];
    int special;
    uint32_t gold_power, gold_now;
    uint32_t moved[P][6];
    uint32_t main_shots[P], main_hits[P], main_gold_shots[P], main_gold_hits[P];
    uint32_t balloon_shots[P], balloon_hits[P], balloon_gold_shots[P], balloon_gold_hits[P];
    uint32_t collections[P], init, bet[P];
    int prize_score[P];
};

#define QMAX 24

static struct {
    struct rc { uint32_t key, next; int rekey; } rx;
    uint32_t tx_key;

    uint32_t set[NSET];
    uint32_t rec[NREC];
    uint32_t date;
    uint32_t keys[10];            /* key bits 8j+b */
    uint32_t coins[P], bullets[P], tickets[P];
    double last_tick;

    /* SystemSetting */
    uint32_t bullet_ticket, gift_ticket[10], gift_hits[10];
    /* Buffer, FakeChanceGenerator */
    struct buffer b;
    uint32_t wheel_odds[P], wheel_value[P], bonus_index[P], bonus_value[P];
    uint32_t wheel_table[10], bonus_table[21];
    uint32_t toys[P][6], gate[P], odds[P];

    uint32_t chanceq[QMAX][6], bulletq[QMAX][2], addq[QMAX];
    int nchance, nbullet, nadd;
    int want_version, want_settings, want_records, want_buffer, want_reset, want_term;
    int credit_dirty, payout_dirty, table_dirty, toy_dirty;
    uint32_t term[5];
    uint32_t batches;
    int help;
} g;

static uint32_t RAND(uint32_t n) { return n ? (uint32_t)rand() % n : 0; }

static uint32_t adj(int i) { return g.set[i]; }
static uint32_t prize_tickets(void) { return adj(1) == 1; }
static uint32_t coin_rate(void) { return con_coin[adj(2) < 3 ? adj(2) : 0]; }
static uint32_t ticket_rate(void) { return con_ticket[adj(3) < 17 ? adj(3) : 0]; }
static uint32_t bullet_rate(void) { return con_bullet[adj(4) < 9 ? adj(4) : 0]; }

/* ---- SystemSetting: the gifts' tickets and hit counts ------------------------------ */

static uint32_t round_ticket(uint32_t n)          /* A36_Round */
{
    if (!n)
        return (n - 50) / 100;
    if (n < 1000) {
        n += 50;
    } else if (n < 10000) {
        uint32_t r = n % 1000;

        n = n / 1000 * 1000 + (r >= 750 ? 1000 : r >= 250 ? 500 : 0);
    } else if (n < 30000) {
        n = (n + 500) / 1000 * 1000;
    } else if (n < 100000) {
        uint32_t r = n % 10000;

        n = n / 10000 * 10000 + (r >= 7500 ? 10000 : r >= 2500 ? 5000 : 0);
    } else {
        n = (n + 5000) / 10000 * 10000;
    }
    return n / 100;
}

static uint32_t round_score(uint32_t n)           /* A36_Round_Score */
{
    if (!n)
        return (n - 5) / 10;
    if (n < 100) {
        n += 5;
    } else if (n < 1000) {
        uint32_t r = n % 100;

        n = n / 100 * 100 + (r >= 75 ? 100 : r >= 25 ? 50 : 0);
    } else if (n < 3000) {
        n = (n + 50) / 100 * 100;
    } else if (n < 10000) {
        uint32_t r = n % 1000;

        n = n / 1000 * 1000 + (r >= 750 ? 1000 : r >= 250 ? 500 : 0);
    } else {
        n = (n + 500) / 1000 * 1000;
    }
    return n / 10;
}

static void gifts(void)
{
    int i;

    g.bullet_ticket = coin_rate() * ticket_rate() * buf_rate[0] / 10 / bullet_rate();
    for (i = 0; i < 10; i++) {
        uint32_t init = gift_init_hits[i] * g.bullet_ticket / 100;

        if (prize_tickets()) {
            g.gift_ticket[i] = round_ticket(init);
            g.gift_hits[i] = g.bullet_ticket ? g.gift_ticket[i] * 10000 / g.bullet_ticket : 0;
        } else {
            g.gift_ticket[i] = round_score(init);
            g.gift_hits[i] = g.bullet_ticket ? g.gift_ticket[i] * 1000 / g.bullet_ticket : 0;
        }
    }
}

/* ---- Buffer ------------------------------------------------------------------------------- */

static const uint32_t buf_init[6] = { 0, 0, 0, 8, 10, 0 };

static void wheel_cycle(int p) { g.b.wheel_cycle[p] = 8 + RAND(8); }
static void bonus_cycle(int p) { g.b.bonus_cycle[p] = 8 + RAND(16); }

static void buffer_reset(void)
{
    int p, k;

    memset(&g.b, 0, sizeof(g.b));
    g.b.gold_power = 20;
    for (p = 0; p < P; p++) {
        wheel_cycle(p);
        bonus_cycle(p);
        for (k = 0; k < 6; k++) {
            g.b.all[p][k] = (int)(buf_init[k] * coin_rate() * ticket_rate());
            g.b.init += buf_init[k] * coin_rate() * ticket_rate();
        }
    }
    g.b.special = (int)(coin_rate() * ticket_rate() * 15);
    g.b.init += coin_rate() * ticket_rate() * 15;
}

static uint32_t per_bullet_ticket(int p)
{
    uint32_t n = bullet_rate();
    uint32_t part = g.b.shoot_cycle[p] != n - 1 ? 1000 / n : 1000 - 1000 / n * g.b.shoot_cycle[p];

    return coin_rate() * ticket_rate() * part;
}

static uint32_t credit_ticket(void) { return coin_rate() * ticket_rate() * 1000; }

static uint32_t per_gold_ticket(void)
{
    uint32_t v = g.b.gold_power * credit_ticket() / bullet_rate() / 1000;

    return prize_tickets() ? v * buf_rate[0] / 1000 / 10 : v * buf_rate[0] / 1000;
}

static void recover(int p, uint32_t shift)
{
    g.b.rec_add_count[p] += (int)(shift * credit_ticket());
    if (g.b.rec_add_count[p] >= 1000) {
        int n = g.b.rec_add_count[p] / 1000;

        g.b.rec_add_count[p] %= 1000;
        g.b.rec_small[p] += n * (int)buf_rate[0];
        if (g.b.rec_small[p] >= 1000) {
            g.b.rec[p] += g.b.rec_small[p] / 1000;
            g.b.rec_small[p] %= 1000;
        }
    }
    if (g.b.rec[p] > 0) {
        g.b.all[p][0] -= g.b.rec[p];
        g.b.all[p][5] += g.b.rec[p];
        g.b.rec[p] = 0;
    }
    if (g.b.all[p][5] >= 100) {
        uint32_t r = RAND(1024);
        int k = r < 307 ? 3 : r < 717 ? 0 : 4;

        g.b.all[p][5] -= 100;
        g.b.all[p][k] += 100;
        g.b.moved[p][k] += 100;
    }
}

/* a normal shot (game type 0 main, 1 balloon) */
static void shoot(int p, int bet, int type)
{
    uint32_t k;

    if (type == 0)
        g.b.main_shots[p]++;
    else
        g.b.balloon_shots[p]++;
    g.b.shoot_cycle[p] = (g.b.shoot_cycle[p] + 1) % bullet_rate();
    g.b.add_count[p] += bet * (int)per_bullet_ticket(p);
    if (g.b.add_count[p] >= 1000) {
        int n = g.b.add_count[p] / 1000;

        g.b.add_count[p] %= 1000;
        g.b.bet[p] += (uint32_t)n;
        for (k = 0; k < (type == 0 ? 6u : 1u); k++) {
            g.b.small[p][k] += n * (type == 0 ? (int)buf_rate[k] : 1000);
            if (g.b.small[p][k] >= 1000) {
                g.b.all[p][k] += g.b.small[p][k] / 1000;
                g.b.small[p][k] %= 1000;
            }
        }
    }
    if (g.b.all[p][1] > 0) {
        g.b.special += g.b.all[p][1];
        g.b.all[p][1] = 0;
    }
    if (type == 0) {
        uint32_t miss = g.b.main_shots[p] - g.b.main_hits[p];

        if (miss > g.b.shift_times[p] && miss - g.b.shift_times[p] >= bullet_rate()) {
            recover(p, 1);
            g.b.shift_times[p] += bullet_rate();
        }
    }
}

static void shoot_gold(int p, int type)
{
    if (type == 2)
        g.b.main_gold_shots[p]++;
    else
        g.b.balloon_gold_shots[p]++;
    if (g.b.gold_now)
        g.b.gold_now--;
}

/* a win out of buffer k (0 main, 2 collection, 3 wheel, 4 bonus) */
static void sub_buffer(int p, int k, int v)
{
    if (prize_tickets()) {
        g.b.all[p][k] -= v;
        g.b.win[p][k] += (uint32_t)v;
        g.b.win_times[p][k]++;
        return;
    }
    g.b.all[p][k] -= v % 10 ? v / 10 + 1 : v / 10;
    g.b.small[p][k] += v % 10 ? (10 - v % 10) * 100 : 0;
    g.b.win[p][k] += (uint32_t)(v / 10);
    g.b.win_small[p][k] += (uint32_t)(v % 10 * 100);
    if (g.b.win_small[p][k] > 1000) {
        g.b.win[p][k] += g.b.win_small[p][k] / 1000;
        g.b.win_small[p][k] %= 1000;
    }
    g.b.win_times[p][k]++;
    g.b.prize_score[p] += v;
}

/* a gold bullet's win, out of the shared special buffer */
static void sub_special(int p, int v)
{
    if (prize_tickets()) {
        g.b.special -= v;
        g.b.win[p][1] += (uint32_t)v;
        g.b.win_times[p][1]++;
        return;
    }
    g.b.special -= v % 10 ? v / 10 + 1 : v / 10;
    g.b.small[p][1] += v % 10 ? (10 - v % 10) * 100 : 0;
    g.b.win[p][1] += (uint32_t)(v / 10);
    g.b.win_small[p][1] += (uint32_t)(v % 10 * 100);
    if (g.b.win_small[p][1] > 1000) {
        g.b.win[p][1] += g.b.win_small[p][1] / 1000;
        g.b.win_small[p][1] %= 1000;
    }
    g.b.win_times[p][1]++;
    g.b.prize_score[p] += v;
}

static uint32_t gold_usable(void)
{
    uint32_t per = per_gold_ticket();
    int left = prize_tickets() ? g.b.special - (int)(per * g.b.gold_now)
                               : g.b.special * 10 - (int)(per * g.b.gold_now);

    return left > 0 && per ? (uint32_t)left / per : 0;
}

/* ---- FakeChanceGenerator ---------------------------------------------------------------- */

static const int hit_range[11] = { -360, -300, -240, -180, -120, -60, 120, 180, 240, 300, 360 };
static const int hit_rate[11] = { 60, 50, 40, 30, 20, 10, 0, -10, -20, -30, -40 };
static const uint32_t wheel_base[9] = { 4, 6, 10, 16, 20, 24, 30, 50, 60 };
static const uint32_t bonus_base[21] = { 0, 3, 4, 5, 6, 7, 8, 0, 10, 12, 14, 16, 18, 20, 0, 25, 30, 35, 50, 70, 100 };
static const uint32_t odds_rate[6] = { 1, 5, 10, 20, 30, 50 };
static const uint32_t wheel_layout[10] = { 0, 4, 5, 3, 8, 2, 6, 7, 1, 9 };
static const uint32_t bonus_occur[21] = { 0, 0, 0, 0, 0, 0, 0, 0, 41, 51, 61, 72, 82, 92, 0, 123, 113, 102, 92, 82, 113 };
static const uint32_t balloon_rate[5] = { 348, 266, 184, 143, 82 };
static const uint32_t balloon_odds[5] = { 5, 7, 11, 15, 30 };

static uint32_t top(void) { return odds_rate[adj(5) < 6 ? adj(5) : 0] * (prize_tickets() ? 1 : 10); }

static void assign_wheel(int p)
{
    g.wheel_odds[p] = RAND(1024) / 114;           /* nine shares of 114 / 1024 (the last one 112) */
    if (g.wheel_odds[p] > 8)
        g.wheel_odds[p] = 8;
    g.wheel_value[p] = wheel_base[g.wheel_odds[p]] * top();
}

static void assign_bonus(int p)
{
    uint32_t r = RAND(1024), acc = 0, i;

    g.bonus_index[p] = 0;
    for (i = 8; i < 21; i++) {
        acc += bonus_occur[i];
        if (r < acc) {
            g.bonus_index[p] = i;
            break;
        }
    }
    g.bonus_value[p] = bonus_base[g.bonus_index[p]] * top();
}

static void tables(void)
{
    int i;

    for (i = 0; i < 10; i++)
        g.wheel_table[i] = wheel_layout[i] == 9 ? 0 : wheel_base[wheel_layout[i]] * top();
    for (i = 0; i < 21; i++)
        g.bonus_table[i] = bonus_base[i] * top();
    g.table_dirty = 1;
}

static int rope_offset(int p)
{
    int pct = g.bullet_ticket ? g.b.all[p][0] * 100 / (int)g.bullet_ticket : 0, r = 0, i;

    for (i = 0; i < 11; i++) {
        r = hit_rate[i];
        if (pct < hit_range[i])
            break;
    }
    return r;
}

static uint32_t rope_breaks(uint32_t gift, uint32_t hits, int p)
{
    uint32_t need = g.gift_hits[gift < 10 ? gift : 9];

    if (hits * 100 > need * 2)
        return 1;
    need = (uint32_t)((int)need * (100 + rope_offset(p))) / 100u;
    return need && RAND(1024) < 102400 / need;
}

static uint32_t rope_breaks_gold(uint32_t gift)
{
    uint32_t need = g.gift_hits[gift < 10 ? gift : 9] * 10 / (g.b.gold_power ? g.b.gold_power : 20);

    return need && RAND(1024) < 102400 / need;
}

static uint32_t balloon_ticket(int p)
{
    uint32_t v = per_bullet_ticket(p), r = RAND(1024), acc = 0, odds = balloon_odds[0], i;

    for (i = 0; i < 5; i++) {
        acc += balloon_rate[i];
        if (r < acc) {
            odds = balloon_odds[i];
            break;
        }
    }
    if (prize_tickets())
        return ((v + 10) / 1000 * odds + 5) / 10;
    return (v * odds + 500) / 1000;
}

static uint32_t check_cycle(int p, uint32_t *cycle, void (*assign)(int), int k, uint32_t value)
{
    if (*cycle)
        (*cycle)--;
    if (*cycle)
        return 0;
    assign(p);
    return (prize_tickets() ? g.b.all[p][k] : g.b.all[p][k] * 10) >= (int)value;
}

static void chance_push(const uint32_t *w)
{
    if (g.nchance < QMAX)
        memcpy(g.chanceq[g.nchance++], w, 6 * 4);
}

/* rope hit: [3 / 4, guid, breaks, wheel or bonus, wheel odds (9 = the bonus), bonus index] */
static void rope(int gold, uint32_t gift, uint32_t hits, int p, uint32_t guid)
{
    uint32_t r[6] = { gold ? 4u : 3u, guid, 0, 0, 0, 0 };

    if (p >= P)
        return;
    if (gold)
        g.b.main_gold_hits[p]++;
    else
        g.b.main_hits[p]++;
    r[2] = gold ? rope_breaks_gold(gift) : rope_breaks(gift, hits, p);
    if (r[2]) {
        if (check_cycle(p, &g.b.bonus_cycle[p], bonus_cycle, 4, g.bonus_value[p])) {
            r[3] = 1;
            r[4] = 9;
            r[5] = g.bonus_index[p];
            sub_buffer(p, 4, (int)g.bonus_value[p]);
        }
        if (!r[3] && check_cycle(p, &g.b.wheel_cycle[p], wheel_cycle, 3, g.wheel_value[p])) {
            r[3] = 1;
            r[4] = g.wheel_odds[p];
            sub_buffer(p, 3, (int)g.wheel_value[p]);
        }
        if (gold)
            sub_special(p, (int)g.gift_ticket[gift < 10 ? gift : 9]);
        else
            sub_buffer(p, 0, (int)g.gift_ticket[gift < 10 ? gift : 9]);
    }
    chance_push(r);
    VLOG("station %d rope %u (%s, %u hits): %s%s", p + 1, gift, gold ? "gold" : "normal", hits,
         r[2] ? "breaks" : "holds", r[3] ? (r[4] == 9 ? ", bonus" : ", wheel") : "");
}

/* balloon hit: [5, guid, ticket] */
static void balloon(int gold, int p, uint32_t guid)
{
    uint32_t r[6] = { 5, guid, 0, 0, 0, 0 };

    if (p >= P)
        return;
    if (gold)
        g.b.balloon_gold_hits[p]++;
    else
        g.b.balloon_hits[p]++;
    r[2] = balloon_ticket(p);
    if (gold)
        sub_special(p, (int)r[2]);
    else
        sub_buffer(p, 0, (int)r[2]);
    chance_push(r);
}

/* a gold-bullet award for breaking a rope of gift kind gift: 0, 1..3 (5, 10, 15 bullets) */
static uint32_t gold_award(uint32_t gift)
{
    uint32_t r = RAND(1024);

    if (gold_usable() < 20 || r > 307 || g.b.gold_now >= 50)
        return 0;
    if (gift >= 1 && gift <= 4) {
        g.b.gold_now += 5;
        return 1;
    }
    if (gift >= 5 && gift <= 7) {
        g.b.gold_now += 10;
        return 2;
    }
    if (gift >= 8 && gift <= 9) {
        g.b.gold_now += 15;
        return 3;
    }
    return 0;
}

static void assign_toys(int p)
{
    uint32_t a[6], n = 0, i, k;

    while (n < 6) {
        uint32_t r = RAND(1024), dup = 0;

        a[n] = n == 0 ? (r < 512 ? 8 : 9) : n <= 2 ? (r < 341 ? 5 : r < 683 ? 6 : 7)
             : (r < 205 ? 0 : r < 410 ? 1 : r < 614 ? 2 : r < 819 ? 3 : 4);
        for (k = 0; k < n; k++)
            dup |= a[n] == a[k];
        if (!dup)
            n++;
    }
    for (i = 0; i < 20; i++)
        for (k = 0; k < 6; k++) {
            uint32_t j = RAND(1024) % 6, t;

            if (j != k) {
                t = a[j];
                a[j] = a[k];
                a[k] = t;
            }
        }
    memcpy(g.toys[p], a, sizeof(a));
}

static void set_gate(int p)
{
    int have = g.b.all[p][2], gift = (int)g.gift_ticket[9];
    uint32_t r = RAND(1024);

    g.gate[p] = have > gift * 3 ? 2 : have > -gift ? 0 : 1;
    if (g.gate[p] == 2)
        g.odds[p] = r < 410 ? 3 : r < 717 ? 2 : 1;
    else if (g.gate[p] == 0)
        g.odds[p] = r < 341 ? 3 : r < 683 ? 2 : 1;
    else
        g.odds[p] = r < 102 ? 3 : r < 307 ? 2 : 1;
}

static void chance_reset(void)
{
    int p;

    gifts();
    tables();
    for (p = 0; p < P; p++) {
        assign_wheel(p);
        assign_bonus(p);
        assign_toys(p);
        set_gate(p);
    }
    g.toy_dirty = 1;
}

/* ---- the books -------------------------------------------------------------------------- */

static void day_roll(void)
{
    uint32_t *di = &g.rec[R_DAY], *mi = &g.rec[R_MONTH], month = g.date / 100, k;

    if (!g.date)
        return;
    if (g.rec[R_DAY + 1 + *di] != g.date) {
        if (g.rec[R_DAY + 1 + *di])
            *di = (*di + 1) % 40;
        for (k = 0; k < 5; k++)
            g.rec[R_DAY + 1 + 40 * k + *di] = 0;
        g.rec[R_DAY + 1 + *di] = g.date;
    }
    if (g.rec[R_MONTH + 1 + *mi] != month) {
        if (g.rec[R_MONTH + 1 + *mi])
            *mi = (*mi + 1) % 12;
        for (k = 0; k < 5; k++)
            g.rec[R_MONTH + 1 + 12 * k + *mi] = 0;
        g.rec[R_MONTH + 1 + *mi] = month;
    }
}

/* row 0 in, 1 out, 2 service, 3 mercy: total, period, day, month */
static void book(int row, int p, uint32_t v)
{
    day_roll();
    g.rec[18 * row + p] += v;
    g.rec[18 * row + 6 + p] += v;
    g.rec[R_DAY + 1 + 40 * (row + 1) + g.rec[R_DAY]] += v;
    g.rec[R_MONTH + 1 + 12 * (row + 1) + g.rec[R_MONTH]] += v;
}

static void close_period(void)
{
    int r, p;

    for (r = 0; r < 4; r++)
        for (p = 0; p < SLOTS; p++) {
            g.rec[18 * r + 12 + p] = g.rec[18 * r + 6 + p];
            g.rec[18 * r + 6 + p] = 0;
        }
    LOG("period closed");
}

/* ---- credits ---------------------------------------------------------------------------- */

static void add_bullets(int p, uint32_t n, int service)
{
    g.bullets[p] += n;
    if (g.nadd < QMAX)
        g.addq[g.nadd++] = (uint32_t)p;
    if (service)
        book(2, p, n);
    g.credit_dirty = 1;
}

static void coin(int p)
{
    book(0, p, 1);
    if (++g.coins[p] >= coin_rate()) {
        g.coins[p] = 0;
        add_bullets(p, bullet_rate(), 0);
    }
    g.credit_dirty = 1;
    LOG("coin at station %d: %u bullets", p + 1, g.bullets[p]);
}

/* message 5's bets: per station, base-100 digits of bet + 10 * shot type */
static void bets(int p, uint32_t w)
{
    uint32_t d[8];
    int n = 0;

    while (w && n < 8) {
        d[n++] = w % 100;
        w /= 100;
    }
    while (n--) {
        int bet = (int)(d[n] % 10), type = (int)(d[n] / 10);

        if (type == 0 || type == 1) {
            if (g.bullets[p])
                g.bullets[p]--;
            shoot(p, bet ? bet : 1, type);
        } else if (type == 2 || type == 3) {
            shoot_gold(p, type);
        }
        g.credit_dirty = 1;
    }
}

/* message 0x1E: [station, command (RequestEventCmd), tickets, score] */
static void payout(uint32_t p, uint32_t cmd, uint32_t n, uint32_t score)
{
    if (p >= P)
        return;
    g.tickets[p] += n;
    g.payout_dirty = 1;
    switch (cmd) {
    case 8:                                       /* AddTicket_Collection: a new set of toys */
        g.b.collections[p]++;
        sub_buffer((int)p, 2, (int)(score ? score : n));
        assign_toys((int)p);
        set_gate((int)p);
        g.toy_dirty = 1;
        break;
    case 10:                                      /* AddTicket_Wheel */
        assign_wheel((int)p);
        break;
    case 11:                                      /* AddTicket_Bonus */
        assign_bonus((int)p);
        break;
    }
    VLOG("station %u: %u tickets (command %u, score %u)", p + 1, n, cmd, score);
}

/* ---- channel ------------------------------------------------------------------------------- */

static void reset(struct e2100_channel *ch)
{
    (void)ch;
    g.rx.key = RX_KEY;
    g.rx.rekey = 0;
    g.tx_key = TX_KEY;
    g.batches = 0;
}

static void decrypt_from_game(uint32_t *w, uint32_t n)
{
    while (n--)
        *w++ ^= g.tx_key;
}

static void encrypt_to_game(uint32_t *w, uint32_t n)
{
    while (n--)
        *w++ ^= g.rx.key;
    /* message 0x1C went out under the old key: the next batch uses the new one */
    if (g.rx.rekey) {
        g.rx.key = g.rx.next;
        g.rx.rekey = 0;
    }
}

static void message(struct e2100_channel *ch, uint32_t type, uint32_t len, const uint32_t *w)
{
    uint32_t i;

    if (type != 5 && type != 0x22 && type != 0x0E)
        VLOG("rx msg 0x%02x len %u w0=0x%08x", type, len, len ? w[0] : 0);
    switch (type) {
    case 3:                                       /* version, please */
        g.want_version = 1;
        break;
    case 5:                                       /* every batch: state, bets, date, next register and key */
        if (len < 13)
            break;
        for (i = 0; i < P; i++)
            if (w[1 + i])
                bets((int)i, w[1 + i]);
        if (w[7] != g.date) {
            g.date = w[7];
            day_roll();
        }
        ch->tx_idx = w[11] & 0x3F;
        g.tx_key = w[12];
        break;
    case 7:                                       /* settings, please */
        g.want_settings = 1;
        break;
    case 8:                                       /* default settings */
        memcpy(g.set, set_default, sizeof(g.set));
        chance_reset();
        g.want_settings = 1;
        LOG("default settings");
        break;
    case 9:                                       /* new settings */
        for (i = 0; i < len && i < NSET; i++)
            g.set[i] = w[i];
        chance_reset();
        LOG("settings stored");
        break;
    case 0x0B:                                    /* records, please */
        g.want_records = 1;
        break;
    case 0x11:                                    /* reset all: the books, credits, buffers */
        memset(g.rec, 0, sizeof(g.rec));
        memset(g.coins, 0, sizeof(g.coins));
        memset(g.bullets, 0, sizeof(g.bullets));
        memset(g.tickets, 0, sizeof(g.tickets));
        buffer_reset();
        chance_reset();
        g.want_reset = g.credit_dirty = g.payout_dirty = 1;
        LOG("reset all");
        break;
    case 0x16:                                    /* the period's book closes */
        close_period();
        break;
    case 0x1E:
        if (len >= 4)
            payout(w[0], w[1], w[2], w[3]);
        break;
    case 0x20:                                    /* the book's term times */
        for (i = 0; i < len && i < 5; i++)
            g.term[i] = w[i];
        g.want_term = 1;
        break;
    case 0x23:                                    /* the PC's credit (bullets) */
        if (len >= 2 && w[0] < P)
            add_bullets((int)w[0], w[1], 1);
        break;
    case 0x24:                                    /* the buffers, please */
        g.want_buffer = 1;
        break;
    case 0x26:                                    /* rope hit, normal bullet: [gift, hits, station, guid] */
        if (len >= 4)
            rope(0, w[0], w[1], (int)w[2], w[3]);
        break;
    case 0x27:                                    /* rope hit, gold bullet: [gift, station, guid] */
        if (len >= 3)
            rope(1, w[0], 0, (int)w[1], w[2]);
        break;
    case 0x28:                                    /* balloon hit: [station, guid] */
    case 0x29:
        if (len >= 2)
            balloon(type == 0x29, (int)w[0], w[1]);
        break;
    case 0x2D:                                    /* gold-bullet award? [999, gift, guid] -> 0x2E [award, guid] */
        if (len >= 3 && g.nbullet < QMAX) {
            g.bulletq[g.nbullet][0] = gold_award(w[1]);
            g.bulletq[g.nbullet][1] = w[2];
            g.nbullet++;
        }
        break;
    case 0x0E: case 0x0F: case 0x13: case 0x15: case 0x1A: case 0x1B: case 0x22: case 0x30:
        /* lamps and counters, payout set-up, tests, debug, the hour, the key's words */
        break;
    }
}

/* ---- the ASIC's batch -------------------------------------------------------------------- */

static void add_buffer_info(struct e2100_batch *b)
{
    uint32_t w[174], *o = w;
    int p, k;

    memset(w, 0, sizeof(w));
    o += 1 + 3;                                   /* start byte, extra temp bets */
    for (p = 0; p < P; p++) *o++ = g.b.shoot_cycle[p];
    for (p = 0; p < P; p++) *o++ = (uint32_t)g.b.add_count[p];
    for (p = 0; p < P; p++) for (k = 0; k < 6; k++) *o++ = (uint32_t)g.b.all[p][k];
    for (p = 0; p < P; p++) for (k = 0; k < 6; k++) *o++ = (uint32_t)g.b.small[p][k];
    for (p = 0; p < P; p++) *o++ = (uint32_t)g.b.rec_add_count[p];
    for (p = 0; p < P; p++) *o++ = (uint32_t)g.b.rec[p];
    for (p = 0; p < P; p++) *o++ = (uint32_t)g.b.rec_small[p];
    for (p = 0; p < P; p++) for (k = 0; k < 6; k++) *o++ = g.b.win[p][k];
    for (p = 0; p < P; p++) for (k = 0; k < 6; k++) *o++ = g.b.win_small[p][k];
    for (p = 0; p < P; p++) for (k = 0; k < 6; k++) *o++ = g.b.win_times[p][k];
    for (p = 0; p < P; p++) *o++ = g.b.shift_times[p];
    for (p = 0; p < P; p++) *o++ = g.b.wheel_cycle[p];
    for (p = 0; p < P; p++) *o++ = g.b.bonus_cycle[p];
    *o++ = (uint32_t)g.b.special;
    *o++ = g.b.gold_power;
    *o++ = g.b.gold_now;
    for (p = 0; p < P; p++) for (k = 0; k < 6; k++) *o++ = g.b.moved[p][k];
    for (p = 0; p < P; p++) *o++ = g.b.main_shots[p];
    for (p = 0; p < P; p++) *o++ = g.b.main_hits[p];
    for (p = 0; p < P; p++) *o++ = g.b.main_gold_shots[p];
    for (p = 0; p < P; p++) *o++ = g.b.main_gold_hits[p];
    for (p = 0; p < P; p++) *o++ = g.b.balloon_shots[p];
    for (p = 0; p < P; p++) *o++ = g.b.balloon_hits[p];
    for (p = 0; p < P; p++) *o++ = g.b.balloon_gold_shots[p];
    for (p = 0; p < P; p++) *o++ = g.b.balloon_gold_hits[p];
    for (p = 0; p < P; p++) *o++ = g.b.collections[p];
    *o++ = g.b.init;
    for (p = 0; p < P; p++) *o++ = g.b.bet[p];
    for (p = 0; p < P; p++) *o++ = (uint32_t)g.b.prize_score[p];
    api->batch_add(b, 0x25, w, 174);
}

static void build(struct e2100_channel *ch, struct e2100_batch *b)
{
    uint32_t w[64], i;
    double now = CORE->now();

    if (g.batches++ == 0) {
        static const uint8_t fafc[16] = "CrazyShootFAFC01";      /* the FPGA file cipher's key (invented) */

        g.rx.next = (uint32_t)rand() << 16 ^ (uint32_t)rand();
        g.rx.rekey = 1;
        api->batch_add(b, 0x1C, &g.rx.next, 1);
        for (i = 0; i < 16; i++)
            w[i] = fafc[i];
        api->batch_add(b, 0x1D, w, 16);
        memset(w, 0, 12 * sizeof(w[0]));
        api->batch_add(b, 0x0D, w, 12);           /* no errors */
        g.credit_dirty = g.payout_dirty = g.table_dirty = g.toy_dirty = 1;
        g.last_tick = now;
    }
    /* tickets run out one every 100 ms */
    if (now - g.last_tick >= 0.1) {
        g.last_tick = now;
        for (i = 0; i < P; i++)
            if (g.tickets[i]) {
                g.tickets[i]--;
                book(1, (int)i, 1);
                g.payout_dirty = 1;
            }
    }
    if (g.want_records) {                         /* 338 words */
        day_roll();
        api->batch_add(b, 0x0C, g.rec, NREC);
        g.want_records = 0;
        return;
    }
    if (g.want_buffer) {
        add_buffer_info(b);
        g.want_buffer = 0;
        return;
    }
    /* inputs: 80 key bits as 10 bytes */
    memset(w, 0, 3 * sizeof(w[0]));
    for (i = 0; i < 10; i++)
        w[i / 4] |= (g.keys[i] & 0xFF) << (8 * (i % 4));
    api->batch_add(b, 2, w, 3);
    if (g.want_version) {                         /* [fuses, version 8 chars, rom 8 chars] */
        static const char ver[8] = "CS-M100", rom[8] = "HLE";

        w[0] = 0;
        memcpy(&w[1], ver, 8);
        memcpy(&w[3], rom, 8);
        api->batch_add(b, 4, w, 5);
        g.want_version = 0;
    }
    if (g.want_settings) {
        api->batch_add(b, 0x0A, g.set, NSET);
        g.want_settings = 0;
    }
    if (g.want_reset) {
        w[0] = 1;
        api->batch_add(b, 0x12, w, 1);
        g.want_reset = 0;
    }
    if (g.want_term) {
        api->batch_add(b, 0x21, g.term, 5);
        g.want_term = 0;
    }
    if (g.credit_dirty) {                         /* coins toward a credit, bullets, the chip's register */
        memset(w, 0, 13 * sizeof(w[0]));
        for (i = 0; i < P; i++) {
            w[i] = g.coins[i];
            w[6 + i] = g.bullets[i];
        }
        w[12] = ch->rx_idx;
        api->batch_add(b, 6, w, 13);
        g.credit_dirty = 0;
    }
    if (g.payout_dirty) {                         /* tickets still to come */
        memset(w, 0, 12 * sizeof(w[0]));
        for (i = 0; i < P; i++)
            w[i] = g.tickets[i];
        api->batch_add(b, 0x10, w, 12);
        g.payout_dirty = 0;
    }
    if (g.table_dirty) {                          /* [0 = new, wheel 10, bonus 21] */
        w[0] = 0;
        memcpy(&w[1], g.wheel_table, 40);
        memcpy(&w[11], g.bonus_table, 84);
        api->batch_add(b, 0x2B, w, 32);
        g.table_dirty = 0;
    }
    if (g.toy_dirty) {                            /* [0 = new for all, toys 3 x 6, gates, odds] */
        w[0] = 0;
        for (i = 0; i < P; i++) {
            memcpy(&w[1 + 6 * i], g.toys[i], 24);
            w[19 + i] = g.gate[i];
            w[22 + i] = g.odds[i];
        }
        api->batch_add(b, 0x2C, w, 25);
        g.toy_dirty = 0;
    }
    for (i = 0; i < (uint32_t)g.nadd && b->nhdrs < 14; i++)
        api->batch_add(b, 0x1F, &g.addq[i], 1);
    g.nadd = 0;
    /* the game keeps 12 answers between its frames: at most 4 a batch */
    for (i = 0; g.nchance && b->nhdrs < 14 && i < 4; i++) {
        api->batch_add(b, 0x2A, g.chanceq[0], 6);
        memmove(g.chanceq[0], g.chanceq[1], sizeof(g.chanceq[0]) * --g.nchance);
    }
    while (g.nbullet && b->nhdrs < 16) {
        api->batch_add(b, 0x2E, g.bulletq[0], 2);
        memmove(g.bulletq[0], g.bulletq[1], sizeof(g.bulletq[0]) * --g.nbullet);
    }
}

/* ---- input ---------------------------------------------------------------------------------- */

/* game key indices (A36.IO_KEY) */
enum { K_UP, K_DOWN, K_RIGHT, K_LEFT, K_SHOOT1, K_SHOOT2, K_COIN, K_KEYIN };
#define K_SERVICE(p) (48 + 3 * (p))
#define K_SHOOT3(p) ((p) == 2 ? 59 : 28 + (p))
#define K_SETTING 78                         /* KEY_RECORD (79): the game never reads it */

/* SDL 1.2 key symbols -> station / key; F1 the help */
static const struct { int sym, p, k; } keymap[] = {
    { 273, 0, K_UP }, { 274, 0, K_DOWN }, { 275, 0, K_RIGHT }, { 276, 0, K_LEFT },
    { 306, 0, K_SHOOT1 }, { 308, 0, K_SHOOT2 }, { 32, 0, -1 }, { '5', 0, K_COIN }, { 286, 0, K_KEYIN },
    { 'r', 1, K_UP }, { 'f', 1, K_DOWN }, { 'g', 1, K_RIGHT }, { 'd', 1, K_LEFT },
    { 'a', 1, K_SHOOT1 }, { 's', 1, K_SHOOT2 }, { 'z', 1, -1 }, { '6', 1, K_COIN }, { 287, 1, K_KEYIN },
    { 'i', 2, K_UP }, { 'k', 2, K_DOWN }, { 'l', 2, K_RIGHT }, { 'j', 2, K_LEFT },
    { 'b', 2, K_SHOOT1 }, { 'v', 2, K_SHOOT2 }, { 'n', 2, -1 }, { '7', 2, K_COIN }, { 288, 2, K_KEYIN },
    { 290, 0, -2 }, { 291, 1, -2 }, { 292, 2, -2 },
    { 283, -1, K_SETTING },
};

static void set_key(int bit, int down)
{
    if (down)
        g.keys[bit >> 3] |= 1u << (bit & 7);
    else
        g.keys[bit >> 3] &= ~(1u << (bit & 7));
}

static void key(int sym, int down)
{
    unsigned i;

    if (sym == 282) {                             /* F1 */
        if (down)
            g.help = !g.help;
        return;
    }
    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        int p = keymap[i].p, k = keymap[i].k;

        if (keymap[i].sym != sym)
            continue;
        if (k == K_COIN) {
            if (down)
                coin(p);
            continue;
        }
        if (k == K_KEYIN) {                       /* the attendant's key-in: a credit's bullets */
            if (down)
                add_bullets(p, bullet_rate(), 1);
            continue;
        }
        set_key(p < 0 ? k : k == -1 ? K_SHOOT3(p) : k == -2 ? K_SERVICE(p) : 8 * p + k, down);
    }
}

static void draw(int w, int h)
{
    static const char *const lines[] = {
        "St Aim U D L R    Shoot 1 2 3     Coin Key-in Service",
        "1  Arrows         Ctrl Alt Space  5    F5     F9",
        "2  R F D G        A S Z           6    F6     F10",
        "3  I K J L        B V N           7    F7     F11",
        "F2 setting menu  F1 hide this",
    };
    int rows = (int)(sizeof(lines) / sizeof(lines[0])), i, y;

    (void)w;
    if (!g.help)
        return;
    /* the overlay's y grows upwards (fishkeys.h) */
    y = h - 4 - rows * CORE->font_h;
    CORE->fill(0, y - 4, 56 * CORE->font_w, rows * CORE->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < rows; i++)
        CORE->text(6, y + (rows - 1 - i) * CORE->font_h + 6, lines[i], 1, 1, 0.6f);
}

static void status(char *buf, size_t n)
{
    snprintf(buf, n, "bullets %u / %u / %u, tickets due %u / %u / %u", g.bullets[0], g.bullets[1], g.bullets[2],
             g.tickets[0], g.tickets[1], g.tickets[2]);
}

static struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Crazy Shoot",
    .status = status,
    .reset = reset,
    .decrypt_from_game = decrypt_from_game,
    .encrypt_to_game = encrypt_to_game,
    .message = message,
    .build = build,
    .key = key,
    .draw = draw,
    .layout = E2100_LAYOUT_RUNNING,
    /* the plugin's P2A_LoaderAdv */
    .handshake = E2100_HS_ADV,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    static const struct { const char *sym; void *fn; } hasp[] = {
        { "hasp_login", (void *)hasp_login }, { "hasp_logout", (void *)hasp_logout },
        { "hasp_read", (void *)hasp_read }, { "hasp_write", (void *)hasp_write },
        { "hasp_encrypt", (void *)hasp_crypt }, { "hasp_decrypt", (void *)hasp_crypt },
    };
    long bullets;
    unsigned i;
    int p;

    api = a;
    srand((unsigned)(CORE->now() * 1000.0) ^ (unsigned)time(NULL));
    hasp_init();
    /* the HASP client is linked into the Unity plugin, which Mono dlopens */
    for (i = 0; i < sizeof(hasp) / sizeof(hasp[0]); i++)
        if (!CORE->hook_lib || CORE->hook_lib("libPcShareram", hasp[i].sym, hasp[i].fn))
            LOG("cannot hook %s", hasp[i].sym);
    memcpy(g.set, set_default, sizeof(g.set));
    buffer_reset();
    chance_reset();
    bullets = atol(CORE->env("CS_BULLETS", "0"));
    for (p = 0; p < P; p++)
        g.bullets[p] = (uint32_t)(bullets > 0 ? bullets : 0);
    g.help = 1;
    return &game;
}
