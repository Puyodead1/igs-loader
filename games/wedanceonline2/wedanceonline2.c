/*
 * We Dance Online 2 (E2000): what its IGS027A firmware does, as far as the
 * game needs.  docs/WeDanceOnline2.md.  The executable (wedo, unpacked) keeps
 * its symbols; names below are its own.
 *
 * Transport: IGS027A packets (machines/e2000, E2000_IGS027A), Go Go Ball 2's
 * framing and keys.  Port 0x30 unless noted:
 *
 *   1   every frame (igs036_iCommunicationWith036): -> key bits (result),
 *       coins on coin 1 / 2 since the last frame (words 23, 24), system
 *       status (word 25)
 *   2   lamps (zllight_iIgs036Write)          3   meters (credit_iIgs036CounterWrite)
 *   8   coin settings, 20 bytes: 0 load (result 1 = blank), 1 store
 *   14  versions, 2 x 28 bytes (region u16 at 8); result: the IGS027A's serial
 *   15  system status (mainStatus_iIgs036): 1 set -> it, 2 next after
 *       status p -> next, 3 -> fuse bits
 *   16  re-key: 0 the chip's stream, 1 the game's (5 words of time)
 *   17  password: 0 check -> result, 1 set
 *   18  network registration, 80 bytes (store id, keys): 1 read, 2 write
 *   19  coin switch (patch pending)           port 0x20 type 20: output test
 *
 * The game logic is in the executable; the chip keeps the system status
 * (0 start-up, 1 attract, 2 game, 3 test) and the settings.  Invented: the
 * version texts, the region default, the password default 0000.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2000/e2000.h"

#define NV_FILE "info/igs027a.nv"         /* cwd is /WeDo/linux (p4) */

/* SDL 1.2 key symbols */
enum {
    K_RETURN = 13, K_SPACE = 32, K_0 = 48, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9,
    K_a = 97, K_c = 99, K_d = 100, K_e = 101, K_q = 113, K_s = 115, K_w = 119,
    K_x = 120, K_z = 122,
    K_KP0 = 256, K_KP1, K_KP2, K_KP3, K_KP4, K_KP5, K_KP6, K_KP7, K_KP8, K_KP9,
    K_KP_PERIOD = 266, K_KP_DIVIDE = 267, K_KP_MULTIPLY = 268, K_KP_MINUS = 269, K_KP_PLUS = 270,
    K_KP_ENTER = 271,
    K_UP = 273, K_DOWN = 274, K_RIGHT = 275, K_LEFT = 276,
    K_F1 = 282, K_F2 = 283,
    K_ARROWS = 1000,              /* WEDO_SCRIPT "arrows": the four arrows at once */
};

/*
 * Key bits (frame result), from the game's map m_saKeyMap (0x8658060) and
 * the key test (key_iTest): A (enter), the arrows, service, test, and a
 * phone keypad 1 2 3 / 4 5 6 / 7 8 9 / * 0 #.
 */
enum {
    B_A = 0, B_UP = 1, B_DOWN = 2, B_RIGHT = 3, B_LEFT = 4, B_SERVICE = 5, B_TEST = 6,
};
static const int pad_bit[12] = {   /* phone keys 1..9, *, 0, # */
    15, 11, 7, 16, 12, 8, 17, 13, 9, 18, 14, 10,
};

static const struct e2000_api *api;

static struct {
    uint32_t buttons;
    int coins;                    /* inserted since the last frame */
    int status;                   /* system status */
    int region;
    int help;
    uint32_t frames;
    struct {
        uint8_t coin_valid, reg_valid;
        uint8_t coin[20];         /* credit_iIgs036SettingRead: start / continue coins ... */
        uint8_t reg[80];          /* net_iInit: store id, CPU and MAC keys, versions */
        uint32_t password[8];
        uint32_t password_len;
    } nv;
} g;

static void nv_load(void)
{
    FILE *f = fopen(NV_FILE, "rb");

    if (!f || fread(&g.nv, sizeof(g.nv), 1, f) != 1) {
        memset(&g.nv, 0, sizeof(g.nv));
        g.nv.password_len = 4;    /* 0000 */
    }
    if (f)
        fclose(f);
}

static void nv_save(void)
{
    FILE *f = fopen(NV_FILE, "wb");

    if (!f) {
        api->core->log("wedanceonline2: cannot write %s", NV_FILE);
        return;
    }
    fwrite(&g.nv, sizeof(g.nv), 1, f);
    fclose(f);
}

static void wd_script(void);

static void wd_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.region = (int)api->core->env_long("WEDO_REGION", 1);
    g.help = 1;
    nv_load();
}

/* the next system status (main(): 0 -> 1 attract -> 2 game -> 1, 3 test -> 1) */
static int next_status(int s)
{
    return s == 1 ? 2 : 1;
}

