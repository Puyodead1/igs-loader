/*
 * Speed Rider scene scripts: script/ground/.../<name>.desc (scene description, sub_8330DD0),
 * the .info it names (trigger table), script/he/<name>.he (the collision half-edge mesh,
 * sub_82D7670), and ui/tex/output/2dtexture.inf (the UI texture atlas,
 * sub_84216C0).  Addresses in the assembled SR.  wiki Format-SRSCENE.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

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

static int ext(const char *path, const char *e)
{
    const char *b = base(path), *x = strrchr(b, '.');
    size_t i;

    if (!x)
        return 0;
    for (i = 0; e[i] || x[i]; i++)
        if ((x[i] | 32) != (e[i] | 32))
            return 0;
    return 1;
}

/* the next line [o, end) without CR/LF */
static int line(const uint8_t *d, size_t n, size_t *o, size_t *s, size_t *e)
{
    if (*o >= n)
        return 0;
    *s = *o;
    while (*o < n && d[*o] != '\n')
        (*o)++;
    *e = *o;
    if (*o < n)
        (*o)++;
    while (*e > *s && d[*e - 1] == '\r')
        (*e)--;
    return 1;
}

/* "8-4-4-4-12@<number>": the scene / layout ids */
static int is_id(const uint8_t *p, size_t n)
{
    static const int dash[4] = { 8, 13, 18, 23 };
    size_t i;
    int k;

    if (n < 38 || p[36] != '@')
        return 0;
    for (k = 0; k < 4; k++)
        if (p[dash[k]] != '-')
            return 0;
    for (i = 0; i < 36; i++)
        if (p[i] != '-' && !((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
            return 0;
    return 1;
}

/* ---- .desc ------------------------------------------------------------------------------- */

static const char *const desc_keys[7] = { "scene_id", "he_file", "he_check", "xml", "header", "layout_id", "info_file" };

static int desc_probe(const uint8_t *d, size_t n, const char *path)
{
    size_t o = 0, s, e;

    if (!line(d, n, &o, &s, &e) || !is_id(d + s, e - s))
        return 0;
    if (!line(d, n, &o, &s, &e) || e - s < 4 || memcmp(d + e - 3, ".he", 3))
        return 0;
    (void)path;
    return 80;
}

static int desc_parse(igsa_ctx *c)
{
    size_t o = 0, s, e;
    int i;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "srdesc");
    for (i = 0; i < 7; i++) {
        if (!line(c->d, c->n, &o, &s, &e))
            return igsa_fail(c, "line %d (%s) missing", i + 1, desc_keys[i]);
        if ((i == 0 || i == 5) && !is_id(c->d + s, e - s))
            return igsa_fail(c, "line %d: not an id", i + 1);
        if (i == 2) {
            char b[32];

            snprintf(b, sizeof(b), "%.*s", (int)(e - s < 31 ? e - s : 31), c->d + s);
            igsa_jint(c, desc_keys[i], strtoll(b, NULL, 10));   /* passed to sub_83324C0, which ignores it */
        } else
            igsa_jstrn(c, desc_keys[i], (const char *)c->d + s, e - s);
    }
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s scene description\n", base(c->path), "desc");
    c->entries++;
    return 0;
}

/* ---- .info: id line, fields, count, count x fields numbers, a last number --------------- */

static int num(const uint8_t *d, size_t n, size_t *o, long long *v)
{
    char b[24];
    size_t k = 0;

    while (*o < n && (d[*o] == ' ' || d[*o] == '\t' || d[*o] == '\r' || d[*o] == '\n'))
        (*o)++;
    if (*o >= n)
        return 0;
    while (*o < n && k < 23 && (d[*o] == '-' || (d[*o] >= '0' && d[*o] <= '9')))
        b[k++] = (char)d[(*o)++];
    if (!k || (*o < n && d[*o] != ' ' && d[*o] != '\t' && d[*o] != '\r' && d[*o] != '\n'))
        return -1;
    b[k] = 0;
    *v = strtoll(b, NULL, 10);
    return 1;
}

static int info_probe(const uint8_t *d, size_t n, const char *path)
{
    size_t o = 0, s, e;
    long long f, cnt;

    if (!line(d, n, &o, &s, &e) || !is_id(d + s, e - s))
        return 0;
    if (num(d, n, &o, &f) != 1 || num(d, n, &o, &cnt) != 1 || f <= 0 || f > 1000 || cnt < 0)
        return 0;
    (void)path;
    return 80;
}

static int info_parse(igsa_ctx *c)
{
    size_t o = 0, s = 0, e = 0;
    long long f, cnt, v, i, k;
    int r;

    line(c->d, c->n, &o, &s, &e);
    if (num(c->d, c->n, &o, &f) != 1 || num(c->d, c->n, &o, &cnt) != 1)
        return igsa_fail(c, "no field / record counts");
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "srinfo");
    igsa_jstrn(c, "scene_id", (const char *)c->d + s, e - s);
    igsa_jint(c, "fields", f);
    igsa_jint(c, "count", cnt);
    igsa_jarr(c, "records");    /* field k goes to trigger handler k (sub_8333E70 registers 0..27) */
    for (i = 0; i < cnt; i++) {
        igsa_jarr(c, NULL);
        for (k = 0; k < f; k++) {
            if ((r = num(c->d, c->n, &o, &v)) != 1)
                return igsa_fail(c, "record %lld field %lld at 0x%zx: %s", i, k, o, r ? "not a number" : "missing");
            igsa_jint(c, NULL, v);
        }
        igsa_jend(c);
    }
    igsa_jend(c);
    if (num(c->d, c->n, &o, &v) != 1)
        return igsa_fail(c, "the last number is missing");
    igsa_jint(c, "last", v);
    while (o < c->n && (c->d[o] == ' ' || c->d[o] == '\r' || c->d[o] == '\n' || c->d[o] == '\t'))
        o++;
    if (o != c->n)
        return igsa_fail(c, "%zu bytes after the table", c->n - o);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %lld records x %lld\n", base(c->path), "info", cnt, f);
    c->entries++;
    return 0;
}

/* ---- .he: u32 52, u32 xor of the header, header[13], vertices, half-edges, faces --------- */

struct he { uint32_t w[13]; const uint8_t *v, *h, *f; uint32_t nv, nh, nf; };

static int he_check(const uint8_t *d, size_t n, struct he *h)
{
    uint32_t x = 0;
    int i;

    if (n < 60 || igsa_le32(d) != 52)
        return 0;
    for (i = 0; i < 13; i++)
        x ^= h->w[i] = igsa_le32(d + 8 + 4 * i);
    if (x != igsa_le32(d + 4))
        return 0;
    h->nv = h->w[1];
    h->nh = h->w[2];
    h->nf = h->w[3];
    if (h->nv > 0x1000000 || h->nh > 0x1000000 || h->nf > 0x1000000 ||
        60 + 28ull * h->nv + 16ull * h->nh + 20ull * h->nf != n)
        return 0;
    h->v = d + 60;
    h->h = h->v + 28 * (size_t)h->nv;
    h->f = h->h + 16 * (size_t)h->nh;
    return 1;
}

static int he_probe(const uint8_t *d, size_t n, const char *path)
{
    struct he h;

    (void)path;
    return he_check(d, n, &h) ? 100 : 0;
}

/* a stored pointer as an index (the file keeps the tool's addresses; sub_82D7670 rebases) */
static long long idx(uint32_t p, uint32_t b, uint32_t sz, uint32_t cnt)
{
    if (p < b || (p - b) % sz || (p - b) / sz >= cnt)
        return -2;
    return (p - b) / sz;
}


static int he_parse(igsa_ctx *c)
{
    struct he h;
    uint32_t i;
    int k;
    char *obj = NULL;
    size_t on = 0, ocap = 0;

    if (!he_check(c->d, c->n, &h))
        return igsa_fail(c, "header check or size");
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "srhe");
    igsa_jhex(c, "word0", h.w[0]);
    igsa_juint(c, "vertices", h.nv);
    igsa_juint(c, "halfedges", h.nh);
    igsa_juint(c, "faces", h.nf);
    igsa_jhex(c, "vertex_base", h.w[4]);
    igsa_jhex(c, "halfedge_base", h.w[5]);
    igsa_jhex(c, "face_base", h.w[6]);
    igsa_jarr(c, "bbox_max");
    for (k = 0; k < 3; k++)
        igsa_jnum(c, NULL, igsa_lef(c->d + 36 + 4 * k));
    igsa_jend(c);
    igsa_jarr(c, "bbox_min");
    for (k = 0; k < 3; k++)
        igsa_jnum(c, NULL, igsa_lef(c->d + 48 + 4 * k));
    igsa_jend(c);
    /* check every link */
    for (i = 0; i < h.nv; i++)
        if (idx(igsa_le32(h.v + 28 * i + 24), h.w[5], 16, h.nh) < 0 && igsa_le32(h.v + 28 * i + 24))
            return igsa_fail(c, "vertex %u: half-edge 0x%08x", i, igsa_le32(h.v + 28 * i + 24));
    for (i = 0; i < h.nh; i++) {
        const uint8_t *e = h.h + 16 * i;

        if ((igsa_le32(e) && idx(igsa_le32(e), h.w[6], 20, h.nf) < 0) ||
            (igsa_le32(e + 4) && idx(igsa_le32(e + 4), h.w[5], 16, h.nh) < 0) ||
            idx(igsa_le32(e + 8), h.w[5], 16, h.nh) < 0 || idx(igsa_le32(e + 12), h.w[4], 28, h.nv) < 0)
            return igsa_fail(c, "half-edge %u: a link out of range", i);
    }
    for (i = 0; i < h.nf; i++)
        if (idx(igsa_le32(h.f + 20 * i), h.w[5], 16, h.nh) < 0)
            return igsa_fail(c, "face %u: half-edge 0x%08x", i, igsa_le32(h.f + 20 * i));
    if (c->mode == IGSA_DUMP || c->mode == IGSA_EXTRACT) {
        igsa_jarr(c, "vertex");
        for (i = 0; i < h.nv; i++) {
            const uint8_t *v = h.v + 28 * i;

            igsa_jobj(c, NULL);
            igsa_jarr(c, "pos");
            for (k = 0; k < 3; k++)
                igsa_jnum(c, NULL, igsa_lef(v + 4 * k));
            igsa_jend(c);
            igsa_jarr(c, "normal");
            for (k = 0; k < 3; k++)
                igsa_jnum(c, NULL, igsa_lef(v + 12 + 4 * k));
            igsa_jend(c);
            igsa_jint(c, "halfedge", igsa_le32(v + 24) ? idx(igsa_le32(v + 24), h.w[5], 16, h.nh) : -1);
            igsa_jend(c);
        }
        igsa_jend(c);
        igsa_jarr(c, "halfedge");
        for (i = 0; i < h.nh; i++) {
            const uint8_t *e = h.h + 16 * i;

            igsa_jobj(c, NULL);
            igsa_jint(c, "face", igsa_le32(e) ? idx(igsa_le32(e), h.w[6], 20, h.nf) : -1);
            igsa_jint(c, "twin", igsa_le32(e + 4) ? idx(igsa_le32(e + 4), h.w[5], 16, h.nh) : -1);
            igsa_jint(c, "next", idx(igsa_le32(e + 8), h.w[5], 16, h.nh));
            igsa_jint(c, "vertex", idx(igsa_le32(e + 12), h.w[4], 28, h.nv));
            igsa_jend(c);
        }
        igsa_jend(c);
        igsa_jarr(c, "face");
        for (i = 0; i < h.nf; i++) {
            const uint8_t *f = h.f + 20 * i;

            igsa_jobj(c, NULL);
            igsa_jint(c, "halfedge", idx(igsa_le32(f), h.w[5], 16, h.nh));
            igsa_jarr(c, "plane");
            for (k = 0; k < 4; k++)
                igsa_jnum(c, NULL, igsa_lef(f + 4 + 4 * k));
            igsa_jend(c);
            igsa_jend(c);
        }
        igsa_jend(c);
    }
    igsa_jend(c);
    /* the mesh as Wavefront OBJ: each face's half-edge loop */
    for (k = 0; k < 2; k++) {
        size_t need = 0;

        for (i = 0; i < h.nv; i++) {
            char b[96];
            int l = snprintf(b, sizeof(b), "v %.6g %.6g %.6g\n", igsa_lef(h.v + 28 * i), igsa_lef(h.v + 28 * i + 4),
                             igsa_lef(h.v + 28 * i + 8));

            if (k)
                memcpy(obj + on, b, (size_t)l), on += (size_t)l;
            else
                need += (size_t)l;
        }
        for (i = 0; i < h.nf; i++) {
            long long e0 = idx(igsa_le32(h.f + 20 * i), h.w[5], 16, h.nh), e = e0;
            int steps = 0;

            if (k)
                obj[on++] = 'f';
            else
                need++;
            do {
                char b[24];
                int l = snprintf(b, sizeof(b), " %lld", idx(igsa_le32(h.h + 16 * e + 12), h.w[4], 28, h.nv) + 1);

                if (k)
                    memcpy(obj + on, b, (size_t)l), on += (size_t)l;
                else
                    need += (size_t)l;
                e = idx(igsa_le32(h.h + 16 * e + 8), h.w[5], 16, h.nh);
            } while (e != e0 && ++steps < 64);
            if (e != e0) {
                free(obj);
                return igsa_fail(c, "face %u: its half-edge loop does not close", i);
            }
            if (k)
                obj[on++] = '\n';
            else
                need++;
        }
        if (!k) {
            ocap = need;
            if (!(obj = malloc(ocap ? ocap : 1)))
                return igsa_fail(c, "out of memory");
        }
    }
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %u vertices, %u faces\n", base(c->path), "he", h.nv, h.nf);
    if (igsa_emit_raw(c, "mesh.obj", obj, on)) {
        free(obj);
        return -1;
    }
    free(obj);
    return 0;
}

