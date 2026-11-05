/*
 * Speed Rider's own records and vehicle data: /rewrite/Ch{Daily,Weekly,Eternal}<n>.dat
 * (champion boards, the game's Archive class), /rewrite/Ranklvl<n>.dat (RankController),
 * /rewrite/Ghostlvl<n>.dat (GhostController), data/vehicle/<name>.dat (the physics
 * parameters, sub_82BA580).  Addresses in the assembled SR.  wiki Format-SRSAVE.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

typedef struct rd { igsa_ctx *c; const uint8_t *d; size_t n, o; int bad; } rd;

static int need(rd *r, size_t k, const char *what)
{
    if (r->bad)
        return 0;
    if (r->o + k > r->n || r->o + k < r->o) {
        igsa_fail(r->c, "%s at 0x%zx: %zu bytes past the end", what, r->o, r->o + k - r->n);
        r->bad = 1;
        return 0;
    }
    return 1;
}

static int32_t i32(rd *r, const char *what)
{
    int32_t v;

    if (!need(r, 4, what))
        return 0;
    v = (int32_t)igsa_le32(r->d + r->o);
    r->o += 4;
    return v;
}

static float f32(rd *r, const char *what)
{
    float v;

    if (!need(r, 4, what))
        return 0;
    v = igsa_lef(r->d + r->o);
    r->o += 4;
    return v;
}

static const char *base(const char *p)
{
    const char *a, *b;

    if (!p)
        return "";
    a = strrchr(p, '/');
    b = strrchr(p, '\\');
    if (b > a)
        a = b;
    return a ? a + 1 : p;
}

static int named(const char *path, const char *prefix)
{
    const char *b = base(path);
    size_t i;

    for (i = 0; prefix[i]; i++)
        if (!b[i] || (b[i] | 32) != (prefix[i] | 32))
            return 0;
    return 1;
}

/* Archive (sub_83127F0 / sub_83124D0): string = u32 length + bytes, int = i32 */
static void astr(rd *r, igsa_ctx *c, const char *key)
{
    uint32_t n = (uint32_t)i32(r, key);

    if (!need(r, n, key))
        return;
    igsa_jgbk(c, key, (const char *)r->d + r->o, strnlen((const char *)r->d + r->o, n));
    r->o += n;
}

static void alist(rd *r, igsa_ctx *c, const char *key)
{
    uint32_t n = (uint32_t)i32(r, key), i;

    if (!need(r, (size_t)n * 4, key))
        return;
    igsa_jarr(c, key);
    for (i = 0; i < n; i++)
        igsa_jint(c, NULL, i32(r, key));
    igsa_jend(c);
}

/* ---- Ch*.dat: champion boards ------------------------------------------------------------ */

/* the 112-byte record (sub_82AA960; defaults sub_82A9480) */
static void ch_record(rd *r, igsa_ctx *c, const char *key)
{
    static const int ints[] = { 56, 12, 60, 64, 8, 68, 72, 76 };
    char k[16];
    int i;

    igsa_jobj(c, key);
    astr(r, c, "name");
    astr(r, c, "text_4");
    astr(r, c, "text_52");
    for (i = 0; i < 8; i++) {
        snprintf(k, sizeof(k), "int_%d", ints[i]);
        igsa_jint(c, k, i32(r, k));
    }
    astr(r, c, "text_80");
    alist(r, c, "list_84");
    alist(r, c, "list_2");
    igsa_jarr(c, "int_16_44");
    for (i = 0; i < 8; i++)
        igsa_jint(c, NULL, i32(r, "int_16_44"));
    igsa_jend(c);
    igsa_jint(c, "int_48", i32(r, "int_48"));
    igsa_jend(c);
}

static int ch_probe(const uint8_t *d, size_t n, const char *path)
{
    uint32_t l;

    if (n < 16 || !(named(path, "ChDaily") || named(path, "ChWeekly") || named(path, "ChEternal")))
        return 0;
    l = igsa_le32(d + 4);
    return l < 64 && 8 + l <= n ? 80 : 0;
}

