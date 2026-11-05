/*
 * Panda Family data files (PF, stripped).  wiki Format-PFRANK, Format-PFVOL,
 * Format-PFSTG.
 *
 * pf_data/rank/rank.bin: 32 x 16 bytes, stage (16) x {normal, plus}: the
 *   best result per stage.  Load sub_80E0280, save sub_80E0120, defaults
 *   sub_80E0470 (table 0x81A60C0), update Rank_vUpdate sub_80E05D0, photos
 *   (pf_data/rank/S<n>_1.bmp) sub_80DDE50.
 * pf_data/volume.desc: "<sound file> <volume>" lines; VolDesc_LoadDescriptor
 *   sub_80700C0, _SaveDescriptor sub_8070E80 (sys/c/voldesc.c), path from
 *   voldesc.ini DescriptorFilePath.
 * main/pf_stg*.csv: stage layouts, Script_blPFStageLoadFile sub_8074530:
 *   a header line, then rows of 14 ints: split point, height, 4 lanes x
 *   {obstacle, item, enemy}.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static const char *base(const char *path)
{
    const char *b = path, *p;

    if (!path)
        return "";
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

/* ---- rank.bin -------------------------------------------------------------------------------- */

static int rank_valid(const uint8_t *d, size_t n)
{
    size_t i;

    if (n != 512)
        return 0;
    for (i = 0; i < n; i += 16)
        if (d[i] > 1 || d[i + 1] > 31 || d[i + 3] > 8 || d[i + 4] > 1 || igsa_le32(d + i + 8) > 999 ||
            igsa_le32(d + i + 12) != 0xFFFFFFFFu || ((d[i + 2] % 20) > 6) || d[i + 2] > 26)
            return 0;
    return 1;
}

static int rank_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return rank_valid(d, n) ? 90 : 0;
}