/* ---- 2dtexture.inf: "[PAGEn]" lines and "<source> <atlas> <w> <h> <x0> <y0> <x1> <y1>" ------ */

static int tok(const uint8_t *d, size_t e, size_t *o, size_t *s, size_t *t)
{
    while (*o < e && (d[*o] == ' ' || d[*o] == '\t'))
        (*o)++;
    if (*o >= e)
        return 0;
    *s = *o;
    while (*o < e && d[*o] != ' ' && d[*o] != '\t')
        (*o)++;
    *t = *o;
    return 1;
}

static int inf_probe(const uint8_t *d, size_t n, const char *path)
{
    return n >= 7 && !memcmp(d, "[PAGE", 5) && ext(path, ".inf") ? 80 : (n >= 7 && !memcmp(d, "[PAGE", 5) ? 40 : 0);
}

static int inf_parse(igsa_ctx *c)
{
    size_t o = 0, s, e;
    int lines = 0, entries = 0, page = 0;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "srtexinf");
    igsa_jarr(c, "pages");
    while (line(c->d, c->n, &o, &s, &e)) {
        size_t p = s, ts[8], te[8];
        int k = 0;

        lines++;
        if (e - s >= 2 && c->d[s] == '/' && c->d[s + 1] == '/')
            continue;
        while (k < 8 && tok(c->d, e, &p, &ts[k], &te[k]))
            k++;
        if (k == 0)
            continue;
        if (k == 1) {               /* a section line: one token, skipped by the game */
            if (page)
                igsa_jend(c), igsa_jend(c);
            igsa_jobj(c, NULL);
            igsa_jstrn(c, "page", (const char *)c->d + ts[0], te[0] - ts[0]);
            igsa_jarr(c, "images");
            page = 1;
            continue;
        }
        if (k != 8 || tok(c->d, e, &p, &s, &s)) {
            if (page)
                igsa_jend(c), igsa_jend(c);
            igsa_jend(c);
            igsa_jend(c);
            return igsa_fail(c, "line %d: %d fields", lines, k);
        }
        {
            long v[6];
            int i;

            for (i = 0; i < 6; i++) {
                char b[16];

                snprintf(b, sizeof(b), "%.*s", (int)(te[i + 2] - ts[i + 2] < 15 ? te[i + 2] - ts[i + 2] : 15),
                         c->d + ts[i + 2]);
                v[i] = strtol(b, NULL, 10);
            }
            if (!v[0] || !v[1]) {
                if (page)
                    igsa_jend(c), igsa_jend(c);
                igsa_jend(c);
                igsa_jend(c);
                return igsa_fail(c, "line %d: atlas size %ldx%ld", lines, v[0], v[1]);
            }
            if (!page) {
                igsa_jobj(c, NULL);
                igsa_jstr(c, "page", "");
                igsa_jarr(c, "images");
                page = 1;
            }
            igsa_jobj(c, NULL);
            igsa_jgbk(c, "source", (const char *)c->d + ts[0], te[0] - ts[0]);
            igsa_jstrn(c, "atlas", (const char *)c->d + ts[1], te[1] - ts[1]);
            igsa_jint(c, "atlas_w", v[0]);
            igsa_jint(c, "atlas_h", v[1]);
            igsa_jint(c, "x0", v[2]);
            igsa_jint(c, "y0", v[3]);
            igsa_jint(c, "x1", v[4]);
            igsa_jint(c, "y1", v[5]);
            igsa_jnum(c, "u0", (double)v[2] / v[0]);
            igsa_jnum(c, "v0", (double)v[3] / v[1]);
            igsa_jnum(c, "u1", (double)v[4] / v[0]);
            igsa_jnum(c, "v1", (double)v[5] / v[1]);
            igsa_jend(c);
            entries++;
        }
    }
    if (page)
        igsa_jend(c), igsa_jend(c);
    igsa_jend(c);
    igsa_jint(c, "images", entries);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %d atlas images\n", base(c->path), "inf", entries);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_srdesc = { "srdesc", "Speed Rider scene description (.desc)", desc_probe, desc_parse };
const igsa_format igsa_fmt_srinfo = { "srinfo", "Speed Rider scene trigger table (.info)", info_probe, info_parse };
const igsa_format igsa_fmt_srhe = { "srhe", "Speed Rider collision half-edge mesh (.he)", he_probe, he_parse };
const igsa_format igsa_fmt_srtexinf = { "srtexinf", "Speed Rider UI texture atlas (2dtexture.inf)", inf_probe, inf_parse };
