/*
 * Speed Rider (E2000): what its IGS036 firmware does, as far as the game
 * needs.  docs/SpeedRider.md.  The executable is stripped; addresses below
 * are its own.
 *
 * Transport: IGS036 messages (machines/e2000, E2000_IGS036_MSG).  Every
 * frame the chip sends header words (the analog inputs and the buttons) and
 * sub-messages [length, type, data..., sum | 0x80]; the game answers with
 * its own.
 *
 *   game -> chip                          chip -> game
 *   1     hello                           1     key index (re-key the stream)
 *   2 n   zip prefix of archive n         2     2000 bytes, 200 at a time
 *   0xC2  member-card bit order           0x42  672 words, 200 bytes at a time
 *   0xC3  member-card key                 0x43  8 words, the first nonzero
 *   0xC4 n  load NVRAM block n            0x44  the block, 200 bytes at a time
 *   0xC5 n, 0xC6...  store block n
 *   0xC0  AI rider: index, speed error,   0x40  index, throttle, brake
 *         last throttle
 *   18 n  coins spent                     0x10  coin count (credits = count / price)
 *   0xC1  UI state                        0x41  UI elements to show / hide
 *   0x80..0x9D  a game mode ended         0x20  the next game mode
 *                                         3     versions (9 words)
 *                                         0x30 / 0x31 c  button c down / up
 *   0x20..0x27, 16, 17, 19, 20: lamps, meters, events (ack)
 *
 * The zip prefixes: data/zip/<name>_2.zipsep are zip archives without their
 * first 2000 bytes; the chip holds those.  They are rebuilt here from the
 * archives' central directories (local headers), from identical files in
 * the other archives, and, for the few files nothing else holds, from
 * templates (see zip_prefix).
 *
 * Invented (the IGS036's ROM is not dumped): the key index (0, "ILoveIGS"),
 * the member-card tables, the AI throttle/brake law, the lost file contents,
 * the game-mode table (no link play).  Reconstructed from the game's own
 * calls: the UI table.  NVRAM starts with the factory settings, calibrated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2000/e2000.h"

#define PRE        2000                   /* zip prefix bytes the chip holds */
#define CHUNK      200                    /* bulk bytes per sub-message */
#define OUT_BYTES  1024                   /* sub-message bytes per chip message */
#define QUEUE      65536

#define NV_FILE    "../../../rewrite/igs036.nv"   /* cwd is /sr/Game/bin_linux */
#define ZIP_DIR    "../data/zip/"

/* NVRAM blocks (0x893D944) */
static const uint16_t nv_size[5] = { 264, 660, 3604, 1444, 1444 };
#define NV_BYTES (264 + 660 + 3604 + 1444 + 1444)

/* archive index -> file (sub_82D9D70 matches "OGRE.", "MENUUI.", ...) */
static const char *const zip_names[13] = {
    "ogre", "MenuUI", "RaceUI", "GreatDam", "ShangHai", "Taipei", "HeyLong",
    "GreatDamA", "ShangHaiA", "TaipeiA", "HeyLongA", "Factory", "Champion",
};
#define ZIPS 13

/* SDL 1.2 key symbols */
enum {
    K_RETURN = 13, K_SPACE = 32, K_1 = 49, K_5 = 53, K_9 = 57,
    K_a = 97, K_d = 100, K_s = 115, K_w = 119, K_x = 120,
    K_UP = 273, K_DOWN = 274, K_RIGHT = 275, K_LEFT = 276, K_F1 = 282, K_F2 = 283,
};

/*
 * Buttons, by the game's index (sr::GameIOHandle +620 + 4i).  Header word
 * (chip -> game, +32..+60) and chip button number (0x30/0x31 events, mapped
 * by 0x893D480) for each.
 */
enum { B_TEST, B_SERVICE, B_LEFT, B_RIGHT, B_UP, B_DOWN, B_BOOST, B_SENSOR, BUTTONS };
static const uint8_t button_hdr[BUTTONS] = { 9, 10, 7, 8, 5, 6, 3, 4 };
static const uint8_t button_chip[BUTTONS] = { 3, 4, 1, 2, 7, 6, 0, 5 };

static const struct e2000_api *api;

static struct {
    int buttons, sent_buttons;            /* bit per index */
    int coins, sent_coins;                /* coin count (the chip's) */
    int throttle_key, brake_key, lean_key;
    float throttle, brake, lean;
    float lean_rate;
    int helloed;
    struct {
        uint8_t valid[5];
        uint8_t data[NV_BYTES];
    } nv;
    int store_block, store_len;           /* NVRAM store in progress */
    uint8_t queue[QUEUE];
    int qlen;
    int help, trace;
    int mode;                             /* game mode chosen (SelectMode) */
    unsigned frames;
} g;

/* ---- sub-messages to the game ----------------------------------------------------- */

static void put(uint8_t type, const void *data, int n)
{
    uint8_t *p = g.queue + g.qlen, sum;
    int i;

    if (n > 252 || g.qlen + n + 3 > QUEUE) {
        api->core->log("speedrider: queue full, sub-message 0x%02x dropped", type);
        return;
    }
    p[0] = n + 3;
    p[1] = type;
    memcpy(p + 2, data, n);
    for (sum = 0, i = 0; i < n + 2; i++)
        sum += p[i];
    p[n + 2] = sum | 0x80;
    g.qlen += n + 3;
}

