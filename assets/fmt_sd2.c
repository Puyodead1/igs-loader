/*
 * Speed Driver 2's own data files (addresses in the assembled sd2):
 *   *.rpd       replays (sub_81267E8, played by sub_8125ECE, recorded by sub_8125892)
 *   car/<id>/<ID>.dat  car physics (sub_812CE2C)
 *   *.aip, *.aii, *.ail   AI paths, AI info, AI lackey lines (sub_82FE114, sub_82FD0B4, sub_8301012)
 *   /data/<name>.bin   settings, bookkeeping, ranking (sub_807DE2C, sub_8076144, ...)
 * wiki Format-SD2RPD, Format-SD2CAR, Format-SD2AI, Format-SD2BIN.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static const char *base_of(const char *p)
{
    const char *a = p ? strrchr(p, '/') : NULL, *b = p ? strrchr(p, '\\') : NULL;

    if (b > a)
        a = b;
    return a ? a + 1 : p ? p : "";
}

static const char *ext_of(const char *p)
{
    const char *b = base_of(p), *e = strrchr(b, '.');

    return e ? e : "";
}

static void jf32s(igsa_ctx *c, const char *key, const uint8_t *p, int n)
{
    int i;

    igsa_jarr(c, key);
    for (i = 0; i < n; i++)
        igsa_jnum(c, NULL, igsa_lef(p + 4 * i));
    igsa_jend(c);
}

/* ---- replays ---------------------------------------------------------------------------------- */

#define RPD_SIZE 277224
#define RPD_FRAMES 3300

/* the replay struct's arrays, file offset = struct offset - 88 */
static const struct { const char *name; uint32_t off; int kind; } rpd_arrays[] = {
    { "position", 20, 3 },              /* car position (g_raRicer[].pos) */
    { "front", 39620, 3 },              /* car heading vector */
    { "up", 79220, 3 },                 /* car up vector */
    { "body_pitch", 118820, 1 },        /* body node 0, about X */
    { "shake_pitch", 132020, 1 },       /* body node 1 jitter, about X (random range) */
    { "shake_roll", 145220, 1 },        /* body node 1 jitter, about Z (x body_factor) */
    { "body_roll", 158420, 1 },         /* body node 1, about Z (x body_factor) */
    { "body_pitch2", 171620, 1 },       /* body node 1, about X */
    { "used", 184820, 0 },              /* 1 while the frame holds data */
    { "time", 188120, 1 },              /* the race clock, seconds */
    { "flag_84", 201320, 0 },
    { "flag_87", 204624, 0 },
    { "flag_86", 207924, 0 },
    { "steer_left", 211224, 1 },        /* front wheel nodes 2, 3: yaw */
    { "steer_right", 224424, 1 },
    { "speed", 237624, 1 },             /* m/s (x 3.6 the speedometer); wheel nodes 14..17 spin */
    { "body_factor", 250824, 1 },       /* multiplies the body roll / shake */
    { "extra", 264024, 1 },
};

static int rpd_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == RPD_SIZE ? (!memcmp(d, "IGS", 3) || !strcasecmp(ext_of(path), ".rpd") ? 80 : 30) : 0;
}

