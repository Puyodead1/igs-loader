/*
 * zlPack archives (Speed Driver 2 data/data.ZPK) and their directory
 * (data/data.hdr): zlPack::Open sub_81BC9E0, IGS_PK::LoadHead sub_808CBF0,
 * zlPack::Save sub_81BA030, zlib callbacks sub_81B9740 / sub_81B9810.
 * wiki Format-ZPK.
 *
 *   .ZPK  "zlPack 0002\0", then the members: u32 size, zlib (stored when
 *         compress failed); no directory (data.hdr has it)
 *   .hdr  u8 xor of the 128-byte prefix the IGS036 sends (0xB2), then the
 *         directory without that prefix:
 *         u32 ndirs, {u32 first file, char name[]}; u32 nfiles,
 *         {u32 dir, u32 offset, u32 size, char name[]}
 *
 * Members in a known format are parsed too (DUMP: "content", EXTRACT:
 * <member>.d/ with its index.json).
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"
#include "fmt_zl.h"

/* the 128-byte prefix (rebuilt, wiki Keys: E2000 Speed Driver 2) */
static size_t prefix(uint8_t *p)
{
    static const struct { uint32_t first; const char *name; } dirs[] = {
        { 0, "data" }, { 0, "data\\2d" }, { 0, "data\\2d\\ui" }, { 39, "data\\2d\\ui\\mark" },
        { 68, "data\\2d\\operator" }, { 72, "data\\2d\\effect" }, { 72, "data\\2d\\effect\\lensflare" },
    };
    size_t n = 4, i;

    p[0] = 143;
    p[1] = p[2] = p[3] = 0;
    for (i = 0; i < 7; i++) {
        p[n] = (uint8_t)dirs[i].first;
        p[n + 1] = p[n + 2] = p[n + 3] = 0;
        strcpy((char *)p + n + 4, dirs[i].name);
        n += 4 + strlen(dirs[i].name) + (i < 6);
    }
    return n;
}

typedef struct dir { uint32_t first; const char *name; } dir;
typedef struct file { uint32_t dir, off, size; const char *name; } file;

typedef struct hdr {
    uint8_t *buf;
    size_t n;
    uint32_t nd, nf;
    dir *d;
    file *f;
    size_t used;            /* bytes of the directory; the rest is the writer's slack */
} hdr;

static void hdr_free(hdr *h)
{
    free(h->buf);
    free(h->d);
    free(h->f);
    memset(h, 0, sizeof(*h));
}

/* the string at o (NUL inside the buffer) or NULL */
static const char *str_at(const uint8_t *b, size_t n, size_t *o)
{
    const uint8_t *e = memchr(b + *o, 0, n - *o);
    const char *s = (const char *)b + *o;

    if (!e)
        return NULL;
    *o = (size_t)(e - b) + 1;
    return s;
}

