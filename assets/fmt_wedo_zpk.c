/*
 * zlPack archives with their own directory (We Dance Online 2
 * model/boy/19101501.ZPK): zlPack::Load 0x832FAF0, zlPack::LoadHead 0x832F9A0.
 * "zlPack 0002\0", members (u32 size, zlib), the directory (u32 size, zlib),
 * then u32 directory offset, u32 directory size.  The directory is Speed
 * Driver 2's data.hdr layout (fmt_zpk.c): u32 ndirs, {u32 first file, name};
 * u32 nfiles, {u32 dir, u32 offset, u32 size, name}.  Members in a known
 * format are parsed too.  wiki Format-ZPK-We-Dance.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

typedef struct ent { uint32_t dir, off, size; const char *name; } ent;
typedef struct zdir {
    uint8_t *buf;
    size_t n;
    uint32_t nd, nf;
    const char **dname;
    uint32_t *dfirst;
    ent *f;
} zdir;

static void zdir_free(zdir *z)
{
    free(z->buf);
    free(z->dname);
    free(z->dfirst);
    free(z->f);
    memset(z, 0, sizeof(*z));
}

static const char *str_at(const uint8_t *b, size_t n, size_t *o)
{
    const uint8_t *e = *o < n ? memchr(b + *o, 0, n - *o) : NULL;
    const char *s = (const char *)b + *o;

    if (!e)
        return NULL;
    *o = (size_t)(e - b) + 1;
    return s;
}

/* the trailer's directory: 0, or -1 with why */
static int zdir_load(zdir *z, const uint8_t *d, size_t n, char *why, size_t wn)
{
    uint32_t off, sz, raw, i;
    size_t o = 4;

    memset(z, 0, sizeof(*z));
    if (n < 20 || memcmp(d, "zlPack 0002", 12)) {
        snprintf(why, wn, "not a zlPack 0002");
        return -1;
    }
    off = igsa_le32(d + n - 8);
    sz = igsa_le32(d + n - 4);
    if (off < 12 || sz < 4 || (uint64_t)off + sz != n - 8) {
        snprintf(why, wn, "trailer: directory @0x%x size %u does not end at the trailer", off, sz);
        return -1;
    }
    raw = igsa_le32(d + off);
    if (igsa_inflate(d + off + 4, sz - 4, &z->buf, &z->n, raw) || z->n != raw || raw < 8) {
        snprintf(why, wn, "directory: zlib error or %zu bytes, header %u", z->n, raw);
        return -1;
    }
    z->nd = igsa_le32(z->buf);
    if (z->nd > 100000 || !(z->dname = calloc(z->nd + 1, sizeof(char *))) || !(z->dfirst = calloc(z->nd + 1, 4))) {
        snprintf(why, wn, "%u directories", z->nd);
        return -1;
    }
    for (i = 0; i < z->nd; i++) {
        if (o + 4 > z->n || (z->dfirst[i] = igsa_le32(z->buf + o), o += 4, !(z->dname[i] = str_at(z->buf, z->n, &o)))) {
            snprintf(why, wn, "directory %u truncated at 0x%zx", i, o);
            return -1;
        }
    }
    if (o + 4 > z->n) {
        snprintf(why, wn, "no file count");
        return -1;
    }
    z->nf = igsa_le32(z->buf + o);
    o += 4;
    if (z->nf > 1000000 || !(z->f = calloc(z->nf + 1, sizeof(ent)))) {
        snprintf(why, wn, "%u files", z->nf);
        return -1;
    }
    for (i = 0; i < z->nf; i++) {
        ent *f = &z->f[i];

        if (o + 12 > z->n) {
            snprintf(why, wn, "file %u truncated", i);
            return -1;
        }
        f->dir = igsa_le32(z->buf + o);
        f->off = igsa_le32(z->buf + o + 4);
        f->size = igsa_le32(z->buf + o + 8);
        o += 12;
        if (!(f->name = str_at(z->buf, z->n, &o)) || f->dir >= z->nd) {
            snprintf(why, wn, "file %u: name or directory %u", i, f->dir);
            return -1;
        }
        if (f->off < 12 || f->size < 4 || (uint64_t)f->off + f->size > off) {
            snprintf(why, wn, "file %u (%s): @0x%x size %u past the directory", i, f->name, f->off, f->size);
            return -1;
        }
    }
    if (z->n - o > 4) {         /* the writer leaves up to 4 bytes (fmt_zpk.c: data.hdr) */
        snprintf(why, wn, "%zu bytes after the directory", z->n - o);
        return -1;
    }
    return 0;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    zdir z;
    char why[128];
    int r;

    (void)path;
    if (n < 20 || memcmp(d, "zlPack 0002", 12))
        return 0;
    r = zdir_load(&z, d, n, why, sizeof(why)) ? 0 : 101;     /* over fmt_zpk's .hdr variant */
    zdir_free(&z);
    return r;
}