static int rpd_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    static const char *const opt[6] = { "type", "tyre", "bigbag", "tail", "texture", "light" };
    int i, frames = 0;
    size_t k, f;

    if (c->n != RPD_SIZE)
        return igsa_fail(c, "replay: %zu bytes, %d expected", c->n, RPD_SIZE);
    for (f = 0; f < RPD_FRAMES && d[184820 + f]; f++)
        frames++;
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Speed Driver 2 replay");
    /* "IGS" marks a stock replay: the game then keeps the player's car and refits */
    igsa_jbool(c, "stock", !memcmp(d, "IGS", 3));
    igsa_jbytes(c, "head", d, 8);
    igsa_jint(c, "car", (int32_t)igsa_le32(d + 8));
    igsa_jobj(c, "refit");
    for (i = 0; i < 6; i++)
        igsa_jint(c, opt[i], d[12 + i]);
    igsa_jend(c);
    igsa_jbytes(c, "pad", d + 18, 2);
    igsa_jint(c, "frames", frames);
    igsa_jbytes(c, "pad_204620", d + 204620, 4);
    igsa_jarr(c, "frame");
    for (f = 0; f < (size_t)frames; f++) {
        igsa_jobj(c, NULL);
        for (k = 0; k < sizeof(rpd_arrays) / sizeof(rpd_arrays[0]); k++) {
            const uint8_t *p;

            if (!strcmp(rpd_arrays[k].name, "used"))
                continue;
            switch (rpd_arrays[k].kind) {
            case 3:
                jf32s(c, rpd_arrays[k].name, d + rpd_arrays[k].off + 12 * f, 3);
                break;
            case 1:
                igsa_jnum(c, rpd_arrays[k].name, igsa_lef(d + rpd_arrays[k].off + 4 * f));
                break;
            default:
                p = d + rpd_arrays[k].off + f;
                igsa_jint(c, rpd_arrays[k].name, *p);
            }
        }
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_sd2rpd = { "sd2rpd", "Speed Driver 2 replay (.rpd): 3300 frames of car pose and body state",
                                      rpd_probe, rpd_parse };

/* ---- car physics ------------------------------------------------------------------------------ */

#define CAR_HEAD 0x314
#define CAR_CURVES 31

/* 31 curves {u32 samples, u32 -, f32 range, f32 param} + samples floats after the 788-byte block */
static size_t car_walk(const uint8_t *d, size_t n)
{
    size_t o = CAR_HEAD + 4;
    int k;

    if (n < o || igsa_le32(d + CAR_HEAD) != CAR_CURVES)
        return 0;
    for (k = 0; k < CAR_CURVES; k++) {
        uint32_t s;

        if (o + 16 > n || !(s = igsa_le32(d + o)) || s > 256)
            return 0;
        o += 16 + 4 * (size_t)s;
        if (o > n)
            return 0;
    }
    return o;
}

static int car_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n && car_walk(d, n) == n ? 90 : 0;
}

