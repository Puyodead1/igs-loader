/*
 * Go Go Ball 2 (E2000): what its IGS027A firmware does, as far as the game
 * needs.  docs/GoGoBall2.md.  The executable is stripped; addresses below
 * are its own.
 *
 * Transport: IGS027A packets (machines/e2000, E2000_IGS027A).  A packet is
 * type, function and parameters (E2000_PKT_PR on); the game's port is 0x20
 * for the sprite stores, 0x30 for everything else, 0x50 for start-up.
 *
 *   port 0x20  1 ActionGroup store: 0 reset, 1 allocate -> index, 2 free
 *              3 object store: 0 reset, 1 allocate -> index, 2 reserve p1,
 *                3 p1 in use?, 4 free p1, 5 free all
 *   port 0x30  3..7 settings blocks (load: result 1 = blank; store), see nv[]
 *              7 light curtain: 6 beam states, 9 beam levels, 14 UR test
 *              9 ticket dispenser: 0 start, 1 -> state (result 2), 4/5 get/set
 *                tickets owed, 6 done
 *              10 ball-stopper motor: 0..2 go to a position, 3 run, 4 stop,
 *                 6 -> 5 when there, 7 -> position
 *              11 coin / ticket meters, 12 lamps (0 set, 1 read back)
 *              13 every frame: -> key bits, light-curtain cells, coins
 *              15 password: 0 check, 1 set
 *              16 re-key: 0 the chip's stream, 1 the game's (5 words of time)
 *              17 flow: 0/1 attract scenes, 2/3 game status, 4 -> fault bits
 *              18 version and region
 *
 * Invented (the IGS027A's ROM is not dumped): the attract order and the
 * game flow (17), the region, the version texts, the default password
 * 0000, motor and dispenser timings, beam levels.  The light curtain is a
 * crosshair: a throw blocks one cell.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2000/e2000.h"

#define GROUPS   128
#define OBJECTS  160

#define NV_FILE  "../rf_data/igs027.nv"   /* cwd is /GoGoBall2 */

/* the light curtain: 40 x 24 beams, cells at half-beam steps (sensor.c) */
#define CELLS_X  79
#define CELLS_Y  47
#define HIT_FRAMES 3                      /* frames a thrown ball blocks its cell */

/* the game's screen */
#define SCREEN_W 800
#define SCREEN_H 600

#define TICKET_FRAMES 12                  /* a ticket every 0.2 s */
#define MOTOR_FRAMES  30                  /* ball stopper travel */
#define DEMO_SCENES   6

/*
 * Game flow (Type 17 function 3): the next status for an event, by status.
 * The handlers (sub_808FE80): 0 how to play, 1 stage select, 2 the stage,
 * 3 stage over (event 0 next stage, 1 last stage done, 2 failed), 4 score,
 * 5 tickets, 6 (empty), 7 game over (event 0 play again, 1 leave).  A game
 * that clears no stage skips 4 and 5 (sub_80A77B0 divides by the stages
 * cleared).  Invented: the IGS027A's own table is not dumped.
 */
static const int8_t flow_next[8][3] = {
    { 1, 1, 1 }, { 2, 2, 2 }, { 3, 3, 3 }, { 1, 4, 4 },
    { 5, 5, 5 }, { 7, 7, 7 }, { 7, 7, 7 }, { 0, 0, 0 },
};

/* SDL 1.2 key symbols */
enum {
    K_SPACE = 32, K_1 = 49, K_2 = 50, K_5 = 53, K_6 = 54, K_9 = 57, K_x = 120, K_z = 122,
    K_UP = 273, K_DOWN = 274, K_RIGHT = 275, K_LEFT = 276,
    K_F1 = 282, K_F2 = 283, K_LCTRL = 306,
};

/* key bits (Type 13 result; sub_8063A10 is the I/O test) */
enum {
    B_KEY2 = 1 << 1,      /* enter */
    B_KEY1 = 1 << 2,      /* start / select */
    B_TEST = 1 << 6,
    B_SERVICE = 1 << 7,   /* a service credit */
    B_COIN = 1 << 30,     /* the coin switch (shown only; coins count from the frame reply) */
};

