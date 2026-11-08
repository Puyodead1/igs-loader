/*
 * MuziBox (E3000) game plugin: its IGS036's side of the request/response
 * transport (e2100.h, E2100_TRANSPORT_REQRESP).  wiki MuziBox.
 *
 * Each message is [u32 cmd][u32 n][payload], the whole of it RC4-encrypted
 * under one S box keyed "MUZIBOX_" that runs on through both directions
 * (the game keys it after the boot, before its first command); each message
 * starts the stream's i and j at 0.  A request's payload
 * is [u32 pad][pad random bytes][data][u32 pad + the sum of the data's
 * signed bytes]; a reply's is the data itself.  Every command gets a reply.
 *
 *   3   versions and fuses        two words: FPGA 1.0, fuses OK
 *   4   per frame: 16 output bytes (lamps, volumes, coin inhibit) -> the
 *       inputs (bits 0..17) and the coins since the last poll
 *   7   the 512-byte challenge table   5 / 6  lamp patterns / groups
 *   8 / 9    store ID write / read (the ID makes the game skip its network
 *            registration)     16 / 17  signature write / read ("MusicBox")
 *   13  per state: a challenge (the game never checks the answer)
 *   14  the main state script, state.mba: FPGA-encrypted Lua with the
 *       script key.  The chip's own is in its image (ROM-encrypted, key not
 *       known); the plugin serves its own (muzibox_state.h).
 *   20..23  noise                 24  the chip's date (operator screen)
 * The store ID and signature persist in /MuziBox/info/a36_nv.bin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"
#include "muzibox_state.h"

static const struct e2100_api *api;
#define CORE (api->core)
#define LOG(...) CORE->log("muzibox: " __VA_ARGS__)
#define VLOG(...) do { if (CORE->verbose) LOG(__VA_ARGS__); } while (0)

/* ---- the channel cipher ------------------------------------------------------------------- */

/* RC4, except that each message starts i and j at 0 again: only the S box runs on */
static struct { uint8_t s[256]; unsigned i, j; } rc4;

static void rc4_key(const char *k, unsigned n)
{
    unsigned i, j = 0;

    for (i = 0; i < 256; i++)
        rc4.s[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) {
        uint8_t t = rc4.s[i];

        j = (j + k[i % n] + t) & 0xff;
        rc4.s[i] = rc4.s[j];
        rc4.s[j] = t;
    }
    rc4.i = rc4.j = 0;
}

static void rc4_run(uint8_t *p, uint32_t n)
{
    uint32_t k;

    rc4.i = rc4.j = 0;
    for (k = 0; k < n; k++) {
        uint8_t t;

        rc4.i = (rc4.i + 1) & 0xff;
        t = rc4.s[rc4.i];
        rc4.j = (rc4.j + t) & 0xff;
        rc4.s[rc4.i] = rc4.s[rc4.j];
        rc4.s[rc4.j] = t;
        p[k] ^= rc4.s[(uint8_t)(rc4.s[rc4.i] + t)];
    }
}

/* ---- the chip's NV: store ID and signature ------------------------------------------------- */

#define STORE_ID 10000001u                   /* valid IDs are 10000000..11000000 */

static struct {
    int loaded;
    uint32_t store_id;
    char sig[8];
} nv;

static void nv_path(char *buf, size_t n)
{
    const char *set = getenv("IGS_SET");

    snprintf(buf, n, "%s/MuziBox/info/a36_nv.bin", set ? set : ".");
}

static void nv_load(void)
{
    char path[1024];
    FILE *f;

    nv.loaded = 1;
    nv.store_id = STORE_ID;
    memcpy(nv.sig, "MusicBox", 8);
    nv_path(path, sizeof(path));
    if ((f = fopen(path, "rb"))) {
        if (fread(&nv.store_id, 4, 1, f) != 1 || fread(nv.sig, 8, 1, f) != 1) {
            nv.store_id = STORE_ID;
            memcpy(nv.sig, "MusicBox", 8);
        }
        fclose(f);
    }
}