static int car_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t o = CAR_HEAD + 4, e = car_walk(d, c->n);
    int k, i;

    if (e != c->n)
        return igsa_fail(c, "car data: curves do not end at the file's end");
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Speed Driver 2 car physics");
    /* the 788-byte block: 197 words (floats; the curve table's words at 168.. are its runtime copy) */
    igsa_jarr(c, "params");
    for (i = 0; i < CAR_HEAD / 4; i++) {
        igsa_jobj(c, NULL);
        igsa_jint(c, "offset", 4 * i);
        igsa_jnum(c, "f32", igsa_lef(d + 4 * i));
        igsa_jint(c, "i32", (int32_t)igsa_le32(d + 4 * i));
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_juint(c, "curve_count", igsa_le32(d + CAR_HEAD));
    igsa_jarr(c, "curves");
    for (k = 0; k < CAR_CURVES; k++) {
        uint32_t s = igsa_le32(d + o);

        igsa_jobj(c, NULL);
        igsa_jint(c, "index", k);
        igsa_juint(c, "samples", s);
        igsa_juint(c, "reserved", igsa_le32(d + o + 4));
        igsa_jnum(c, "range", igsa_lef(d + o + 8));
        igsa_jnum(c, "param", igsa_lef(d + o + 12));
        jf32s(c, "values", d + o + 16, (int)s);
        igsa_jend(c);
        o += 16 + 4 * (size_t)s;
    }
    igsa_jend(c);
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_sd2car = { "sd2car", "Speed Driver 2 car physics (car/*.dat): parameters and 31 curves",
                                      car_probe, car_parse };

/* ---- AI paths, info, lackey lines ---------------------------------------------------------------- */

/*
 * f32 version, u32 head size (from offset 12), u32 record size, u32 count,
 * [.aip: u8 flag]; records at 12 + head size.  Fields newer than the file's
 * version are read but not kept (the loader compares with a 0.001 margin).
 */
enum { AI_PATH, AI_INFO, AI_LINE };

static int ai_kind(const uint8_t *d, size_t n, const char *path)
{
    float v;
    uint32_t h, r, k;
    const char *e = ext_of(path);

    if (n < 16)
        return -1;
    v = igsa_lef(d);
    h = igsa_le32(d + 4);
    r = igsa_le32(d + 8);
    k = igsa_le32(d + 12);
    if (!(v > 0.1f && v < 2.0f) || h < 5 || h > 4096 || r > 4096 || 12ull + h + (uint64_t)r * k != n)
        return -1;
    if (!strcasecmp(e, ".aip"))
        return r >= 45 ? AI_PATH : -1;
    if (!strcasecmp(e, ".aii"))
        return r >= 73 ? AI_INFO : -1;
    if (!strcasecmp(e, ".ail"))
        return r >= 28 ? AI_LINE : -1;
    return -1;
}

static int ai_probe(const uint8_t *d, size_t n, const char *path)
{
    return ai_kind(d, n, path) >= 0 ? 85 : 0;
}

static int ver_ge(float v, float t) { return !(t - v > 0.001f); }

static int ai_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    int kind = ai_kind(d, c->n, c->path);
    float v;
    uint32_t h, r, k, i;
    static const float maxv[3] = { 0.54f, 0.44f, 0.45f };
    static const char *const names[3] = { "AI path (.aip)", "AI info (.aii)", "AI lackey line (.ail)" };

    if (kind < 0)
        return igsa_fail(c, "AI data: header and size do not agree");
    v = igsa_lef(d);
    h = igsa_le32(d + 4);
    r = igsa_le32(d + 8);
    k = igsa_le32(d + 12);
    if (v - maxv[kind] > 0.001f)
        return igsa_fail(c, "version %.2f newer than the loader's %.2f", v, maxv[kind]);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", names[kind]);
    igsa_jnum(c, "version", v);
    igsa_juint(c, "head_size", h);
    igsa_juint(c, "record_size", r);
    igsa_juint(c, "records", k);
    if (kind == AI_PATH)
        igsa_jint(c, "flag", d[16]);
    igsa_jarr(c, "record");
    for (i = 0; i < k; i++) {
        const uint8_t *p = d + 12 + h + (size_t)r * i;
        size_t used;

        igsa_jobj(c, NULL);
        jf32s(c, "position", p, 3);
        if (kind == AI_PATH) {      /* sub_8301ED0 .. sub_83020B2 */
            igsa_jnum(c, "f12", igsa_lef(p + 12));
            igsa_jnum(c, "f16", igsa_lef(p + 16));
            igsa_jnum(c, "f20", igsa_lef(p + 20));
            igsa_jnum(c, "f24", igsa_lef(p + 24));
            igsa_jnum(c, "f28", igsa_lef(p + 28));
            if (ver_ge(v, 0.420704f))
                igsa_jint(c, "b32", p[32]);
            if (ver_ge(v, 0.43f)) {
                igsa_jnum(c, "f33", igsa_lef(p + 33));
                igsa_jnum(c, "f37", igsa_lef(p + 37));
            }
            if (ver_ge(v, 0.44f))
                igsa_jnum(c, "f41", igsa_lef(p + 41));
            used = 45;
            if (ver_ge(v, 0.54f)) {
                jf32s(c, "vec45", p + 45, 3);
                jf32s(c, "vec57", p + 57, 3);
                used = 69;
            }
        } else if (kind == AI_INFO) {   /* sub_83023CA .. sub_83024BE */
            igsa_jint(c, "b12", p[12]);
            igsa_jnum(c, "f13", igsa_lef(p + 13));
            igsa_jnum(c, "f17", igsa_lef(p + 17));
            igsa_jnum(c, "f21", igsa_lef(p + 21));
            igsa_jnum(c, "f25", igsa_lef(p + 25));
            igsa_jnum(c, "f29", igsa_lef(p + 29));
            igsa_jnum(c, "f33", igsa_lef(p + 33));
            igsa_jint(c, "i37", (int32_t)igsa_le32(p + 37));
            igsa_jint(c, "i41", (int32_t)igsa_le32(p + 41));
            igsa_jint(c, "i45", (int32_t)igsa_le32(p + 45));
            igsa_jarr(c, "i49");
            for (int j = 0; j < 5; j++)
                igsa_jint(c, NULL, (int32_t)igsa_le32(p + 49 + 4 * j));
            igsa_jend(c);
            if (ver_ge(v, 0.44f))
                igsa_jint(c, "i69", (int32_t)igsa_le32(p + 69));
            used = 73;
        } else {                        /* sub_83025A8 .. sub_8302624 */
            igsa_jnum(c, "f12", igsa_lef(p + 12));
            igsa_jnum(c, "f16", igsa_lef(p + 16));
            if (ver_ge(v, 0.44f))
                igsa_jnum(c, "f20", igsa_lef(p + 20));
            if (ver_ge(v, 0.45f))
                igsa_jnum(c, "f24", igsa_lef(p + 24));
            used = 28;
        }
        if (used > r)
            return igsa_fail(c, "record %u: %zu bytes read, records are %u", i, used, r);
        for (size_t z = used; z < r; z++)
            if (p[z]) {
                igsa_jbytes(c, "rest", p + used, r - used);    /* skipped by the loader */
                break;
            }
        igsa_jend(c);
    }
    igsa_jend(c);
    if (h > 5 || kind != AI_PATH)
        igsa_jbytes(c, "head_rest", d + (kind == AI_PATH ? 17 : 16), 12 + h - (kind == AI_PATH ? 17 : 16));
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_sd2ai = { "sd2ai", "Speed Driver 2 AI data (.aip paths, .aii info, .ail lackey lines)",
                                     ai_probe, ai_parse };

/* ---- /data/<name>.bin ------------------------------------------------------------------------------- */

enum { B_BOOK, B_GAMEDATA, B_GAMESETUP, B_CONTROL, B_LINKSETUP, B_RANK, B_STORESETUP };

static const struct { const char *name; size_t size; } bins[] = {
    { "book.bin", 876 }, { "gamedata.bin", 2140 }, { "gamesetup.bin", 32 }, { "control.bin", 28 },
    { "linksetup.bin", 116 }, { "rank.bin", 16200 }, { "storesetup.bin", 40 },
};

/* fixed-size records with nothing to check: the file name picks them */
static int bin_kind(size_t n, const char *path)
{
    int i;

    for (i = 0; i < (int)(sizeof(bins) / sizeof(bins[0])); i++)
        if (!strcasecmp(base_of(path), bins[i].name) && n == bins[i].size)
            return i;
    return -1;
}

static int bin_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)d;
    return path && bin_kind(n, path) >= 0 ? 60 : 0;
}

