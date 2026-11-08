/*
 * Speed Rider 2 (E3000) game plugin: its IGS036's side of the ring transport
 * (machines/e2100, E2100_TRANSPORT_RING).  wiki Speed Rider 2.
 *
 * A C++ game (sr:: classes), not the Speed engine: no Lua on the chip, no
 * flow.  The chip carries the I/O board, the AI riders' controls, the FRAM,
 * the key, a version check and the first KiB of the scene archives.
 *
 * Commands a game waits on carry bit 31 (cmd | 0x80000000) and want their
 * reply with it; the rest (0, 3) get an action without it, which unlocks
 * the game's chip mutex.  Every action feeds the game's watchdog.
 *
 *   0         per-frame random words            ack
 *   3 a=0     AI setup [x, y]                   ack (value 0)
 *   3 a=1     per AI rider [id, speed error,    action 3 value 1: per rider
 *             throttle, brake] x1000            [id, throttle, brake] x1000
 *   4 a=-1    FRAM read [off, len]              the words
 *   4 a=off   FRAM write                        -
 *   6         the FPGA key (16 bytes)           7  versions, region
 *   8 a=0     I/O board: 11 output words        21 input words, RC4
 *   8 a=2/3   I/O reset / counters              9  member card (none here)
 *   11 a      chip memory 0x100A0000 + k KiB    a scene archive's first KiB
 *
 * The scene archives (data/general/scene/<name>.sepzip, header_index) lack
 * their first 1024 bytes, which the chip holds: each is the first entry's
 * local header and the start of its deflated data, rebuilt offline
 * (sr2_heads.h).
 *
 * Invented (the IGS036's program is not dumped): the AI riders' law (Speed
 * Rider's), the I/O board's RC4 stream start (the game's own key).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"
#include "sr2_heads.h"

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(fmt, ...) CORE->log("speedrider2: " fmt, ##__VA_ARGS__)
#define VLOG(...) do { if (CORE->verbose) LOG(__VA_ARGS__); } while (0)

#define WAIT       0x80000000u
#define FRAM_WORDS 0x10000
#define FRAM_FILE  "/SR2/rewrite/a36_fram.bin"

/* SDL 1.2 key symbols */
enum {
    K_RETURN = 13, K_SPACE = 32, K_1 = 49, K_5 = 53, K_9 = 57,
    K_a = 97, K_b = 98, K_d = 100, K_s = 115, K_w = 119, K_x = 120,
    K_UP = 273, K_DOWN, K_RIGHT, K_LEFT, K_F1 = 282, K_F2,
};

/*
 * Buttons by the game's slot (sr::GameIOMgr +36 + 4b); the reply carries
 * slot order[k] in words 2k (held) and 2k+1 (presses since the last poll).
 */
enum { B_TEST, B_SERVICE, B_LEFT, B_RIGHT, B_UP, B_DOWN, B_START, B_REDLINE, BUTTONS };
static const uint8_t order[BUTTONS] = { 6, 2, 3, 0, 1, 7, 5, 4 };

static struct {
    uint32_t fram[FRAM_WORDS];
    int fram_loaded, fram_dirty;
    double fram_save_at;
    /* I/O board */
    int held[BUTTONS], toggles[BUTTONS];
    int throttle_key, lbrake_key, rbrake_key, lean_key;
    float throttle, lbrake, rbrake, lean;
    int coins;
    uint8_t S[256], si, sj;                    /* the reply's RC4 stream */
    int help;
    const char *region;
    uint32_t version;
} g;

/* ---- FRAM ------------------------------------------------------------------------------- */

static void fram_path(char *buf, size_t n)
{
    const char *set = getenv("IGS_SET");

    snprintf(buf, n, "%s%s", set ? set : ".", FRAM_FILE);
}

static void fram_load(void)
{
    char path[1024];
    FILE *f;

    g.fram_loaded = 1;
    memset(g.fram, 0xff, sizeof(g.fram));
    fram_path(path, sizeof(path));
    if ((f = fopen(path, "rb"))) {
        if (fread(g.fram, 4, FRAM_WORDS, f) != FRAM_WORDS)
            LOG("short FRAM file %s", path);
        fclose(f);
    }
}

static void fram_save(void)
{
    char path[1024];
    FILE *f;

    fram_path(path, sizeof(path));
    if ((f = fopen(path, "wb"))) {
        fwrite(g.fram, 4, FRAM_WORDS, f);
        fclose(f);
    } else
        LOG("cannot write %s", path);
    g.fram_dirty = 0;
}

/* ---- the I/O board ------------------------------------------------------------------------ */

/* RC4 under the game's key {22, 3, 0, 0} (sr::GameIOMgr's KSA); one stream for the session */
static void rc4_reset(void)
{
    static const uint8_t key[4] = { 22, 3, 0, 0 };
    unsigned i, j = 0;

    for (i = 0; i < 256; i++)
        g.S[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) {
        uint8_t t = g.S[i];

        j = (j + t + key[i & 3]) & 0xff;
        g.S[i] = g.S[j];
        g.S[j] = t;
    }
    g.si = g.sj = 0;
}

