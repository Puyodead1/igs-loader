/*
 * Speed Driver 2 (E2000): what its IGS036 firmware does, as far as the game
 * needs.  docs/SpeedDriver2.md, docs/IGS_E2000.md (Speed Driver 2).
 *
 * The chip talks the IGS036 message transport in Speed Driver 2's layout:
 * header words (steer, gas, brake, the main state, credits, two counters)
 * and sub-messages [length, type, data, sum | 0x80] both ways.
 *
 * The chip directs the game: it sets the main state (the game reports its
 * own moves inside the race and the replay), loads 2D texture pages, draws
 * the select, name entry, continue and game-over screens from the game's 2D
 * art, sends the game data (mode, course, car), the buttons and the race's
 * commands, runs the ranking and the AI throttle, plays the title music and
 * counts the coins.  Its ROM is not dumped: the flow is reconstructed from
 * the game's reactions; layouts and timing are invented.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2000/e2000.h"

static const struct e2000_api *api;

#define LOG(...) api->core->log("speeddriver2: " __VA_ARGS__)

/* main states (high word) and sub-states: hdr[E2000_SD2_STATE] = main << 16 | sub */
#define ST(m, s)   ((uint32_t)(m) << 16 | (s))
enum { M_INIT = 0, M_TITLE = 2, M_SELECT = 6, M_RACE = 8, M_RESULT = 9, M_OPERATOR = 11 };
enum { MODE_TA, MODE_STORY, MODE_OT };
#define CARS 20

/* chip -> game sub-messages */
enum {
    C_LOAD_2D = 0x13, C_RELEASE_2D = 0x19, C_TEXT = 0x43, C_NUMBER = 0x44, C_COLOR = 0x47, C_SPRITES_OFF = 0x4B, C_SCALE = 0x4C,
    C_SPRITE_OFF = 0x4D, C_GEAR_UP = 0x5A, C_GEAR_DOWN = 0x5B, C_VIEW = 0x5C, C_N2O = 0x5E, C_SPRITE = 0x6C, C_LOAD_SOUND = 0x70, C_PLAY = 0x72, C_STOP = 0x73, C_PLAYER_DATA = 0x89,
    C_RANK = 0x8A, C_RANK_ENTRY = 0x8B, C_RANK_NAME = 0x8C, C_AUTOPILOT = 0x8E, C_GAME_DATA = 0x90, C_KEYS = 0x91,
    C_HDR_PREFIX = 0xB2, C_AI = 0xC0,
};
/* game -> chip sub-messages */
enum {
    G_RANK_IN = 0x30, G_WIN = 0x32, G_STORE_SETUP = 0x39, G_FILM_END = 0x40, G_AI = 0x60, G_TITLE_STEP = 0x80,
    G_HDR_ACK = 0xB3,
};

/* buttons (0x91 bits, the game's key table byte_84A66C0) */
enum { B_START, B_VIEW, B_N2O, B_GEAR_DOWN, B_GEAR_UP, B_COUNT };

/* SDL 1.2 key symbols */
enum {
    K_RETURN = 13, K_1 = 49, K_5 = 53, K_c = 99, K_q = 113, K_w = 119, K_x = 120,
    K_UP = 273, K_DOWN = 274, K_RIGHT = 275, K_LEFT = 276, K_F1 = 282, K_F2 = 283,
};

/* sprite slots the chip's overlays use (the game's own run up to 370) */
#define SLOT_CREDITS 391
#define SLOT_NUMBER  392
/* 2D images (2dtexture.inf, global index) */
#define IMG_CREDITS  75
#define IMG_FREEPLAY 76
#define IMG_DIGITS   84
#define IMG_FONT     734            /* A..Z 0..9 */
#define IMG_SMALL_DIGITS 958
#define IMG_BIG_DIGITS   753

enum { PH_BOOT, PH_ATTRACT, PH_MENU, PH_RACE, PH_AFTER, PH_TEST };
enum { SCR_MODE, SCR_CAR, SCR_GEAR, SCR_COURSE, SCR_DIR, SCR_NAME, SCR_RANK, SCR_CONTINUE, SCR_OVER };

/*
 * Exchanges a state the chip sets is held: the game also exchanges inside its
 * loading loops, where a state read is not acted on (the main loop's read is).
 */
#define HOLD 6

static struct {
    uint32_t frame;
    uint32_t state;                  /* what the chip tells the game */
    uint32_t game_state;             /* the game's own main state (reported) */
    int phase;
    uint32_t phase_frame;            /* frame the phase started */
    int hdr_acked, overlay, start_pending, bgm;
    int title_step, film_end;
    /* coins: the game's store settings (0x39) */
    int free_play, start_coins, cont_coins;
    int coins, credits_shown;
    int games;
    /* the chip's screens and the player's choices */
    int scr, cursor, drawn, dirty, nav_left, nav_right;
    int test;
    uint32_t scr_frame;
    double scr_time;
    int mode, car, gear, course, dir, stage;
    int rank_in, win;                /* the race's results (0x30, 0x32; -1 none) */
    char name[9];
    int name_len, name_shown, letter;
    /* controls */
    int key_left, key_right, key_gas, key_brake, key_n2o, n2o_edge;
    float steer, gas, brake;
    int pressed[B_COUNT];            /* edge events not yet sent */
    int lean_frames;
    int help;
} g;