static void jstr_n(igsa_ctx *c, const char *key, const uint8_t *p, size_t n)
{
    size_t l = 0;

    while (l < n && p[l])
        l++;
    igsa_jgbk(c, key, (const char *)p, l);
}

static void ju32s(igsa_ctx *c, const char *key, const uint8_t *p, int n)
{
    int i;

    igsa_jarr(c, key);
    for (i = 0; i < n; i++)
        igsa_jint(c, NULL, (int32_t)igsa_le32(p + 4 * i));
    igsa_jend(c);
}

static int bin_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    int k = bin_kind(c->n, c->path), i, j;

    if (k < 0)
        return igsa_fail(c, "%s: %zu bytes, not a Speed Driver 2 settings file", base_of(c->path), c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", bins[k].name);
    switch (k) {
    case B_BOOK:            /* sub_807DE2C: 31 + 12 records of 20 bytes, then the totals */
        igsa_jarr(c, "records");
        for (i = 0; i < 31; i++)
            ju32s(c, NULL, d + 20 * i, 5);
        igsa_jend(c);
        igsa_jarr(c, "records2");
        for (i = 0; i < 12; i++)
            ju32s(c, NULL, d + 620 + 20 * i, 5);
        igsa_jend(c);
        igsa_jint(c, "byte_860", d[860]);
        igsa_jint(c, "byte_861", d[861]);
        igsa_juint(c, "u32_862", igsa_le32(d + 862));
        igsa_juint(c, "u32_866", igsa_le32(d + 866));
        igsa_juint(c, "u32_870", igsa_le32(d + 870));
        igsa_juint(c, "u16_874", igsa_le16(d + 874));
        break;
    case B_GAMEDATA:        /* book_vGameDataWriteToFile sub_807E19E: 0x85C bytes */
        ju32s(c, "words", d, 535);
        break;
    case B_GAMESETUP:       /* sub_8079AB6: 8 words; word 7 the wide-screen flag (message 0x3A) */
        ju32s(c, "words", d, 7);
        igsa_jint(c, "wide_screen", (int32_t)igsa_le32(d + 28));
        break;
    case B_CONTROL:         /* sub_80838C6: 7 words sent to the IGS036 (message 0xF1, vPci_SetDriveValueTo27) */
        ju32s(c, "drive_values", d, 7);
        break;
    case B_LINKSETUP:       /* sub_807B302: the network settings */
        igsa_jint(c, "byte_0", d[0]);
        jstr_n(c, "ip", d + 1, 16);
        jstr_n(c, "netmask", d + 17, 16);
        jstr_n(c, "gateway", d + 33, 16);
        jstr_n(c, "string_49", d + 49, 16);
        igsa_jbytes(c, "rest", d + 65, 51);
        break;
    case B_RANK:            /* m_vRankDataInit sub_8076144: 15 courses x 30 records of 36 bytes */
        igsa_jarr(c, "courses");
        for (i = 0; i < 15; i++) {
            igsa_jarr(c, NULL);
            for (j = 0; j < 30; j++) {
                const uint8_t *p = d + 1080 * i + 36 * j;   /* 30 records of 36 per course */

                igsa_jobj(c, NULL);
                jstr_n(c, "tag", p, 3);
                igsa_jbytes(c, "pad", p + 3, 5);
                igsa_jint(c, "time", (int32_t)igsa_le32(p + 8));
                igsa_jint(c, "car", p[12]);
                igsa_jint(c, "gearbox", p[13]);
                igsa_jint(c, "store", p[14]);
                jstr_n(c, "name", p + 15, 7);
                igsa_jint(c, "byte_22", p[22]);
                jstr_n(c, "store_name", p + 23, 10);
                igsa_jbytes(c, "pad2", p + 33, 3);
                igsa_jend(c);
            }
            igsa_jend(c);
        }
        igsa_jend(c);
        break;
    case B_STORESETUP:      /* sub_808A22C; message 0x39 sub_8114B88 */
        igsa_jint(c, "store", (int32_t)igsa_le32(d));
        ju32s(c, "words_4", d + 4, 3);
        igsa_jint(c, "store_name_index", (int32_t)igsa_le32(d + 16));   /* 8: the name below */
        jstr_n(c, "store_name", d + 20, 10);
        igsa_jbytes(c, "pad_30", d + 30, 2);
        igsa_jbool(c, "free_play", d[32]);
        igsa_jarr(c, "start_coin");
        igsa_jint(c, NULL, d[33]);
        igsa_jint(c, NULL, d[34]);
        igsa_jend(c);
        igsa_jarr(c, "continue_coin");
        igsa_jint(c, NULL, d[35]);
        igsa_jint(c, NULL, d[36]);
        igsa_jend(c);
        igsa_jbytes(c, "pad_37", d + 37, 3);
        break;
    }
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_sd2bin = { "sd2bin", "Speed Driver 2 /data/*.bin: settings, bookkeeping, ranking", bin_probe,
                                      bin_parse };