static void put_bulk(uint8_t type, const uint8_t *data, int n)
{
    int k;

    for (k = 0; k < n; k += CHUNK)
        put(type, data + k, n - k < CHUNK ? n - k : CHUNK);
}

/* ---- NVRAM ------------------------------------------------------------------------- */

static uint8_t *nv_block(int b)
{
    int k, off = 0;

    for (k = 0; k < b; k++)
        off += nv_size[k];
    return g.nv.data + off;
}

/*
 * A blank NVRAM would show the "values not set" warning (controls not
 * calibrated) until the operator calibrates.  Block 0 starts as the game's
 * factory settings (sub_830D640, laid out as sub_830E9B0 reads them) with
 * "calibrated" set: the factory calibration is what the inputs give.
 */
static void st32(uint8_t *p, uint32_t v);

static void nv_factory(void)
{
    static const struct { uint8_t off; uint32_t v; } f[] = {
        { 0, 3 }, { 4, 1 }, { 8, 1 }, { 12, 0xbf800000 }, { 16, 0x3f800000 }, { 28, 0x3f800000 },
        { 36, 0x3f800000 }, { 55, 1 }, { 61, 2 }, { 65, 2 }, { 69, 1 }, { 73, 1 }, { 77, 1 },
        { 83, 9 }, { 91, 20 }, { 95, 25 }, { 99, 22 }, { 107, 18 }, { 111, 22 }, { 115, 11 },
        { 123, 1 }, { 127, 11 },
    };
    uint8_t *b = nv_block(0);
    unsigned i;

    memset(b, 0, nv_size[0]);
    for (i = 0; i < sizeof(f) / sizeof(f[0]); i++)
        st32(b + f[i].off, f[i].v);
    b[40] = 1;                                 /* +36 */
    b[81] = 1;                                 /* +76 */
    b[135] = 1;                                /* +132 controls calibrated */
    g.nv.valid[0] = 1;
}

static void nv_load(void)
{
    FILE *f = fopen(NV_FILE, "rb");

    if (!f || fread(&g.nv, sizeof(g.nv), 1, f) != 1)
        memset(&g.nv, 0, sizeof(g.nv));
    if (f)
        fclose(f);
    if (!g.nv.valid[0])
        nv_factory();
}

static void nv_save(void)
{
    FILE *f = fopen(NV_FILE, "wb");

    if (!f) {
        api->core->log("speedrider: cannot write %s", NV_FILE);
        return;
    }
    fwrite(&g.nv, sizeof(g.nv), 1, f);
    fclose(f);
}

/* ---- zip prefixes -------------------------------------------------------------------- */

