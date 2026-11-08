/*
 * The Speed engine games (E3000: Speed Driver 3, Power Truck): their
 * IGS036's side of the ring transport (e2100.h, E2100_TRANSPORT_RING), the
 * I/O board and the cabinet files.  A game's plugin includes this file,
 * which defines its e2100_game_entry, then defines
 *
 *   static const struct speed_title title = { ... };
 *
 * wiki Speed Driver 3, Power Truck.
 *
 * The game logic is the PC's (Lua); the chip holds the FRAM, the key, the
 * track's road links (distance queries) and two Lua chunks in its image,
 * and keeps the main flow.  Every command gets exactly one action back:
 * the game leaves its chip mutex locked after a command and only an
 * action unlocks it, and an action every few frames feeds its watchdog.
 *
 *   0  per-frame random words           ack
 *   3  distance: a=0 range, 1 reset, 2 road record, 3 one node, 4 all cars
 *   4  FRAM: a=-1 read {off,len}, a=-2 fill {start,count,value}, else write at a
 *   5  state: a=0 current (waits), a=1 next -> action 5 with the state
 *   6  key (16 bytes)                   7  information (fuses, versions)
 *   9  chip memory 0x10000000 + offset: the image's Lua chunks
 *
 * The image is encrypted with the IGS036's address-keyed ROM cipher, whose
 * table is not known; the two chunks are served from the plugin instead
 * (speed_lua.h), FPGA-encrypted with the script key like the game's files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"

/*
 * The main flow, which the chip keeps: Lua sends a branch (game.setMainState
 * with a number, 0..4) and the chip answers with the next state's id
 * (game.state.map), which Lua enters; 0 = none.
 */
#define SPEED_BRANCHES 5
struct speed_title {
    const char *name, *log;               /* the game's name; the log prefix (the plugin) */
    const char *fram;                     /* the FRAM file, in the set */
    const uint8_t (*flow)[SPEED_BRANCHES];
    unsigned flow_states;
    const char *chunk[2];                 /* lib.machine, the boot */
    /* the boot loader: E2100_HS_LADDER (Speed Driver 3's plain LoaderAdv),
       E2100_HS_SESSION (Power Truck's iLoadDataPointer: the same ladder under
       the session key) */
    int handshake;
    /* the cipher's CBC mode (Speed Driver 4): mask deltas per 64 KiB chunk 1.. */
    const uint8_t (*cbc_delta)[16];
    unsigned cbc_chunks;
};
static const struct speed_title title;    /* the game's, defined after this file */

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(fmt, ...) CORE->log("%s: " fmt, title.log, ##__VA_ARGS__)
#define VLOG(...) do { if (CORE->verbose) LOG(__VA_ARGS__); } while (0)

#define FRAM_WORDS 0x10000
#define ROADS 256

/* the game's fastCheckSum: a skip chain through the bytes */
static uint32_t fast_checksum(const uint8_t *b, uint32_t n)
{
    uint32_t sum = 0, pos = 0;

    while (pos < n) {
        sum += b[pos];
        pos += b[pos] + 1;
    }
    return sum;
}

static struct {
    uint32_t state;
    uint32_t fram[FRAM_WORDS];
    int fram_loaded, fram_dirty;
    double fram_save_at;
    uint32_t range;
    struct { uint32_t nodes, nnext, nprev, next[16], prev[16]; int set; } road[ROADS];
} g;

static void fram_path(char *buf, size_t n)
{
    const char *set = getenv("IGS_SET");

    snprintf(buf, n, "%s%s", set ? set : ".", title.fram);
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
    }
    g.fram_dirty = 0;
}

/*
 * The two Lua chunks, run after `require 'config'` and the C bindings,
 * FPGA-encrypted to the size the game asks for (its arguments: the image
 * offset and the two sizes), with the trailer [0 x 8][checksum][length].
 */
static uint32_t serve_chunk(int which, uint32_t size, uint8_t *out)
{
    const char *t = title.chunk[which];
    uint32_t len = strlen(t), ck;

    if (size < 32 || len + 16 > size)
        return 0;
    memset(out, 0, size);
    memcpy(out, t, len);
    ck = fast_checksum(out, len);
    memcpy(out + size - 8, &ck, 4);
    memcpy(out + size - 4, &len, 4);
    api->asset_encrypt(out, size);
    return size;
}

/*
 * Distance lists: range words road | count << 8 | first << 16 around (road,
 * node), `range` nodes either way, following the road links at the ends.
 */
static uint32_t add_run(uint32_t *out, uint32_t n, uint32_t max, uint32_t road, int32_t first, int32_t count)
{
    while (count > 0 && n < max) {
        int32_t c = count > 255 ? 255 : count;

        out[n++] = (road & 0xff) | (uint32_t)c << 8 | (uint32_t)first << 16;
        first += c;
        count -= c;
    }
    return n;
}

