/*
 * Panda Family (E2000): what its IGS036 firmware does, as far as the game
 * needs.  docs/PandaFamily.md.
 *
 * The game logic is all in the executable; the IGS036 is the I/O board, the
 * settings store and the action-group allocator.  It talks the IGS036
 * packet transport (app/c/shareram.c, docs/IGS_E2000.md): each exchange is
 * a list of sub-messages [type, length (16-bit), data].
 *
 * Chip -> game, every packet: 2 key bits, 0x0B/0x0C tickets still to pay
 * (1P/2P), 0x0D/0x0E dispenser faults; 3/4 coins on coin 1/2 since the last
 * packet (the game answers 7/8).  Answers: 5/6 I/O board versions and fuses,
 * 9/10 tickets to pay set, 0x0F/0x10 allocate / free an action group, 0x11
 * the settings (3132 bytes), 0x12 settings stored, 0x15 the I/O registers
 * (debug), 0x16 state, 0x17/0x18 a second key block (the photo booth).
 * The version numbers and the 0x15 / 0x17 contents are invented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2000/e2000.h"

#define NV_BYTES  3132            /* SettingData (sizeof printed by the game) */
#define NV_FILE   "pf_data/igs036.nv"

#define TICKET_SECONDS 0.2        /* the dispenser's pace */
#define GROUPS    128             /* action groups (ActionGroupUse checks 0..127) */

/* sub-message types */
enum {
    T_TEXT = 1, T_KEYS = 2, T_COIN1 = 3, T_COIN2 = 4, T_IO_IN = 5, T_IO_OUT = 6,
    T_COIN1_ACK = 7, T_COIN2_ACK = 8, T_TICKETS_1P = 9, T_TICKETS_2P = 10,
    T_OWED_1P = 0x0B, T_OWED_2P = 0x0C, T_FAULT_1P = 0x0D, T_FAULT_2P = 0x0E,
    T_GROUP_ALLOC = 0x0F, T_GROUP_FREE = 0x10, T_NV_LOAD = 0x11, T_NV_STORE = 0x12, T_NV_DEFAULT = 0x13,
    T_EVENT = 0x14, T_JM_REGS = 0x15, T_STATE = 0x16, T_KEYS2 = 0x17, T_KEYS2_DONE = 0x18,
};

/* SDL 1.2 key symbols (the core translates X11 / Win32 keys to these) */
enum {
    K_2 = 50, K_5 = 53, K_6 = 54, K_9 = 57,
    K_a = 97, K_d = 100, K_f = 102, K_h = 104, K_j = 106, K_k = 107, K_l = 108,
    K_s = 115, K_v = 118, K_x = 120, K_z = 122,
    K_RIGHT = 275, K_LEFT = 276, K_F1 = 282, K_F2 = 283,
};

/*
 * Key bits (Setting_IsGameKeyPressed: key i is bit i, or the game's own
 * keyboard fallback, the key table at 0x8153F00 "asdfhjklv678"): 0..3 1P
 * buttons left to right (yellow, red, red, yellow), 4..7 2P's, 8 service,
 * 9 test.  The game sees its own keys too; these mirror them.
 */
static const struct { int sym, bit; } keymap[] = {
    { K_a, 0 }, { K_s, 1 }, { K_d, 2 }, { K_f, 3 },
    { K_LEFT, 0 }, { K_z, 1 }, { K_x, 2 }, { K_RIGHT, 3 },
    { K_h, 4 }, { K_j, 5 }, { K_k, 6 }, { K_l, 7 },
    { K_v, 8 }, { K_9, 8 }, { K_6, 9 }, { K_F2, 9 },
};

static const struct e2000_api *api;

static struct {
    uint32_t keys;
    int32_t coins[2];             /* not yet acknowledged */
    int32_t owed[2];              /* tickets still to pay, 1P / 2P */
    double next_ticket[2];
    int help;
    uint8_t groups[GROUPS];       /* action groups in use */
    uint8_t nv[NV_BYTES];
    int nv_valid;
} g;

#define LOG(...) api->core->log("pandafamily: " __VA_ARGS__)

static void nv_load(void)
{
    FILE *f = fopen(NV_FILE, "rb");

    g.nv_valid = f && fread(g.nv, 1, NV_BYTES, f) == NV_BYTES;
    if (f)
        fclose(f);
    /*
     * blank (erased, 0xFF): the game's check (0x8077C90) fails and it writes
     * its factory settings; all zeros would pass it and leave the coin rate 0
     */
    if (!g.nv_valid)
        memset(g.nv, 0xff, sizeof(g.nv));
}