/* settings blocks: {type, load function (store = load + 1), bytes} */
static const struct { uint8_t type, fn; uint16_t size; } nv_blocks[] = {
    { 3, 0, 2436 },       /* game settings (0x86F6B00) */
    { 3, 2, 204 },        /* 0x86F6A20 */
    { 4, 0, 1044 },       /* bookkeeping (0x8540380) */
    { 5, 0, 32 },         /* 0x8541080 */
    { 6, 0, 40 },         /* 0x8540F20 */
    { 7, 3, 48 },         /* light-curtain calibration (0x8451F60) */
};
#define NV_BLOCKS (int)(sizeof(nv_blocks) / sizeof(nv_blocks[0]))
#define NV_BYTES  (2436 + 204 + 1044 + 32 + 40 + 48)
#define NV_CAL    5

/*
 * factory calibration (0x81E5EE0): pixel of x beam 19 and y beam 11, beam
 * spacing in tenths of a pixel; the rest the start-up check recomputes
 */
static const int32_t cal_default[12] = { 390, 292, 187, 249, 0, 800, 0, 600, 0, 78, 0, 46 };

static const struct e2000_api *api;

static struct {
    uint32_t buttons;
    int coins[2];                 /* inserted since the last frame */
    int scene;                    /* 0 attract, 1 game, 2 test (frame packet) */
    uint8_t groups[GROUPS], objects[OBJECTS];
    struct {
        uint8_t valid[NV_BLOCKS];
        uint8_t data[NV_BYTES];
        uint32_t password[4];
    } nv;
    double aim_x, aim_y;          /* crosshair, game screen pixels */
    int aim_dx, aim_dy, aim_speed;
    int hit_x, hit_y, hit_frames;
    int ticket_wait, tickets;     /* dispenser: frames to the next ticket, paid */
    uint32_t owed[2];             /* tickets the game says it still owes */
    int motor_pos, motor_target, motor_wait;
    uint32_t lamps[3];
    int demo;                     /* attract scene */
    int status;                   /* game status (flow_next) */
    int cleared;                  /* stages cleared this game */
    int region;
    int help;
} g;

static uint8_t *nv_block(int i)
{
    int k, off = 0;

    for (k = 0; k < i; k++)
        off += nv_blocks[k].size;
    return g.nv.data + off;
}

static void nv_load(void)
{
    FILE *f = fopen(NV_FILE, "rb");

    if (!f || fread(&g.nv, sizeof(g.nv), 1, f) != 1)
        memset(&g.nv, 0, sizeof(g.nv));
    if (f)
        fclose(f);
}

static void nv_save(void)
{
    FILE *f = fopen(NV_FILE, "wb");

    if (!f) {
        api->core->log("gogoball2: cannot write %s", NV_FILE);
        return;
    }
    fwrite(&g.nv, sizeof(g.nv), 1, f);
    fclose(f);
}

/* the calibration in use: the stored one, or the factory one the game will store */
static void calibration(int32_t *c)
{
    if (g.nv.valid[NV_CAL])
        memcpy(c, nv_block(NV_CAL), 48);
    else
        memcpy(c, cal_default, 48);
}

/*
 * A throw at the crosshair: the cell the game maps nearest to it.  The
 * calibration puts x beam 19 at cal[0] pixels and y beam 11 at cal[1], cal[2]
 * / cal[3] tenths of a pixel apart; the start-up sensor check (sub_8067420)
 * derives the rest, so that sub_8065340 maps cell (x, y) to
 * cal[0] + cal[2] * (x / 2 - 19) / 10, cal[1] + cal[3] * (y / 2 - 11) / 10.
 */