static uint32_t near_nodes(uint32_t road, uint32_t node, uint32_t *out, uint32_t n, uint32_t max)
{
    int32_t r = g.range, lo, hi, nodes;
    uint32_t i;

    if (road >= ROADS || !g.road[road].set)
        return n;
    nodes = g.road[road].nodes;
    lo = (int32_t)node - r;
    hi = (int32_t)node + r;
    n = add_run(out, n, max, road, lo < 0 ? 0 : lo, (hi >= nodes ? nodes - 1 : hi) - (lo < 0 ? 0 : lo) + 1);
    if (hi >= nodes)                 /* past the end: the next roads from their start */
        for (i = 0; i < g.road[road].nnext; i++) {
            uint32_t nr = g.road[road].next[i];

            if (nr < ROADS && g.road[nr].set)
                n = add_run(out, n, max, nr, 0, hi - nodes + 1 > (int32_t)g.road[nr].nodes ?
                            (int32_t)g.road[nr].nodes : hi - nodes + 1);
        }
    if (lo < 0)                      /* before the start: the previous roads' ends */
        for (i = 0; i < g.road[road].nprev; i++) {
            uint32_t pr = g.road[road].prev[i];
            int32_t pn, c;

            if (pr >= ROADS || !g.road[pr].set)
                continue;
            pn = g.road[pr].nodes;
            c = -lo > pn ? pn : -lo;
            n = add_run(out, n, max, pr, pn - c, c);
        }
    return n;
}

static void distance(uint32_t a, const uint32_t *d, uint32_t n)
{
    static uint32_t out[0x1000];
    uint32_t k = 0, i;

    switch (a) {
    case 0:
        if (n)
            g.range = d[0];
        break;
    case 1:
        memset(g.road, 0, sizeof(g.road));
        break;
    case 2:
        if (n >= 4 && d[0] < ROADS) {
            uint32_t r = d[0], na = d[2], nb = d[3];

            g.road[r].set = 1;
            g.road[r].nodes = d[1];
            g.road[r].nnext = na > 16 ? 16 : na;
            g.road[r].nprev = nb > 16 ? 16 : nb;
            for (i = 0; i < g.road[r].nnext && 4 + i < n; i++)
                g.road[r].next[i] = d[4 + i];
            for (i = 0; i < g.road[r].nprev && 4 + na + i < n; i++)
                g.road[r].prev[i] = d[4 + na + i];
        }
        break;
    case 3:
        if (n >= 2)
            k = near_nodes(d[0], d[1], out, 0, 0xF00);
        api->ring_action(3, 0, out, k);
        return;
    case 4:
        for (i = 0; i + 1 < n; i += 2) {
            k = near_nodes(d[i], d[i + 1], out, k, 0xF00);
            if (k < 0x1000)
                out[k++] = 0;
        }
        api->ring_action(3, 0, out, k);
        return;
    }
    api->ring_action(3, 0, NULL, 0);
}

static void fram(uint32_t a, const uint32_t *d, uint32_t n)
{
    static uint32_t out[4096];

    if (!g.fram_loaded)
        fram_load();
    if (a == 0xFFFFFFFFu) {                   /* read {offset, len} */
        uint32_t off = n ? d[0] : 0, len = n > 1 ? d[1] : 0, i;

        if (len > 4096)
            len = 4096;
        for (i = 0; i < len; i++)
            out[i] = off + i < FRAM_WORDS ? g.fram[off + i] : 0xFFFFFFFFu;
        api->ring_action(4, 0, out, len);
        return;
    }
    if (a == 0xFFFFFFFEu) {                   /* fill {start, count, value} */
        uint32_t i;

        for (i = 0; n >= 3 && i < d[1] && d[0] + i < FRAM_WORDS; i++)
            g.fram[d[0] + i] = d[2];
    } else {                                  /* write n words at a */
        uint32_t i;

        for (i = 0; i < n && a + i < FRAM_WORDS; i++)
            g.fram[a + i] = d[i];
    }
    g.fram_dirty = 1;
    g.fram_save_at = CORE->now() + 1;
    api->ring_action(4, 0, NULL, 0);          /* writeFram reads no reply data */
}