static uint32_t wd_packet(int port, uint32_t *m)
{
    uint32_t type = m[E2000_PKT_PR], fn = m[E2000_PKT_PR + 1], len = m[E2000_PKT_PR + 3];
    uint8_t *d = (uint8_t *)(m + E2000_PKT_DATA);
    uint32_t p[8], i;

    for (i = 0; i < 8; i++)
        p[i] = m[E2000_PKT_PR + 4 + i];         /* params after the data length */
    if (api->core->verbose && !(port == 0x30 && type == 1))
        api->core->log("wedanceonline2: port 0x%x type %u.%u len %u (%u %u)", port, type, fn, len, p[0], p[1]);
    for (i = E2000_PKT_PR + 2; i < E2000_PKT_DATA; i++)
        m[i] = 0;

    if (port == 0x50) {                          /* start-up: result 1, no error */
        m[E2000_PKT_RET] = 1;
        return 0;
    }
    if (port != 0x30)                            /* 0x20: output test */
        return 0;
    switch (type) {
    case 1:                                      /* every frame */
        wd_script();
        g.frames++;
        m[E2000_PKT_RET] = g.buttons;
        m[E2000_PKT_PR + 5] = g.coins;
        m[E2000_PKT_PR + 7] = g.status;
        g.coins = 0;
        return 0;
    case 8:                                      /* coin settings */
        if (fn == 0) {
            if (!g.nv.coin_valid) {
                m[E2000_PKT_RET] = 1;
                return 0;
            }
            memcpy(d, g.nv.coin, 20);
            m[E2000_PKT_PR + 3] = 20;
            return 20;
        }
        memcpy(g.nv.coin, d, 20);
        g.nv.coin_valid = 1;
        nv_save();
        return 0;
    case 14:                                     /* versions (version_vGetAsic27Version) */
        memset(d, 0, 56);
        memcpy(d, "IGS027A", 7);
        d[8] = (uint8_t)g.region;
        memcpy(d + 20, "HLE", 3);
        memcpy(d + 28, "IGS027A", 7);
        memcpy(d + 48, "EMU", 3);
        m[E2000_PKT_PR + 3] = 56;
        m[E2000_PKT_RET] = 1;                    /* serial number */
        return 56;
    case 15:                                     /* system status */
        if (fn == 1)
            g.status = (int)p[0];
        else if (fn == 2)
            g.status = next_status((int)p[0]);
        else {
            m[E2000_PKT_RET] = 0;                /* 3: fuses all good */
            return 0;
        }
        api->core->log("wedanceonline2: system status %d", g.status);
        m[E2000_PKT_RET] = (uint32_t)g.status;
        return 0;
    case 16:                                     /* the game re-keys a stream after sending */
        api->rekey(fn == 0, p, 20);
        return 0;
    case 17:                                     /* password, len words */
        if (len > 8)
            len = 8;
        if (fn == 0)
            m[E2000_PKT_RET] = len == g.nv.password_len && !memcmp(p, g.nv.password, 4 * len);
        else {
            memcpy(g.nv.password, p, 4 * len);
            g.nv.password_len = len;
            nv_save();
        }
        return 0;
    case 18:                                     /* network registration */
        if (len > 80)
            len = 80;
        if (fn == 1) {
            if (g.nv.reg_valid)
                memcpy(d, g.nv.reg, len);
            else
                memset(d, 0, len);
            m[E2000_PKT_PR + 3] = len;
            m[E2000_PKT_RET] = 1;
            return len;
        }
        if (fn == 2) {
            memcpy(g.nv.reg, d, len);
            g.nv.reg_valid = 1;
            nv_save();
        }
        m[E2000_PKT_RET] = 1;
        return 0;
    default:                                     /* 2 lamps, 3 meters, 19 coin switch */
        return 0;
    }
}

static const struct e2000_bios bios[] = {        /* bios_serial_number (0x852B9A0) */
    { 0xF086E, "i852-W83627HF" },
    { 0xFEC7C, "i852-W83627HF" },
    { 0xFEC8A, "6A69YILTC-00" },
    { 0xFECDE, "Ph6A69YILT" },
    { 0, NULL },
};

/*
 * Keys: the phone keypad on 1 2 3 / Q W E / A S D / Z X C, or the numeric
 * keypad (7 8 9 top row = 1 2 3, 0, / = *, * = #); A on Enter or Space,
 * the arrows, 5 coin, 9 service, F2 test.  The game's own SDL keyboard
 * only uses / (the warm-up clock).
 */
static int key_bit(int sym)
{
    switch (sym) {
    case K_RETURN: case K_KP_ENTER: case K_SPACE: return B_A;
    case K_UP: return B_UP;
    case K_DOWN: return B_DOWN;
    case K_RIGHT: return B_RIGHT;
    case K_LEFT: return B_LEFT;
    case K_9: return B_SERVICE;
    case K_F2: return B_TEST;
    case K_1: case K_KP7: return pad_bit[0];
    case K_2: case K_KP8: return pad_bit[1];
    case K_3: case K_KP9: return pad_bit[2];
    case K_q: case K_KP4: return pad_bit[3];
    case K_w: case K_KP5: return pad_bit[4];
    case K_e: case K_KP6: return pad_bit[5];
    case K_a: case K_KP1: return pad_bit[6];
    case K_s: case K_KP2: return pad_bit[7];
    case K_d: case K_KP3: return pad_bit[8];
    case K_z: case K_KP_DIVIDE: return pad_bit[9];
    case K_x: case K_KP0: return pad_bit[10];
    case K_c: case K_KP_MULTIPLY: return pad_bit[11];
    default: return -1;
    }
}