static uint32_t put(uint8_t *out, uint32_t n, uint32_t max, int type, const void *data, uint32_t len);
static void sd_script(void);

/*
 * The first 128 bytes of data/data.hdr (the zpk directory), which the chip
 * supplies (0xB2): the directory count and the first seven directory records
 * {first file index, name}, the last one without its NUL.  Rebuilt from the
 * file records' directory indices and the paths in 2dtexture.inf; the XOR of
 * the bytes matches the file's check byte (0xDD).
 */
static uint32_t hdr_prefix(uint8_t *p)
{
    static const struct { uint32_t first; const char *name; } dirs[] = {
        { 0, "data" }, { 0, "data\\2d" }, { 0, "data\\2d\\ui" }, { 39, "data\\2d\\ui\\mark" },
        { 68, "data\\2d\\operator" }, { 72, "data\\2d\\effect" }, { 72, "data\\2d\\effect\\lensflare" },
    };
    uint32_t n, i, v = 143;   /* directories */

    memcpy(p, &v, 4);
    n = 4;
    for (i = 0; i < 7; i++) {
        memcpy(p + n, &dirs[i].first, 4);
        strcpy((char *)p + n + 4, dirs[i].name);
        n += 4 + strlen(dirs[i].name) + (i < 6);
    }
    return n;
}

/* append one sub-message [length, type, data, sum | 0x80] */
static uint32_t put(uint8_t *out, uint32_t n, uint32_t max, int type, const void *data, uint32_t len)
{
    uint8_t sum = 0;
    uint32_t i;

    if (len > 252 || n + len + 3 > max)
        return n;
    out[n] = len + 3;
    out[n + 1] = type;
    if (len)
        memcpy(out + n + 2, data, len);
    for (i = 0; i < len + 2; i++)
        sum += out[n + i];
    out[n + len + 2] = sum | 0x80;
    return n + len + 3;
}

static uint32_t put8(uint8_t *out, uint32_t n, uint32_t max, int type, int v)
{
    uint8_t b = v;

    return put(out, n, max, type, &b, 1);
}

/* 0x6C: sprite slot shows image at x, y (i16 each) */
static uint32_t sprite(uint8_t *out, uint32_t n, uint32_t max, int slot, int img, int x, int y)
{
    int16_t d[4] = { slot, img, x, y };

    return put(out, n, max, C_SPRITE, d, 8);
}

/* 0x44: number sprite: slot, digit image, x, y, digit width, digits, value (i32) */
static uint32_t number(uint8_t *out, uint32_t n, uint32_t max, int slot, int img, int x, int y,
                       int w, int digits, int32_t v)
{
    uint8_t d[16];
    int16_t h[6] = { slot, img, x, y, w, digits };

    memcpy(d, h, 12);
    memcpy(d + 12, &v, 4);
    return put(out, n, max, C_NUMBER, d, 16);
}

static int credits(void)
{
    return g.start_coins > 0 ? g.coins / g.start_coins : g.coins;
}

static int env_int(const char *name, int def)
{
    return (int)strtol(api->core->env(name, ""), NULL, 0) ?: def;
}

static void set_phase(int ph)
{
    g.phase = ph;
    g.phase_frame = g.frame;
}

static void sd_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.start_coins = g.cont_coins = 1;
    g.help = 1;
    g.lean_frames = env_int("SD2_STEER", 20);
    g.state = ST(M_INIT, 0);
}

/*
 * The AI throttle (0x60 -> 0xC0): the game's own law for overtake mode's
 * cars (sub_8131A3C, mode 2), with the controllers' defaults (thresholds 10,
 * −10, −20 m/s; the gas and brake move at most dt / 0.1 per frame).  The
 * query carries the car, its speed error (target − current, ×1000) and a
 * second value (×1000, unused here); the answer sets gas and brake.
 */