static void throw_ball(double sx, double sy)
{
    int32_t c[12];
    double fx, fy;

    calibration(c);
    fx = c[2] > 0 ? 2 * ((sx - c[0]) * 10.0 / c[2] + 19) : 0;
    fy = c[3] > 0 ? 2 * ((sy - c[1]) * 10.0 / c[3] + 11) : 0;
    g.hit_x = fx < 0 ? 0 : fx > CELLS_X - 1 ? CELLS_X - 1 : (int)(fx + 0.5);
    g.hit_y = fy < 0 ? 0 : fy > CELLS_Y - 1 ? CELLS_Y - 1 : (int)(fy + 0.5);
    g.hit_frames = HIT_FRAMES;
    if (api->core->verbose)
        api->core->log("gogoball2: ball at %.0f,%.0f -> cell %d,%d", sx, sy, g.hit_x, g.hit_y);
}

static void ggb_script(void);

static void ggb_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.aim_x = SCREEN_W / 2;
    g.aim_y = SCREEN_H / 2;
    g.aim_speed = (int)api->core->env_long("GGB_AIM", 8);
    if (g.aim_speed < 1)
        g.aim_speed = 8;
    g.region = (int)api->core->env_long("GGB_REGION", 4);
    g.motor_pos = g.motor_target = 1;
    g.ticket_wait = -1;
    g.help = 1;
    nv_load();
}

/* the frame packet's reply: keys, the blocked cells, coins */
static uint32_t frame(uint32_t *m)
{
    uint8_t *d = (uint8_t *)(m + E2000_PKT_DATA);
    int n = 0;

    ggb_script();
    g.aim_x += g.aim_dx * g.aim_speed;
    g.aim_y += g.aim_dy * g.aim_speed;
    g.aim_x = g.aim_x < 0 ? 0 : g.aim_x > SCREEN_W - 1 ? SCREEN_W - 1 : g.aim_x;
    g.aim_y = g.aim_y < 0 ? 0 : g.aim_y > SCREEN_H - 1 ? SCREEN_H - 1 : g.aim_y;
    if (g.hit_frames > 0) {
        g.hit_frames--;
        d[0] = g.hit_x;
        d[1] = g.hit_y;
        n = 1;
    }
    m[E2000_PKT_RET] = g.buttons;
    m[E2000_PKT_PR + 4] = n;                 /* cells, 2 bytes each from the data */
    m[E2000_PKT_PR + 5] = g.coins[0];
    m[E2000_PKT_PR + 6] = g.coins[1];
    g.coins[0] = g.coins[1] = 0;
    return 2 * n;
}

static uint32_t settings(uint32_t *m, uint32_t type, uint32_t fn)
{
    int i;

    for (i = 0; i < NV_BLOCKS; i++) {
        uint32_t size = nv_blocks[i].size;

        if (nv_blocks[i].type != type || (fn != nv_blocks[i].fn && fn != nv_blocks[i].fn + 1u))
            continue;
        if (fn == nv_blocks[i].fn) {     /* load: result 1 = blank, the game writes its defaults */
            if (!g.nv.valid[i]) {
                m[E2000_PKT_RET] = 1;
                return 0;
            }
            memcpy(m + E2000_PKT_DATA, nv_block(i), size);
            m[E2000_PKT_PR + 3] = size;
            return size;
        }
        memcpy(nv_block(i), m + E2000_PKT_DATA, size);
        g.nv.valid[i] = 1;
        nv_save();
        return 0;
    }
    return 0;
}

