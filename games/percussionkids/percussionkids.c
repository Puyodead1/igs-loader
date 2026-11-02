/*
 * Percussion Kids (E1000): what its IGS027A firmware does, rebuilt.
 * docs/PercussionKids.md.  The executable (pkids) keeps its symbols; names
 * below are its own.
 *
 * On E1000 the a27 runs the game and the executable draws and plays what it
 * is told: each frame the chip sends a list of commands (main_vProtect), a
 * 16-bit id and its 16-bit arguments, structs aligned to 4.  None of the
 * firmware is dumped; the flow below follows what each command does
 * (docs/IGS_E1000.md has the command set):
 *
 *   boot      0 SystemInit, 1 signals, 2 common sprites, 3 region, 93
 *             language, 5 fonts, 59 volumes, 118/119 credit display
 *   attract   IGS logo (128..134), coin page (135..139, a 20 s timer),
 *             repeat; the text warning (7..9) is left out: with region 1 it
 *             crashes the executable
 *   game      select mode (145..159: Junior / Senior), select song (168..176),
 *             loading (200..202), the song (192, 203..213: the chart is the
 *             executable's own song table, the chip judges the drums), result
 *             (177..189), next song or game over (160..167)
 *   test      operator menu (13..16) with sound and game settings (59..63,
 *             52..58), the hardware menu's key test (19..27), language
 *             (94..97)
 *
 * The cabinet has three drums: the large one in the middle, a blue one on
 * the left and a red one on the right.  Invented: the flow and its timing,
 * the judgement windows and scores, the countdowns, the settings layout.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e1000/e1000.h"

#define NV_FILE "../log/igs027a.nv"      /* cwd /pkids/linux: p5, the cabinet's writable log partition */

/* SDL 1.2 key symbols */
enum {
    K_RETURN = 13, K_SPACE = 32, K_1 = 49, K_5 = 53, K_9 = 57,
    K_c = 99, K_f = 102, K_j = 106, K_x = 120, K_z = 122,
    K_UP = 273, K_DOWN = 274, K_RIGHT = 275, K_LEFT = 276,
    K_F1 = 282, K_F2 = 283,
};

/* inputs */
enum { IN_LEFT, IN_RIGHT, IN_BIG, IN_COIN, IN_SERVICE, IN_TEST, IN_N };

/* the executable's tables (pkids) */
#define BAND1_LIST    0x8083A80u          /* g_raBand1List: 31 x 100 bytes, [0] the song */
#define BAND1_COUNT   31
#define SONG_NAMES    22                  /* m_pchaSongPreviewFileName */
#define SONG_TRACKS   100                 /* 36 track pointers */

/* song play */
#define SLOTS         30                  /* note cursors per player */
#define JUDGE_Y       393                 /* cursor y at the judge line */
#define LOOK_Y        420                 /* notes enter this far above it */
#define START_TICK    32                  /* BG.wav starts here (track 17) */

enum { G_GREAT = 1, G_COOL, G_NICE, G_MISS };

enum phase {
    P_BOOT, P_LOGO, P_WARNING, P_COIN, P_SELMODE, P_SELSONG, P_LOAD, P_SONG, P_SONGEND,
    P_RESULT, P_GAMEOVER, P_TEST, P_TEST_SOUND, P_TEST_GAME, P_TEST_HW, P_TEST_KEYS, P_TEST_LANG,
};

static const struct e1000_api *api;

#define LOG(...) api->core->log("percussionkids: " __VA_ARGS__)

struct note {
    uint16_t tick, end;                   /* end: a roll's last tick */
    uint8_t lane, wav, roll, done;
    int8_t slot;
};

static struct {
    enum phase phase;
    int t;                                /* frames in the phase */
    int region, lang;
    int help, autoplay;
    /* inputs: held, and presses since the last frame */
    int held[IN_N], pressed[IN_N];
    double last_left, last_right;
    int credits, coins;
    /* game */
    int mode;                             /* 0 Junior, 1 Senior */
    int stage, stages;
    int cursor, prev, countdown, decided, slide;
    int song_name, band;                  /* name index, g_raBand1List index */
    /* the song */
    struct note notes[1200];
    int nnotes;
    struct { uint16_t tick, wav, slot; uint8_t done; } bg[64];
    int nbg;
    int tempo, length, px;                /* tempo x100, length in ticks, pixels per tick */
    double t_start, now_tick;
    int8_t slot_note[SLOTS];
    uint8_t slot_free[SLOTS];             /* freed this frame: reusable from the next */
    int score, max_score, combo, best_combo, hits, misses, gauge;
    uint8_t judge[8], flash[8], boom[8];
    uint16_t sound[32];
    int roll_hits;
    /* test menu */
    int menu_cur, menu_prev;
    /* settings */
    struct {
        uint32_t magic;
        int32_t sound[8];                 /* vSoundSettingInit: day/night start, game/demo volume */
        int32_t game[8];                  /* vGameSettingSetProcess: start credit, songs, free play, tickets */
        uint32_t coins_total, games_total;
    } nv;
} g;

/* ---- the command list ------------------------------------------------------------ */

static uint8_t *out;
static int nout;

static void word(int v)
{
    if (nout + 2 > E1000_DATA_MAX) {
        LOG("command list full");
        return;
    }
    out[nout] = v & 0xff;
    out[nout + 1] = (v >> 8) & 0xff;
    nout += 2;
}