static int rank_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    int i;

    if (!rank_valid(d, c->n))
        return igsa_fail(c, "%zu bytes: not 32 rank records (fields out of the ranges Rank_vUpdate writes)", c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Panda Family rank");
    igsa_jarr(c, "ranks");
    for (i = 0; i < 32; i++) {
        const uint8_t *r = d + 16 * i;

        igsa_jobj(c, NULL);
        igsa_jint(c, "stage", i % 16 + 1);
        igsa_jbool(c, "plus", i >= 16);
        igsa_jbool(c, "photo", r[0]);
        igsa_jint(c, "stage_byte", r[1]);
        igsa_jint(c, "title", r[2] % 20);
        igsa_jbool(c, "single", r[2] < 20);
        igsa_jint(c, "face", r[3]);
        igsa_jint(c, "player", r[4]);
        igsa_jbytes(c, "pad", r + 5, 3);
        igsa_juint(c, "score", igsa_le32(r + 8));
        igsa_jint(c, "reserved", (int32_t)igsa_le32(r + 12));
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

/* ---- volume.desc ----------------------------------------------------------------------------- */

/* one "<name> <volume>\n" line: name without blanks (73 chars max: 74-byte field), volume 0..255 */
static int vol_line(const uint8_t *p, size_t n, size_t *nl, size_t *vo, long *vol)
{
    size_t i = 0, sp;
    char num[8];

    while (i < n && p[i] > ' ')
        i++;
    if (!i || i > 73 || i >= n || p[i] != ' ')
        return 0;
    sp = i++;
    while (i < n && p[i] >= '0' && p[i] <= '9' && i - sp <= 4)
        i++;
    if (i == sp + 1 || i - sp > 4 || i >= n || p[i] != '\n')
        return 0;
    memcpy(num, p + sp + 1, i - sp - 1);
    num[i - sp - 1] = 0;
    *vol = strtol(num, NULL, 10);
    *nl = sp;
    *vo = i + 1;
    return *vol <= 255;
}

static int vol_valid(const uint8_t *d, size_t n, size_t *bad)
{
    size_t o = 0, nl, len;
    long v;

    if (!n)
        return 0;
    while (o < n) {
        if (!vol_line(d + o, n - o, &nl, &len, &v)) {
            if (bad)
                *bad = o;
            return 0;
        }
        o += len;
    }
    return 1;
}

static int vol_probe(const uint8_t *d, size_t n, const char *path)
{
    const char *b = base(path);
    size_t l = strlen(b);

    if (!vol_valid(d, n, NULL))
        return 0;
    return l > 5 && !strcmp(b + l - 5, ".desc") ? 90 : 30;
}

static int vol_parse(igsa_ctx *c)
{
    size_t o = 0, nl, len, bad = 0;
    long v;

    if (!vol_valid(c->d, c->n, &bad))
        return igsa_fail(c, "line at 0x%zx: not \"<name> <volume 0..255>\\n\"", bad);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Panda Family volume.desc");
    igsa_jarr(c, "sounds");
    while (o < c->n) {
        vol_line(c->d + o, c->n - o, &nl, &len, &v);
        igsa_jobj(c, NULL);
        igsa_jstrn(c, "file", (const char *)c->d + o, nl);
        igsa_jint(c, "volume", v);
        igsa_jend(c);
        c->entries++;
        o += len;
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

/* ---- pf_stg*.csv ----------------------------------------------------------------------------- */

/* Big5 分段點 (split point), the header's first column */
static const uint8_t stg_head[] = { 0xA4, 0xC0, 0xAC, 0x71, 0xC2, 0x49, ',' };

/* a row: 14 comma-separated decimal ints, "\n" or "\r\n" */
static int stg_row(const uint8_t *p, size_t n, long v[14], size_t *len)
{
    size_t i = 0;
    int k;

    for (k = 0; k < 14; k++) {
        size_t s = i;
        int neg = 0;

        if (i < n && p[i] == '-')
            neg = 1, i++;
        v[k] = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9' && i - s < 9)
            v[k] = v[k] * 10 + (p[i++] - '0');
        if (i == s + (size_t)neg)
            return 0;
        if (neg)
            v[k] = -v[k];
        if (k < 13) {
            if (i >= n || p[i] != ',')
                return 0;
            i++;
        }
    }
    if (i < n && p[i] == '\r')
        i++;
    if (i < n && p[i] == '\n')
        i++;
    else if (i < n)
        return 0;
    *len = i;
    return 1;
}

static int stg_valid(const uint8_t *d, size_t n, size_t *body, size_t *bad)
{
    size_t o = 0, len;
    long v[14];
    int commas = 0;

    if (n < sizeof(stg_head) || memcmp(d, stg_head, sizeof(stg_head)))
        return 0;
    while (o < n && d[o] != '\n')
        commas += d[o++] == ',';
    if (o >= n || commas != 13)
        return 0;
    *body = ++o;
    while (o < n) {
        if (!stg_row(d + o, n - o, v, &len)) {
            if (bad)
                *bad = o;
            return 0;
        }
        o += len;
    }
    return 1;
}

static int stg_probe(const uint8_t *d, size_t n, const char *path)
{
    size_t b;

    (void)path;
    return stg_valid(d, n, &b, NULL) ? 90 : 0;
}

static const char *const lanef[] = { "obstacle", "item", "enemy" };

static int stg_parse(igsa_ctx *c)
{
    size_t o, len, bad = 0, h0, h;
    long v[14];
    int k, rows = 0, splits = 0, obj = 0;

    if (!stg_valid(c->d, c->n, &o, &bad)) {
        if (bad)
            return igsa_fail(c, "row at 0x%zx: not 14 comma-separated ints", bad);
        return igsa_fail(c, "no split-point header line with 14 columns");
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Panda Family stage CSV");
    /* the header line (Big5), split into columns */
    igsa_jarr(c, "header");
    for (h0 = h = 0; h < o; h++)
        if (c->d[h] == ',' || c->d[h] == '\n' || c->d[h] == '\r') {
            igsa_jbig5(c, NULL, (const char *)c->d + h0, h - h0);
            if (c->d[h] == '\r')
                h++;
            h0 = h + 1;
        }
    igsa_jend(c);
    igsa_jarr(c, "rows");
    while (o < c->n) {
        stg_row(c->d + o, c->n - o, v, &len);
        o += len;
        rows++;
        splits += v[0] > 0;
        for (k = 2; k < 14; k++)
            obj += v[k] > 0;
        if (v[0] <= 0 && !v[2] && !v[3] && !v[4] && !v[5] && !v[6] && !v[7] && !v[8] && !v[9] && !v[10] &&
            !v[11] && !v[12] && !v[13])
            continue;       /* the loader takes only the non-zero cells; empty rows are left out */
        igsa_jobj(c, NULL);
        igsa_jint(c, "row", rows);
        igsa_jint(c, "split", v[0]);
        igsa_jint(c, "height", v[1]);
        igsa_jarr(c, "lanes");
        for (k = 0; k < 4; k++) {
            int f;

            igsa_jobj(c, NULL);
            igsa_jint(c, "lane", k);
            for (f = 0; f < 3; f++)
                igsa_jint(c, lanef[f], v[2 + 3 * k + f]);
            igsa_jend(c);
        }
        igsa_jend(c);
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
    igsa_jint(c, "row_count", rows);
    igsa_jint(c, "split_points", splits);
    igsa_jint(c, "objects", obj);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_pfrank = { "pfrank", "Panda Family rank.bin: best result per stage", rank_probe, rank_parse };
const igsa_format igsa_fmt_pfvol = { "pfvol", "Panda Family volume.desc: per-sound volumes", vol_probe, vol_parse };
const igsa_format igsa_fmt_pfstg = { "pfstg", "Panda Family stage CSV: split points, heights, lane objects", stg_probe, stg_parse };