static uint32_t ggb_packet(int port, uint32_t *m)
{
    uint32_t type = m[E2000_PKT_PR], fn = m[E2000_PKT_PR + 1];
    uint32_t p[6], i;

    for (i = 0; i < 6; i++)
        p[i] = m[E2000_PKT_PR + 3 + i];          /* params 0..5 */
    if (api->core->verbose && !(port == 0x30 && type == 13))
        api->core->log("gogoball2: port 0x%x type %u.%u (%u %u %u)", port, type, fn, p[0], p[1], p[2]);
    for (i = E2000_PKT_PR + 2; i < E2000_PKT_DATA; i++)
        m[i] = 0;

    if (port == 0x50) {                          /* start-up: result 1, no error */
        m[E2000_PKT_RET] = 1;
        return 0;
    }
    if (port == 0x20) {
        uint8_t *used = type == 1 ? g.groups : g.objects;
        uint32_t n = type == 1 ? GROUPS : OBJECTS;

        switch (fn) {
        case 0:                                  /* reset */
        case 5:
            memset(used, 0, n);
            break;
        case 1:                                  /* allocate */
            for (i = 0; i < n - 1 && used[i]; i++)
                ;
            used[i] = 1;
            m[E2000_PKT_RET] = i;
            break;
        case 2:
            if (type == 1) {                     /* ActionGroup free */
                if (p[1] < n)
                    used[p[1]] = 0;
            } else if (p[1] < n) {               /* reserve: 0 = done */
                used[p[1]] = 1;
            }
            break;
        case 3:
            m[E2000_PKT_RET] = p[1] < n && used[p[1]];
            break;
        case 4:
            if (p[1] < n)
                used[p[1]] = 0;
            break;
        }
        return 0;
    }

    switch (type) {
    case 13:
        g.scene = p[2];
        return frame(m);
    case 3: case 4: case 5: case 6:
        return settings(m, type, fn);
    case 7:
        if (fn == 3 || fn == 4)
            return settings(m, type, fn);
        if (fn == 6) {                           /* beam states: 40 x then 24 y bits, all clear */
            m[E2000_PKT_PR + 4] = 4;
            memset(m + E2000_PKT_DATA, 0xff, 8);
            return 8;
        }
        if (fn == 9) {                           /* beam levels, 4 bits each */
            m[E2000_PKT_PR + 4] = 16;
            memset(m + E2000_PKT_DATA, 0x88, 32);
            return 32;
        }
        return 0;                                /* 0/2 on/off, 5/7 beam test, 8/10 levels, 11/12, 14 UR ok */
    case 9:                                      /* ticket dispenser */
        if (fn == 0)
            g.ticket_wait = TICKET_FRAMES;
        else if (fn == 1) {                      /* 1 paying, 6 one ticket out, (2 empty, 3 jam) */
            if (g.ticket_wait > 0) {
                g.ticket_wait--;
                m[E2000_PKT_RET2] = 1;
            } else if (g.ticket_wait == 0) {
                g.ticket_wait = -1;
                g.tickets++;
                m[E2000_PKT_RET2] = 6;
                api->core->log("gogoball2: ticket out (%d)", g.tickets);
            }
        } else if (fn == 4 && p[1] < 2)          /* tickets still owed */
            m[E2000_PKT_PR + 5] = g.owed[p[1]];
        else if (fn == 5 && p[1] < 2)
            g.owed[p[1]] = p[2];
        return 0;
    case 10:                                     /* ball stopper */
        if (fn <= 2) {
            g.motor_target = fn;
            g.motor_wait = MOTOR_FRAMES;
        } else if (fn == 3) {
            g.motor_wait = 1 << 30;
        } else if (fn == 4) {
            g.motor_wait = 0;
        } else if (fn == 6) {
            if (g.motor_wait > 0)
                g.motor_wait--;
            if (!g.motor_wait)
                g.motor_pos = g.motor_target;
            m[E2000_PKT_RET] = g.motor_wait ? 1 : 5;
            m[E2000_PKT_PR + 4] = MOTOR_FRAMES + 2;   /* the test's max / min times */
            m[E2000_PKT_PR + 5] = MOTOR_FRAMES - 2;
        } else if (fn == 7) {
            m[E2000_PKT_PR + 4] = g.motor_pos;
        }
        return 0;
    case 12:                                     /* lamps: 0 (value, lamp), 1 read back */
        if (fn == 0 && p[2] < 3)
            g.lamps[p[2]] = p[1];
        else if (fn == 1)
            for (i = 0; i < 3; i++)
                m[E2000_PKT_PR + 4 + i] = g.lamps[i];
        return 0;
    case 15:                                     /* password, 4 digits */
        if (fn == 0)
            m[E2000_PKT_RET] = !memcmp(p + 1, g.nv.password, sizeof(g.nv.password));
        else if (fn == 1) {
            memcpy(g.nv.password, p + 1, sizeof(g.nv.password));
            nv_save();
        }
        return 0;
    case 16:                                     /* the game re-keys a stream after sending */
        api->rekey(fn == 0, p + 1, 20);
        return 0;
    case 18: {                                   /* versions and region (0x8072440) */
        uint8_t *d = (uint8_t *)(m + E2000_PKT_DATA);

        memset(d, 0, 56);
        memcpy(d, "IGS027A", 7);
        d[8] = g.region;
        memcpy(d + 20, "HLE", 3);                /* "I:S-" */
        memcpy(d + 28, "IGS027A", 7);
        memcpy(d + 48, "EMU", 3);                /* "O:S-" */
        m[E2000_PKT_PR + 3] = 56;
        return 56;
    }
    case 17:
        /*
         * Flow (invented order).  Attract (sub_80905B0): 0 sets the scene, 1
         * asks for the next (0 logos, 1 team mark, 2 title, 3 how to play, 4
         * demo, 5 skill demos), each in turn.  Game (sub_808FE80): 2 sets
         * the status, 3 an event -> the next status (flow_next).  4 -> fault
         * bits (0 = ok).
         */
        if (fn == 0)
            g.demo = p[1] % DEMO_SCENES;
        else if (fn == 1)
            m[E2000_PKT_RET] = g.demo = (g.demo + 1) % DEMO_SCENES;
        else if (fn == 2) {
            g.status = p[1] & 7;
            g.cleared = 0;
        } else if (fn == 3) {
            int next = flow_next[g.status & 7][p[1] < 3 ? p[1] : 0];

            /* the score screen averages over the stages cleared: none, no score */
            if (g.status == 3 && p[1] == 0)
                g.cleared++;
            if (next == 4 && !g.cleared)
                next = 7;
            api->core->log("gogoball2: game status %d, event %u -> %d", g.status, p[1], next);
            m[E2000_PKT_RET] = g.status = next;
        }
        return 0;
    default:                                     /* 11 meters, 14 */
        return 0;
    }
}