#define AI_CARS 8
static uint32_t ai(uint8_t *out, uint32_t n, uint32_t max, const uint8_t *d)
{
    static float gas[AI_CARS], brk[AI_CARS];
    const float up = 10, down = -10, hard = -20, step = (1.0f / 60) / 0.1f, q = 1.5707964f;
    int32_t idx, e1000, x, r[4];
    float e, v;

    memcpy(&idx, d, 4);
    memcpy(&e1000, d + 4, 4);
    memcpy(&x, d + 8, 4);
    if (idx < 0 || idx >= AI_CARS)
        return n;
    e = e1000 / 1000.0f;
    if (fabsf(e) >= 0.83f) {
        if (e <= 0) {
            if (hard < e) {
                if (e <= down) {        /* past −10: brake */
                    v = 1 - cosf((e - down) / (hard - down) * q);
                    brk[idx] += v < step ? v : step;
                    gas[idx] = 0;
                } else {                /* ease off */
                    v = 1 - cosf(e / down * q);
                    gas[idx] -= v < step ? v : step;
                    brk[idx] = 0;
                }
            } else {
                v = 1 - brk[idx];
                brk[idx] += v < step ? v : step;
            }
        } else {
            if (e < up) {
                if (gas[idx] == 0)
                    gas[idx] = 0.5f;
                v = 1 - cosf(e / up * q);
            } else {
                v = 1 - gas[idx];
            }
            gas[idx] += v < step ? v : step;
            brk[idx] = 0;
        }
    } else {
        brk[idx] = 0;
    }
    gas[idx] = gas[idx] < 0 ? 0 : gas[idx] > 1 ? 1 : gas[idx];
    brk[idx] = brk[idx] < 0 ? 0 : brk[idx] > 1 ? 1 : brk[idx];
    r[0] = idx;
    r[1] = (int32_t)(gas[idx] * 1000);
    r[2] = (int32_t)(brk[idx] * 1000);
    r[3] = x;
    return put(out, n, max, C_AI, r, 16);
}

/*
 * The chip's screens.  The game supplies the art (2dtexture.inf pages 1..3)
 * and a 3D background (main state 6); the chip draws the 2D sprites.  The
 * screens' order follows the art and the game data; layout and timing are
 * invented.
 */
static const uint16_t mode_card[3] = { 142, 190, 236 };          /* TIME ATTACK, STORY MODE, OVERTAKE */
static const uint8_t mode_id[3] = { MODE_TA, MODE_STORY, MODE_OT };
/* car type -> name plate (the ranking's small names, word_83435A0, matched by name) */
static const uint16_t car_plate[CARS] = {
    727, 744, 746, 747, 739, 740, 732, 733, 729, 730, 728, 731, 736, 735, 738, 737, 742, 741, 743, 745,
};
/* map 0..6: Shanghai, Taroko, Taipei, Great Wall, Macau, F1 (神州), Alishan (scene table 0x834FF40) */
static const uint16_t course_button[7] = { 613, 614, 615, 618, 619, 620, 610 };

static uint32_t text(uint8_t *out, uint32_t n, uint32_t max, int slot, int x, int y, const char *s)
{
    uint8_t d[12 + 32];
    int16_t h[6];
    int len = 0;

    /* the font's glyphs: A..Z 1..26, 0..9 27..36 (byte_8342F60) */
    for (; s[len] && len < 31; len++)
        d[12 + len] = s[len] >= 'A' && s[len] <= 'Z' ? s[len] - 'A' + 1 :
                      s[len] >= '0' && s[len] <= '9' ? s[len] - '0' + 27 : 0;
    d[12 + len] = 0;
    h[0] = slot, h[1] = IMG_FONT, h[2] = x, h[3] = y, h[4] = 1, h[5] = len;
    memcpy(d, h, 12);
    return put(out, n, max, C_TEXT, d, 13 + len);
}

static uint32_t color(uint8_t *out, uint32_t n, uint32_t max, int slot, uint32_t argb)
{
    uint8_t d[6];
    int16_t s = slot;

    memcpy(d, &s, 2);
    memcpy(d + 2, &argb, 4);
    return put(out, n, max, C_COLOR, d, 6);
}

static uint32_t scale(uint8_t *out, uint32_t n, uint32_t max, int slot, float f)
{
    uint8_t d[6];
    int16_t s = slot;

    memcpy(d, &s, 2);
    memcpy(d + 2, &f, 4);
    return put(out, n, max, C_SCALE, d, 6);
}

static void menu_open(int scr)
{
    g.phase = PH_MENU;
    g.scr = scr;
    g.scr_frame = g.frame;
    g.scr_time = api->core->now();
    g.drawn = 0;
    g.cursor = 0;
    if (scr == SCR_CAR)
        g.cursor = g.car;
    if (scr == SCR_COURSE)
        g.cursor = g.course % 7;
    if (scr == SCR_NAME) {
        memset(g.name, 0, sizeof(g.name));
        g.name_len = 0;
    }
    LOG("screen %d", scr);
}