/* build the directory from the .hdr bytes: 0, or -1 with why */
static int hdr_load(hdr *h, const uint8_t *d, size_t n, char *why, size_t wn)
{
    uint8_t pre[160], x = 0;
    size_t pn = prefix(pre), o = 4, i;

    memset(h, 0, sizeof(*h));
    if (n < 2) {
        snprintf(why, wn, "too short");
        return -1;
    }
    for (i = 0; i < pn; i++)
        x ^= pre[i];
    if (x != d[0]) {
        snprintf(why, wn, "check byte 0x%02x, the prefix gives 0x%02x", d[0], x);
        return -1;
    }
    h->n = pn + n - 1;
    if (!(h->buf = malloc(h->n + 1))) {
        snprintf(why, wn, "out of memory");
        return -1;
    }
    memcpy(h->buf, pre, pn);
    memcpy(h->buf + pn, d + 1, n - 1);
    h->buf[h->n] = 0;
    h->nd = igsa_le32(h->buf);
    if (h->nd > 100000 || !(h->d = calloc(h->nd + 1, sizeof(dir)))) {
        snprintf(why, wn, "%u directories", h->nd);
        return -1;
    }
    for (i = 0; i < h->nd; i++) {
        if (o + 4 > h->n || (h->d[i].first = igsa_le32(h->buf + o), o += 4, !(h->d[i].name = str_at(h->buf, h->n, &o)))) {
            snprintf(why, wn, "directory %zu truncated at 0x%zx", i, o);
            return -1;
        }
    }
    if (o + 4 > h->n) {
        snprintf(why, wn, "no file count at 0x%zx", o);
        return -1;
    }
    h->nf = igsa_le32(h->buf + o);
    o += 4;
    if (h->nf > 1000000 || !(h->f = calloc(h->nf + 1, sizeof(file)))) {
        snprintf(why, wn, "%u files", h->nf);
        return -1;
    }
    for (i = 0; i < h->nf; i++) {
        file *f = &h->f[i];

        if (o + 12 > h->n) {
            snprintf(why, wn, "file %zu truncated at 0x%zx", i, o);
            return -1;
        }
        f->dir = igsa_le32(h->buf + o);
        f->off = igsa_le32(h->buf + o + 4);
        f->size = igsa_le32(h->buf + o + 8);
        o += 12;
        if (!(f->name = str_at(h->buf, h->n, &o))) {
            snprintf(why, wn, "file %zu name truncated", i);
            return -1;
        }
        if (f->dir >= h->nd) {
            snprintf(why, wn, "file %zu (%s): directory %u of %u", i, f->name, f->dir, h->nd);
            return -1;
        }
    }
    h->used = o;
    /* the writer allocates 4 bytes more than it fills (sub_81BA030) */
    if (h->n - o > 4) {
        snprintf(why, wn, "%zu bytes after the directory", h->n - o);
        return -1;
    }
    return 0;
}

static int hdr_probe(const uint8_t *d, size_t n, const char *path)
{
    hdr h;
    char why[128];
    int r;

    (void)path;
    if (n < 16 || n > (64u << 20))
        return 0;
    r = hdr_load(&h, d, n, why, sizeof(why)) ? 0 : 95;
    hdr_free(&h);
    return r;
}

static void dump_dirs(igsa_ctx *c, const hdr *h)
{
    uint32_t i;

    igsa_jarr(c, "directories");
    for (i = 0; i < h->nd; i++) {
        igsa_jobj(c, NULL);
        igsa_jgbk(c, "name", h->d[i].name, strlen(h->d[i].name));
        igsa_juint(c, "first_file", h->d[i].first);
        igsa_jend(c);
    }
    igsa_jend(c);
}

static int hdr_parse(igsa_ctx *c)
{
    hdr h;
    char why[256];
    uint32_t i;

    if (hdr_load(&h, c->d, c->n, why, sizeof(why))) {
        hdr_free(&h);
        return igsa_fail(c, "zpk directory: %s", why);
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "zlPack directory");
    igsa_jhex(c, "check", c->d[0]);
    igsa_juint(c, "prefix", h.n - (c->n - 1));
    igsa_juint(c, "slack", h.n - h.used);
    dump_dirs(c, &h);
    igsa_jarr(c, "files");
    for (i = 0; i < h.nf; i++) {
        const file *f = &h.f[i];
        char full[600];

        snprintf(full, sizeof(full), "%s\\%s", h.d[f->dir].name, f->name);
        igsa_jobj(c, NULL);
        igsa_jgbk(c, "path", full, strlen(full));
        igsa_juint(c, "dir", f->dir);
        igsa_juint(c, "offset", f->off);
        igsa_juint(c, "size", f->size);
        igsa_jend(c);
        if (c->mode == IGSA_LIST) {
            char info[48];

            snprintf(info, sizeof(info), "@0x%x, %u bytes", f->off, f->size);
            fprintf(c->out, "%-48s %-5s %s\n", full, "ref", info);
        }
        c->entries++;
    }
    igsa_jend(c);
    igsa_jend(c);
    hdr_free(&h);
    return 0;
}

const igsa_format igsa_fmt_zpkhdr = { "zpkhdr", "zlPack directory (Speed Driver 2 data.hdr, prefix from the IGS036)", hdr_probe,
                                      hdr_parse };

/* ---- the archive ---------------------------------------------------------------------------- */

static int zpk_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 12 && !memcmp(d, "zlPack 0002", 12) ? 100 : 0;
}