static void nv_save(void)
{
    char path[1024];
    FILE *f;

    nv_path(path, sizeof(path));
    if ((f = fopen(path, "wb"))) {
        fwrite(&nv.store_id, 4, 1, f);
        fwrite(nv.sig, 8, 1, f);
        fclose(f);
    }
}

/*
 * info/setting.cfg: Lua source, byte i XORed with (i - 88).  The factory file's
 * storeName is "MuziBox", and a name starting with that (any case) sends the
 * game into its first-boot store wizard; an operator names the store there.
 * The plugin names it "IGS" instead.
 */
static void setting_seed(void)
{
    static const char name[] = "IGS";
    char path[1024];
    uint8_t *p, *q, *v, *e;
    long n, i;
    FILE *f;

    snprintf(path, sizeof(path), "%s/MuziBox/info/setting.cfg", getenv("IGS_SET") ? getenv("IGS_SET") : ".");
    if (!(f = fopen(path, "rb")))
        return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(n + sizeof(name) + 1);
    if (!p || fread(p, 1, n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return;
    }
    fclose(f);
    for (i = 0; i < n; i++)
        p[i] ^= (uint8_t)(i - 88);
    p[n] = 0;
    q = (uint8_t *)strstr((char *)p, "storeName");
    v = q ? (uint8_t *)strchr((char *)q, '\'') : NULL;
    e = v ? (uint8_t *)strchr((char *)v + 1, '\'') : NULL;
    if (e) {
        char up[8] = {0};

        v++;
        for (i = 0; i < 7 && v + i < e; i++)
            up[i] = (char)(v[i] >= 'a' && v[i] <= 'z' ? v[i] - 32 : v[i]);
        if (e == v || !strcmp(up, "MUZIBOX")) {
            long tail = n - (e - p);

            memmove(v + strlen(name), e, tail + 1);
            memcpy(v, name, strlen(name));
            n = (v - p) + (long)strlen(name) + tail;
            for (i = 0; i < n; i++)
                p[i] ^= (uint8_t)(i - 88);
            if ((f = fopen(path, "wb"))) {
                fwrite(p, 1, n, f);
                fclose(f);
                LOG("store named \"%s\" (%s)", name, path);
            }
        }
    }
    free(p);
}

/* ---- inputs -------------------------------------------------------------------------------- */

/*
 * Input bits (active high), the game's own key names for them: the ten note
 * buttons z x c v b n m , . / are bits 14, 15, 0..7; OK (space) 9; the two
 * shift buttons 10, 11; F7 8; TEST 12; SERVICE 13.  Keys (SDL 1.2): the same
 * letters, space / Enter OK, left / right shift, F7, F2 test, F3 service,
 * 5 coin.
 */
enum {
    K_RETURN = 13, K_SPACE = 32, K_COMMA = 44, K_PERIOD = 46, K_SLASH = 47, K_5 = 53,
    K_b = 98, K_c = 99, K_m = 109, K_n = 110, K_v = 118, K_x = 120, K_z = 122,
    K_RSHIFT = 303, K_LSHIFT = 304, K_F2 = 283, K_F3 = 284, K_F7 = 288,
};

static struct {
    uint32_t held;
    int coins;                     /* coins not yet reported */
} io;

static int key_bit(int sym)
{
    switch (sym) {
    case K_z: return 14;
    case K_x: return 15;
    case K_c: return 0;
    case K_v: return 1;
    case K_b: return 2;
    case K_n: return 3;
    case K_m: return 4;
    case K_COMMA: return 5;
    case K_PERIOD: return 6;
    case K_SLASH: return 7;
    case K_F7: return 8;
    case K_SPACE: case K_RETURN: return 9;
    case K_LSHIFT: return 10;
    case K_RSHIFT: return 11;
    case K_F2: return 12;
    case K_F3: return 13;
    }
    return -1;
}

static void key(int sym, int down)
{
    int b;

    if (sym == K_5) {
        if (down)
            io.coins++;
        return;
    }
    if ((b = key_bit(sym)) < 0)
        return;
    if (down)
        io.held |= 1u << b;
    else
        io.held &= ~(1u << b);
    VLOG("key %d %s", sym, down ? "down" : "up");
}

/* ---- the exchange -------------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* the state script, FPGA-encrypted with the script key (the list's asset_mask), padded to 16 */
static uint32_t state_script(uint8_t *out, uint32_t max)
{
    uint32_t n = (uint32_t)strlen(muzibox_state);
    uint32_t padded = (n + 15) & ~15u;

    if (padded > max || padded > 0x3FF0) {
        LOG("state script too large (%u bytes)", n);
        return 0;
    }
    memcpy(out, muzibox_state, n);
    memset(out + n, '\n', padded - n);
    api->asset_encrypt(out, padded);
    return padded;
}

static uint32_t exchange(const uint8_t *req, uint32_t len, uint8_t *reply, uint32_t max)
{
    static uint8_t m[0x4000];
    uint32_t cmd, n, rn = 0;
    const uint8_t *data = NULL;
    uint32_t dn = 0;
    uint8_t *r = reply + 8;

    if (len < 8 || len > sizeof(m) || max < 16)
        return 0;
    memcpy(m, req, len);
    rc4_run(m, len);
    cmd = rd32(m);
    n = rd32(m + 4);
    if (n >= 8 && 8 + n <= len) {                  /* [pad][pad bytes][data][sum] */
        uint32_t pad = rd32(m + 8);

        if (pad < 4 && 4 + pad + 4 <= n) {
            data = m + 12 + pad;
            dn = n - 4 - pad - 4;
        }
    }
    if (!nv.loaded)
        nv_load();
    switch (cmd) {
    case 3:                                        /* FPGA 1.0, fuses OK */
        wr32(r, 1u << 24);
        wr32(r + 4, 0);
        rn = 8;
        break;
    case 4:                                        /* the inputs, the coins since the last poll */
        wr32(r, io.held);
        r[4] = (uint8_t)(io.coins > 127 ? 127 : io.coins);
        io.coins -= r[4];
        r[5] = r[6] = r[7] = 0;
        rn = 8;
        break;
    case 8:                                        /* store ID */
        if (dn >= 4) {
            nv.store_id = rd32(data);
            nv_save();
        }
        break;
    case 9:
        wr32(r, nv.store_id);
        rn = 4;
        break;
    case 14:
        rn = state_script(r, max - 8);
        break;
    case 16:                                       /* signature */
        if (dn >= 8) {
            memcpy(nv.sig, data, 8);
            nv_save();
        }
        break;
    case 17:
        memcpy(r, nv.sig, 8);
        rn = 8;
        break;
    case 24:                                       /* the chip's date */
        wr32(r, 20111201);
        rn = 4;
        break;
    default:                                       /* 5, 6, 7, 13, 20..23: acknowledged */
        break;
    }
    if (cmd != 4 && cmd != 13)
        VLOG("cmd %u, %u data bytes -> %u", cmd, dn, rn);
    wr32(reply, cmd);
    wr32(reply + 4, rn);
    rc4_run(reply, 8 + rn);
    return 8 + rn;
}

static void reset(struct e2100_channel *ch)
{
    (void)ch;
    setting_seed();
    rc4_key("MUZIBOX_", 8);
    io.coins = 0;
}

static void status(char *buf, size_t n)
{
    snprintf(buf, n, "store %u", nv.store_id);
}

static const struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "MuziBox",
    .reset = reset,
    .transport = E2100_TRANSPORT_REQRESP,
    .handshake = E2100_HS_SESSION,
    .exchange = exchange,
    .key = key,
    .status = status,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    api = a;
    return &game;
}