/* the race: the race's 2D pages stack on page 1 (its HUD's indices count from there) */
static uint32_t race_start(uint8_t *out, uint32_t n, uint32_t max)
{
    uint8_t d[0x6C];
    float limit = 0;

    n = put(out, n, max, C_SPRITES_OFF, NULL, 0);
    n = put8(out, n, max, C_RELEASE_2D, 3);
    n = put8(out, n, max, C_RELEASE_2D, 2);
    memset(d, 0, sizeof(d));
    d[0x00] = g.mode;                /* ubMode */
    d[0x01] = g.course % 7;          /* ubMap */
    d[0x02] = g.dir;                 /* ubFromTo */
    d[0x03] = g.stage;               /* ubSelLevel: the story stage */
    memcpy(d + 0x1A, "PLAYER", 6);   /* sbRicerName */
    d[0x23] = g.car;                 /* ubCarType */
    d[0x25] = g.gear;                /* ubATMT: 1 manual */
    memcpy(d + 0x38, &limit, 4);
    LOG("race: mode %d, course %d direction %d, stage %d, car %d, %s", g.mode, d[1], d[2], d[3], g.car,
        g.gear ? "MT" : "AT");
    if (getenv("SD2_AUTO"))          /* testing: the game's autopilot drives the player's car */
        n = put8(out, n, max, C_AUTOPILOT, 1);
    n = put(out, n, max, C_GAME_DATA, d, sizeof(d));
    n = put8(out, n, max, C_PLAYER_DATA, 0);
    g.rank_in = g.win = -1;
    g.games++;
    g.state = ST(M_RACE, 0);
    g.game_state = 0;
    set_phase(PH_RACE);
    return n;
}