static void nv_save(void)
{
    FILE *f = fopen(NV_FILE, "wb");

    if (!f) {
        LOG("cannot write %s", NV_FILE);
        return;
    }
    fwrite(g.nv, 1, NV_BYTES, f);
    fclose(f);
}

static void pf_script(void);

static void pf_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.help = 1;
    nv_load();
}

/* append one sub-message */
static uint32_t put(uint8_t *out, uint32_t n, uint32_t max, int type, const void *data, uint32_t len)
{
    if (n + 3 + len > max)
        return n;
    out[n] = type;
    out[n + 1] = len & 0xff;
    out[n + 2] = len >> 8;
    if (len)
        memcpy(out + n + 3, data, len);
    return n + 3 + len;
}

static uint32_t put32(uint8_t *out, uint32_t n, uint32_t max, int type, uint32_t v)
{
    return put(out, n, max, type, &v, 4);
}

/* one exchange: the game's sub-messages in, ours out */
static uint32_t pf_message(const uint8_t *in, uint32_t nin, uint32_t *hdr, uint8_t *out, uint32_t max)
{
    uint32_t n = 0, i, len, w[8];
    double now = api->core->now();
    int type, p;

    (void)hdr;
    pf_script();
    for (i = 0; in && i + 3 <= nin; i += 3 + len) {
        const uint8_t *d = in + i + 3;
        int32_t v;

        type = in[i];
        len = in[i + 1] | in[i + 2] << 8;
        if (!type || i + 3 + len > nin)
            break;
        v = len >= 4 ? (int32_t)(d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24) : len ? d[0] : 0;
        if (api->core->verbose || (type != T_EVENT && type != T_STATE && type != T_COIN1_ACK &&
                                   type != T_COIN2_ACK && type != T_TICKETS_1P && type != T_TICKETS_2P &&
                                   type != T_GROUP_ALLOC && type != T_GROUP_FREE))
            LOG("sub-message 0x%02x (%u bytes) %d", type, len, v);
        memset(w, 0, sizeof(w));
        switch (type) {
        case T_COIN1_ACK:
        case T_COIN2_ACK:
            g.coins[type - T_COIN1_ACK] = 0;
            break;
        case T_IO_IN:       /* "I:S%01d%02d": version words 0, 1; word 5 bits 0/1 fuse faults */
            w[0] = 1;
            n = put(out, n, max, T_IO_IN, w, 24);
            break;
        case T_IO_OUT:      /* "O:S%01d%02d" */
            w[0] = 1;
            n = put(out, n, max, T_IO_OUT, w, 20);
            break;
        case T_TICKETS_1P:
        case T_TICKETS_2P:  /* tickets to pay (the game also counts them down by hand) */
            p = type - T_TICKETS_1P;
            if (v != g.owed[p])
                LOG("%dP tickets to pay: %d", p + 1, v);
            g.owed[p] = v < 0 ? 0 : v;
            g.next_ticket[p] = now + TICKET_SECONDS;
            n = put32(out, n, max, type, 0);
            break;
        case T_GROUP_ALLOC: /* ActionGroupUse: the lowest free action group */
            for (p = 0; p < GROUPS - 1 && g.groups[p]; p++)
                ;
            g.groups[p] = 1;
            n = put32(out, n, max, type, p);
            break;
        case T_GROUP_FREE:  /* ActionGroupFree */
            g.groups[(uint32_t)v % GROUPS] = 0;
            n = put32(out, n, max, type, 0);
            break;
        case T_STATE:
            n = put32(out, n, max, type, 0);
            break;
        case T_NV_LOAD:
            n = put(out, n, max, T_NV_LOAD, g.nv, NV_BYTES);
            break;
        case T_NV_STORE:
            if (len >= NV_BYTES) {
                memcpy(g.nv, d, NV_BYTES);
                g.nv_valid = 1;
                nv_save();
            }
            n = put32(out, n, max, T_NV_STORE, 0);
            break;
        case T_JM_REGS:
            n = put(out, n, max, T_JM_REGS, w, 24);
            break;
        case T_KEYS2:
        case T_KEYS2_DONE:
            n = put(out, n, max, T_KEYS2, w, 32);
            n = put32(out, n, max, T_KEYS2_DONE, 0);
            break;
        default:            /* 0x13 factory settings, 0x14 events and lamps */
            break;
        }
    }
    /* the dispensers: one ticket each per TICKET_SECONDS */
    for (p = 0; p < 2; p++)
        if (g.owed[p] > 0 && now >= g.next_ticket[p]) {
            g.owed[p]--;
            g.next_ticket[p] = now + TICKET_SECONDS;
        }
    n = put32(out, n, max, T_KEYS, g.keys);
    for (p = 0; p < 2; p++) {
        uint8_t b = g.owed[p] > 255 ? 255 : g.owed[p], z = 0;

        if (g.coins[p])
            n = put32(out, n, max, T_COIN1 + p, g.coins[p]);
        n = put(out, n, max, T_OWED_1P + p, &b, 1);
        n = put(out, n, max, T_FAULT_1P + p, &z, 1);
    }
    return n;
}