static void ring_cmd(uint32_t cmd, uint32_t a, const uint32_t *data, uint32_t n)
{
    static uint32_t out[0x1000];

    if (cmd != 0)
        VLOG("cmd %u a 0x%x n %u%s%08x %08x", cmd, a, n, n ? " data " : "", n ? data[0] : 0, n > 1 ? data[1] : 0);
    switch (cmd) {
    case 0:                                    /* the per-frame random words: an ack */
        api->ring_action(0, 0, NULL, 0);
        break;
    case 3:
        distance(a, data, n);
        break;
    case 4:
        fram(a, data, n);
        break;
    case 5:
        if (a == 1) {                          /* a branch of the current state: the next one */
            uint32_t c = n ? data[0] : 0, next = 0;

            if (g.state < title.flow_states && c < SPEED_BRANCHES)
                next = title.flow[g.state][c];
            if (!next) {
                LOG("flow: no branch %u from state %u; staying", c, g.state);
                next = g.state;
            }
            VLOG("flow: state %u branch %u -> %u", g.state, c, next);
            g.state = next;
            api->ring_action(5, next, NULL, 0);
        } else {                               /* the state Lua went to by name */
            g.state = n ? data[0] : 0;
            VLOG("flow: state %u", g.state);
            api->ring_action(5, 0, NULL, 0);
        }
        break;
    case 6: {                                  /* the FPGA key: our cipher takes its mask from the list (asset_mask) */
        static const uint32_t key[4];

        api->ring_action(6, 0, key, 4);
        break;
    }
    case 7: {                                  /* fuses 0 0, chip version, FPGA fuse OK, FPGA 1.0 */
        static const uint32_t info[6] = { 0, 0, 100, 0, 1, 0 };

        api->ring_action(7, 0, info, 6);
        break;
    }
    case 9: {                                  /* chip memory: the two Lua chunks */
        uint32_t words = n ? data[0] : 0;
        static uint32_t first;
        int which;

        if (words > 0x1000)
            words = 0x1000;
        if (!first)
            first = a;
        which = a != first;                    /* offset, then offset + size1 */
        if (!serve_chunk(which, 4 * words, (uint8_t *)out)) {
            LOG("chip memory 0x%x: %u bytes too small for chunk %d", a, 4 * words, which);
            memset(out, 0, 4 * words);
        }
        LOG("chip memory 0x%x, %u bytes: chunk %d", a, 4 * words, which);
        api->ring_action(9, 0, out, words);
        break;
    }
    default:
        LOG("unknown command %u a 0x%x n %u", cmd, a, n);
        api->ring_action(cmd, 0, NULL, 0);
        break;
    }
}

static void ring_tick(void)
{
    if (g.fram_dirty && CORE->now() >= g.fram_save_at)
        fram_save();
}

/*
 * The I/O board on COM1 (/dev/ttyS0, 115200 8N1), GameIO: the game polls it
 * by writing and reading the answer.
 *   C6 55 AA C5             fuse query -> f0, f1, f0 + f1 (0 = fuses OK)
 *   C3 lo mid hi sum        steering bounds (20, 500, 980): no answer
 *   C1 ... (22 B) / 11 B    motor (force feedback) -> the input frame:
 *     b0  START VIEW BOOST GEAR_UP GEAR_DOWN COIN TEST SERVICE (bits 0..7)
 *     b1  LEFT BOOST_L RIGHT UP DOWN - G2 G1
 *     b2..3 throttle (68..800), b4..5 brake (42..370), b6..7 wheel
 *     (20..980, 500 centre), u16 little-endian, 10 bits; b8 = sum of b0..b7
 * (Power Truck's VR maps the same bits; its scripts read no gear buttons.)
 * Keys (SDL 1.2): 5 coin, 1 / Enter start, arrows wheel, throttle, brake
 * (and the menu buttons), X boost, Z left boost, Q / W gear up / down,
 * C view, F2 test, F3 service.
 */
enum {
    K_RETURN = 13, K_1 = 49, K_5 = 53, K_c = 99, K_q = 113, K_w = 119, K_x = 120, K_z = 122,
    K_UP = 273, K_DOWN, K_RIGHT, K_LEFT, K_F2 = 283, K_F3,
};
enum { B_START, B_VIEW, B_BOOST, B_GEAR_UP, B_GEAR_DOWN, B_COIN, B_TEST, B_SERVICE,
       B_LEFT, B_BOOST_L, B_RIGHT, B_UP, B_DOWN, B_COUNT };

static struct {
    int held[B_COUNT];
    int coin_pulse;                        /* frames the coin switch stays closed */
    float wheel, gas, brake;               /* -1..1, 0..1, 0..1 */
} io;

static float ramp(float v, float target, float step)
{
    return v < target ? (v + step > target ? target : v + step) : (v - step < target ? target : v - step);
}

static void put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }

static uint32_t io_frame(uint8_t *f)
{
    unsigned i, sum = 0;

    io.wheel = ramp(io.wheel, (float)(io.held[B_RIGHT] - io.held[B_LEFT]), 0.08f);
    io.gas = ramp(io.gas, io.held[B_UP] ? 1.0f : 0.0f, 0.15f);
    io.brake = ramp(io.brake, io.held[B_DOWN] ? 1.0f : 0.0f, 0.25f);
    f[0] = io.held[B_START] | io.held[B_VIEW] << 1 | io.held[B_BOOST] << 2 | io.held[B_GEAR_UP] << 3 |
           io.held[B_GEAR_DOWN] << 4 | (io.coin_pulse > 0) << 5 | io.held[B_TEST] << 6 | io.held[B_SERVICE] << 7;
    f[1] = io.held[B_LEFT] | io.held[B_BOOST_L] << 1 | io.held[B_RIGHT] << 2 | io.held[B_UP] << 3 |
           io.held[B_DOWN] << 4;
    put16(f + 2, 68 + (unsigned)(io.gas * (800 - 68)));
    put16(f + 4, 42 + (unsigned)(io.brake * (370 - 42)));
    put16(f + 6, (unsigned)(500 + io.wheel * (io.wheel < 0 ? 500 - 20 : 980 - 500)));
    for (i = 0; i < 8; i++)
        sum += f[i];
    f[8] = sum;
    if (io.coin_pulse > 0)
        io.coin_pulse--;
    return 9;
}

static uint32_t io_write(const uint8_t *p, uint32_t n, uint8_t *reply, uint32_t max)
{
    static unsigned writes;

    if (CORE->verbose && !(writes++ % 300))
        LOG("io: write %u: %u bytes, %02x", writes, n, n ? p[0] : 0);
    if (max < 9 || !n)
        return 0;
    if (n == 4 && p[0] == 0xC6) {                 /* fuse query: both OK */
        reply[0] = reply[1] = reply[2] = 0;
        return 3;
    }
    if (p[0] == 0xC3)                             /* steering bounds */
        return 0;
    io_frame(reply);                              /* motor frame: the inputs */
    if (CORE->verbose && (reply[0] || reply[1]))
        LOG("io: %u-byte motor frame, inputs %02x %02x", n, reply[0], reply[1]);
    return 9;
}

static const struct e2100_serial serial[] = {
    { "/dev/ttyS0", io_write },
    { NULL, NULL },
};

static void key(int sym, int down)
{
    int b = -1;

    switch (sym) {
    case K_5: if (down) io.coin_pulse = 3; return;
    case K_1: case K_RETURN: b = B_START; break;
    case K_LEFT: b = B_LEFT; break;
    case K_RIGHT: b = B_RIGHT; break;
    case K_UP: b = B_UP; break;
    case K_DOWN: b = B_DOWN; break;
    case K_x: b = B_BOOST; break;
    case K_z: b = B_BOOST_L; break;
    case K_q: b = B_GEAR_UP; break;
    case K_w: b = B_GEAR_DOWN; break;
    case K_c: b = B_VIEW; break;
    case K_F2: b = B_TEST; break;
    case K_F3: b = B_SERVICE; break;
    }
    if (b >= 0)
        io.held[b] = down;
    VLOG("key %d %s", sym, down ? "down" : "up");
}

/* the cabinet's network card: the game makes its machine UUID from the MAC of
   eth<config.ethernetDevNo> (db.mac2uuid); a fixed, locally administered one */
static const char *vfile(const char *name)
{
    static char ifc[160];

    if (!strncmp(name, "|ifconfig eth", 13)) {
        snprintf(ifc, sizeof(ifc), "eth%c      Link encap:Ethernet  HWaddr 02:49:47:53:44:33  \n"
                 "          inet addr:172.16.1.1  Bcast:172.16.1.255  Mask:255.255.255.0\n",
                 name[13] >= '0' && name[13] <= '9' ? name[13] : '0');
        return ifc;
    }
    /* boot.lua's version checks against the maker's server: offline (no output) */
    if (!strncmp(name, "|./curl ", 8))
        return "";
    /*
     * OpenAL Soft's settings: the cabinet's ask for ALSA in four channels
     * (drivers = alsa, AL_FORMAT_QUAD16); the loaders play OSS /dev/dsp in
     * stereo, on both hosts (the Linux host has no such file, Windows reads
     * the set's)
     */
    if (strstr(name, "openal/alsoft.conf"))
        return "format = AL_FORMAT_STEREO16\ndrivers = oss\n";
    return NULL;
}

static void reset(struct e2100_channel *ch)
{
    (void)ch;
    g.range = 50;
}

static struct e2100_game game = {
    .version = E2100_API_VERSION,
    .reset = reset,
    .transport = E2100_TRANSPORT_RING,
    .ring_cmd = ring_cmd,
    .ring_tick = ring_tick,
    .vfile = vfile,
    .serial = serial,
    .key = key,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    api = a;
    game.name = title.name;
    game.handshake = title.handshake;
    game.cbc_delta = title.cbc_delta;
    game.cbc_chunks = title.cbc_chunks;
    return &game;
}
