/*
 * libigsa core: loading, format selection, JSON, and the standard outputs
 * (PNG through stb_image_write, WAV, raw).  igsa.h has the API.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "zlib.h"
#include "igsa.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_OPEN_FIX
#include "stb_image_write.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <iconv.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

static FILE *open_out(igsa_ctx *c, const char *name, char *path, size_t pn);

int igsa_probe_decrypted;

const igsa_format *igsa_find(const char *name)
{
    int i;

    for (i = 0; igsa_formats[i]; i++)
        if (!strcmp(igsa_formats[i]->name, name))
            return igsa_formats[i];
    return NULL;
}

int igsa_fail(igsa_ctx *c, const char *fmt, ...)
{
    va_list ap;

    if (c->err[0])              /* keep the innermost message */
        return -1;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err), fmt, ap);
    va_end(ap);
    return -1;
}

void igsa_str(char *out, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n && p[i]; i++)
        out[i] = (char)p[i];
    out[i] = 0;
}

/* ---- loading ------------------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long len;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    d = malloc((size_t)len + 16);
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) {
        free(d);
        d = NULL;
    }
    fclose(f);
    if (d) {
        memset(d + len, 0, 16);
        *n = (size_t)len;
    }
    return d;
}

static const igsa_format *best(const uint8_t *d, size_t n, const char *path)
{
    const igsa_format *b = NULL;
    int i, s, bs = 0;

    for (i = 0; igsa_formats[i]; i++)
        if ((s = igsa_formats[i]->probe(d, n, path)) > bs) {
            bs = s;
            b = igsa_formats[i];
        }
    return b;
}

const igsa_format *igsa_probe_buf(const uint8_t *d, size_t n, const char *path)
{
    return best(d, n, path);
}

const igsa_format *igsa_open(igsa_ctx *c, const char *path, const igsa_format *fmt)
{
    uint8_t *d;
    size_t n;
    const igsa_format *f;

    c->err[0] = 0;
    c->path = path;
    c->decrypted = 0;
    c->entries = 0;
    if (!(d = read_file(path, &n))) {
        igsa_fail(c, "%s: %s", path, strerror(errno));
        return NULL;
    }
    c->d = d;
    c->n = n;
    f = fmt ? (fmt->probe(d, n, path) ? fmt : NULL) : best(d, n, path);
    if (!f && c->mask && n >= 16) {
        uint8_t *e = malloc((n + 15) / 16 * 16 + 16);

        if (e) {
            memcpy(e, d, n);
            memset(e + n, 0, (n + 15) / 16 * 16 - n + 16);
            fpga_decrypt(c->mask, e, (n + 15) / 16 * 16);
            igsa_probe_decrypted = 1;
            f = fmt ? (fmt->probe(e, n, path) ? fmt : NULL) : best(e, n, path);
            igsa_probe_decrypted = 0;
            if (f) {
                free(d);
                c->d = e;
                c->decrypted = 1;
            } else
                free(e);
        }
    }
    if (!f && fmt)              /* forced: parse anyway, the parser reports what is wrong */
        f = fmt;
    return f;
}

int igsa_run(igsa_ctx *c, const igsa_format *f)
{
    char path[1400];
    FILE *idx = NULL;
    int r;

    c->j.f = c->mode == IGSA_DUMP ? c->out : NULL;
    c->j.depth = 0;
    c->j.first[0] = 1;
    c->j.key = 0;
    /* EXTRACT: the structure goes to <outdir>/index.json alongside the entries */
    if (c->mode == IGSA_EXTRACT && !(idx = c->j.f = open_out(c, "index.json", path, sizeof(path))))
        return -1;
    r = f->parse(c);
    if (c->j.f)
        fputc('\n', c->j.f);
    if (idx && fclose(idx) && !r)
        r = igsa_fail(c, "%s: write failed", path);
    return r;
}

void igsa_close(igsa_ctx *c)
{
    free((void *)c->d);
    c->d = NULL;
    c->n = 0;
}

/* ---- JSON --------------------------------------------------------------------------------- */

/*
 * c->j.first[d] per open level d (1-based): bit 0 "nothing written yet",
 * bit 1 "an array".  Writes only in DUMP mode or inside igsa_emit_json.
 */