static void pf_key(int sym, int down)
{
    unsigned i;

    switch (sym) {
    case K_5: if (down) g.coins[0]++; return;
    case K_2: if (down) g.coins[1]++; return;
    case K_F1: if (down) g.help = !g.help; return;
    }
    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
        if (keymap[i].sym == sym) {
            if (down)
                g.keys |= 1u << keymap[i].bit;
            else
                g.keys &= ~(1u << keymap[i].bit);
        }
}

static void pf_draw(int w, int h)
{
    static const char *lines[] = {
        "5 coin 1P  2 coin 2P",
        "1P A S D F (Left Z X Right)",
        "2P H J K L  F2 test  9 service",
        "F1 hide this",
    };
    const struct igs_core *c = api->core;
    int i, y;

    (void)w;
    if (!g.help)
        return;
    y = h - 4 - 4 * c->font_h;
    c->fill(0, y - 4, 31 * c->font_w, 4 * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < 4; i++)
        c->text(6, y + (3 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

/*
 * PF_SCRIPT="sec:key[+],..." presses a key at a time from start-up (key:
 * coin, coin2, a, s, d, f, h, j, k, l, test, service).  A press lasts
 * 0.2 s, or with a trailing + until the next entry.  Runs from the exchange.
 */
static void pf_script(void)
{
    static const struct { const char *name; int sym; } keys[] = {
        { "coin", K_5 }, { "coin2", K_2 }, { "a", K_a }, { "s", K_s }, { "d", K_d }, { "f", K_f },
        { "h", K_h }, { "j", K_j }, { "k", K_k }, { "l", K_l }, { "test", K_F2 }, { "service", K_9 },
    };
    static const char *p;
    static double t0, release_at;
    static int held;
    const struct igs_core *c = api->core;
    double now, at;
    char name[16];
    int n = 0, hold, i;

    if (!p) {
        p = c->env("PF_SCRIPT", "");
        t0 = c->now();
    }
    now = c->now() - t0;
    if (held && release_at && now >= release_at) {
        pf_key(held, 0);
        held = 0;
    }
    if (!*p || sscanf(p, "%lf:%15[a-z0-9]%n", &at, name, &n) != 2 || now < at)
        return;
    hold = p[n] == '+';
    p += n + hold;
    if (*p == ',')
        p++;
    if (held)
        pf_key(held, 0);
    held = 0;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (!strcmp(name, keys[i].name)) {
            held = keys[i].sym;
            pf_key(held, 1);
            release_at = hold ? 0 : now + 0.2;
            LOG("script %.1f s %s%s", now, name, hold ? " (held)" : "");
        }
}

static void pf_status(char *buf, size_t n)
{
    if (g.owed[0] > 0 || g.owed[1] > 0)
        snprintf(buf, n, "paying %d / %d tickets", g.owed[0], g.owed[1]);
    else
        buf[0] = 0;
}

static const struct e2000_game game = {
    .version = E2000_API_VERSION,
    .name = "pandafamily",
    .transport = E2000_IGS036_PKT,
    .pkt_key_to_game = "RC4_36",
    .pkt_key_from_game = "RC4_PC",
    .pkt_key_len = 7,             /* with the NUL */
    .reset = pf_reset,
    .message = pf_message,
    .key = pf_key,
    .draw = pf_draw,
    .status = pf_status,
};

__attribute__((visibility("default")))
const struct e2000_game *e2000_game_entry(const struct e2000_api *a)
{
    api = a;
    return &game;
}