static uint32_t menu(uint8_t *out, uint32_t n, uint32_t max)
{
    static const int items[] = { 3, CARS, 2, 7, 2, 0, 0, 2, 0 };
    uint32_t t = g.frame - g.scr_frame, limit = g.scr == SCR_NAME ? 30 * 60 : g.scr == SCR_CONTINUE ? 10 * 60 :
                 g.scr == SCR_RANK ? 8 * 60 : g.scr == SCR_OVER ? 4 * 60 : 20 * 60;
    int left = g.nav_left, right = g.nav_right, start = g.pressed[B_START], moved = 0, i, cr = credits();
    int done = start || t >= limit;

    g.nav_left = g.nav_right = 0;
    g.pressed[B_START] = 0;
    if (items[g.scr] && (left || right)) {
        g.cursor = (g.cursor + (right ? 1 : items[g.scr] - 1)) % items[g.scr];
        moved = 1;
    }
    if (!g.drawn) {
        /* the select background; ranking: result page 13 */
        g.state = g.scr == SCR_RANK ? ST(M_RESULT, 13 << 8) : ST(M_SELECT, 0);
        n = put(out, n, max, C_SPRITES_OFF, NULL, 0);
        if (g.scr != SCR_RANK)       /* a full-screen page-1 backdrop */
            n = sprite(out, n, max, 299, g.scr == SCR_OVER ? 3 : 5, 0, 0);
        g.overlay = 0;
        switch (g.scr) {
        case SCR_MODE:
            n = sprite(out, n, max, 300, 689, 213, 24);
            for (i = 0; i < 3; i++)
                n = sprite(out, n, max, 301 + i, mode_card[i], 40 + 187 * i, 120);
            break;
        case SCR_CAR:
            n = sprite(out, n, max, 300, 677, 213, 24);
            break;
        case SCR_GEAR:
            n = sprite(out, n, max, 300, 679, 213, 24);
            n = sprite(out, n, max, 301, 394, 100, 170);
            n = sprite(out, n, max, 302, 395, 338, 170);
            break;
        case SCR_COURSE:
            n = sprite(out, n, max, 300, 691, 213, 24);
            for (i = 0; i < 7; i++)
                n = sprite(out, n, max, 301 + i, course_button[i], 10 + 157 * (i % 4) + (i / 4) * 78, 150 + 110 * (i / 4));
            break;
        case SCR_DIR:
            n = sprite(out, n, max, 300, 684, 213, 24);
            n = sprite(out, n, max, 301, 611, 150, 200);
            n = sprite(out, n, max, 302, 612, 340, 200);
            break;
        case SCR_NAME:
            n = sprite(out, n, max, 300, 690, 213, 24);
            break;
        case SCR_RANK:
            n = put8(out, n, max, C_RANK_ENTRY, 0);  /* the race's ranking entry (course, place) */
            n = put8(out, n, max, C_RANK, 2);        /* load that course's table */
            n = put8(out, n, max, C_RANK, 3);        /* show it, the entry marked */
            break;
        case SCR_CONTINUE:
            n = sprite(out, n, max, 300, 687, 213, 24);
            n = sprite(out, n, max, 301, 405, 100, 220);
            n = sprite(out, n, max, 302, 406, 330, 220);
            break;
        case SCR_OVER:
            n = sprite(out, n, max, 300, 95, 72, 72);
            break;
        }
        g.drawn = 1;
        g.dirty = 1;
    }
    if (moved)
        g.dirty = 1;
    if (g.frame < g.scr_frame + HOLD)    /* let the state take first */
        return n;
    /* the cursor: unselected items dimmed */
    if (g.dirty && g.scr != SCR_NAME) {
        g.dirty = 0;
        switch (g.scr) {
        case SCR_MODE: case SCR_GEAR: case SCR_COURSE: case SCR_DIR: case SCR_CONTINUE:
            for (i = 0; i < items[g.scr]; i++)
                n = color(out, n, max, 301 + i, i == g.cursor ? 0xFFFFFFFF : 0xFF505050);
            break;
        case SCR_CAR:
            n = sprite(out, n, max, 301, car_plate[g.cursor], 200, 170);
            n = scale(out, n, max, 301, 2.5f);
            n = number(out, n, max, 302, IMG_BIG_DIGITS, 280, 330, 0, 2, g.cursor + 1);
            break;
        }
    }
    if (g.scr != SCR_RANK && g.scr != SCR_OVER)    /* seconds left */
        n = number(out, n, max, 310, IMG_BIG_DIGITS, 540, 24, 0, 2, (limit - t) / 60);
    if (g.scr == SCR_CONTINUE || g.scr == SCR_OVER || g.scr == SCR_NAME || g.scr == SCR_RANK) {
        if (!g.overlay || cr != g.credits_shown) {
            n = sprite(out, n, max, SLOT_CREDITS, g.free_play ? IMG_FREEPLAY : IMG_CREDITS, 478, 430);
            if (!g.free_play)
                n = number(out, n, max, SLOT_NUMBER, IMG_DIGITS, 600, 441, 0, 2, cr);
            g.overlay = 1;
            g.credits_shown = cr;
        }
    }
    if (g.scr == SCR_NAME) {
        /* letters A..Z, 0..9, then END */
        static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        int nset = (int)sizeof(set) - 1;
        char shown[12];

        if (left || right) {
            g.letter = (g.letter + (right ? 1 : nset)) % (nset + 1);
            moved = 1;
        }
        if (moved || g.dirty || g.name_len != g.name_shown) {
            g.dirty = 0;
            snprintf(shown, sizeof(shown), "%s%c", g.name, g.letter < nset ? set[g.letter] : ' ');
            n = text(out, n, max, 301, 200, 160, shown);
            n = text(out, n, max, 302, 280, 280, "END");
            n = scale(out, n, max, 301, 3.0f);
            n = scale(out, n, max, 302, 2.0f);
            n = color(out, n, max, 302, g.letter == nset ? 0xFFFFFFFF : 0xFF505050);
            g.name_shown = g.name_len;
        }
        if (start && g.letter < nset && g.name_len < 8) {
            g.name[g.name_len++] = set[g.letter];
            if (g.name_len < 8)
                return n;
        } else if (!start && t < limit) {
            return n;
        }
        /* done: the name (8 bytes) and "named" (1) */
        {
            uint8_t d[9];

            if (!g.name_len)
                strcpy(g.name, "PLAYER");
            memset(d, 0, sizeof(d));
            memcpy(d, g.name, strlen(g.name));
            d[8] = 1;
            n = put(out, n, max, C_RANK_NAME, d, 9);
            LOG("name %s", g.name);
        }
        menu_open(SCR_RANK);
        return n;
    }
    if (!done)
        return n;
    switch (g.scr) {
    case SCR_MODE:
        g.mode = mode_id[g.cursor];
        menu_open(SCR_CAR);
        break;
    case SCR_CAR:
        g.car = g.cursor;
        menu_open(SCR_GEAR);
        break;
    case SCR_GEAR:
        g.gear = g.cursor;
        if (g.mode == MODE_STORY)
            n = race_start(out, n, max);
        else
            menu_open(SCR_COURSE);
        break;
    case SCR_COURSE:
        g.course = g.cursor;
        g.dir = 0;
        if (g.mode == MODE_TA)
            menu_open(SCR_DIR);
        else
            n = race_start(out, n, max);
        break;
    case SCR_DIR:
        g.dir = g.cursor;
        n = race_start(out, n, max);
        break;
    case SCR_RANK:
        menu_open(SCR_CONTINUE);
        break;
    case SCR_CONTINUE:
        if (start && g.cursor == 0 && (g.free_play || g.coins >= g.cont_coins)) {
            if (!g.free_play)
                g.coins -= g.cont_coins;
            if (g.mode == MODE_STORY) {
                if (g.win == 1)
                    g.stage++;
                n = race_start(out, n, max);
            } else {
                menu_open(SCR_COURSE);
            }
        } else if (start && g.cursor == 0) {
            g.scr_frame = g.frame - (limit - 60 * 10);    /* no credit: keep waiting */
        } else {
            menu_open(SCR_OVER);
        }
        break;
    case SCR_OVER:
        n = put(out, n, max, C_SPRITES_OFF, NULL, 0);
        g.state = ST(M_TITLE, 0);
        g.title_step = 0;
        set_phase(PH_ATTRACT);
        break;
    }
    return n;
}