/* a member through the format that claims it: 0, -1, 1 (none) */
static int member(igsa_ctx *c, const char *name, const uint8_t *d, size_t n)
{
    const igsa_format *f = igsa_probe_buf(d, n, name);
    igsa_ctx s;
    char od[1400];
    int r;

    if (!f)
        return 1;
    memset(&s, 0, sizeof(s));
    s.path = name;
    s.d = d;
    s.n = n;
    s.mode = c->mode == IGSA_EXTRACT ? IGSA_EXTRACT : IGSA_CHECK;
    s.out = c->out;
    snprintf(od, sizeof(od), "%s/%s.d", c->outdir ? c->outdir : ".", name);
    s.outdir = od;
    igsa_jstr(c, "format", f->name);
    r = f->parse(&s);
    c->entries += s.entries;
    if (r)
        snprintf(c->err, sizeof(c->err), "%s (%s): %s", name, f->name, s.err);
    return r ? -1 : 0;
}

static int parse(igsa_ctx *c)
{
    zdir z;
    char why[256];
    uint32_t i;
    int r = 0, parsed = 0;

    if (zdir_load(&z, c->d, c->n, why, sizeof(why))) {
        zdir_free(&z);
        return igsa_fail(c, "zlPack: %s", why);
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "zlPack 0002 (own directory)");
    igsa_juint(c, "directory_offset", igsa_le32(c->d + c->n - 8));
    igsa_juint(c, "directory_size", igsa_le32(c->d + c->n - 4));
    igsa_jarr(c, "directories");
    for (i = 0; i < z.nd; i++) {
        igsa_jobj(c, NULL);
        igsa_jbig5(c, "name", z.dname[i], strlen(z.dname[i]));
        igsa_juint(c, "first_file", z.dfirst[i]);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jarr(c, "files");
    for (i = 0; i < z.nf && !r; i++) {
        const ent *f = &z.f[i];
        char full[600];
        uint8_t *u = NULL;
        size_t un = 0;
        uint32_t raw = igsa_le32(c->d + f->off);

        snprintf(full, sizeof(full), "%s%s%s", z.dname[f->dir], *z.dname[f->dir] ? "/" : "", f->name);
        igsa_safe_name(full);
        igsa_jobj(c, NULL);
        igsa_jbig5(c, "path", full, strlen(full));
        igsa_juint(c, "offset", f->off);
        igsa_juint(c, "packed", f->size);
        igsa_juint(c, "size", raw);
        if (igsa_inflate(c->d + f->off + 4, f->size - 4, &u, &un, raw ? raw : 16) || un != raw) {
            free(u);
            igsa_jend(c);
            r = igsa_fail(c, "%s: zlib error (u32 %u, %u bytes)", full, raw, f->size);
            break;
        }
        if (igsa_emit_raw(c, full, u, un))
            r = -1;
        else if (un >= 4 && !memcmp(u, "DDS ", 4)) {     /* textures also as PNG */
            char png[620];

            snprintf(png, sizeof(png), "%s.png", full);
            igsa_jstr(c, "format", "dds");
            if (igsa_emit_dds(c, png, u, un))
                r = -1;
            else
                parsed++;
        } else {
            int k = member(c, full, u, un);

            if (k < 0)
                r = -1;
            parsed += !k;
        }
        igsa_jend(c);
        free(u);
    }
    igsa_jend(c);
    igsa_juint(c, "members", z.nf);
    igsa_jint(c, "members_parsed", parsed);
    igsa_jend(c);
    zdir_free(&z);
    return r;
}

const igsa_format igsa_fmt_wedozpk = { "wedozpk", "zlPack archive with its own directory (We Dance): zlib members", probe, parse };