static int ch_parse(igsa_ctx *c)
{
    rd r = { c, c->d, c->n, 0, 0 };
    uint32_t i, cnt;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "srchamp");
    igsa_jint(c, "serial", i32(&r, "serial"));
    ch_record(&r, c, "champion");
    cnt = (uint32_t)i32(&r, "count");
    if (r.bad)
        return -1;
    if (cnt > 10000)
        return igsa_fail(c, "count %u", cnt);
    igsa_jarr(c, "entries");
    for (i = 0; i < cnt && !r.bad; i++)
        ch_record(&r, c, NULL);
    igsa_jend(c);
    if (r.bad)
        return -1;
    if (r.o != r.n)
        return igsa_fail(c, "%zu bytes after the records", r.n - r.o);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %u entries\n", base(c->path), "champ", cnt);
    c->entries++;
    return 0;
}

/* ---- Ranklvl / Ghostlvl: checksum, "RevisionL:" version ------------------------------------ */

static const char rev[] = "RevisionL:";

/* sub_8330610 / sub_83307E0: the sum of the bytes "encrypted" with r = (c + r) * 52845 + 22719 */
static uint32_t sr_sum(const uint8_t *p, size_t n)
{
    uint16_t k = 0xD971;
    uint32_t s = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        uint8_t x = (uint8_t)(p[i] ^ (k >> 8));

        k = (uint16_t)((x + k) * 0xCE6D + 0x58BF);
        s += x;
    }
    return s;
}

static int rev_probe(const uint8_t *d, size_t n, uint32_t ver)
{
    return n >= 18 && !memcmp(d + 4, rev, 10) && igsa_le32(d + 14) == ver && sr_sum(d + 4, n - 4) == igsa_le32(d) ? 100
                                                                                                                  : 0;
}

static int rank_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return rev_probe(d, n, 6);
}

static int ghost_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return rev_probe(d, n, 3);
}

/* sub_82A4300 */
static int rank_parse(igsa_ctx *c)
{
    rd r = { c, c->d, c->n, 18, 0 };
    static const char *const s[4] = { "name", "text_28", "text_32", "team" };
    int k = 0, i;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "srrank");
    igsa_jhex(c, "checksum", igsa_le32(c->d));
    igsa_jint(c, "version", 6);
    igsa_jarr(c, "entries");
    while (r.o < r.n && !r.bad) {
        igsa_jobj(c, NULL);
        igsa_jint(c, "place", ++k);
        igsa_jint(c, "int_0", i32(&r, "int_0"));
        igsa_juint(c, "date", (uint32_t)i32(&r, "date"));
        igsa_jint(c, "time_cs", i32(&r, "time"));
        igsa_jint(c, "int_12", i32(&r, "int_12"));
        igsa_jint(c, "int_16", i32(&r, "int_16"));
        igsa_jint(c, "int_20", i32(&r, "int_20"));
        for (i = 0; i < 4; i++)
            astr(&r, c, s[i]);
        igsa_jarr(c, "int_40_60");
        for (i = 0; i < 6; i++)
            igsa_jint(c, NULL, i32(&r, "int_40_60"));
        igsa_jend(c);
        alist(&r, c, "list_68");
        alist(&r, c, "list_80");
        igsa_jend(c);
    }
    igsa_jend(c);
    if (r.bad)
        return -1;
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %d entries\n", base(c->path), "rank", k);
    c->entries++;
    return 0;
}