static FILE *jf(igsa_ctx *c)
{
    return (c->mode == IGSA_DUMP || c->j.f != c->out) ? c->j.f : NULL;
}

static void jkey(FILE *f, igsa_ctx *c, const char *key)
{
    igsa_json *j = &c->j;
    const char *k;

    if (j->depth > 0) {
        if (!(j->first[j->depth] & 1))
            fputc(',', f);
        j->first[j->depth] &= ~1;
        fprintf(f, "\n%*s", 2 * j->depth, "");
        if (!(j->first[j->depth] & 2)) {
            fputc('"', f);
            for (k = key ? key : ""; *k; k++) {
                if (*k == '"' || *k == '\\')
                    fputc('\\', f);
                fputc(*k, f);
            }
            fputs("\": ", f);
        }
    }
}

static void jopen(igsa_ctx *c, const char *key, int arr)
{
    FILE *f = jf(c);

    if (!f)
        return;
    jkey(f, c, key);
    fputc(arr ? '[' : '{', f);
    if (c->j.depth < 63)
        c->j.first[++c->j.depth] = 1 | (arr ? 2 : 0);
}

void igsa_jobj(igsa_ctx *c, const char *key) { jopen(c, key, 0); }
void igsa_jarr(igsa_ctx *c, const char *key) { jopen(c, key, 1); }

void igsa_jend(igsa_ctx *c)
{
    FILE *f = jf(c);
    int fl;

    if (!f || c->j.depth == 0)
        return;
    fl = c->j.first[c->j.depth--];
    if (!(fl & 1))
        fprintf(f, "\n%*s", 2 * c->j.depth, "");
    fputc(fl & 2 ? ']' : '}', f);
}

#define JVAL(c, key) FILE *f = jf(c); if (!f) return; jkey(f, c, key)

void igsa_jint(igsa_ctx *c, const char *key, long long v) { JVAL(c, key); fprintf(f, "%lld", v); }
void igsa_juint(igsa_ctx *c, const char *key, unsigned long long v) { JVAL(c, key); fprintf(f, "%llu", v); }
void igsa_jhex(igsa_ctx *c, const char *key, unsigned long long v) { JVAL(c, key); fprintf(f, "\"0x%llx\"", v); }
void igsa_jbool(igsa_ctx *c, const char *key, int v) { JVAL(c, key); fputs(v ? "true" : "false", f); }

void igsa_jnum(igsa_ctx *c, const char *key, double v)
{
    JVAL(c, key);
    if (v != v || v - v != 0)   /* NaN, inf: not JSON */
        fputs("null", f);
    else
        fprintf(f, "%.9g", v);
}

static int utf8_len(const unsigned char *s, size_t n)
{
    int l, i;

    if (s[0] < 0xC2 || s[0] > 0xF4)
        return 0;
    l = s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : 4;
    if ((size_t)l > n)
        return 0;
    for (i = 1; i < l; i++)
        if ((s[i] & 0xC0) != 0x80)
            return 0;
    return l;
}

void igsa_jstrn(igsa_ctx *c, const char *key, const char *s, size_t n)
{
    size_t i;
    JVAL(c, key);

    fputc('"', f);
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        int l;

        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch == '\n')
            fputs("\\n", f);
        else if (ch == '\r')
            fputs("\\r", f);
        else if (ch == '\t')
            fputs("\\t", f);
        else if (ch < 0x20 || ch == 0x7F)
            fprintf(f, "\\u%04x", ch);
        else if (ch < 0x80)
            fputc(ch, f);
        else if ((l = utf8_len((const unsigned char *)s + i, n - i))) {
            fwrite(s + i, 1, (size_t)l, f);
            i += (size_t)l - 1;
        } else
            fprintf(f, "\\u%04x", ch);
    }
    fputc('"', f);
}

void igsa_jstr(igsa_ctx *c, const char *key, const char *s)
{
    igsa_jstrn(c, key, s ? s : "", s ? strlen(s) : 0);
}

void igsa_jbytes(igsa_ctx *c, const char *key, const uint8_t *p, size_t n)
{
    size_t i;
    JVAL(c, key);

    fputc('"', f);
    for (i = 0; i < n; i++)
        fprintf(f, "%02x", p[i]);
    fputc('"', f);
}