static float ramp(float v, float to, float step)
{
    return v < to ? (v + step > to ? to : v + step) : (v - step < to ? to : v - step);
}

static void controls(void)
{
    float step = 1.0f / (g.lean_frames > 0 ? g.lean_frames : 1);

    g.steer = ramp(g.steer, (float)(g.key_right - g.key_left), step);
    g.gas = ramp(g.gas, g.key_gas ? 1.0f : 0.0f, 0.1f);
    g.brake = ramp(g.brake, g.key_brake ? 1.0f : 0.0f, 0.2f);
}

static uint32_t sd_message(const uint8_t *in, uint32_t nin, uint32_t *hdr, uint8_t *out, uint32_t max)
{
    uint32_t i, len, n = 0, w = hdr[E2000_SD2_IN_WORD];
    uint16_t keys = 0;
    int b, cr;

    sd_script();
    /* the game's sub-messages */
    for (i = 0; in && i + 3 <= nin; i += len) {
        const uint8_t *d = in + i + 2;
        int type;

        len = in[i];
        if (len < 3 || i + len > nin)
            break;
        type = in[i + 1];
        if (api->core->verbose) {
            char buf[64] = "";
            uint32_t k;
            int o = 0;

            for (k = 0; k + 3 < len && o < 48; k++)
                o += snprintf(buf + o, sizeof(buf) - o, " %02x", d[k]);
            LOG("f%u in 0x%02x:%s", g.frame, type, buf);
        }
        switch (type) {
        case G_HDR_ACK:
            g.hdr_acked = 1;
            break;
        case G_STORE_SETUP:          /* bFreePlay, iStartCoin[2], iContinueCoin[2] */
            g.free_play = d[0];
            g.start_coins = d[1] ? d[1] : 1;
            g.cont_coins = d[3] ? d[3] : 1;
            LOG("store settings: free play %d, %d coin(s) a game", g.free_play, g.start_coins);
            break;
        case G_AI:
            n = ai(out, n, max, d);
            break;
        case G_RANK_IN:              /* time attack: the time makes the ranking */
            g.rank_in = getenv("SD2_TEST_RANKIN") ? 1 : d[0];    /* testing: force the name entry */
            LOG("ranking: %s", d[0] ? "in" : "out");
            break;
        case G_WIN:                  /* story, overtake: stage won */
            g.win = d[0];
            LOG("stage %s", d[0] ? "won" : "lost");
            break;
        case G_TITLE_STEP:           /* the live show's step; 6 = over */
            g.title_step = d[0];
            break;
        case G_FILM_END:
            g.film_end = 1;
            break;
        }
    }
    /* the game's own moves (race, replay): high word 0xE = no change */
    if ((w >> 16) != 0xE && w != g.game_state) {
        g.game_state = w;
        g.state = w;
        LOG("f%u game state %u.%u", g.frame, w >> 16, w & 0xffff);
    }
    {
        static uint32_t last = 0xffffffff;

        if (g.state != last)
            LOG("f%u chip state %u.%u (phase %d)", g.frame, g.state >> 16, g.state & 0xffff, g.phase);
        last = g.state;
    }

    switch (g.phase) {
    case PH_BOOT:                    /* the zpk header prefix, then the title's 2D pages */
        if (!g.hdr_acked) {
            uint8_t p[256];

            n = put(out, n, max, C_HDR_PREFIX, p, hdr_prefix(p));
            break;
        }
        /* the common sounds (this opens the audio device); the title's 2D pages (indices are in load order) */
        n = put8(out, n, max, C_LOAD_SOUND, 1);
        n = put8(out, n, max, C_LOAD_2D, 1);
        n = put8(out, n, max, C_LOAD_2D, 2);
        n = put8(out, n, max, C_LOAD_2D, 3);
        set_phase(PH_ATTRACT);
        g.state = ST(M_TITLE, 0);
        break;
    case PH_ATTRACT:
        /* title init for one exchange, then the live show; again when it is over */
        if (g.state == ST(M_TITLE, 0) && g.frame > g.phase_frame + HOLD) {
            g.state = ST(M_TITLE, 1);
            if (!g.bgm) {           /* the title music on channel 0 (the live show fades it) */
                uint8_t p[2] = { 0, 0 };

                n = put(out, n, max, C_PLAY, p, 2);
                g.bgm = 1;
            }
            g.film_end = 0;
            g.overlay = 0;
        } else if (g.film_end) {
            g.film_end = 0;
            g.state = ST(M_TITLE, 0);
            g.phase_frame = g.frame;
        }
        /* overlay (invented): CREDITS / FREE PLAY and the count */
        cr = credits();
        if (g.state == ST(M_TITLE, 1) && g.title_step == 3) {
            if (!g.overlay || cr != g.credits_shown) {
                n = sprite(out, n, max, SLOT_CREDITS, g.free_play ? IMG_FREEPLAY : IMG_CREDITS, 478, 430);
                if (!g.free_play)
                    n = number(out, n, max, SLOT_NUMBER, IMG_DIGITS, 600, 441, 10, 2, cr);
                g.overlay = 1;
                g.credits_shown = cr;
            }
        }
        if (g.test && g.state == ST(M_TITLE, 1) && g.title_step == 3) {
            g.test = 0;
            n = put8(out, n, max, C_STOP, 0);
            g.bgm = 0;
            g.state = ST(M_OPERATOR, 0);
            set_phase(PH_TEST);
            break;
        }
        g.test = 0;
        if (g.pressed[B_START] && (g.free_play || cr > 0))
            g.start_pending = 1;
        /*
         * start (a credit, or free play) once the live show runs (step 3):
         * while it loads or is being released it owns the physics world
         */
        if (g.start_pending && (g.free_play || cr > 0) && g.state == ST(M_TITLE, 1) && g.title_step == 3) {
            g.start_pending = 0;
            if (!g.free_play)
                g.coins -= g.start_coins;
            g.pressed[B_START] = 0;
            n = put8(out, n, max, C_STOP, 0);
            g.bgm = 0;
            g.stage = 0;
            menu_open(SCR_MODE);
        }
        break;
    case PH_MENU:
        n = menu(out, n, max);
        break;
    case PH_TEST:
        /*
         * The operator menu runs in the game (state 11: sub 0 init, 2..9 its
         * pages, each with its own navigation by gear up / down and Start;
         * 10 leaves).  Seen: 2 hardware tests, 3 link settings, 4 game
         * settings, 5..8 the operator settings menu, 9 data reset.  F2 steps
         * to the next page and leaves after the last (invented).
         */
        if (g.frame < g.phase_frame + HOLD)
            break;
        if (g.state == ST(M_OPERATOR, 0)) {
            g.state = ST(M_OPERATOR, 2);
            g.phase_frame = g.frame;
        } else if (g.state == ST(M_OPERATOR, 10)) {     /* left: the title's pages back */
            n = put8(out, n, max, C_LOAD_2D, 3);
            g.state = ST(M_TITLE, 0);
            g.title_step = 0;
            set_phase(PH_ATTRACT);
        } else if (g.test) {          /* F2: the next page, after the last leave */
            g.state = (g.state & 0xffff) >= 9 ? ST(M_OPERATOR, 10) : g.state + 1;
            g.phase_frame = g.frame;
        }
        g.test = 0;
        break;    case PH_RACE:
        if (g.state == ST(M_RACE, 0) && g.frame > g.phase_frame + HOLD)
            g.state = ST(M_RACE, 1);
        /* in the race the buttons are commands (0x5A..0x5E, sub_812C190 ..) */
        if (g.state >= ST(M_RACE, 4) && g.state <= ST(M_RACE, 5)) {
            if (g.pressed[B_GEAR_UP])
                n = put(out, n, max, C_GEAR_UP, NULL, 0);
            if (g.pressed[B_GEAR_DOWN])
                n = put(out, n, max, C_GEAR_DOWN, NULL, 0);
            if (g.pressed[B_VIEW])
                n = put(out, n, max, C_VIEW, NULL, 0);
            if (g.n2o_edge)
                n = put(out, n, max, C_N2O, NULL, 0);
        }
        g.n2o_edge = 0;
        /* the game reports 8.3 .. 8.8 */
        if (g.game_state == ST(M_RACE, 8))
            set_phase(PH_AFTER);
        break;
    case PH_AFTER:                   /* the game's exit screen (invented timing) */
        if (g.frame - g.phase_frame > 180) {
            /* release what the race loaded (8.1: page 4, the mode's page, the course's), last first */
            n = put8(out, n, max, C_RELEASE_2D, 9 + g.course % 7);
            n = put8(out, n, max, C_RELEASE_2D, 5 + g.mode);
            n = put8(out, n, max, C_RELEASE_2D, 4);
            n = put8(out, n, max, C_LOAD_2D, 2);
            n = put8(out, n, max, C_LOAD_2D, 3);
            if (g.mode == MODE_TA && g.rank_in == 1)
                menu_open(SCR_NAME);
            else if (g.mode == MODE_TA)
                menu_open(SCR_RANK);
            else
                menu_open(SCR_CONTINUE);
        }
        break;
    }
    /* buttons: start, view and the gears as one-exchange events, N2O held */
    for (b = 0; b < B_COUNT; b++)
        if (g.pressed[b]) {
            keys |= 1u << b;
            g.pressed[b] = 0;
        }
    if (g.key_n2o)
        keys |= 1u << B_N2O;
    n = put(out, n, max, C_KEYS, &keys, 2);

    controls();
    memcpy(&hdr[E2000_SD2_STEER], &g.steer, 4);
    memcpy(&hdr[E2000_SD2_GAS], &g.gas, 4);
    memcpy(&hdr[E2000_SD2_BRAKE], &g.brake, 4);
    hdr[E2000_SD2_STATE] = g.state;
    hdr[E2000_SD2_W72] = g.free_play ? 0 : credits();
    hdr[E2000_SD2_W28] = g.frame;
    hdr[E2000_SD2_W68] = g.frame;
    g.frame++;
    return n;
}