/* GhostController: u32 count, 96-byte frames every 0.12 s (sub_8124300) */
static int ghost_parse(igsa_ctx *c)
{
    uint32_t cnt, i;
    int k;

    if (c->n < 22)
        return igsa_fail(c, "no frame count");
    cnt = igsa_le32(c->d + 18);
    if ((c->n - 22) / 96 < cnt || 22 + (size_t)cnt * 96 != c->n)
        return igsa_fail(c, "%u frames in %zu bytes", cnt, c->n - 22);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "srghost");
    igsa_jhex(c, "checksum", igsa_le32(c->d));
    igsa_jint(c, "version", 3);
    igsa_juint(c, "frames", cnt);
    igsa_jarr(c, "frame");
    for (i = 0; i < cnt; i++) {
        const uint8_t *f = c->d + 22 + 96 * (size_t)i;

        igsa_jobj(c, NULL);
        igsa_jint(c, "state", f[0]);
        igsa_jnum(c, "f4", igsa_lef(f + 4));
        igsa_jnum(c, "f8", igsa_lef(f + 8));
        igsa_jint(c, "i12", (int32_t)igsa_le32(f + 12));
        igsa_jint(c, "i16", (int32_t)igsa_le32(f + 16));
        igsa_jnum(c, "time", igsa_lef(f + 20));
        igsa_jnum(c, "f24", igsa_lef(f + 24));
        igsa_jnum(c, "angle_deg", igsa_lef(f + 28));
        igsa_jnum(c, "drift", igsa_lef(f + 32));
        igsa_jnum(c, "f36", igsa_lef(f + 36));
        igsa_jnum(c, "f40", igsa_lef(f + 40));
        igsa_jnum(c, "lean_deg", igsa_lef(f + 44));
        igsa_jnum(c, "f48", igsa_lef(f + 48));
        igsa_jnum(c, "suspension_pos", igsa_lef(f + 52));
        igsa_jnum(c, "suspension_ang", igsa_lef(f + 56));
        igsa_jarr(c, "look");
        for (k = 0; k < 3; k++)
            igsa_jnum(c, NULL, igsa_lef(f + 60 + 4 * k));
        igsa_jend(c);
        igsa_jarr(c, "v72");
        for (k = 0; k < 3; k++)
            igsa_jnum(c, NULL, igsa_lef(f + 72 + 4 * k));
        igsa_jend(c);
        igsa_jarr(c, "v84");
        for (k = 0; k < 3; k++)
            igsa_jnum(c, NULL, igsa_lef(f + 84 + 4 * k));
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %u frames\n", base(c->path), "ghost", cnt);
    c->entries++;
    return 0;
}

/* ---- data/vehicle/<name>.dat (sub_82BA580): scalars, counted arrays, lookup tables ------------ */

static void floats(rd *r, igsa_ctx *c, const char *key, int n)
{
    igsa_jarr(c, key);
    while (n-- > 0 && !r->bad)
        igsa_jnum(c, NULL, f32(r, key));
    igsa_jend(c);
}

/* LookupTable (sub_82B4600): u32 points (1..999), f32 x min, f32 x max, f32 scale, points f32 */
static void table(rd *r, igsa_ctx *c)
{
    uint32_t n = (uint32_t)i32(r, "table");

    if (r->bad)
        return;
    if (n - 1 > 998) {
        igsa_fail(c, "table at 0x%zx: %u points", r->o - 4, n);
        r->bad = 1;
        return;
    }
    igsa_jobj(c, NULL);
    igsa_juint(c, "points", n);
    igsa_jnum(c, "x_min", f32(r, "table"));
    igsa_jnum(c, "x_max", f32(r, "table"));
    igsa_jnum(c, "scale", f32(r, "table"));
    floats(r, c, "y", (int)n);
    igsa_jend(c);
}

static int veh_check(const uint8_t *d, size_t n, size_t *end)
{
    size_t o = 53 * 4 + 3 + 70 * 4, s = 0;
    int i, k;

    if (n < o + 32 || !memcmp(d, "DDS ", 4) || d[212] > 1 || d[213] > 1 || d[214] > 1)
        return 0;
    for (i = 0; i < 53; i++)
        if ((igsa_le32(d + 4 * i) >> 23 & 0xFF) == 0xFF)
            return 0;               /* NaN or infinity */
    for (i = 0; i < 8; i++) {
        uint32_t v = igsa_le32(d + o + 4 * i);

        if (v > 1000)
            return 0;
        s += v;
    }
    o += 32 + 4 * s + 13 * 4;
    for (k = 0; k < 3; k++)
        for (i = 0; i < (k == 2 ? 10 : 8); i++) {
            uint32_t v;

            if (o + 16 > n || (v = igsa_le32(d + o)) - 1 > 998)
                return 0;
            o += 16 + 4 * (size_t)v;
            if (i == 7 && k < 2)
                o += k ? 8 : 48;
        }
    if (o > n)
        return 0;
    *end = o;
    return 1;
}