static const struct e2000_bios bios[] = {
    { 0xFE0C1, "L4S5MG3" },
    { 0xFEC84, "6A6IXE19C-00" },
    { 0xFECDF, "I6A6IXE19" },
    { 0, NULL },
};

/*
 * Keys.  The game's own SDL sees the keyboard too: Esc quits it, / starts
 * the warm-up clock.  None of them is used here.
 */
static void ggb_key(int sym, int down)
{
    uint32_t bit = 0;

    switch (sym) {
    case K_LEFT: g.aim_dx = down ? -1 : g.aim_dx < 0 ? 0 : g.aim_dx; return;
    case K_RIGHT: g.aim_dx = down ? 1 : g.aim_dx > 0 ? 0 : g.aim_dx; return;
    case K_UP: g.aim_dy = down ? -1 : g.aim_dy < 0 ? 0 : g.aim_dy; return;
    case K_DOWN: g.aim_dy = down ? 1 : g.aim_dy > 0 ? 0 : g.aim_dy; return;
    case K_SPACE: case K_LCTRL:
        if (down)
            throw_ball(g.aim_x, g.aim_y);
        return;
    case K_1: case K_z: bit = B_KEY1; break;
    case K_2: case K_x: bit = B_KEY2; break;
    case K_F2: bit = B_TEST; break;
    case K_9: bit = B_SERVICE; break;
    case K_5: case K_6:
        if (down) {
            g.coins[sym == K_6]++;
            g.buttons |= B_COIN;
        } else
            g.buttons &= ~B_COIN;
        return;
    case K_F1:
        if (down)
            g.help = !g.help;
        return;
    default: return;
    }
    if (down)
        g.buttons |= bit;
    else
        g.buttons &= ~bit;
}