static void jcp(igsa_ctx *c, const char *key, const char *s, size_t n, int cp, const char *cpname)
{
    char *u = NULL;
    size_t un = 0;

    (void)cp;
    (void)cpname;
    if (!jf(c))
        return;
    while (n && !s[n - 1])
        n--;
#ifdef _WIN32
    {
        int w = MultiByteToWideChar((UINT)cp, 0, s, (int)n, NULL, 0);
        wchar_t *ws = w > 0 ? malloc(sizeof(wchar_t) * (size_t)w) : NULL;

        if (ws && MultiByteToWideChar((UINT)cp, 0, s, (int)n, ws, w) == w) {
            int l = WideCharToMultiByte(CP_UTF8, 0, ws, w, NULL, 0, NULL, NULL);

            if (l > 0 && (u = malloc((size_t)l)))
                un = (size_t)WideCharToMultiByte(CP_UTF8, 0, ws, w, u, l, NULL, NULL);
        }
        free(ws);
    }
#else
    {
        iconv_t cd = iconv_open("UTF-8", cpname);

        if (cd != (iconv_t)-1) {
            size_t in = n, out = n * 4 + 4;
            char *ip = (char *)s, *op;

            u = malloc(out);
            op = u;
            if (u && iconv(cd, &ip, &in, &op, &out) != (size_t)-1)
                un = (size_t)(op - u);
            else {
                free(u);
                u = NULL;
            }
            iconv_close(cd);
        }
    }
#endif
    if (u)
        igsa_jstrn(c, key, u, un);
    else
        igsa_jstrn(c, key, s, n);
    free(u);
}

void igsa_jgbk(igsa_ctx *c, const char *key, const char *s, size_t n) { jcp(c, key, s, n, 936, "GB18030"); }
void igsa_jbig5(igsa_ctx *c, const char *key, const char *s, size_t n) { jcp(c, key, s, n, 950, "BIG5"); }

/* ---- outputs ------------------------------------------------------------------------------ */

void igsa_safe_name(char *s)
{
    for (; *s; s++)
        if (*s == ':' || *s == '*' || *s == '?' || *s == '"' || *s == '<' || *s == '>' || *s == '|' ||
            (unsigned char)*s < 0x20)
            *s = '_';
        else if (*s == '\\')
            *s = '/';
}

static int mkdirs(char *p)
{
    char *s;

    for (s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (MKDIR(p) && errno != EEXIST) {
                *s = '/';
                return -1;
            }
            *s = '/';
        }
    return 0;
}

static FILE *open_out(igsa_ctx *c, const char *name, char *path, size_t pn)
{
    FILE *f;
    char *s;

    snprintf(path, pn, "%s/%s", c->outdir ? c->outdir : ".", name);
    igsa_safe_name(path + strlen(c->outdir ? c->outdir : "."));
    /* no climbing out of outdir */
    for (s = path; (s = strstr(s, "..")); s += 2)
        if ((s == path || s[-1] == '/') && (s[2] == '/' || !s[2]))
            s[0] = s[1] = '_';
    if (mkdirs(path)) {
        igsa_fail(c, "%s: %s", path, strerror(errno));
        return NULL;
    }
    if (!(f = fopen(path, "wb")))
        igsa_fail(c, "%s: %s", path, strerror(errno));
    return f;
}

static void list_line(igsa_ctx *c, const char *name, const char *kind, const char *info)
{
    if (c->mode == IGSA_LIST && c->out)
        fprintf(c->out, "%-48s %-5s %s\n", name, kind, info);
}

int igsa_emit_raw(igsa_ctx *c, const char *name, const void *p, size_t n)
{
    char path[1400], info[32];
    FILE *f;
    int ok;

    c->entries++;
    snprintf(info, sizeof(info), "%zu bytes", n);
    list_line(c, name, "raw", info);
    if (c->mode != IGSA_EXTRACT)
        return 0;
    if (!(f = open_out(c, name, path, sizeof(path))))
        return -1;
    ok = fwrite(p, 1, n, f) == n;
    if (fclose(f) || !ok)
        return igsa_fail(c, "%s: write failed", path);
    return 0;
}