static uint8_t rc4_byte(void)
{
    uint8_t t;

    g.si++;
    g.sj += g.S[g.si];
    t = g.S[g.si];
    g.S[g.si] = g.S[g.sj];
    g.S[g.sj] = t;
    return g.S[(uint8_t)(g.S[g.si] + g.S[g.sj])];
}

static float ramp(float v, float target, float step)
{
    return v < target ? (v + step > target ? target : v + step) : (v - step < target ? target : v - step);
}

/*
 * The reply to a poll: per button its state and the presses since the last
 * poll; lean 0..2000 (1000 centre), left brake, throttle, right brake
 * (0..1000), coins since the last poll.  The game's calibration defaults
 * (offset 0, gain 1) take these as they are.
 */
static void io_poll(uint32_t *w)
{
    int k;

    g.throttle = ramp(g.throttle, g.throttle_key ? 1.0f : 0.0f, 0.1f);
    g.lbrake = ramp(g.lbrake, g.lbrake_key ? 1.0f : 0.0f, 0.15f);
    g.rbrake = ramp(g.rbrake, g.rbrake_key ? 1.0f : 0.0f, 0.15f);
    g.lean = ramp(g.lean, (float)g.lean_key, 0.07f);
    for (k = 0; k < BUTTONS; k++) {
        w[2 * k] = (uint32_t)g.held[order[k]];
        w[2 * k + 1] = (uint32_t)g.toggles[order[k]];
        g.toggles[order[k]] = 0;
    }
    w[16] = (uint32_t)lrintf(1000 + g.lean * 1000);
    w[17] = (uint32_t)lrintf(g.lbrake * 1000);
    w[18] = (uint32_t)lrintf(g.throttle * 1000);
    w[19] = (uint32_t)lrintf(g.rbrake * 1000);
    w[20] = (uint32_t)g.coins;
    g.coins = 0;
    for (k = 0; k < 21; k++) {
        uint32_t ks = rc4_byte();

        ks |= (uint32_t)rc4_byte() << 8;
        ks |= (uint32_t)rc4_byte() << 16;
        ks |= (uint32_t)rc4_byte() << 24;
        w[k] ^= ks;
    }
}

/* ---- AI riders ------------------------------------------------------------------------------- */

/*
 * Per rider the game sends its id, (target speed - speed) x 1000 and the
 * last throttle and brake x 1000; the chip answers the new throttle and
 * brake.  Invented: Speed Rider's proportional law.
 */
static uint32_t ai(const uint32_t *d, uint32_t n, uint32_t *out)
{
    uint32_t i, k = 0;

    for (i = 0; i + 3 < n && k + 3 <= 0x1000; i += 4) {
        float err = (float)(int32_t)d[i + 1] / 1000, thr, brk;

        thr = err > 0 ? 0.4f + err * 0.1f : 0.4f + err * 0.2f;
        thr = thr < 0 ? 0 : thr > 1 ? 1 : thr;
        brk = err < -2 ? -err * 0.05f : 0;
        brk = brk > 1 ? 1 : brk;
        out[k++] = d[i];
        out[k++] = (uint32_t)lrintf(thr * 1000);
        out[k++] = (uint32_t)lrintf(brk * 1000);
    }
    return k;
}

/* ---- commands --------------------------------------------------------------------------------- */

static void ring_cmd(uint32_t cmd, uint32_t a, const uint32_t *d, uint32_t n)
{
    static uint32_t out[0x1000];
    uint32_t id = cmd & ~WAIT, wait = cmd & WAIT, k;

    if (id != 0 && !(id == 8 && a == 0) && !(id == 3 && a == 1))
        VLOG("cmd %u%s a 0x%x n %u%s%08x %08x", id, wait ? " (wait)" : "", a, n, n ? " data " : "", n ? d[0] : 0,
             n > 1 ? d[1] : 0);
    switch (id) {
    case 0:                                     /* per-frame random words: an ack */
        api->ring_action(wait, 0, NULL, 0);
        break;
    case 3:                                     /* AI riders */
        if (a == 1) {
            k = ai(d, n, out);
            api->ring_action(3 | wait, 1, out, k);
        } else
            api->ring_action(3 | wait, 0, NULL, 0);
        break;
    case 4:                                     /* FRAM */
        if (!g.fram_loaded)
            fram_load();
        if (a == 0xFFFFFFFFu) {
            uint32_t off = n ? d[0] : 0, len = n > 1 ? d[1] : 0, i;

            if (len > 0x1000)
                len = 0x1000;
            for (i = 0; i < len; i++)
                out[i] = off + i < FRAM_WORDS ? g.fram[off + i] : 0xFFFFFFFFu;
            api->ring_action(4 | wait, 0, out, len);
        } else {
            uint32_t i;

            for (i = 0; i < n && a + i < FRAM_WORDS; i++)
                g.fram[a + i] = d[i];
            g.fram_dirty = 1;
            g.fram_save_at = CORE->now() + 1;
            api->ring_action(4 | wait, 0, NULL, 0);
        }
        break;
    case 6: {                                   /* the FPGA key: the list's asset_mask is the cipher's */
        static const uint32_t key[4];

        api->ring_action(6 | wait, 0, key, 4);
        break;
    }
    case 7: {                                   /* fuses OK, chip 100, FPGA 1.0, the build's version and region */
        uint32_t info[9] = { 0, 0, 100, 0, 1, 0, g.version, (uint8_t)g.region[0], (uint8_t)g.region[1] };

        api->ring_action(7 | wait, 0, info, 9);
        break;
    }
    case 8:                                     /* the I/O board */
        if (a == 0) {
            io_poll(out);
            api->ring_action(8 | wait, 0, out, 21);
        } else if (a == 3) {
            static const uint32_t counters[2];

            api->ring_action(8 | wait, 0, counters, 2);
        } else
            api->ring_action(8 | wait, 0, NULL, 0);
        break;
    case 9:                                     /* member card protection: no card */
        memset(out, 0, 64 * 4);
        api->ring_action(9 | wait, 0, out, 64);
        break;
    case 11: {                                  /* chip memory: a scene archive's first KiB */
        uint32_t i = (a >> 10) - 0x40028u;

        if (i < SR2_HEADS && !(a & 0x3FF)) {
            memcpy(out, sr2_heads[i], 1024);
            LOG("scene archive %u: its first KiB", i);
        } else {
            memset(out, 0, 1024);
            LOG("chip memory 0x%x: nothing there", a);
        }
        api->ring_action(11 | wait, 0, out, 256);
        break;
    }
    default:
        LOG("unknown command %u a 0x%x n %u", id, a, n);
        api->ring_action(id | wait, 0, NULL, 0);
        break;
    }
}