static void ggb_draw(int w, int h)
{
    static const char *lines[] = {
        "5/6 coin  1 start  2 enter",
        "Arrows aim  Space throw",
        "F2 test  9 service",
        "F1 hide this",
    };
    const struct igs_core *c = api->core;
    int x = (int)(g.aim_x * w / SCREEN_W), y = (int)(g.aim_y * h / SCREEN_H), i, ty;

    c->fill(x - 12, y - 1, 25, 3, 1, 1, 0, 0.9f);
    c->fill(x - 1, y - 12, 3, 25, 1, 1, 0, 0.9f);
    if (!g.help)
        return;
    ty = h - 4 - 4 * c->font_h;
    c->fill(0, ty - 4, 27 * c->font_w, 4 * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < 4; i++)
        c->text(6, ty + (3 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

/*
 * GGB_SCRIPT="sec:key[+],..." presses a key at a time from start-up (key:
 * coin, coin2, key1, key2, test, service, left, right, up, down, throw, and
 * rthrow, a throw at a random point).  A press lasts 0.2 s, or with a
 * trailing + until the next entry.  Runs from the frame packet.
 */
static void ggb_script(void)
{
    static const struct { const char *name; int sym; } keys[] = {
        { "coin", K_5 }, { "coin2", K_6 }, { "key1", K_1 }, { "key2", K_2 }, { "test", K_F2 },
        { "service", K_9 }, { "left", K_LEFT }, { "right", K_RIGHT }, { "up", K_UP },
        { "down", K_DOWN }, { "throw", K_SPACE }, { "rthrow", -1 },
    };
    static const char *p;
    static double t0, release_at;
    static int held;
    const struct igs_core *c = api->core;
    double now, at;
    char name[16];
    int n = 0, hold, i;

    if (!p) {
        p = c->env("GGB_SCRIPT", "");
        t0 = c->now();
    }
    now = c->now() - t0;
    if (held && release_at && now >= release_at) {
        ggb_key(held, 0);
        held = 0;
    }
    if (!*p || sscanf(p, "%lf:%15[a-z0-9]%n", &at, name, &n) != 2 || now < at)
        return;
    hold = p[n] == '+';
    p += n + hold;
    if (*p == ',')
        p++;
    if (held)
        ggb_key(held, 0);
    held = 0;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (!strcmp(name, keys[i].name)) {
            c->log("gogoball2: script %.1f s %s%s", now, name, hold ? " (held)" : "");
            if (keys[i].sym < 0) {
                throw_ball(80 + rand() % (SCREEN_W - 160), 80 + rand() % (SCREEN_H - 160));
                return;
            }
            held = keys[i].sym;
            ggb_key(held, 1);
            release_at = hold ? 0 : now + 0.2;
        }
}

static void ggb_status(char *buf, size_t n)
{
    static const char *scenes[] = { "attract", "game", "test" };

    if (g.scene >= 0 && g.scene <= 2)
        snprintf(buf, n, "%s", scenes[g.scene]);
    else
        buf[0] = 0;
    if (g.tickets)
        snprintf(buf + strlen(buf), n - strlen(buf), ", %d tickets paid", g.tickets);
}

static const struct e2000_game game = {
    .version = E2000_API_VERSION,
    .name = "gogoball2",
    .bios = bios,
    .reset = ggb_reset,
    .key = ggb_key,
    .draw = ggb_draw,
    .status = ggb_status,
    .transport = E2000_IGS027A,
    /* sub_80832F0: 12 bytes of "EncodeGGB    " / "DecodeGGB    " */
    .pkt_key_from_game = "EncodeGGB   ",
    .pkt_key_to_game = "DecodeGGB   ",
    .pkt_key_len = 12,
    .packet = ggb_packet,
};

__attribute__((visibility("default")))
const struct e2000_game *e2000_game_entry(const struct e2000_api *a)
{
    api = a;
    return &game;
}
