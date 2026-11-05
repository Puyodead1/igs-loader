/*
 * Speed Rider data/zip/<name>_2.zipsep: zip archives without their first 2000
 * bytes (the IGS036 holds them; libZipSepArchive asks for them, sub_82DA320).
 * The central directory is intact: members are listed from it, members whose
 * bytes lie past the cut are extracted (stored or deflated, CRC-checked) and
 * handed to the format that claims them.  wiki Format-ZIPSEP.
 */
#include <stdlib.h>
#include <string.h>

#include "zlib.h"
#include "igsa.h"

#define CUT 2000

static long eocd(const uint8_t *d, size_t n)
{
    long k, lo = n > 65557 ? (long)(n - 65557) : 0;

    if (n < 22)
        return -1;
    for (k = (long)n - 22; k >= lo; k--)
        if (igsa_le32(d + k) == 0x06054b50)
            return k;
    return -1;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    long e = eocd(d, n);
    uint32_t cs, co;

    (void)path;
    if (e < 0 || igsa_le32(d) == 0x04034b50)    /* a whole zip is not ours */
        return 0;
    cs = igsa_le32(d + e + 12);
    co = igsa_le32(d + e + 16);
    /* the directory sits where the offsets say once CUT bytes are added */
    return co >= CUT && co - CUT + (size_t)cs == (size_t)e && igsa_le32(d + co - CUT) == 0x02014b50 ? 100 : 0;
}

/* raw deflate */
static int inflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t on)
{
    z_stream z;
    int r;

    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK)
        return -1;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    z.next_out = out;
    z.avail_out = (uInt)on;
    r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.total_out == on ? 0 : -1;
}

/* a member through the format that claims it (as fmt_zpk.c); 0, -1, 1 (no format) */
static int sub_parse(igsa_ctx *c, const char *name, const uint8_t *d, size_t n)
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
    s.mode = c->mode;
    s.out = c->out;
    s.mask = c->mask;
    snprintf(od, sizeof(od), "%s/%s.d", c->outdir ? c->outdir : ".", name);
    igsa_safe_name(od + strlen(c->outdir ? c->outdir : "."));
    s.outdir = od;
    s.j = c->j;
    if (c->mode == IGSA_EXTRACT)
        s.j.f = NULL;
    igsa_jstr(c, "format", f->name);
    if (c->mode == IGSA_DUMP) {
        int depth;

        igsa_jarr(c, "content");
        depth = c->j.depth;
        s.j = c->j;
        r = f->parse(&s);
        c->j = s.j;
        while (c->j.depth > depth)
            igsa_jend(c);
        igsa_jend(c);
    } else
        r = f->parse(&s);
    c->entries += s.entries;
    if (r) {
        snprintf(c->err, sizeof(c->err), "%s (%s): %s", name, f->name, s.err);
        return -1;
    }
    return 0;
}

static int parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    long e = eocd(d, c->n);
    uint32_t cnt, cs, co, i;
    size_t p;
    int whole = 0, lost = 0, sub = 0;

    if (e < 0)
        return igsa_fail(c, "no end of central directory");
    cnt = igsa_le16(d + e + 10);
    cs = igsa_le32(d + e + 12);
    co = igsa_le32(d + e + 16);
    if (co < CUT || co - CUT + (size_t)cs > (size_t)e)
        return igsa_fail(c, "central directory at %u (+%u)", co, cs);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "zipsep");
    igsa_jint(c, "cut", CUT);
    igsa_juint(c, "members", cnt);
    igsa_jarr(c, "files");
    for (i = 0, p = co - CUT; i < cnt; i++) {
        uint32_t crc, csz, usz, lho;
        uint16_t meth, fl, xl, cl;
        char name[512];
        size_t data;

        if (p + 46 > (size_t)e || igsa_le32(d + p) != 0x02014b50)
            return igsa_fail(c, "directory entry %u at 0x%zx", i, p);
        meth = igsa_le16(d + p + 10);
        crc = igsa_le32(d + p + 16);
        csz = igsa_le32(d + p + 20);
        usz = igsa_le32(d + p + 24);
        fl = igsa_le16(d + p + 28);
        xl = igsa_le16(d + p + 30);
        cl = igsa_le16(d + p + 32);
        lho = igsa_le32(d + p + 42);
        if (p + 46 + fl + xl + cl > (size_t)e)
            return igsa_fail(c, "directory entry %u: name past the directory", i);
        snprintf(name, sizeof(name), "%.*s", fl < 511 ? fl : 511, d + p + 46);
        p += 46 + fl + xl + cl;
        igsa_jobj(c, NULL);
        igsa_jstr(c, "name", name);
        igsa_jint(c, "method", meth);
        igsa_juint(c, "size", usz);
        igsa_juint(c, "packed", csz);
        igsa_jhex(c, "crc", crc);
        igsa_juint(c, "offset", lho);
        if (meth != 0 && meth != 8) {
            igsa_jend(c);
            return igsa_fail(c, "%s: method %u", name, meth);
        }
        if (lho >= CUT) {
            /* the local header is in the file */
            size_t l = lho - CUT;

            if (l + 30 > c->n || igsa_le32(d + l) != 0x04034b50) {
                igsa_jend(c);
                return igsa_fail(c, "%s: no local header at 0x%zx", name, l);
            }
            data = l + 30 + igsa_le16(d + l + 26) + igsa_le16(d + l + 28) + CUT;
        } else
            data = lho + 30 + fl + xl;          /* the cut local header: the central one's lengths */
        if (data < CUT) {
            igsa_juint(c, "lost", (data + csz > CUT ? CUT - data : csz));
            igsa_jend(c);
            lost++;
            continue;
        }
        if (data - CUT + csz > c->n) {
            igsa_jend(c);
            return igsa_fail(c, "%s: data past the end", name);
        }
        {
            const uint8_t *src = d + data - CUT;
            uint8_t *u = NULL;
            const uint8_t *m = src;
            int r;

            if (meth == 8) {
                if (!(u = malloc(usz ? usz : 1))) {
                    igsa_jend(c);
                    return igsa_fail(c, "out of memory");
                }
                if (inflate_raw(src, csz, u, usz)) {
                    free(u);
                    igsa_jend(c);
                    return igsa_fail(c, "%s: inflate failed", name);
                }
                m = u;
            } else if (csz != usz) {
                igsa_jend(c);
                return igsa_fail(c, "%s: stored with %u packed, %u bytes", name, csz, usz);
            }
            if (crc32(0, m, usz) != crc) {
                free(u);
                igsa_jend(c);
                return igsa_fail(c, "%s: CRC mismatch", name);
            }
            if (igsa_emit_raw(c, name, m, usz)) {
                free(u);
                igsa_jend(c);
                return -1;
            }
            r = sub_parse(c, name, m, usz);
            free(u);
            if (r < 0) {
                igsa_jend(c);
                return -1;
            }
            sub += !r;
        }
        whole++;
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jint(c, "extracted", whole);
    igsa_jint(c, "lost", lost);
    igsa_jint(c, "parsed_members", sub);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_zipsep = { "zipsep", "Speed Rider .zipsep: a zip without its first 2000 bytes", probe, parse };