static void ring_tick(void)
{
    if (g.fram_dirty && CORE->now() >= g.fram_save_at)
        fram_save();
}

/* ---- the cabinet ------------------------------------------------------------------------------- */

/* the lamp board (ttyS0) and the card dealer (ttyS1): write only, nothing answers */
static uint32_t no_reply(const uint8_t *p, uint32_t n, uint8_t *reply, uint32_t max)
{
    (void)p; (void)n; (void)reply; (void)max;
    return 0;
}

static const struct e2100_serial serial[] = {
    { "/dev/ttyS0", no_reply },
    { "/dev/ttyS1", no_reply },
    { NULL, NULL },
};

/* OpenAL Soft's settings: the cabinet's ask for ALSA in four channels; the loaders play OSS in stereo */
static const char *vfile(const char *name)
{
    if (strstr(name, "openal/alsoft.conf"))
        return "format = AL_FORMAT_STEREO16\ndrivers = oss\n";
    return NULL;
}

static void press(int b, int down)
{
    if (down && !g.held[b])
        g.toggles[b]++;
    g.held[b] = down;
}

static void key(int sym, int down)
{
    switch (sym) {
    case K_5: if (down) g.coins++; return;
    case K_UP: g.throttle_key = down; return;
    case K_DOWN: g.lbrake_key = g.rbrake_key = down; return;
    case K_b: g.rbrake_key = down; return;
    case K_LEFT: g.lean_key = down ? -1 : g.lean_key < 0 ? 0 : g.lean_key; return;
    case K_RIGHT: g.lean_key = down ? 1 : g.lean_key > 0 ? 0 : g.lean_key; return;
    case K_1: case K_RETURN: case K_SPACE: press(B_START, down); return;
    case K_w: press(B_UP, down); return;
    case K_s: press(B_DOWN, down); return;
    case K_a: press(B_LEFT, down); return;
    case K_d: press(B_RIGHT, down); return;
    case K_x: press(B_REDLINE, down); return;
    case K_F2: press(B_TEST, down); return;
    case K_9: press(B_SERVICE, down); return;
    case K_F1: if (down) g.help = !g.help; return;
    }
}

static void draw(int w, int h)
{
    static const char *lines[] = {
        "5 coin  1/Enter start, nitro",
        "Up throttle  Down brakes  B rear brake",
        "Left/Right lean  X red line",
        "W/S/A/D panel  F2 test  9 service",
        "F1 hide this",
    };
    int i, ty, n = 5;

    (void)w;
    if (!g.help)
        return;
    ty = h - 4 - n * CORE->font_h;
    CORE->fill(0, ty - 4, 40 * CORE->font_w, n * CORE->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < n; i++)
        CORE->text(6, ty + (n - 1 - i) * CORE->font_h + 6, lines[i], 1, 1, 0.6f);
}

static void reset(struct e2100_channel *ch)
{
    (void)ch;
    rc4_reset();
    g.region = CORE->env("SR2_REGION", "CN");
    if (strlen(g.region) < 2)
        g.region = "CN";
    g.version = (uint32_t)CORE->env_long("SR2_VERSION", !strcmp(g.region, "CN") ? 101 : 100);
}

static struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Speed Rider 2",
    .reset = reset,
    .transport = E2100_TRANSPORT_RING,
    .ring_cmd = ring_cmd,
    .ring_tick = ring_tick,
    .serial = serial,
    .vfile = vfile,
    .key = key,
    .draw = draw,
    .handshake = E2100_HS_SESSION,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    api = a;
    g.help = 1;
    return &game;
}