static void png_write(void *ctx, void *data, int size)
{
    if (fwrite(data, 1, (size_t)size, (FILE *)ctx) != (size_t)size)
        ;                       /* checked by ferror at close */
}

int igsa_emit_png(igsa_ctx *c, const char *name, int w, int h, const uint8_t *rgba)
{
    char path[1400], info[48];
    FILE *f;
    int ok;

    c->entries++;
    if (w <= 0 || h <= 0)
        return igsa_fail(c, "%s: empty image %dx%d", name, w, h);
    snprintf(info, sizeof(info), "%dx%d", w, h);
    list_line(c, name, "png", info);
    if (c->mode != IGSA_EXTRACT)
        return 0;
    if (!(f = open_out(c, name, path, sizeof(path))))
        return -1;
    ok = stbi_write_png_to_func(png_write, f, w, h, 4, rgba, w * 4);
    ok = ok && !ferror(f);
    if (fclose(f) || !ok)
        return igsa_fail(c, "%s: write failed", path);
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int igsa_emit_wav(igsa_ctx *c, const char *name, int rate, int ch, int bits, const void *pcm, size_t n)
{
    char path[1400], info[64];
    uint8_t h[44];
    FILE *f;
    int ok;

    c->entries++;
    snprintf(info, sizeof(info), "%d Hz %d ch %d bit, %zu bytes", rate, ch, bits, n);
    list_line(c, name, "wav", info);
    if (c->mode != IGSA_EXTRACT)
        return 0;
    if (!(f = open_out(c, name, path, sizeof(path))))
        return -1;
    memcpy(h, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0", 22);
    put32(h + 4, (uint32_t)(36 + n));
    h[22] = (uint8_t)ch;
    h[23] = 0;
    put32(h + 24, (uint32_t)rate);
    put32(h + 28, (uint32_t)(rate * ch * bits / 8));
    h[32] = (uint8_t)(ch * bits / 8);
    h[33] = 0;
    h[34] = (uint8_t)bits;
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    put32(h + 40, (uint32_t)n);
    ok = fwrite(h, 1, 44, f) == 44 && fwrite(pcm, 1, n, f) == n;
    if (fclose(f) || !ok)
        return igsa_fail(c, "%s: write failed", path);
    return 0;
}

int igsa_emit_json(igsa_ctx *c, const char *name, void (*fn)(igsa_ctx *c, void *arg), void *arg)
{
    char path[1400];
    igsa_json save = c->j;
    FILE *f;
    int ok;

    c->entries++;
    list_line(c, name, "json", "");
    if (c->mode != IGSA_EXTRACT)
        return 0;
    if (!(f = open_out(c, name, path, sizeof(path))))
        return -1;
    c->j.f = f;
    c->j.depth = 0;
    c->j.first[0] = 1;
    fn(c, arg);
    fputc('\n', f);
    ok = !ferror(f);
    c->j = save;
    if (fclose(f) || !ok)
        return igsa_fail(c, "%s: write failed", path);
    return 0;
}

/* ---- decoding ----------------------------------------------------------------------------- */

int igsa_inflate(const uint8_t *in, size_t n, uint8_t **out, size_t *outn, size_t hint)
{
    z_stream z;
    size_t cap = hint ? hint : n * 4 + 64;
    uint8_t *o = malloc(cap + 1);
    int r;

    if (!o)
        return -1;
    memset(&z, 0, sizeof(z));
    if (inflateInit(&z) != Z_OK) {
        free(o);
        return -1;
    }
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    for (;;) {
        z.next_out = o + z.total_out;
        z.avail_out = (uInt)(cap - z.total_out);
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_STREAM_END)
            break;
        if (r != Z_OK && r != Z_BUF_ERROR) {
            inflateEnd(&z);
            free(o);
            return -1;
        }
        if (z.avail_out == 0) {
            uint8_t *t = realloc(o, cap * 2 + 1);

            if (!t) {
                inflateEnd(&z);
                free(o);
                return -1;
            }
            o = t;
            cap *= 2;
        } else if (z.avail_in == 0) {   /* truncated stream */
            inflateEnd(&z);
            free(o);
            return -1;
        }
    }
    *outn = z.total_out;
    *out = o;
    inflateEnd(&z);
    return 0;
}

void igsa_rgb565(uint8_t *d, const uint8_t *s, size_t px)
{
    for (; px--; s += 2, d += 4) {
        unsigned v = igsa_le16(s), r = v >> 11, g = v >> 5 & 63, b = v & 31;

        d[0] = (uint8_t)(r << 3 | r >> 2);
        d[1] = (uint8_t)(g << 2 | g >> 4);
        d[2] = (uint8_t)(b << 3 | b >> 2);
        d[3] = 255;
    }
}

void igsa_argb1555(uint8_t *d, const uint8_t *s, size_t px)
{
    for (; px--; s += 2, d += 4) {
        unsigned v = igsa_le16(s), r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;

        d[0] = (uint8_t)(r << 3 | r >> 2);
        d[1] = (uint8_t)(g << 3 | g >> 2);
        d[2] = (uint8_t)(b << 3 | b >> 2);
        d[3] = v & 0x8000 ? 255 : 0;
    }
}

void igsa_argb4444(uint8_t *d, const uint8_t *s, size_t px)
{
    for (; px--; s += 2, d += 4) {
        unsigned v = igsa_le16(s);

        d[0] = (uint8_t)((v >> 8 & 15) * 17);
        d[1] = (uint8_t)((v >> 4 & 15) * 17);
        d[2] = (uint8_t)((v & 15) * 17);
        d[3] = (uint8_t)((v >> 12) * 17);
    }
}

void igsa_bgra8(uint8_t *d, const uint8_t *s, size_t px)
{
    for (; px--; s += 4, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
    }
}

void igsa_bgr8(uint8_t *d, const uint8_t *s, size_t px)
{
    for (; px--; s += 3, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
    }
}

static void c565(uint8_t *o, unsigned v)
{
    unsigned r = v >> 11, g = v >> 5 & 63, b = v & 31;

    o[0] = (uint8_t)(r << 3 | r >> 2);
    o[1] = (uint8_t)(g << 2 | g >> 4);
    o[2] = (uint8_t)(b << 3 | b >> 2);
    o[3] = 255;
}

int igsa_dxt(uint8_t *dst, const uint8_t *src, size_t n, int w, int h, int dxt)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bx, by, i, bs = dxt == 1 ? 8 : 16;

    if ((size_t)bw * bh * bs > n)
        return -1;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++, src += bs) {
            const uint8_t *cb = src + (dxt == 1 ? 0 : 8);
            unsigned c0 = igsa_le16(cb), c1 = igsa_le16(cb + 2);
            uint32_t bits = igsa_le32(cb + 4);
            uint8_t pal[4][4], al[16];

            c565(pal[0], c0);
            c565(pal[1], c1);
            for (i = 0; i < 3; i++) {
                if (c0 > c1 || dxt != 1) {
                    pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3);
                    pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3);
                } else {
                    pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
                    pal[3][i] = 0;
                }
            }
            pal[2][3] = 255;
            pal[3][3] = (c0 > c1 || dxt != 1) ? 255 : 0;
            if (dxt == 3)
                for (i = 0; i < 16; i++)
                    al[i] = (uint8_t)(((src[i / 2] >> (4 * (i & 1))) & 15) * 17);
            else if (dxt == 5) {
                unsigned a0 = src[0], a1 = src[1], k;
                uint64_t ab = 0;
                uint8_t ap[8];

                for (i = 5; i >= 0; i--)
                    ab = ab << 8 | src[2 + i];
                ap[0] = (uint8_t)a0;
                ap[1] = (uint8_t)a1;
                for (k = 2; k < 8; k++)
                    ap[k] = (uint8_t)(a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7
                                     : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : k == 6 ? 0 : 255);
                for (i = 0; i < 16; i++)
                    al[i] = ap[ab >> (3 * i) & 7];
            }
            for (i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                uint8_t *o;

                if (x >= w || y >= h)
                    continue;
                o = dst + ((size_t)y * w + x) * 4;
                memcpy(o, pal[bits >> (2 * i) & 3], 4);
                if (dxt != 1)
                    o[3] = al[i];
            }
        }
    return 0;
}