/* a command and its 16-bit arguments */
static void cmd(int id, int nargs, ...)
{
    va_list ap;
    int i;

    word(id);
    va_start(ap, nargs);
    for (i = 0; i < nargs; i++)
        word(va_arg(ap, int));
    va_end(ap);
}

#define C0(id) cmd(id, 0)
#define C1(id, a) cmd(id, 1, a)
#define C2(id, a, b) cmd(id, 2, a, b)
#define C3(id, a, b, c) cmd(id, 3, a, b, c)
#define C4(id, a, b, c, d) cmd(id, 4, a, b, c, d)
#define C5(id, a, b, c, d, e) cmd(id, 5, a, b, c, d, e)

/* a command whose argument is a struct: main_vProtect aligns it to 4 */
static uint8_t *cmd_struct(int id, int len)
{
    uint8_t *p;

    word(id);
    while (nout & 3)
        out[nout++] = 0;
    if (nout + len > E1000_DATA_MAX) {
        LOG("command list full");
        return NULL;
    }
    p = out + nout;
    memset(p, 0, len);
    nout += len;
    return p;
}

static double dabs(double v) { return v < 0 ? -v : v; }
static int iround(double v) { return (int)(v < 0 ? v - 0.5 : v + 0.5); }
static void put16(uint8_t *p, int v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* ---- settings ----------------------------------------------------------------------- */

#define NV_MAGIC 0x4B503031u              /* "PK01" */

static void nv_defaults(void)
{
    static const int32_t sound[8] = { 0, 28, 20, 0, 28, 20, 0, 24 };   /* day 0:00..24:00 */
    static const int32_t game[8] = { 0, 0, 1, 0, 0, 0, 20, 0 };        /* 1 credit, 2 songs */

    memset(&g.nv, 0, sizeof(g.nv));
    g.nv.magic = NV_MAGIC;
    memcpy(g.nv.sound, sound, sizeof(sound));
    memcpy(g.nv.game, game, sizeof(game));
}

static void nv_load(void)
{
    FILE *f = fopen(NV_FILE, "rb");

    if (!f || fread(&g.nv, sizeof(g.nv), 1, f) != 1 || g.nv.magic != NV_MAGIC)
        nv_defaults();
    if (f)
        fclose(f);
}

static void nv_save(void)
{
    FILE *f = fopen(NV_FILE, "wb");

    if (!f) {
        LOG("cannot write %s", NV_FILE);
        return;
    }
    fwrite(&g.nv, sizeof(g.nv), 1, f);
    fclose(f);
}

static int credits_per_play(void) { return g.nv.game[0] + 1; }
static int songs_per_play(void) { return g.nv.game[2] + 1; }
static int free_play(void) { return g.nv.game[3] != 0; }

static void send_volumes(void)
{
    uint8_t *p = cmd_struct(59, 32);
    int i;

    for (i = 0; p && i < 8; i++)
        put32(p + 4 * i, g.nv.sound[i]);
}

static void credit_show(void)
{
    C3(123, g.credits, g.coins, credits_per_play());
}

/* ---- the executable's song table ------------------------------------------------ */

static uint32_t rd32(uint32_t va)
{
    const uint32_t *p = api->game_ptr(va, 4);

    return p ? *p : 0;
}

static int rd8(uint32_t va)
{
    const uint8_t *p = api->game_ptr(va, 1);

    return p ? *p : 0;
}

static int rd16(uint32_t va)
{
    const uint16_t *p = api->game_ptr(va, 2);

    return p ? *p : 0;
}

/* song struct of g_raBand1List[i] */
static uint32_t band_song(int i)
{
    return i >= 0 && i < BAND1_COUNT ? rd32(BAND1_LIST + 100 * i) : 0;
}

/* the band1 entry for a song name and mode: Junior prefers rank 0 (Rookie), Senior rank 1 (Easy) */
static int band_for(int name, int mode)
{
    int i, any = -1;

    for (i = 0; i < BAND1_COUNT; i++) {
        uint32_t s = band_song(i);

        if (!s || rd8(s + 6) != name)
            continue;
        if (rd8(s + 8) == mode)
            return i;
        if (any < 0)
            any = i;
    }
    return any < 0 ? 0 : any;
}

static void chart_load(int band)
{
    uint32_t s = band_song(band), tr, e;
    int lane, k, i;

    g.nnotes = g.nbg = 0;
    g.tempo = rd16(s);
    g.length = rd16(s + 2);
    g.px = rd8(s + 5);
    if (g.tempo < 3000 || g.tempo > 30000)
        g.tempo = 12000;
    if (g.px < 4 || g.px > 40)
        g.px = 12;
    for (lane = 0; lane < 8; lane++) {
        tr = rd32(s + SONG_TRACKS + 4 * lane);
        for (k = 0; tr && k < 2000 && g.nnotes < (int)(sizeof(g.notes) / sizeof(g.notes[0])); k++) {
            struct note *n;

            e = rd32(tr + 4 * k);
            if ((e & 0x3fff) == 0x3fff)
                break;
            if ((e >> 24) == 2)
                continue;                 /* a roll's end: taken with its start */
            n = &g.notes[g.nnotes++];
            memset(n, 0, sizeof(*n));
            n->tick = e & 0x3fff;
            n->end = n->tick;
            n->lane = lane;
            n->wav = (e >> 14) & 0x3ff;
            n->slot = -1;
            if ((e >> 24) == 1) {
                uint32_t f;

                n->roll = 1;
                for (i = k + 1; i < k + 64; i++) {
                    f = rd32(tr + 4 * i);
                    if ((f & 0x3fff) == 0x3fff)
                        break;
                    if ((f >> 24) == 2) {
                        n->end = f & 0x3fff;
                        break;
                    }
                }
            }
        }
    }
    /* background events: tracks 17..24 (BG.wav at tick 32), sound slots 16.. */
    for (lane = 16; lane < 24; lane++) {
        tr = rd32(s + SONG_TRACKS + 4 * lane);
        for (k = 0; tr && k < 64 && g.nbg < 64; k++) {
            e = rd32(tr + 4 * k);
            if ((e & 0x3fff) == 0x3fff)
                break;
            g.bg[g.nbg].tick = e & 0x3fff;
            g.bg[g.nbg].wav = (e >> 14) & 0x3ff;
            g.bg[g.nbg].slot = lane;
            g.bg[g.nbg++].done = 0;
        }
    }
    if (!g.nbg) {                         /* every song has it; just in case */
        g.bg[0].tick = START_TICK;
        g.bg[0].wav = 4;
        g.bg[0].slot = 16;
        g.nbg = 1;
    }
    g.max_score = 0;
    for (i = 0; i < g.nnotes; i++)
        if (!g.notes[i].roll)
            g.max_score += 100;
    if (g.length < 64)
        g.length = 2000;
    LOG("song %d: tempo %d.%02d, %d ticks, %d notes, %d px/tick", band, g.tempo / 100, g.tempo % 100,
        g.length, g.nnotes, g.px);
}

/* ---- phases ----------------------------------------------------------------------- */

static void go(enum phase p)
{
    if (api->core->verbose)
        LOG("phase %d -> %d after %d frames", g.phase, p, g.t);
    g.phase = p;
    g.t = -1;                             /* 0 at the next frame */
}

static int was(int in)
{
    return g.pressed[in];
}

/* a credit's worth to start, or free play */
static int can_start(void)
{
    return free_play() || g.credits >= credits_per_play();
}

static void coins_frame(void)
{
    if (was(IN_COIN) || was(IN_SERVICE)) {
        int n = was(IN_COIN) + was(IN_SERVICE);

        g.credits += n;
        g.nv.coins_total += was(IN_COIN);
        C0(121);                          /* the coin sound */
    }
}

/* the attract screens' F2: end the screen (its end command), then the operator menu */
static int test_entered(int end_id)
{
    if (!was(IN_TEST))
        return 0;
    C0(end_id);
    C1(240, 2);
    C0(14);
    g.menu_cur = 0;
    go(P_TEST);
    return 1;
}

static void phase_boot(void)
{
    C0(0);                                /* SystemInit */
    C0(1);
    C0(2);
    C1(3, g.region);
    C1(93, g.lang);
    C0(5);
    send_volumes();
    C0(118);
    C0(119);
    C1(240, 0);
    go(P_LOGO);
}

static void phase_logo(void)
{
    if (g.t == 0) {
        C0(128);
    } else if (g.t == 150) {
        C1(130, g.region);
    } else if (g.t == 200) {
        C0(132);
    }
    if (g.t < 150)
        C0(129);
    else if (g.t < 200)
        C0(131);
    else
        C0(133);
    if (g.t >= 260) {
        C0(134);
        go(P_COIN);
        return;
    }
    if (g.t > 1)
        test_entered(134);
}

static void phase_warning(void)
{
    if (g.t == 0)
        C1(7, g.region);
    C0(8);
    if (g.t >= 240) {
        C0(9);
        go(P_COIN);
    } else {
        test_entered(9);
    }
}

#define COIN_FRAMES 1200

static void phase_coin(void)
{
    int timer = COIN_FRAMES - g.t;

    if (g.t == 0) {
        C1(240, 0);
        C4(136, free_play(), 0, 0, g.lang);
    }
    C1(135, can_start());
    C2(137, timer, was(IN_BIG));
    C0(6);
    credit_show();
    if (can_start() && was(IN_BIG)) {
        if (!free_play())
            g.credits -= credits_per_play();
        g.nv.games_total++;
        nv_save();
        C1(138, 1);
        C0(139);
        g.stage = 0;
        g.stages = songs_per_play();
        g.mode = 0;
        go(P_SELMODE);
        return;
    }
    if (test_entered(139))
        return;
    if (timer <= 0) {
        C0(139);
        go(P_LOGO);
    }
}

#define SELMODE_SECONDS 10

static void phase_selmode(void)
{
    int sec;

    if (g.t == 0) {
        C1(240, 1);
        C1(146, g.lang);
        C0(147);
        g.cursor = g.prev = 0;
        g.decided = 0;
        g.countdown = SELMODE_SECONDS;
        return;
    }
    if (g.t == 1) {
        C0(148);
        C0(149);
        C1(150, g.countdown);
        C3(155, 0, 0, 0);
        return;
    }
    C0(145);
    C0(158);
    if (!(g.t & 1))
        C0(154);
    if (g.t % 1280 == 0)
        C0(153);
    C0(6);
    credit_show();
    if (g.decided) {
        if (g.t - g.decided >= 60) {
            C0(159);
            go(P_SELSONG);
        }
        return;
    }
    sec = (g.t - 1) / 60;
    if (g.t > 1 && (g.t - 1) % 60 == 0 && SELMODE_SECONDS - sec >= 0) {
        g.countdown = SELMODE_SECONDS - sec;
        C1(151, g.countdown);
        C1(152, g.countdown);
    }
    if (was(IN_LEFT) && g.cursor != 0) {
        g.prev = g.cursor;
        g.cursor = 0;
        C3(155, 0, 0, g.prev);
    } else if (was(IN_RIGHT) && g.cursor != 1) {
        g.prev = g.cursor;
        g.cursor = 1;
        C3(155, 1, 1, g.prev);
    } else if (was(IN_BIG) || g.countdown <= 0) {
        C3(155, 6, g.cursor, g.prev);
        C0(156 + g.cursor);
        g.mode = g.cursor;
        g.decided = g.t;
    }
}

#define SELSONG_SECONDS 20

static int name_wrap(int n)
{
    return ((n % SONG_NAMES) + SONG_NAMES) % SONG_NAMES;
}

static void selsong_names(void)
{
    int i;

    for (i = 0; i < 4; i++)
        C2(169, i, name_wrap(g.cursor + i - 1));
}

static void phase_selsong(void)
{
    int ev = 0, sec;

    switch (g.t) {
    case 0:
        if (!g.stage)
            g.cursor = 0;
        C4(171, 0, g.lang, g.cursor, g.mode);
        return;
    case 1: C4(171, 1, g.lang, g.cursor, g.mode); return;
    case 2: C4(171, 2, g.lang, g.cursor, g.mode); return;
    case 3: C4(171, 3, g.lang, g.cursor, g.mode); return;
    case 4: C4(171, 4, g.lang, g.cursor, g.mode); return;
    case 5: C4(171, 5, g.lang, g.cursor, g.mode); return;
    case 6:
        g.countdown = SELSONG_SECONDS;
        g.decided = 0;
        g.slide = 0;
        C4(171, 6, g.lang, g.countdown, g.mode);
        selsong_names();
        return;
    }
    C0(168);
    C0(6);
    credit_show();
    if (g.decided) {
        C3(175, 0, 0, g.cursor);
        if (g.t - g.decided >= 60) {
            C0(176);
            g.song_name = g.cursor;
            g.band = band_for(g.song_name, g.mode);
            go(P_LOAD);
        }
        return;
    }
    sec = (g.t - 7) / 60;
    if ((g.t - 7) % 60 == 0 && sec > 0 && SELSONG_SECONDS - sec >= 0) {
        g.countdown = SELSONG_SECONDS - sec;
        C1(172, g.countdown);
    }
    if (g.slide && g.t - g.slide == 12) {
        C1(170, 0);
        selsong_names();
    }
    if (!g.slide || g.t - g.slide > 12) {
        if (was(IN_LEFT)) {
            g.cursor = name_wrap(g.cursor - 1);
            ev = 1;
        } else if (was(IN_RIGHT)) {
            g.cursor = name_wrap(g.cursor + 1);
            ev = 2;
        }
        if (ev) {
            g.slide = g.t;
            C1(170, 1);
        }
    }
    if (!ev && (was(IN_BIG) || g.countdown <= 0)) {
        ev = 16;
        C0(173);
        g.decided = g.t;
    }
    C3(175, 0, ev, g.cursor);
    C2(174, 0, 0);
}

/* loading: one step per frame, as the cabinet's percentage climbs */
static void phase_load(void)
{
    int name = g.song_name, act = 0;

    switch (g.t) {
    case 0:
        C1(240, 1);
        C3(207, 0, 1, 0);
        C5(205, 0, 0, 0, 0, 0);
        C5(200, 0, g.stage, g.stages, name, act);
        C5(200, 2, g.stage, g.stages, name, act);
        C1(201, 0);
        chart_load(g.band);
        break;
    case 1:
        C5(204, 0, 0, 0, 0, 0);
        C5(204, 13, 0, g.band, 0, 0);
        C1(201, 20);
        break;
    case 2:
        C3(203, g.band, 0, 0);
        C1(201, 50);
        break;
    case 3:
        C1(192, name);
        C1(201, 70);
        break;
    case 4:
        C5(204, 7, 0, 0, 0, 0);
        C5(204, 5, 0, 0, 0, 0);
        C5(204, 9, 0, 0, 0, 0);
        C1(201, 100);
        break;
    case 40:
        C0(202);
        C5(206, 0, 1, name, act, 0);
        C3(207, 1, 0, 0);
        go(P_SONG);
        break;
    }
}

/* ---- the song ---------------------------------------------------------------------- */

static void song_start(void)
{
    int i;

    g.t_start = api->core->now();
    g.now_tick = 0;
    g.score = g.combo = g.best_combo = g.hits = g.misses = g.gauge = g.roll_hits = 0;
    memset(g.slot_note, -1, sizeof(g.slot_note));
    memset(g.slot_free, 0, sizeof(g.slot_free));
    for (i = 0; i < g.nnotes; i++) {
        g.notes[i].done = 0;
        g.notes[i].slot = -1;
    }
}

static int drum_lane(int in)
{
    return in == IN_LEFT ? 0 : in == IN_RIGHT ? 5 : 1;
}

static int drum_wav(int lane)
{
    return lane == 0 ? 0 : lane == 5 ? 1 : lane == 2 ? 3 : 2;
}

static void play_wav(int wav)
{
    int i;

    for (i = 0; i < 16; i++)
        if (!g.sound[i]) {
            g.sound[i] = wav + 1;
            return;
        }
}

static void note_done(struct note *n)
{
    n->done = 1;
}

static void judge(struct note *n, int grade)
{
    static const int points[] = { 0, 100, 80, 50, 0 };

    note_done(n);
    g.judge[n->lane] = grade;
    if (grade == G_MISS) {
        g.combo = 0;
        g.misses++;
        return;
    }
    g.boom[n->lane] = 1;
    g.hits++;
    g.combo++;
    if (g.combo > g.best_combo)
        g.best_combo = g.combo;
    g.score += points[grade];
}

/* the grade for a hit dt ticks from the note (windows in seconds) */
static int grade_of(double dt_ticks)
{
    double dt = dabs(dt_ticks) * 6000.0 / (g.tempo * 8.0);

    return dt <= 0.050 ? G_GREAT : dt <= 0.100 ? G_COOL : dt <= 0.150 ? G_NICE : dt <= 0.200 ? G_MISS : 0;
}

/* the unjudged note nearest to now in lane, within the judgement window */
static struct note *nearest(int lane)
{
    struct note *best = NULL;
    double bd = 1e9;
    int i;

    for (i = 0; i < g.nnotes; i++) {
        struct note *n = &g.notes[i];
        double d = n->tick - g.now_tick;

        if (n->done || n->lane != lane)
            continue;
        if (n->roll && g.now_tick >= n->tick && g.now_tick <= n->end)
            d = 0;
        if (dabs(d) < bd) {
            bd = dabs(d);
            best = n;
        }
    }
    return best && grade_of(bd) ? best : NULL;
}

static void hit(int in, double now)
{
    int lane = drum_lane(in);
    struct note *n;

    g.flash[lane] = 1;
    /* both small drums together: lane 2 */
    if (in != IN_BIG && dabs(g.last_left - g.last_right) < 0.12 && (n = nearest(2))) {
        g.flash[2] = 1;
        play_wav(n->wav);
        judge(n, grade_of(n->tick - g.now_tick));
        return;
    }
    (void)now;
    n = nearest(lane);
    if (!n) {
        play_wav(drum_wav(lane));
        return;
    }
    play_wav(n->wav);
    if (n->roll) {                        /* a roll: every hit counts until its end */
        g.roll_hits++;
        g.score += 10;
        g.boom[lane] = 1;
        g.judge[lane] = G_GREAT;
        return;
    }
    judge(n, grade_of(n->tick - g.now_tick));
}

static void song_inputs(void)
{
    double now = api->core->now();
    int in;

    if (was(IN_LEFT))
        g.last_left = now;
    if (was(IN_RIGHT))
        g.last_right = now;
    for (in = IN_LEFT; in <= IN_BIG; in++)
        if (was(in))
            hit(in, now);
}

/* PK_AUTOPLAY=1: the chip plays every note itself (testing) */
static void song_autoplay(void)
{
    int i;

    for (i = 0; i < g.nnotes; i++) {
        struct note *n = &g.notes[i];

        if (n->done || g.now_tick < n->tick)
            continue;
        g.flash[n->lane] = 1;
        play_wav(n->wav);
        if (n->roll) {
            if (g.now_tick <= n->end) {
                if ((int)g.now_tick % 2 == 0)
                    g.boom[n->lane] = 1;
                continue;
            }
            note_done(n);
            continue;
        }
        judge(n, G_GREAT);
    }
}

/* misses, roll ends, the cursors and the 208 struct */
static void song_frame(void)
{
    uint8_t *p;
    int i, s;

    g.now_tick = (api->core->now() - g.t_start) * g.tempo * 8.0 / 6000.0;
    memset(g.judge, 0, sizeof(g.judge));
    memset(g.flash, 0, sizeof(g.flash));
    memset(g.boom, 0, sizeof(g.boom));
    memset(g.sound, 0, sizeof(g.sound));
    for (i = 0; i < g.nbg; i++)
        if (!g.bg[i].done && g.now_tick >= g.bg[i].tick) {
            g.bg[i].done = 1;
            if (g.bg[i].slot < 32)
                g.sound[g.bg[i].slot] = g.bg[i].wav + 1;
        }
    if (g.autoplay)
        song_autoplay();
    else
        song_inputs();
    for (i = 0; i < g.nnotes; i++) {
        struct note *n = &g.notes[i];

        if (n->done)
            continue;
        if (n->roll && g.now_tick > n->end + 2)
            note_done(n);
        else if (!n->roll && grade_of(g.now_tick - n->tick) == 0 && g.now_tick > n->tick)
            judge(n, G_MISS);
    }
    g.gauge = g.max_score ? 20 * g.score / g.max_score : 0;
    if (g.gauge > 20)
        g.gauge = 20;

    p = cmd_struct(208, 656);
    if (!p)
        return;
    for (i = 0; i < 32; i++)
        put16(p + 8 + 2 * i, g.sound[i]);
    /* cursors: free the ones whose note is done, then place the visible notes */
    for (s = 0; s < SLOTS; s++) {
        uint8_t *c = p + 72 + 8 * s;
        int k = g.slot_note[s];

        if (g.slot_free[s]) {
            g.slot_free[s] = 0;
            continue;                     /* freed last frame: state 0 already sent */
        }
        if (k < 0)
            continue;
        if (g.notes[k].done) {
            put16(c, 0);                  /* free */
            g.notes[k].slot = -1;
            g.slot_note[s] = -1;
            g.slot_free[s] = 1;
        }
    }
    for (i = 0; i < g.nnotes; i++) {
        struct note *n = &g.notes[i];
        double y = JUDGE_Y - (n->tick - g.now_tick) * g.px;

        if (n->done || n->slot >= 0 || y < JUDGE_Y - LOOK_Y)
            continue;
        for (s = 0; s < SLOTS; s++)
            if (g.slot_note[s] < 0 && !g.slot_free[s])
                break;
        if (s == SLOTS)
            break;
        g.slot_note[s] = i;
        n->slot = s;
    }
    for (s = 0; s < SLOTS; s++) {
        uint8_t *c = p + 72 + 8 * s;
        struct note *n;
        double y;

        if (g.slot_note[s] < 0)
            continue;
        n = &g.notes[g.slot_note[s]];
        y = JUDGE_Y - (n->tick - g.now_tick) * g.px;
        put16(c, 2 | n->lane << 6);
        c[3] = n->roll ? 0x02 : 0;
        put16(c + 4, iround(y));
        put16(c + 6, n->roll ? -iround((n->end - n->tick) * g.px) : 0);
    }
    put16(p + 556, g.combo);
    p[560] = 1;
    p[562] = 1;
    memcpy(p + 564, g.judge, 8);
    memcpy(p + 580, g.flash, 8);
    memcpy(p + 596, g.boom, 8);
    put32(p + 628, g.score);
    put16(p + 648, g.gauge);
    C1(210, 0);
}

static void phase_song(void)
{
    if (g.t == 0)
        song_start();
    song_frame();
    C0(6);
    if (api->core->verbose && g.t % 600 == 599)
        LOG("tick %.0f: score %d, %d hits, %d misses, combo %d", g.now_tick, g.score, g.hits, g.misses, g.combo);
    if (g.now_tick >= g.length + 32) {
        LOG("song over: score %d of %d, %d hits, %d misses, best combo %d, %d roll hits", g.score,
            g.max_score, g.hits, g.misses, g.best_combo, g.roll_hits);
        C0(211);
        go(P_SONGEND);
    }
}

static void phase_songend(void)
{
    C1(210, 0);
    if (g.t >= 50) {
        C0(194);
        C0(212);
        C1(213, 0);
        go(P_RESULT);
    }
}

static int percent(void)
{
    int p = g.max_score ? 100 * g.score / g.max_score : 0;

    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void phase_result(void)
{
    int t = g.t;

    if (t <= 8) {
        if (!(t & 1))
            C2(187, t, g.lang);
        return;
    }
    C1(177, 0);
    C0(6);
    switch (t) {
    case 10: C4(178, 0, 0, 0, 0); break;
    case 20: C4(178, 1, 0, 0, 0); break;
    case 30: C4(178, 2, 0, 0, 0); break;
    case 90: C4(178, 4, 0, 0, 0); break;
    case 100: C4(178, 5, 0, 1, 0); break;
    case 120: C3(181, 0, 0, 0); break;
    case 180: C3(181, 1, percent(), 0); break;
    case 210: C3(181, 3, 0, 0); break;
    case 240: C3(181, 5, 0, 0); break;
    }
    if (t >= 30 && t <= 90) {
        int sc = g.score * (t - 30) / 60;

        C4(178, 3, 1, sc >> 16, sc & 0xffff);
    }
    if (t >= 120 && t < 180)
        C1(179 + (t / 6 & 1), (t / 6) % 5);
    if (t >= 420 || (t > 260 && was(IN_BIG))) {
        C1(188, 1);
        C0(189);
        g.stage++;
        if (g.stage < g.stages) {
            go(P_SELSONG);
        } else {
            go(P_GAMEOVER);
        }
    }
}

static void phase_gameover(void)
{
    if (g.t == 0) {
        C0(166);
        C0(160);
    } else if (g.t == 60) {
        C0(163);
    }
    if (g.t < 60)
        C0(g.t & 1 ? 162 : 161);
    else
        C0(164);
    C0(6);
    if (g.t >= 300) {
        C0(165);
        C0(167);
        C1(240, 0);
        go(P_COIN);
    }
}

/* ---- operator menu -------------------------------------------------------------------- */

/* the cabinet's menu keys: blue (left) drum up, red (right) drum down, large drum enter */
static int menu_move(int count)
{
    int prev = g.menu_cur;

    if (was(IN_LEFT))
        g.menu_cur = (g.menu_cur + count - 1) % count;
    else if (was(IN_RIGHT))
        g.menu_cur = (g.menu_cur + 1) % count;
    g.menu_prev = prev;
    return prev != g.menu_cur;
}

static void test_back(void)
{
    C0(18);
    C0(14);
    g.menu_cur = 0;
    go(P_TEST);
}

static void phase_test(void)
{
    if (menu_move(8))
        C0(13);
    C2(15, 8, g.menu_cur);
    if (!was(IN_BIG))
        return;
    switch (g.menu_cur) {
    case 0:                               /* Hardware Test */
        C0(16);
        C0(17);
        C0(23);
        C2(20, 0, 0);
        g.menu_cur = 0;
        go(P_TEST_HW);
        break;
    case 1:                               /* Sound Volume Setting */
        C0(16);
        C0(17);
        send_volumes();
        C0(60);
        g.menu_cur = g.menu_prev = 0;
        go(P_TEST_SOUND);
        break;
    case 2:                               /* Game Option Setting */
        C0(16);
        C0(17);
        C0(52);
        g.menu_cur = g.menu_prev = 0;
        go(P_TEST_GAME);
        break;
    case 6:                               /* Language Menu */
        C0(16);
        C0(17);
        C0(94);
        g.menu_cur = 0;
        go(P_TEST_LANG);
        break;
    case 7:                               /* Exit */
        C0(16);
        C1(240, 0);
        nv_save();
        go(P_LOGO);
        break;
    default:
        C0(13);                           /* bookkeeping, time, reset: not emulated */
        break;
    }
}

/* sound settings: items 0..5 (day start, game, demo, night start, game, demo), 6 exit */
static void phase_test_sound(void)
{
    static const int limit[6] = { 24, 32, 32, 24, 32, 32 };
    uint8_t *p;
    int i, k;

    if (g.t == 0)
        return;
    if (was(IN_BIG)) {
        if (g.menu_cur == 6) {
            C0(63);
            send_volumes();
            nv_save();
            test_back();
            return;
        }
        /* the start times change by the hour (words 6 and 7) */
        k = g.menu_cur % 3 ? g.menu_cur : 6 + g.menu_cur / 3;
        g.nv.sound[k] = (g.nv.sound[k] + 1) % (limit[g.menu_cur] + 1);
        if (g.menu_cur % 3)
            C2(61, g.menu_cur, g.nv.sound[g.menu_cur]);
    } else if (menu_move(7)) {
        C0(13);
    }
    p = cmd_struct(62, 32);
    for (i = 0; p && i < 8; i++)
        put32(p + 4 * i, g.nv.sound[i]);
    word(g.menu_cur);
    word(g.menu_prev);
}

/* game settings: items 0 start credit, 2 songs per play, 3 free play, 8 exit */
static void phase_test_game(void)
{
    static const int items[] = { 0, 2, 3, 8 };
    static const int limit[4] = { 8, 1, 1, 0 };
    int k, i;
    uint8_t *p;

    if (g.t == 0)
        return;
    for (k = 0; k < 4 && items[k] != g.menu_cur; k++)
        ;
    if (k == 4)
        k = 0;
    if (was(IN_BIG)) {
        if (items[k] == 8) {
            C0(58);
            nv_save();
            test_back();
            return;
        }
        g.nv.game[items[k]] = (g.nv.game[items[k]] + 1) % (limit[k] + 1);
        C0(56);
    } else if (was(IN_LEFT) || was(IN_RIGHT)) {
        g.menu_prev = items[k];
        k = (k + (was(IN_LEFT) ? 3 : 1)) % 4;
        g.menu_cur = items[k];
        C0(56);
    }
    p = cmd_struct(57, 32);
    for (i = 0; p && i < 8; i++)
        put32(p + 4 * i, g.nv.game[i]);
    word(g.menu_cur);
    word(g.menu_prev);
}

/* hardware test menu: item 0 key test, the last (8) exit; the rest not emulated */
static void phase_test_hw(void)
{
    if (g.t == 0)
        return;
    if (menu_move(9))
        C0(19);
    C2(21, 9, g.menu_cur);
    if (!was(IN_BIG))
        return;
    if (g.menu_cur == 0) {
        C0(22);
        C0(25);
        go(P_TEST_KEYS);
    } else if (g.menu_cur == 8) {
        C0(22);
        C0(24);
        test_back();
    }
}

/* key test: both small drums together leave it */
static void phase_test_keys(void)
{
    static const int key_of[IN_N] = { 19, 20, 18, 0, 2, 1 };
    uint8_t *p = cmd_struct(26, 68);
    int in;

    for (in = 0; p && in < IN_N; in++) {
        p[4 + key_of[in]] = g.held[in] != 0;
        p[36 + key_of[in]] = g.pressed[in] != 0;
    }
    if (g.t > 30 && g.held[IN_LEFT] && g.held[IN_RIGHT]) {
        C0(27);
        C0(23);
        C2(20, 0, 0);
        g.menu_cur = 0;
        go(P_TEST_HW);
    }
}

/* language: item 0 cycles the language, 1 exit */
static void phase_test_lang(void)
{
    if (g.t == 0)
        return;
    if (menu_move(2))
        C0(95);
    if (was(IN_BIG)) {
        if (g.menu_cur == 1) {
            C1(97, g.lang);
            test_back();
            return;
        }
        g.lang = (g.lang + 1) % 3;
        C1(93, g.lang);
    }
    C3(96, g.lang, g.menu_cur, g.menu_prev);
}

/* ---- a fast path for the executable's bitmap blits ----------------------------------- */

/*
 * gfx2_color_bitmap_to_screen_blt (0x80766A0) copies a bitmap from system
 * memory one line at a time through the 2D engine's scratch area: four
 * trapped register accesses per line, over a thousand lines a frame in a
 * song, too slow on Windows.  It is replaced by a jump to the same blit done
 * at once (the machine's bitmap), with the raster mode the function would
 * write (gu2_alpha_active ? gu2_alpha32 : gu2_rop32 | GFXsourceFlags).
 */
#define BITMAP_BLT     0x80766A0u
#define GU2_DST_PITCH  0x8097268u
#define GU2_XSHIFT     0x809726Cu
#define GFX_SRC_FLAGS  0x84859B6u
#define GU2_ALPHA32    0x84859C4u
#define GU2_ALPHA_ON   0x84859D0u
#define GU2_BM_THROTTLE 0x84859DCu
#define GU2_ROP32      0x84A2124u

static int bitmap_blt(int x, int y, int dst, int w, int h, int src, int pitch)
{
    uint32_t rm = rd32(GU2_ALPHA_ON) ? rd32(GU2_ALPHA32) : rd32(GU2_ROP32) | (uint32_t)rd16(GFX_SRC_FLAGS);
    int xs = rd8(GU2_XSHIFT);
    uint16_t *thr = (uint16_t *)api->game_ptr(GU2_BM_THROTTLE, 4);

    x &= 0xffff;
    y &= 0xffff;
    w &= 0xffff;
    h &= 0xffff;
    pitch = (int16_t)pitch;
    if (thr)
        thr[0] = thr[1] = 0;                  /* gu2_bm_throttle, gu2_vm_throttle */
    api->bitmap((const uint8_t *)(uintptr_t)(uint32_t)src + (x << xs) + pitch * y, pitch, (uint32_t)dst,
                rd16(GU2_DST_PITCH), w, h, rm);
    return 0;
}

static void hooks_install(void)
{
    static const uint8_t expect[] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c };
    static int done;

    if (done++ || api->core->env_long("PK_NO_FASTPATH", 0))
        return;
    if (!api->patch_jump(BITMAP_BLT, (void *)bitmap_blt, expect, sizeof(expect)))
        LOG("bitmap fast path not installed (unexpected code)");
}

/* ---- the plugin ---------------------------------------------------------------------- */

static void pk_reset(void)
{
    int region = g.region, lang = g.lang;

    memset(&g, 0, sizeof(g));
    g.region = region;
    g.lang = lang;
    g.help = 1;
    g.autoplay = (int)api->core->env_long("PK_AUTOPLAY", 0);
    nv_load();
    hooks_install();
    LOG("a27 start: region %d, language %d", g.region, g.lang);
}

static int pk_frame(uint32_t mode, const uint8_t *in, int nin, uint8_t *buf)
{
    (void)mode;
    (void)in;
    (void)nin;
    out = buf;
    nout = 0;
    coins_frame();
    switch (g.phase) {
    case P_BOOT: phase_boot(); break;
    case P_LOGO: phase_logo(); break;
    case P_WARNING: phase_warning(); break;
    case P_COIN: phase_coin(); break;
    case P_SELMODE: phase_selmode(); break;
    case P_SELSONG: phase_selsong(); break;
    case P_LOAD: phase_load(); break;
    case P_SONG: phase_song(); break;
    case P_SONGEND: phase_songend(); break;
    case P_RESULT: phase_result(); break;
    case P_GAMEOVER: phase_gameover(); break;
    case P_TEST: phase_test(); break;
    case P_TEST_SOUND: phase_test_sound(); break;
    case P_TEST_GAME: phase_test_game(); break;
    case P_TEST_HW: phase_test_hw(); break;
    case P_TEST_KEYS: phase_test_keys(); break;
    case P_TEST_LANG: phase_test_lang(); break;
    }
    g.t++;
    memset(g.pressed, 0, sizeof(g.pressed));
    return nout;
}

static int key_input(int sym)
{
    switch (sym) {
    case K_LEFT: case K_z: case K_f: return IN_LEFT;
    case K_RIGHT: case K_x: case K_j: return IN_RIGHT;
    case K_SPACE: case K_DOWN: case K_c: case K_RETURN: case K_1: return IN_BIG;
    case K_5: return IN_COIN;
    case K_9: return IN_SERVICE;
    case K_F2: return IN_TEST;
    default: return -1;
    }
}

static void pk_key(int sym, int down)
{
    int in;

    if (sym == K_F1) {
        if (down)
            g.help = !g.help;
        return;
    }
    if ((in = key_input(sym)) < 0)
        return;
    if (down && !g.held[in])
        g.pressed[in]++;
    g.held[in] = down;
}

static void pk_draw(int w, int h)
{
    static const char *lines[] = {
        "Space large drum (start)  Left / Right small drums",
        "5 coin  9 service  F2 test  F1 hide",
    };
    const struct igs_core *c = api->core;
    int i, ty;

    (void)w;
    if (!g.help)
        return;
    ty = h - 4 - 2 * c->font_h;
    c->fill(0, ty - 4, 52 * c->font_w, 2 * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < 2; i++)
        c->text(6, ty + (1 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

static void pk_status(char *buf, size_t n)
{
    static const char *names[] = {
        "boot", "logo", "warning", "insert coin", "select mode", "select song", "loading", "song",
        "song", "result", "game over", "test", "test", "test", "test", "test", "test",
    };

    if (g.phase == P_SONG)
        snprintf(buf, n, "credit %d | stage %d | score %d combo %d", g.credits, g.stage + 1, g.score, g.combo);
    else
        snprintf(buf, n, "credit %d | %s", g.credits, names[g.phase]);
}

static const struct e1000_game game = {
    .version = E1000_API_VERSION,
    .name = "percussionkids",
    .reset = pk_reset,
    .frame = pk_frame,
    .key = pk_key,
    .draw = pk_draw,
    .status = pk_status,
};

__attribute__((visibility("default")))
const struct e1000_game *e1000_game_entry(const struct e1000_api *a)
{
    api = a;
    g.region = (int)a->core->env_long("PK_REGION", 1);
    g.lang = g.region == 1 ? 1 : g.region >= 3 ? 2 : 0;
    return &game;
}