static void wd_key(int sym, int down)
{
    int bit;

    if (sym == K_5) {
        if (down)
            g.coins++;
        return;
    }
    if (sym == K_F1) {
        if (down)
            g.help = !g.help;
        return;
    }
    if (sym == K_ARROWS) {
        uint32_t b = 1u << B_UP | 1u << B_DOWN | 1u << B_LEFT | 1u << B_RIGHT;

        g.buttons = down ? g.buttons | b : g.buttons & ~b;
        return;
    }
    if ((bit = key_bit(sym)) < 0)
        return;
    if (down)
        g.buttons |= 1u << bit;
    else
        g.buttons &= ~(1u << bit);
}

static void wd_draw(int w, int h)
{
    static const char *lines[] = {
        "5 coin  Enter A  arrows",
        "1 2 3 / Q W E / A S D / Z X C keypad",
        "F2 test  9 service  F1 hide",
    };
    const struct igs_core *c = api->core;
    int i, ty;

    (void)w;
    if (!g.help)
        return;
    ty = h - 4 - 3 * c->font_h;
    c->fill(0, ty - 4, 38 * c->font_w, 3 * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < 3; i++)
        c->text(6, ty + (2 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

/*
 * WEDO_SCRIPT="sec:key[+],..." presses a key at a time from start-up (key:
 * coin, a, up, down, left, right, arrows (all four), service, test, 1..9,
 * star, 0, hash).  A
 * press lasts 0.2 s, or with a trailing + until the next entry.  Runs from
 * the frame packet.
 */
static void wd_script(void)
{
    static const struct { const char *name; int sym; } keys[] = {
        { "coin", K_5 }, { "a", K_RETURN }, { "up", K_UP }, { "down", K_DOWN },
        { "left", K_LEFT }, { "right", K_RIGHT }, { "service", K_9 }, { "test", K_F2 },
        { "1", K_1 }, { "2", K_2 }, { "3", K_3 }, { "4", K_q }, { "5", K_w }, { "6", K_e },
        { "7", K_a }, { "8", K_s }, { "9", K_d }, { "star", K_z }, { "0", K_x }, { "hash", K_c },
        { "arrows", K_ARROWS },
    };
    static const char *p;
    static double t0, release_at;
    static int held;
    const struct igs_core *c = api->core;
    double now, at;
    char name[16];
    int n = 0, hold, i;

    if (!p) {
        p = c->env("WEDO_SCRIPT", "");
        t0 = c->now();
    }
    now = c->now() - t0;
    if (held && release_at && now >= release_at) {
        wd_key(held, 0);
        held = 0;
    }
    if (!*p || sscanf(p, "%lf:%15[a-z0-9]%n", &at, name, &n) != 2 || now < at)
        return;
    hold = p[n] == '+';
    p += n + hold;
    if (*p == ',')
        p++;
    if (held)
        wd_key(held, 0);
    held = 0;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (!strcmp(name, keys[i].name)) {
            c->log("wedanceonline2: script %.1f s %s%s", now, name, hold ? " (held)" : "");
            held = keys[i].sym;
            wd_key(held, 1);
            release_at = hold ? 0 : now + 0.2;
        }
}

/*
 * main_iFreeRamSize (each song's start) parses a 2.4 kernel's /proc/meminfo
 * (free + buffers + cached) and the RAM disk's size from jlist, then fills
 * the free RAM less 50 MB to flush the cache: 96 MB free, an empty /tmp.
 */
static const char *wd_vfile(const char *name)
{
    if (!strcmp(name, "/proc/meminfo"))
        return "        total:    used:    free:  shared: buffers:  cached:\n"
               "Mem:  268435456 167772160 100663296        0        0        0\n"
               "Swap:         0        0        0\n";
    if (!strncmp(name, "|./jlist /tmp", 13))
        return " Total Size = 0 Byte, 0 KByte, 0 MByte\n";
    return NULL;
}

static void wd_status(char *buf, size_t n)
{
    static const char *names[] = { "start-up", "attract", "game", "test" };

    snprintf(buf, n, "%s", g.status >= 0 && g.status <= 3 ? names[g.status] : "");
}

static const struct e2000_game game = {
    .version = E2000_API_VERSION,
    .name = "wedanceonline2",
    .bios = bios,
    .reset = wd_reset,
    .key = wd_key,
    .draw = wd_draw,
    .status = wd_status,
    .transport = E2000_IGS027A,
    /* protect_vKeyInit: 12 bytes of "EncodeGGB    " / "DecodeGGB    " */
    .pkt_key_from_game = "EncodeGGB   ",
    .pkt_key_to_game = "DecodeGGB   ",
    .pkt_key_len = 12,
    .packet = wd_packet,
    .vfile = wd_vfile,
};

__attribute__((visibility("default")))
const struct e2000_game *e2000_game_entry(const struct e2000_api *a)
{
    api = a;
    return &game;
}