/* <dir>/<base>.hdr next to the archive */
static uint8_t *read_hdr(const char *path, size_t *n)
{
    char p[1400], *dot, *sl;
    FILE *f;
    long len;
    uint8_t *d;

    snprintf(p, sizeof(p), "%s", path);
    dot = strrchr(p, '.');
    sl = strrchr(p, '/');
    if (strrchr(p, '\\') > sl)
        sl = strrchr(p, '\\');
    if (!dot || (sl && dot < sl))
        dot = p + strlen(p);
    snprintf(dot, sizeof(p) - (size_t)(dot - p), ".hdr");
    if (!(f = fopen(p, "rb")))
        return NULL;
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) || !(d = malloc((size_t)len))) {
        fclose(f);
        return NULL;
    }
    if (fread(d, 1, (size_t)len, f) != (size_t)len) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return d;
}

struct mrun { igsa_ctx *s; const igsa_format *f; int r; };

/* EXTRACT: the member's structure as <member>.d/index.json */
static void run_member(igsa_ctx *c, void *arg)
{
    struct mrun *m = arg;

    m->s->j = c->j;
    m->r = m->f->parse(m->s);
    c->j = m->s->j;
}

/* a member through the format that claims it; 0, -1 (c->err), 1 (no format) */
static int sub_parse(igsa_ctx *c, const char *name, const uint8_t *d, size_t n, const char **fname)
{
    const igsa_format *f = igsa_probe_buf(d, n, name);
    igsa_ctx s;
    char od[1400];
    int r;

    *fname = f ? f->name : NULL;
    if (!f || f == &igsa_fmt_zpkhdr)
        return 1;
    memset(&s, 0, sizeof(s));
    s.path = name;
    s.d = d;
    s.n = n;
    s.mode = c->mode;
    s.out = c->out;
    s.mask = c->mask;
    snprintf(od, sizeof(od), "%s/%s.d", c->outdir ? c->outdir : ".", name);
    igsa_safe_name(od + strlen(c->outdir ? c->outdir : "."));
    s.outdir = od;
    s.j = c->j;
    igsa_jstr(c, "format", f->name);
    if (c->mode == IGSA_DUMP) {
        int depth;

        igsa_jarr(c, "content");    /* the member's document */
        depth = c->j.depth;
        s.j = c->j;
        r = f->parse(&s);
        c->j = s.j;
        while (c->j.depth > depth)
            igsa_jend(c);
        igsa_jend(c);
    } else if (c->mode == IGSA_EXTRACT) {
        struct mrun m = { &s, f, 0 };
        char ix[1500];

        snprintf(ix, sizeof(ix), "%s.d/index.json", name);
        r = igsa_emit_json(c, ix, run_member, &m);
        if (!r)
            r = m.r;
        else if (!s.err[0])
            snprintf(s.err, sizeof(s.err), "%s", c->err);
    } else
        r = f->parse(&s);
    c->entries += s.entries;
    if (r) {
        snprintf(c->err, sizeof(c->err), "%s (%s): %s", name, f->name, s.err);
        return -1;
    }
    return 0;
}

/* the archive being parsed, for igsa_zl_sibling (a model's shader file) */
static struct { igsa_ctx *c; hdr *h; uint8_t *buf; } cur;

static int same_path(const char *dir, const char *name, const char *want)
{
    size_t dl = strlen(dir), i;

    for (i = 0; i < dl; i++)
        if ((dir[i] == '\\' ? '/' : dir[i]) != want[i])
            return 0;
    return want[dl] == '/' && !strcmp(name, want + dl + 1);
}

static const uint8_t *sibling(const char *want, size_t *n)
{
    uint32_t i;

    free(cur.buf);
    cur.buf = NULL;
    for (i = 0; i < cur.h->nf; i++) {
        const file *f = &cur.h->f[i];
        size_t un;

        if (!same_path(cur.h->d[f->dir].name, f->name, want))
            continue;
        if (f->size < 4 || (size_t)f->off + f->size > cur.c->n)
            return NULL;
        if (igsa_inflate(cur.c->d + f->off + 4, f->size - 4, &cur.buf, &un, igsa_le32(cur.c->d + f->off) + 1)) {
            *n = f->size - 4;
            return cur.c->d + f->off + 4;
        }
        *n = un;
        return cur.buf;
    }
    return NULL;
}