static uint32_t crc32(const uint8_t *p, size_t n)
{
    static uint32_t t[256];
    uint32_t c = 0xffffffff;
    size_t i;

    if (!t[1])
        for (i = 0; i < 256; i++) {
            uint32_t v = (uint32_t)i;
            int k;

            for (k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
    for (i = 0; i < n; i++)
        c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void st16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void st32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

struct zent {
    char name[96];
    uint32_t lho, crc, csz, usz;
    uint16_t need, flag, meth, mt, md;
};

static struct zarc {
    FILE *f;
    struct zent *e;
    int n, tried;
} arcs[ZIPS];

/* an archive's central directory (stored PRE bytes before the offsets it gives) */
static struct zarc *zarc(int i)
{
    struct zarc *a = &arcs[i];
    char path[256];
    uint8_t *buf;
    long size, start, k;
    uint32_t cd_size, n, j, p;

    if (a->tried)
        return a->e ? a : NULL;
    a->tried = 1;
    snprintf(path, sizeof(path), ZIP_DIR "%s_2.zipsep", zip_names[i]);
    if (!(a->f = fopen(path, "rb")))
        return NULL;
    fseek(a->f, 0, SEEK_END);
    size = ftell(a->f);
    start = size > 65557 ? size - 65557 : 0;
    buf = malloc(size - start);
    fseek(a->f, start, SEEK_SET);
    if (!buf || fread(buf, 1, size - start, a->f) != (size_t)(size - start)) {
        free(buf);
        return NULL;
    }
    for (k = size - start - 22; k >= 0 && le32(buf + k) != 0x06054b50; k--)
        ;
    if (k < 0) {
        free(buf);
        return NULL;
    }
    n = le16(buf + k + 10);
    cd_size = le32(buf + k + 12);
    free(buf);
    if (!(buf = malloc(cd_size)) || !(a->e = calloc(n, sizeof(*a->e)))) {
        free(buf);
        return NULL;
    }
    fseek(a->f, start + k - cd_size, SEEK_SET);
    if (fread(buf, 1, cd_size, a->f) != cd_size)
        n = 0;
    for (j = 0, p = 0; j < n && p + 46 <= cd_size && le32(buf + p) == 0x02014b50; j++) {
        struct zent *e = &a->e[j];
        uint16_t fl = le16(buf + p + 28);

        e->need = le16(buf + p + 6);
        e->flag = le16(buf + p + 8);
        e->meth = le16(buf + p + 10);
        e->mt = le16(buf + p + 12);
        e->md = le16(buf + p + 14);
        e->crc = le32(buf + p + 16);
        e->csz = le32(buf + p + 20);
        e->usz = le32(buf + p + 24);
        e->lho = le32(buf + p + 42);
        memcpy(e->name, buf + p + 46, fl < sizeof(e->name) - 1 ? fl : sizeof(e->name) - 1);
        p += 46 + fl + le16(buf + p + 30) + le16(buf + p + 32);
    }
    a->n = j;
    free(buf);
    return a;
}

/* an entry's data offset in the full archive (local extras are empty) */
static uint32_t data_off(const struct zent *e)
{
    return e->lho + 30 + (uint32_t)strlen(e->name);
}

/* read [from, to) of an entry's data, as far as the file holds it; returns bytes read */
static long zread(struct zarc *a, const struct zent *e, uint8_t *buf, uint32_t from, uint32_t to)
{
    uint32_t d = data_off(e);

    if (d + from < PRE)
        from = PRE - d;
    if (from >= to)
        return 0;
    fseek(a->f, (long)(d + from - PRE), SEEK_SET);
    return (long)fread(buf + from, 1, to - from, a->f);
}

static const char *suffix(const char *name)
{
    const char *u = strrchr(name, '_'), *dot = strrchr(name, '.');

    return u && dot && u < dot ? u : "";
}

/* an intact copy (same CRC and size) in any archive */
static int find_copy(const struct zent *x, uint8_t *buf)
{
    int i, j;

    for (i = 0; i < ZIPS; i++) {
        struct zarc *a = zarc(i);

        for (j = 0; a && j < a->n; j++) {
            struct zent *e = &a->e[j];

            if (e->crc == x->crc && e->csz == x->csz && e->lho >= PRE &&
                zread(a, e, buf, 0, e->csz) == (long)e->csz)
                return 1;
        }
    }
    return 0;
}

/* DDS: the header most textures of the same size and suffix have; black pixels */
static int synth_dds(const struct zent *x, uint8_t *buf)
{
    static uint8_t hdr[16][128];
    int count[16] = { 0 }, nh = 0, best = -1, i, j, k, pass;

    for (pass = 0; pass < 2 && best < 0; pass++)
        for (i = 0; i < ZIPS; i++) {
            struct zarc *a = zarc(i);

            for (j = 0; a && j < a->n; j++) {
                struct zent *e = &a->e[j];
                uint8_t h[128];

                if (e->csz != x->csz || e->lho < PRE || !strstr(e->name, ".dds") ||
                    (!pass && strcmp(suffix(e->name), suffix(x->name))))
                    continue;
                if (zread(a, e, h, 0, 128) != 128 || memcmp(h, "DDS ", 4))
                    continue;
                for (k = 0; k < nh && memcmp(hdr[k], h, 128); k++)
                    ;
                if (k == nh && nh < 16)
                    memcpy(hdr[nh++], h, 128);
                if (k < 16 && ++count[k] > (best < 0 ? 0 : count[best]))
                    best = k;
            }
        }
    memset(buf, 0, x->csz);
    if (best < 0)
        return 0;
    memcpy(buf, hdr[best], 128);
    return 1;
}

/*
 * Ogre .mesh (oFusion exports: one submesh of quads, its own vertices).  The
 * layout comes from an intact mesh of the same name stem ("ag03-" for
 * "ag03-01.mesh"): version, material, vertex declaration and buffers, the
 * chunks after them.  Its vertex count makes the size match; indices are the
 * quads' (0 1 2 2 3 0, ...).  Lost positions collapse to the bounds' centre
 * (the part draws nothing), normals point up, colours white, UVs 0.
 */
static int synth_mesh(int arc, const struct zent *x, uint8_t *buf, const uint8_t *known, uint32_t known_from)
{
    const struct zent *t = NULL;
    struct zarc *ta = NULL;
    char stem[96];
    const char *dash = strrchr(x->name, '-');
    uint8_t *tb, *o = buf;
    uint32_t p, q, vt, count, nbuf = 0, per = 0, fixed, v, i, k, decl_at, decl_len, trail_at, ver_len;
    uint32_t idx_at, new_trail, buf_at[8];
    uint16_t bind[8], size[8];
    int32_t grow;
    int pass, j;

    snprintf(stem, sizeof(stem), "%.*s", dash ? (int)(dash - x->name + 1) : 0, x->name);
    for (pass = 0; pass < 2 && !t; pass++)
        for (k = 0; k < ZIPS && !t; k++) {
            struct zarc *a = zarc(pass ? (int)k : arc);

            for (j = 0; a && j < a->n && !t; j++)
                if (a->e[j].lho >= PRE && strstr(a->e[j].name, ".mesh") && *stem &&
                    !strncmp(a->e[j].name, stem, strlen(stem)) && a->e[j].csz < 1 << 20) {
                    t = &a->e[j];
                    ta = a;
                }
            if (!pass)
                break;
        }
    if (!t || !(tb = malloc(t->csz)) || zread(ta, t, tb, 0, t->csz) != (long)t->csz)
        return 0;
    /* parse the template: header, 0x3000, 0x4000 (material, indices), 0x5000 */
    for (p = 2; p < t->csz && tb[p] != '\n'; p++)
        ;
    ver_len = ++p;                         /* 0x3000 starts here */
    if (le16(tb) != 0x1000 || le16(tb + p) != 0x3000 || le16(tb + p + 7) != 0x4000)
        goto bad;
    for (q = p + 13; q < t->csz && tb[q] != '\n'; q++)
        ;
    q++;                                   /* past the material: shared, count, 32-bit */
    if (q + 6 > t->csz || tb[q] || tb[q + 5])
        goto bad;                          /* shared vertices or 32-bit indices */
    idx_at = q + 6;
    count = le32(tb + q + 1);
    q = idx_at + 2 * count;
    if (q + 10 > t->csz || le16(tb + q) != 0x5000)
        goto bad;
    vt = le32(tb + q + 6);
    decl_at = q + 10;
    if (le16(tb + decl_at) != 0x5100 || count * 2 != vt * 3 || vt % 4)
        goto bad;
    decl_len = le32(tb + decl_at + 2);
    for (q = decl_at + decl_len; q + 16 <= t->csz && le16(tb + q) == 0x5200 && nbuf < 8; nbuf++) {
        bind[nbuf] = le16(tb + q + 6);
        size[nbuf] = le16(tb + q + 8);
        per += size[nbuf];
        q += 16 + vt * size[nbuf];
    }
    trail_at = q;
    if (!nbuf || trail_at > t->csz)
        goto bad;
    fixed = t->csz - vt * (3 + per);
    if (x->csz < fixed || (x->csz - fixed) % (3 + per) || ((x->csz - fixed) / (3 + per)) % 4)
        goto bad;
    v = (x->csz - fixed) / (3 + per);
    count = v * 3 / 2;
    /*
     * chunk lengths as the exporter writes them: it counts 4 bytes per index
     * (6 per vertex) in 0x3000 / 0x4000 (checked against ag03-02.mesh's CRC)
     */
    grow = ((int32_t)v - (int32_t)vt) * (int32_t)(per + 6);
    /* build: header, 0x3000, 0x4000 with the quads' indices, 0x5000 */
    memcpy(o, tb, idx_at);
    st32(o + ver_len + 2, le32(tb + ver_len + 2) + grow);
    st32(o + ver_len + 9, le32(tb + ver_len + 9) + grow);
    st32(o + idx_at - 5, count);
    p = idx_at;
    for (i = 0; i < count; i++) {
        static const uint8_t quad[6] = { 0, 1, 2, 2, 3, 0 };

        st16(o + p + 2 * i, (uint16_t)(i / 6 * 4 + quad[i % 6]));
    }
    p += 2 * count;
    st16(o + p, 0x5000);
    st32(o + p + 2, le32(tb + decl_at - 8) + ((int32_t)v - (int32_t)vt) * (int32_t)per);
    st32(o + p + 6, v);
    p += 10;
    memcpy(o + p, tb + decl_at, decl_len);
    p += decl_len;
    for (k = 0; k < nbuf; k++) {
        uint32_t d = v * size[k], e;

        buf_at[k] = p + 16;
        st16(o + p, 0x5200);
        st32(o + p + 2, 16 + d);
        st16(o + p + 6, bind[k]);
        st16(o + p + 8, size[k]);
        st16(o + p + 10, 0x5210);
        st32(o + p + 12, 6 + d);
        memset(o + p + 16, 0, d);
        /* this buffer's elements: normals up, colours white */
        for (e = decl_at + 6; e + 16 <= decl_at + decl_len; e += 16) {
            uint16_t src = le16(tb + e + 6), type = le16(tb + e + 8), sem = le16(tb + e + 10),
                     off = le16(tb + e + 12);

            for (i = 0; src == bind[k] && i < v; i++) {
                uint8_t *at = o + buf_at[k] + i * size[k] + off;

                if (sem == 4 && type == 2)
                    st32(at + 4, 0x3f800000);
                else if (sem == 5)
                    st32(at, 0xffffffff);
            }
        }
        p += 16 + d;
    }
    new_trail = p;
    memcpy(o + p, tb + trail_at, t->csz - trail_at);
    p += t->csz - trail_at;
    if (p != x->csz)
        goto bad;
    /* the known tail over it; lost positions at the bounds' centre */
    memcpy(buf + known_from, known + known_from, x->csz - known_from);
    for (q = new_trail; q + 34 <= x->csz; q++)
        if (le16(buf + q) == 0x9000 && le32(buf + q + 2) == 34)
            break;
    if (q + 34 <= x->csz) {
        uint8_t c[12];
        float mn, mx, m;
        uint32_t e;

        for (k = 0; k < 3; k++) {
            memcpy(&mn, buf + q + 6 + 4 * k, 4);
            memcpy(&mx, buf + q + 18 + 4 * k, 4);
            m = (mn + mx) / 2;
            memcpy(c + 4 * k, &m, 4);
        }
        for (k = 0; k < nbuf; k++)
            for (e = decl_at + 6; e + 16 <= decl_at + decl_len; e += 16)
                if (le16(tb + e + 6) == bind[k] && le16(tb + e + 10) == 1 && le16(tb + e + 8) == 2)
                    for (i = 0; i < v; i++) {
                        uint32_t at = buf_at[k] + i * size[k] + le16(tb + e + 12);

                        if (at + 12 <= known_from)
                            memcpy(buf + at, c, 12);
                    }
    }
    free(tb);
    return 1;
bad:
    api->core->log("speedrider: zip prefix: %s: template %s does not fit", x->name, t->name);
    free(tb);
    return 0;
}

/* the first PRE bytes of archive n */
static void zip_prefix(int n, uint8_t *pre)
{
    static uint8_t cache[ZIPS][PRE], done[ZIPS];
    struct zarc *a;
    int j;

    if (n < 0 || n >= ZIPS) {
        memset(pre, 0, PRE);
        return;
    }
    if (done[n]) {
        memcpy(pre, cache[n], PRE);
        return;
    }
    memset(cache[n], 0, PRE);
    if (!(a = zarc(n))) {
        api->core->log("speedrider: zip prefix %d: no " ZIP_DIR "%s_2.zipsep", n, zip_names[n]);
    } else {
        for (j = 0; j < a->n; j++) {
            struct zent *e = &a->e[j];
            uint8_t h[30 + 96], *buf;
            uint32_t d, i, nl = (uint32_t)strlen(e->name);
            const char *how = "stand-in";

            if (e->lho >= PRE)
                continue;
            /* the local header, from the central directory */
            st32(h, 0x04034b50);
            st16(h + 4, e->need);
            st16(h + 6, e->flag);
            st16(h + 8, e->meth);
            st16(h + 10, e->mt);
            st16(h + 12, e->md);
            st32(h + 14, e->crc);
            st32(h + 18, e->csz);
            st32(h + 22, e->usz);
            st16(h + 26, (uint16_t)nl);
            st16(h + 28, 0);
            memcpy(h + 30, e->name, nl);
            for (i = 0; i < 30 + nl && e->lho + i < PRE; i++)
                cache[n][e->lho + i] = h[i];
            d = data_off(e);
            if (d >= PRE || !(buf = calloc(1, e->csz + 1)))
                continue;
            if (find_copy(e, buf)) {
                how = "copy";
            } else {
                uint8_t *known = calloc(1, e->csz + 1);
                uint32_t from = PRE - d;   /* the data the file still holds starts here */

                if (known)
                    zread(a, e, known, 0, e->csz);
                if (strstr(e->name, ".dds"))
                    synth_dds(e, buf);
                else if (strstr(e->name, ".mesh") && known)
                    synth_mesh(n, e, buf, known, from < e->csz ? from : e->csz);
                if (known && from < e->csz)
                    memcpy(buf + from, known + from, e->csz - from);
                free(known);
                if (crc32(buf, e->csz) == e->crc)
                    how = "exact";
            }
            for (i = 0; i < e->csz && d + i < PRE; i++)
                cache[n][d + i] = buf[i];
            api->core->log("speedrider: zip prefix %s: %s (%u bytes, %u lost) %s", zip_names[n], e->name,
                           e->csz, PRE - d < e->csz ? PRE - d : e->csz, how);
            free(buf);
        }
    }
    done[n] = 1;
    memcpy(pre, cache[n], PRE);
    {
        /* SR_PREFIX_DIR=<dir>: also write each prefix as <dir>/<name>.pre (checking) */
        const char *dir = api->core->env("SR_PREFIX_DIR", "");
        char path[512];
        FILE *f;

        snprintf(path, sizeof(path), "%s/%s.pre", dir, zip_names[n]);
        if (*dir && (f = fopen(path, "wb"))) {
            fwrite(pre, 1, PRE, f);
            fclose(f);
        }
    }
}

/* ---- the game's sub-messages ------------------------------------------------------------- */

/*
 * AI riders (sub_805E900): the game sends its speed error (wanted - actual)
 * and the last throttle; the chip answers throttle and brake (0..1).
 * Invented: a proportional law.
 */
static void ai(const uint8_t *d)
{
    struct { int32_t idx; float thr, brk; } r;
    float err;

    memcpy(&r.idx, d, 4);
    memcpy(&err, d + 4, 4);
    r.thr = err > 0 ? 0.4f + err * 0.1f : 0.4f + err * 0.2f;
    r.thr = r.thr < 0 ? 0 : r.thr > 1 ? 1 : r.thr;
    r.brk = err < -2 ? -err * 0.05f : 0;
    r.brk = r.brk > 1 ? 1 : r.brk;
    put(0x40, &r, 12);
}

/*
 * UI (sub_834E030, UImapping): for each UI state the game hides everything
 * and then calls show/hide on its UI elements with a "the IGS036 does it"
 * flag, which makes the call a no-op (sub_83606F0); it sends the state
 * (0xC1) and the chip answers with the list of elements to show (+index) or
 * hide (-index), index = (member offset - 84) / 4.  Reconstructed from
 * those flagged calls.
 */
static const struct { uint16_t state; int8_t el[5]; } ui_table[] = {
    { 257, { 1, 14, 15 } },       /* race HUD */
    { 258, { -23 } },
    { 259, { 1, 3 } },
    { 260, { 1, 3 } },
    { 261, { 1, 3 } },
    { 262, { 1, 14, 15 } },
    { 263, { 1, 8 } },
    { 264, { 1, 23 } },
    { 265, { -10, -23 } },
    { 272, { -23 } },
    { 769, { 34, 41 } },          /* reward */
    { 770, { 34, 41 } },
    { 771, { 34, 41 } },
    { 772, { 7, 34, 31 } },       /* continue */
    { 773, { 4 } },               /* mode select */
    { 774, { 7, 34, 38 } },       /* mode select */
    { 775, { 7, 34, 30 } },       /* stage select */
    { 776, { 7, 34, 29 } },       /* stage select */
    { 777, { 7, 34, 24, 2 } },    /* information */
    { 778, { 7, 34, 25 } },       /* factory */
    { 779, { 7, 34, 26 } },       /* player select */
    { 780, { 7, 34, 27, 2 } },    /* cars select */
    { 781, { 7, 34, 28 } },       /* colour select */
    { 782, { 7, 34, 42 } },       /* select logo */
    { 784, { 7, 34, 44, 2 } },    /* join team */
    { 800, { 7, 37, 2 } },        /* wait join */
    { 801, { 7, 34, 30 } },       /* stage select */
    { 815, { 35, 32 } },
    { 818, { 36 } },
    { 821, { 46 } },
    { 822, { 32, 2 } },           /* attract backgrounds */
    { 823, { 32, 2 } },
    { 832, { 2, 7, 34 } },        /* enter name */
    { 833, { 2, 7, 34 } },        /* enter team */
    { 834, { 2, 7, 34 } },        /* enter moto ID */
    { 835, { 7, 34, 45, 2 } },
    { 1281, { 1, 32 } },
    { 1282, { 1, 34 } },
    { 1537, { 47 } },
};

static void ui_state(uint32_t state)
{
    unsigned i, k;

    if (api->core->verbose)
        api->core->log("speedrider: UI state %u", state);
    for (i = 0; i < sizeof(ui_table) / sizeof(ui_table[0]); i++)
        if (ui_table[i].state == state) {
            int32_t v[6];

            for (k = 0; k < 5 && ui_table[i].el[k]; k++)
                v[k + 1] = ui_table[i].el[k];
            v[0] = k;
            put(0x41, v, 4 * (k + 1));
            return;
        }
}

/*
 * Game flow (sr::GameFSM, sub_80B8C60).  Each game mode ends by sending its
 * event (0x80 + its class's place in the alphabet, sub_82DB870) with a few
 * bytes of outcome and waits; the chip answers 0x20 with the next mode
 * (the FSM's index, sub_80B8560).  Reconstructed: the IGS036 holds the real
 * table.
 */
enum {
    M_RACE = 0, M_TA = 1, M_STORY = 2, M_NET = 3, M_FACTORY = 4, M_OPERATOR = 5, M_CHAMPION = 6,
    M_IDLE = 7, M_FOURSCREEN = 8, M_RANK = 9, M_SELECTCAR = 10, M_PROFILER = 11, M_SELECTMODE = 12,
    M_LOBBY = 13, M_SINGLESELECT = 14, M_REWARD = 15, M_TEAM = 16, M_ADDTEAM = 17, M_CONTINUE = 18,
    M_TEST = 19, M_CHANGECARD = 21, M_SIGNATURE = 22, M_HWTEST = 23, M_CONNECTSET = 24,
    M_CONTROLSET = 25, M_STORESET = 26, M_TIMESET = 27, M_DATAINIT = 28, M_INCOME = 29,
    M_CARDREVERT = 30, M_COINSET = 31,
};

static int flow_next(uint8_t event, const uint8_t *d, int n)
{
    uint8_t b[8] = { 0 };

    memcpy(b, d, n < 8 ? n : 8);
    switch (event) {
    case 0x8B:                                 /* Idle: start, attract mode, ..., test */
        if (b[6])
            return M_TEST;
        if (b[0])
            return M_SELECTMODE;
        return b[1] == 1 || b[1] == 2 ? M_RANK : M_IDLE;
    case 0x92:                                 /* SelectMode: 0 link, 1 story, 2 time attack */
        g.mode = b[0];
        return b[0] == 0 ? M_SELECTMODE : M_SELECTCAR;   /* one cabinet: no link play */
    case 0x91: return M_SINGLESELECT;          /* SelectCarAndPlayer */
    case 0x94: return g.mode == 2 ? M_TA : M_STORY;   /* SingleSelect (stage) */
    case 0x8E: return M_REWARD;                /* Race */
    case 0x97: return M_REWARD;                /* TA */
    case 0x96: return M_REWARD;                /* Story */
    case 0x90: return M_CONTINUE;              /* Reward */
    case 0x85: return b[0] ? M_RANK : M_SINGLESELECT; /* Continue: 1 = no */
    case 0x8F: return M_IDLE;                  /* Rank */
    case 0x81: return M_IDLE;                  /* Champion */
    case 0x89: return M_IDLE;                  /* FourScreen */
    case 0x8A: case 0x84: case 0x86: case 0x95: case 0x99: case 0x87: case 0x9C: case 0x9D:
    case 0x8D: case 0x88:
        return M_TEST;                         /* hardware test, settings, operator, factory */
    default:
        return M_IDLE;
    }
}

static void handle(uint8_t type, const uint8_t *d, int n)
{
    static uint8_t buf[4096];
    int i;

    switch (type) {
    case 1:                                    /* hello: the stream's key index */
        buf[0] = 0;
        put(1, buf, 1);
        api->rekey(1, "ILoveIGS", 8);
        /*
         * versions (sub_80F5260 checks them, sub_832C120): words 1, 2, 3, 0 the
         * ROM's {0, 1, 0x30001, 100} ("100.0.1"), 5, 6, 7, 4 the IGS036
         * program's {0, 1, 0x30001, 109} ("109.0.1", the CF card's), 8 fault
         * bits (I/O test: bit 0, 1 "NOT OK")
         */
        {
            static const int32_t ver[9] = { 100, 0, 1, 0x30001, 109, 0, 1, 0x30001, 0 };

            put(3, ver, sizeof(ver));
        }
        g.helloed = 1;
        g.sent_coins = -1;
        api->core->log("speedrider: IGS036 hello");
        break;
    case 2:                                    /* zip prefix */
        zip_prefix(n ? d[0] : -1, buf);
        put_bulk(2, buf, PRE);
        break;
    case 0xC2:                                 /* member-card bit order: identity */
        for (i = 0; i < 672; i++)
            st32(buf + 4 * i, i);
        put_bulk(0x42, buf, 2688);
        break;
    case 0xC3:
        memset(buf, 0, 32);
        buf[0] = 1;
        put(0x43, buf, 32);
        break;
    case 0xC4:                                 /* NVRAM load (zeros = blank) */
        /*
         * Operator settings (block 0, version 3, sub_830E9B0): byte 135 is
         * "controls calibrated" (sub_83031C0); unset, the attract stops at
         * the "values not set" warning.  The factory calibration (steer
         * -1..1, throttle and brake 0..1) is what the inputs here give, so
         * mark it calibrated.
         */
        if (n && d[0] == 0 && g.nv.valid[0] && le32(nv_block(0)) == 3 && !nv_block(0)[135])
            nv_block(0)[135] = 1;
        if (n && d[0] < 5)
            put_bulk(0x44, g.nv.valid[d[0]] ? nv_block(d[0]) : memset(buf, 0, nv_size[d[0]]),
                     nv_size[d[0]]);
        break;
    case 0xC5:                                 /* NVRAM store: block, then 0xC6 data */
        g.store_block = n && d[0] < 5 ? d[0] : -1;
        g.store_len = 0;
        break;
    case 0xC6:
        if (g.store_block >= 0) {
            int size = nv_size[g.store_block];

            if (g.store_len + n > size)
                n = size - g.store_len;
            memcpy(nv_block(g.store_block) + g.store_len, d, n);
            g.store_len += n;
            if (g.store_len == size) {
                g.nv.valid[g.store_block] = 1;
                nv_save();
                api->core->log("speedrider: NVRAM block %d stored", g.store_block);
                g.store_block = -1;
            }
        }
        break;
    case 0xC0:
        if (n >= 12)
            ai(d);
        break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: {  /* a mode ended */
        uint8_t next = (uint8_t)flow_next(type, d, n);

        api->core->log("speedrider: mode event 0x%02x (%d bytes%s%02x %02x) -> mode %d", type, n,
                       n ? ": " : " ", n ? d[0] : 0, n > 1 ? d[1] : 0, next);
        put(0x20, &next, 1);
        break;
    }
    case 0xC1:                                 /* UI state -> elements to show / hide */
        if (n >= 4)
            ui_state(le32(d));
        break;
    case 18:                                   /* coins spent on a credit */
        if (n >= 4) {
            int32_t v;

            memcpy(&v, d, 4);
            g.coins = g.coins > v ? g.coins - v : 0;
            g.sent_coins = g.coins;            /* the game counted it itself */
        }
        break;
    default:
        if (api->core->verbose) {
            char hex[40] = "";

            for (i = 0; i < n && i < 12; i++)
                snprintf(hex + 3 * i, 4, " %02x", d[i]);
            api->core->log("speedrider: sub-message 0x%02x, %d bytes:%s", type, n, hex);
        }
        break;
    }
}

/* ---- inputs ------------------------------------------------------------------------- */

static float ramp(float v, float target, float up, float down)
{
    float step = (target > v ? (v >= 0 ? up : down) : (v <= 0 ? up : down));

    if (target > v)
        return v + step < target ? v + step : target;
    return v - step > target ? v - step : target;
}

static void inputs(uint32_t *hdr)
{
    int i;

    g.throttle = ramp(g.throttle, g.throttle_key ? 1.0f : 0.0f, 1 / 12.0f, 1 / 8.0f);
    g.brake = ramp(g.brake, g.brake_key ? 1.0f : 0.0f, 1 / 8.0f, 1 / 8.0f);
    g.lean = ramp(g.lean, (float)g.lean_key, g.lean_rate, g.lean_rate * 2);
    memcpy(&hdr[0], &g.lean, 4);
    memcpy(&hdr[1], &g.throttle, 4);
    memcpy(&hdr[2], &g.brake, 4);
    for (i = 0; i < BUTTONS; i++)
        hdr[button_hdr[i]] = g.buttons >> i & 1;
    /* the same buttons as events (the game's default event mode, 0x893D480) */
    for (i = 0; i < BUTTONS; i++)
        if ((g.buttons ^ g.sent_buttons) >> i & 1) {
            uint8_t c = button_chip[i];

            put(g.buttons >> i & 1 ? 0x30 : 0x31, &c, 1);
            if (g.trace)
                api->core->log("speedrider: button %d %s", i, g.buttons >> i & 1 ? "down" : "up");
        }
    g.sent_buttons = g.buttons;
    if (g.helloed && g.coins != g.sent_coins) {
        int32_t v = g.coins;

        put(0x10, &v, 4);
        g.sent_coins = g.coins;
    }
}

static uint32_t sr_message(const uint8_t *in, uint32_t nin, uint32_t *hdr, uint8_t *out, uint32_t max)
{
    uint32_t i = 0, n = 0;

    while (in && i + 3 <= nin && in[i]) {
        uint8_t len = in[i], sum = 0;
        uint32_t k;

        if (len < 3 || i + len > nin)
            break;
        for (k = 0; k + 1 < len; k++)
            sum += in[i + k];
        if ((uint8_t)(sum | 0x80) != in[i + len - 1])
            api->core->log("speedrider: sub-message 0x%02x: bad sum", in[i + 1]);
        else {
            if (g.trace)
                api->core->log("speedrider: in 0x%02x, %d bytes", in[i + 1], len - 3);
            handle(in[i + 1], in + i + 2, len - 3);
        }
        i += len;
    }
    g.frames++;
    inputs(hdr);
    if (max > OUT_BYTES)
        max = OUT_BYTES;
    while (n < (uint32_t)g.qlen && n + g.queue[n] <= max)
        n += g.queue[n];
    memcpy(out, g.queue, n);
    memmove(g.queue, g.queue + n, g.qlen - n);
    g.qlen -= n;
    return n;
}

static void sr_reset(void)
{
    int i;

    for (i = 0; i < ZIPS; i++)
        if (arcs[i].f)
            fclose(arcs[i].f);
    memset(arcs, 0, sizeof(arcs));
    memset(&g, 0, sizeof(g));
    g.store_block = -1;
    g.sent_coins = -1;
    g.help = 1;
    g.trace = (int)api->core->env_long("SR_TRACE", 0);
    g.lean_rate = 1.0f / (float)api->core->env_long("SR_LEAN", 15);
    if (g.lean_rate <= 0 || g.lean_rate > 1)
        g.lean_rate = 1.0f / 15;
    nv_load();
}

/* ---- keys, overlay, title ----------------------------------------------------------- */

static void sr_key(int sym, int down)
{
    int b = -1;

    switch (sym) {
    case K_UP: g.throttle_key = down; return;
    case K_DOWN: g.brake_key = down; return;
    case K_LEFT: g.lean_key = down ? -1 : g.lean_key < 0 ? 0 : g.lean_key; return;
    case K_RIGHT: g.lean_key = down ? 1 : g.lean_key > 0 ? 0 : g.lean_key; return;
    case K_1: case K_RETURN: case K_SPACE: b = B_BOOST; break;
    case K_w: b = B_UP; break;
    case K_s: b = B_DOWN; break;
    case K_a: b = B_LEFT; break;
    case K_d: b = B_RIGHT; break;
    case K_x: b = B_SENSOR; break;
    case K_F2: b = B_TEST; break;
    case K_9: b = B_SERVICE; break;
    case K_5:
        if (down)
            g.coins++;
        return;
    case K_F1:
        if (down)
            g.help = !g.help;
        return;
    default: return;
    }
    if (down)
        g.buttons |= 1 << b;
    else
        g.buttons &= ~(1 << b);
}

static void sr_draw(int w, int h)
{
    static const char *lines[] = {
        "5 coin  1/Enter start, boost",
        "Up throttle  Down brake",
        "Left/Right lean",
        "W/S/A/D panel  F2 test  9 service",
        "F1 hide this",
    };
    const struct igs_core *c = api->core;
    int i, ty, n = 5;

    (void)w;
    if (!g.help)
        return;
    ty = h - 4 - n * c->font_h;
    c->fill(0, ty - 4, 35 * c->font_w, n * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < n; i++)
        c->text(6, ty + (n - 1 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

static void sr_status(char *buf, size_t n)
{
    snprintf(buf, n, "coins %d", g.coins);
}

static const struct e2000_game game = {
    .version = E2000_API_VERSION,
    .name = "speedrider",
    .reset = sr_reset,
    .key = sr_key,
    .draw = sr_draw,
    .status = sr_status,
    .transport = E2000_IGS036_MSG,
    /* sub_82D9B20: RC4 over "ILoveIGS" (8 bytes), re-keyed by sub-message 1 */
    .msg_key = "ILoveIGS",
    .msg_key_len = 8,
    .message = sr_message,
};

__attribute__((visibility("default")))
const struct e2000_game *e2000_game_entry(const struct e2000_api *a)
{
    api = a;
    return &game;
}