static int veh_probe(const uint8_t *d, size_t n, const char *path)
{
    size_t e;

    (void)path;
    return veh_check(d, n, &e) ? 70 : 0;
}

static int veh_parse(igsa_ctx *c)
{
    rd r = { c, c->d, c->n, 0, 0 };
    int32_t cnt[8];
    int i;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "srvehicle");
    /* units as stored: speeds km/h (the game divides by 3.6), angles degrees, wheel radius inches */
    floats(&r, c, "body", 10);              /* +1020 +1024 mass parts, +992..+1012 sizes, +1104 wheel (in), +1028 */
    floats(&r, c, "f1252", 3);
    floats(&r, c, "gear", 4);               /* first gear, gear count, +668, +1108 */
    floats(&r, c, "gear_speed_kmh", 5);
    floats(&r, c, "top_speed_kmh", 1);
    floats(&r, c, "gear_ratio", 6);
    floats(&r, c, "gear_x", 6);
    floats(&r, c, "f1116", 3);
    floats(&r, c, "f1140", 5);
    floats(&r, c, "f376", 1);
    floats(&r, c, "steer", 5);              /* +1172 +1176 +1168 (deg) +1180 +1160 */
    floats(&r, c, "f468", 4);
    if (need(&r, 3, "flags")) {
        igsa_jarr(c, "flags");
        for (i = 0; i < 3; i++)
            igsa_jint(c, NULL, r.d[r.o + i]);
        igsa_jend(c);
        r.o += 3;
    }
    floats(&r, c, "engine", 12);
    floats(&r, c, "suspension", 7);
    floats(&r, c, "susp_a", 5);
    floats(&r, c, "susp_b", 5);
    floats(&r, c, "drift", 15);             /* +1300 (%), speeds (km/h), angles (deg) */
    floats(&r, c, "f920", 8);
    floats(&r, c, "f336", 4);
    floats(&r, c, "f1212", 5);
    floats(&r, c, "f100", 3);
    floats(&r, c, "f1120", 6);
    igsa_jarr(c, "arrays");
    for (i = 0; i < 8; i++)
        cnt[i] = i32(&r, "array count");
    for (i = 0; i < 8 && !r.bad; i++) {
        if (cnt[i] < 0 || cnt[i] > 1000) {
            igsa_jend(c);
            return igsa_fail(c, "array %d: %d values", i, cnt[i]);
        }
        floats(&r, c, NULL, cnt[i]);
    }
    igsa_jend(c);
    floats(&r, c, "table_args", 13);
    igsa_jarr(c, "tables");
    for (i = 0; i < 8; i++)
        table(&r, c);
    igsa_jend(c);
    floats(&r, c, "f720", 6);
    floats(&r, c, "f724", 6);
    igsa_jarr(c, "tables2");
    for (i = 0; i < 8; i++)
        table(&r, c);
    igsa_jend(c);
    floats(&r, c, "f1164", 2);
    igsa_jarr(c, "tables3");
    for (i = 0; i < 10; i++)
        table(&r, c);
    igsa_jend(c);
    if (r.bad)
        return -1;
    igsa_juint(c, "unread_tail", r.n - r.o);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s 26 lookup tables\n", base(c->path), "veh");
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_srchamp = { "srchamp", "Speed Rider Ch{Daily,Weekly,Eternal}*.dat: champion boards", ch_probe,
                                       ch_parse };
const igsa_format igsa_fmt_srrank = { "srrank", "Speed Rider Ranklvl*.dat: rankings", rank_probe, rank_parse };
const igsa_format igsa_fmt_srghost = { "srghost", "Speed Rider Ghostlvl*.dat: ghost runs", ghost_probe, ghost_parse };
const igsa_format igsa_fmt_srvehicle = { "srvehicle", "Speed Rider data/vehicle/<name>.dat: vehicle physics", veh_probe, veh_parse };