static void sd_key(int sym, int down)
{
    switch (sym) {
    case K_5: if (down) g.coins++; break;
    case K_F1: if (down) g.help = !g.help; break;
    case K_F2: if (down) g.test = 1; break;
    case K_LEFT: g.key_left = down; g.nav_left |= down; break;
    case K_RIGHT: g.key_right = down; g.nav_right |= down; break;
    case K_UP: g.key_gas = down; break;
    case K_DOWN: g.key_brake = down; break;
    case K_x: g.n2o_edge |= down && !g.key_n2o; g.key_n2o = down; break;
    case K_1: case K_RETURN: if (down) g.pressed[B_START] = 1; break;
    case K_c: if (down) g.pressed[B_VIEW] = 1; break;
    case K_q: if (down) g.pressed[B_GEAR_UP] = 1; break;
    case K_w: if (down) g.pressed[B_GEAR_DOWN] = 1; break;
    }
}

static void sd_draw(int w, int h)
{
    static const char *lines[] = {
        "5 coin  1 start  arrows drive",
        "Q/W gear up/down  X N2O  C view",
        "F1 hide this",
    };
    const struct igs_core *c = api->core;
    int i, y;

    (void)w;
    if (!g.help)
        return;
    y = h - 4 - 3 * c->font_h;
    c->fill(0, y - 4, 32 * c->font_w, 3 * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < 3; i++)
        c->text(6, y + (2 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

/*
 * SD2_SCRIPT="sec:key[+],..." presses a key at a time from start-up, or with
 * "@screen+sec:key" that long after the chip's screen opened (key:
 * coin, start, left, right, gas, brake, n2o, view, up, down).  A press lasts
 * 0.2 s, or with a trailing + until the next entry.
 */
static void sd_script(void)
{
    static const struct { const char *name; int sym; } keys[] = {
        { "coin", K_5 }, { "start", K_1 }, { "left", K_LEFT }, { "right", K_RIGHT }, { "gas", K_UP },
        { "brake", K_DOWN }, { "n2o", K_x }, { "view", K_c }, { "up", K_q }, { "down", K_w }, { "test", K_F2 },
    };
    static const char *p;
    static double t0, release_at;
    static int held;
    const struct igs_core *c = api->core;
    double now, at;
    char name[16];
    int n = 0, hold, i;

    if (!p) {
        p = c->env("SD2_SCRIPT", "");
        t0 = c->now();
    }
    now = c->now() - t0;
    if (held && release_at && now >= release_at) {
        sd_key(held, 0);
        held = 0;
    }
    if (*p == '@') {                 /* @screen+sec:key: relative to a screen's opening */
        int scr;

        if (sscanf(p, "@%d+%lf:%15[a-z0-9]%n", &scr, &at, name, &n) != 3 || g.phase != PH_MENU ||
            g.scr != scr || now - (g.scr_time - t0) < at)
            return;
    } else if (!*p || sscanf(p, "%lf:%15[a-z0-9]%n", &at, name, &n) != 2 || now < at) {
        return;
    }
    hold = p[n] == '+';
    p += n + hold;
    if (*p == ',')
        p++;
    if (held)
        sd_key(held, 0);
    held = 0;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (!strcmp(name, keys[i].name)) {
            held = keys[i].sym;
            sd_key(held, 1);
            release_at = hold ? 0 : now + 0.2;
            LOG("script %.1f s %s%s", now, name, hold ? " (held)" : "");
        }
}

static void sd_status(char *buf, size_t n)
{
    if (g.free_play)
        snprintf(buf, n, "free play");
    else
        snprintf(buf, n, "credits %d", credits());
}

static const struct e2000_game game = {
    .version = E2000_API_VERSION,
    .name = "speeddriver2",
    .transport = E2000_IGS036_MSG,
    .msg_layout = E2000_MSG_SD2,
    .msg_key = "ILoveIGS",
    .msg_key_len = 8,
    .reset = sd_reset,
    .message = sd_message,
    .key = sd_key,
    .draw = sd_draw,
    .status = sd_status,
};

__attribute__((visibility("default")))
const struct e2000_game *e2000_game_entry(const struct e2000_api *a)
{
    api = a;
    return &game;
}