/* standard images in the archive: also as PNG (<member>.png) */
static int member_image(igsa_ctx *c, const char *nm, const uint8_t *p, size_t n)
{
    const char *e = strrchr(nm, '.');
    char png[640];

    if (!e)
        return 0;
    snprintf(png, sizeof(png), "%s.png", nm);
    if (!strcasecmp(e, ".dds"))
        return igsa_emit_dds(c, png, p, n);
    if (!strcasecmp(e, ".tga"))
        return igsa_emit_tga(c, png, p, n);
    return 0;
}

static int zpk_parse(igsa_ctx *c)
{
    uint8_t *hd;
    size_t hn;
    hdr h;
    char why[256];
    uint32_t i;
    size_t end = 12;
    int r = 0, nparsed = 0;

    if (!(hd = read_hdr(c->path, &hn)))
        return igsa_fail(c, "%s: no .hdr next to it (the archive has no directory of its own)", c->path);
    if (hdr_load(&h, hd, hn, why, sizeof(why))) {
        free(hd);
        hdr_free(&h);
        return igsa_fail(c, "zpk directory: %s", why);
    }
    free(hd);
    cur.c = c;
    cur.h = &h;
    igsa_zl_sibling = sibling;
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "zlPack 0002");
    dump_dirs(c, &h);
    igsa_jarr(c, "files");
    for (i = 0; i < h.nf && !r; i++) {
        const file *f = &h.f[i];
        char full[600];
        uint8_t *u = NULL;
        size_t un = 0;
        uint32_t raw;
        int stored = 0;
        const char *fn;

        snprintf(full, sizeof(full), "%s\\%s", h.d[f->dir].name, f->name);
        if (f->size < 4 || f->off < 12 || (size_t)f->off + f->size > c->n) {
            r = igsa_fail(c, "%s: @0x%x size %u outside the archive (%zu bytes)", full, f->off, f->size, c->n);
            break;
        }
        if ((size_t)f->off + f->size > end)
            end = (size_t)f->off + f->size;
        raw = igsa_le32(c->d + f->off);
        if (igsa_inflate(c->d + f->off + 4, f->size - 4, &u, &un, raw ? raw : 16)) {
            /* compress failed when the archive was written: u32 size, the bytes */
            if (raw != f->size - 4) {
                r = igsa_fail(c, "%s: zlib error (u32 %u, %u bytes)", full, raw, f->size);
                break;
            }
            stored = 1;
        } else if (un != raw) {
            free(u);
            r = igsa_fail(c, "%s: unpacked %zu bytes, header says %u", full, un, raw);
            break;
        }
        igsa_jobj(c, NULL);
        igsa_jgbk(c, "path", full, strlen(full));
        igsa_juint(c, "offset", f->off);
        igsa_juint(c, "packed", f->size);
        igsa_juint(c, "size", raw);
        if (stored)
            igsa_jbool(c, "stored", 1);
        {
            char nm[620];
            const uint8_t *p = stored ? c->d + f->off + 4 : u;

            snprintf(nm, sizeof(nm), "%s", full);
            igsa_safe_name(nm);
            if (igsa_emit_raw(c, nm, p, raw) || member_image(c, nm, p, raw))
                r = -1;
            else if ((r = sub_parse(c, nm, p, raw, &fn)) > 0)
                r = 0;
            else if (!r)
                nparsed++;
        }
        igsa_jend(c);
        free(u);
    }
    igsa_jend(c);
    if (!r && end != c->n)
        r = igsa_fail(c, "the members end at 0x%zx, the archive at 0x%zx", end, c->n);
    igsa_juint(c, "members", h.nf);
    igsa_jint(c, "members_parsed", nparsed);
    igsa_jend(c);
    igsa_zl_sibling = NULL;
    free(cur.buf);
    memset(&cur, 0, sizeof(cur));
    hdr_free(&h);
    return r;
}

const igsa_format igsa_fmt_zpk = { "zpk", "zlPack archive (Speed Driver 2 data.ZPK): zlib members, directory in data.hdr",
                                   zpk_probe, zpk_parse };
