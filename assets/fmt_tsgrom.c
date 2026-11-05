/*
 * TSGROM01: the TSG image/action banks of the E2000 and E1000 2D engine
 * (the .rom files of image/, main/ and system/biglogo.rom).  Chunks of tag[8] size(u32);
 * ActionGroupLoad (sship 0x805D9D0, pkids 0x8056814).  wiki Format-TSGROM.
 *
 * A " VERSION" chunk ("00.0000.0004") marks the current layout, in which every
 * PCX/PK0/TGA/BMP image chunk starts with one extra byte; banks without it
 * (biglogo.rom) have no such byte.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"


static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 12 && !memcmp(d, "TSGROM01", 8) ? 100 : 0;
}

typedef struct bank {
    igsa_ctx *c;
    int pass;               /* 1: walk and collect, 2: dump and emit */
    int versioned;
    int nimg, npal, nact;
    int cap;
    signed char *imgpal;    /* palette of image i (first "show" step that draws it), -1 none */
    size_t *pals;           /* offsets of the 768-byte palettes */
    int palcap;
} bank;

static int need(bank *b, size_t o, size_t k)
{
    return o <= b->c->n && k <= b->c->n - o;
}

/* room for image index i in imgpal */
static int ensure(bank *b, int i)
{
    while (i >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 64;
        signed char *t = realloc(b->imgpal, (size_t)nc);

        if (!t)
            return igsa_fail(b->c, "out of memory");
        memset(t + b->cap, -1, (size_t)(nc - b->cap));
        b->imgpal = t;
        b->cap = nc;
    }
    return 0;
}

static int grow_img(bank *b) { return ensure(b, b->nimg); }

/* ---- pixels -------------------------------------------------------------------------------- */

static uint8_t x5(unsigned v) { return (uint8_t)(v << 3 | v >> 2); }

/* 8-bit indices through palette p (file RGB, entries 0 and 255 swapped as the loader does);
 * the colour key is the swapped entry 0 (DDSetColorKey(surface, pal[0]), pkids) */
static void indexed(uint8_t *o, const uint8_t *px, size_t n, const uint8_t *pal)
{
    size_t i;

    for (i = 0; i < n; i++, o += 4) {
        unsigned k = px[i], e = k == 0 ? 255 : k == 255 ? 0 : k;

        if (!pal) {
            o[0] = o[1] = o[2] = (uint8_t)k;
            o[3] = 255;
            continue;
        }
        o[0] = pal[3 * e];
        o[1] = pal[3 * e + 1];
        o[2] = pal[3 * e + 2];
        /* the key compares the 16-bit converted colours */
        {
            const uint8_t *z = pal + 3 * 255;
            unsigned a = (o[0] >> 3) << 11 | (o[1] >> 3) << 6 | o[2] >> 3;
            unsigned zk = (z[0] >> 3) << 11 | (z[1] >> 3) << 6 | z[2] >> 3;

            o[3] = a == zk ? 0 : 255;
        }
    }
}

/* GL_RGBA + GL_UNSIGNED_INT_8_8_8_8: bytes A B G R */
static void abgr(uint8_t *o, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++, o += 4, p += 4) {
        o[0] = p[3];
        o[1] = p[2];
        o[2] = p[1];
        o[3] = p[0];
    }
}

/* GL_RGBA + GL_UNSIGNED_SHORT_5_5_5_1: R 15-11, G 10-6, B 5-1, A bit 0 (also ImageLoadBMP's software layout) */
static void rgba5551(uint8_t *o, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++, o += 4, p += 2) {
        unsigned v = igsa_le16(p);

        o[0] = x5(v >> 11 & 31);
        o[1] = x5(v >> 6 & 31);
        o[2] = x5(v >> 1 & 31);
        o[3] = v & 1 ? 255 : 0;
    }
}

/* E1000 16-bit surfaces: R 15-11, G 10-5, B 4-0 (the loaders write G>>3<<6); colour key 0 */
static void rgb565k(uint8_t *o, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++, o += 4, p += 2) {
        unsigned v = igsa_le16(p), g = v >> 5 & 63;

        o[0] = x5(v >> 11 & 31);
        o[1] = (uint8_t)(g << 2 | g >> 4);
        o[2] = x5(v & 31);
        o[3] = v ? 255 : 0;
    }
}

/* E2000 software 32-bit (TgaMergeBytes): R>>3, G>>3, B>>3, A */
static void rgb5a8(uint8_t *o, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++, o += 4, p += 4) {
        o[0] = x5(p[0] & 31);
        o[1] = x5(p[1] & 31);
        o[2] = x5(p[2] & 31);
        o[3] = p[3];
    }
}

static int emit_rgba(bank *b, int idx, unsigned w, unsigned h, const uint8_t *px, size_t n, size_t bpp,
                     void (*conv)(uint8_t *, const uint8_t *, size_t))
{
    char name[64];
    uint8_t *rgba;
    int r;

    if (!w || !h || w > 16384 || h > 16384)
        return igsa_fail(b->c, "image %d: size %ux%u", idx, w, h);
    if ((size_t)w * h * bpp > n)
        return igsa_fail(b->c, "image %d: %ux%u wants %zu bytes, has %zu", idx, w, h, (size_t)w * h * bpp, n);
    if (!(rgba = malloc((size_t)w * h * 4)))
        return igsa_fail(b->c, "out of memory");
    conv(rgba, px, (size_t)w * h);
    snprintf(name, sizeof(name), "images/%04d.png", idx);
    r = igsa_emit_png(b->c, name, (int)w, (int)h, rgba);
    free(rgba);
    return r;
}

static const uint8_t *palette_of(bank *b, int idx)
{
    int p = idx < b->cap && b->imgpal[idx] >= 0 ? b->imgpal[idx] : 0;

    return p < b->npal ? b->c->d + b->pals[p] : NULL;
}

static int emit_indexed(bank *b, int idx, unsigned w, unsigned h, const uint8_t *px, size_t n)
{
    char name[64];
    uint8_t *rgba;
    int r;

    if (!w || !h || w > 16384 || h > 16384)
        return igsa_fail(b->c, "image %d: size %ux%u", idx, w, h);
    if ((size_t)w * h > n)
        return igsa_fail(b->c, "image %d: %ux%u wants %zu bytes, has %zu", idx, w, h, (size_t)w * h, n);
    if (!(rgba = malloc((size_t)w * h * 4)))
        return igsa_fail(b->c, "out of memory");
    indexed(rgba, px, (size_t)w * h, palette_of(b, idx));
    snprintf(name, sizeof(name), "images/%04d.png", idx);
    r = igsa_emit_png(b->c, name, (int)w, (int)h, rgba);
    free(rgba);
    return r;
}

/* ImageLoadPCXData: 128-byte PCX header, then RLE rows of bytes-per-line, w = xmax-xmin+1 */
static int pcx_rle(bank *b, int idx, const uint8_t *p, size_t n)
{
    unsigned w, h, bpl, y;
    size_t i = 128;
    uint8_t *px, *line;
    int r;

    if (n < 128)
        return igsa_fail(b->c, "image %d: PCX header truncated", idx);
    w = (unsigned)((int16_t)igsa_le16(p + 8) - (int16_t)igsa_le16(p + 4) + 1);
    h = (unsigned)((int16_t)igsa_le16(p + 10) - (int16_t)igsa_le16(p + 6) + 1);
    bpl = igsa_le16(p + 66);
    if (b->pass == 2) {
        igsa_jint(b->c, "w", w);
        igsa_jint(b->c, "h", h);
        igsa_jint(b->c, "bytes_per_line", bpl);
    }
    if (!w || !h || w > 16384 || h > 16384 || bpl < w)
        return igsa_fail(b->c, "image %d: PCX %ux%u, %u bytes per line", idx, w, h, bpl);
    px = malloc((size_t)w * h);
    line = malloc(bpl + 64);
    if (!px || !line) {
        free(px);
        free(line);
        return igsa_fail(b->c, "out of memory");
    }
    for (y = 0; y < h; y++) {
        unsigned k = 0;

        while (k < bpl) {
            unsigned v, cnt = 1;

            if (i >= n)
                goto trunc;
            v = p[i++];
            if ((v & 0xC0) == 0xC0) {
                cnt = v & 0x3F;
                if (i >= n)
                    goto trunc;
                v = p[i++];
            }
            while (cnt--) {
                if (k < bpl + 64)
                    line[k] = (uint8_t)v;
                k++;
            }
        }
        memcpy(px + (size_t)y * w, line, w);
    }
    free(line);
    r = emit_indexed(b, idx, w, h, px, (size_t)w * h);
    free(px);
    return r;
trunc:
    free(px);
    free(line);
    return igsa_fail(b->c, "image %d: PCX data truncated", idx);
}

static int unlz(bank *b, int idx, const uint8_t *p, size_t n, uint8_t **u, size_t *un)
{
    if (igsa_lzss(p, n, u, un))
        return igsa_fail(b->c, "out of memory");
    if (b->pass == 2)
        igsa_juint(b->c, "unpacked", *un);
    (void)idx;
    return 0;
}

/*
 * An image chunk at o (after tag and size).  Returns the bytes it takes, or 0
 * (failed).  Pass 2 decodes and emits it.
 */
static size_t image(bank *b, const char *tag, size_t o, uint32_t size)
{
    igsa_ctx *c = b->c;
    const uint8_t *d = c->d;
    int idx = b->nimg, f = 0;
    size_t start = o, k;
    int two = b->pass == 2, r = 0;

    if (grow_img(b))
        return 0;
    if (b->versioned && strncmp(tag, "DDS_", 4)) {
        if (!need(b, o, 1)) {
            igsa_fail(c, "%s at 0x%zx: no flag byte", tag, o - 12);
            return 0;
        }
        f = d[o++];
    }
    if (two) {
        igsa_jint(c, "image", idx);
        if (b->versioned && strncmp(tag, "DDS_", 4))
            igsa_jint(c, "flag", f);
    }
    if (!strcmp(tag, "PCX_DATA")) {
        /* ImageLoadPCXData: size bytes */
        if (!need(b, o, size)) {
            igsa_fail(c, "PCX_DATA at 0x%zx: %u bytes past the end", start - 12, size);
            return 0;
        }
        if (two && pcx_rle(b, idx, d + o, size))
            return 0;
        o += size;
    } else if (!strcmp(tag, "PCX_LZSS") || !strcmp(tag, "TGA_LZSS") || !strcmp(tag, "BMP_LZSS") ||
               !strcmp(tag, "PK0_LZSS")) {
        /* ImageLoad{PCX,TGA,BMP,Zero}Lzss: w(u16) h(u16) [PK0: unpacked(u32)] LZSS */
        unsigned w, h;
        size_t hl = tag[0] == 'P' && tag[1] == 'K' ? 8 : 4;
        uint8_t *u;
        size_t un;

        if (size < hl || !need(b, o, size)) {
            igsa_fail(c, "%s at 0x%zx: size %u", tag, start - 12, size);
            return 0;
        }
        w = igsa_le16(d + o);
        h = igsa_le16(d + o + 2);
        if (two) {
            igsa_jint(c, "w", w);
            igsa_jint(c, "h", h);
            if (hl == 8)
                igsa_juint(c, "unpacked_size", igsa_le32(d + o + 4));
        }
        if (two) {
            if (unlz(b, idx, d + o + hl, size - hl, &u, &un))
                return 0;
            if (tag[0] == 'P' && tag[1] == 'C')
                r = emit_indexed(b, idx, w, h, u, un);
            else if (tag[0] == 'B') {
                igsa_jstr(c, "pixels", "RGB565, key 0");
                r = emit_rgba(b, idx, w, h, u, un, 2, rgb565k);
            } else if (tag[0] == 'T') {
                /* E2000 TgaMergeBytes layout (4 bytes) or E1000 A4R4G4B4 (ImageLoadTGALzss, pkids) */
                if (un >= (size_t)w * h * 4) {
                    igsa_jstr(c, "pixels", "R5 G5 B5 A8 bytes");
                    r = emit_rgba(b, idx, w, h, u, un, 4, rgb5a8);
                } else {
                    igsa_jstr(c, "pixels", "ARGB4444");
                    r = emit_rgba(b, idx, w, h, u, un, 2, igsa_argb4444);
                }
            } else {
                /* PK0: the "zero" type; no bank uses it and its blitters are empty: raw bytes */
                char name[64];

                snprintf(name, sizeof(name), "images/%04d.pk0", idx);
                r = igsa_emit_raw(c, name, u, un);
            }
            free(u);
            if (r)
                return 0;
        }
        o += size;
    } else if (!strcmp(tag, "TGA_DATA") || !strcmp(tag, "BMP_DATA")) {
        /* ImageLoad{TGA,BMP}Data: w(i32) h(i32) pixels (4 or 2 bytes), whatever the size says */
        int bpp = tag[0] == 'T' ? 4 : 2;
        uint32_t w, h;

        if (!need(b, o, 8)) {
            igsa_fail(c, "%s at 0x%zx: truncated", tag, start - 12);
            return 0;
        }
        w = igsa_le32(d + o);
        h = igsa_le32(d + o + 4);
        k = (size_t)w * h * bpp;
        if (w > 16384 || h > 16384 || !need(b, o + 8, k)) {
            igsa_fail(c, "%s at 0x%zx: %ux%u past the end", tag, start - 12, w, h);
            return 0;
        }
        if (two) {
            igsa_jint(c, "w", w);
            igsa_jint(c, "h", h);
            if (emit_rgba(b, idx, w, h, d + o + 8, k, (size_t)bpp, bpp == 4 ? rgb5a8 : rgba5551))
                return 0;
        }
        o += 8 + k;
    } else if (!strcmp(tag, "TGA_OPGL") || !strcmp(tag, "BMP_OPGL") || !strcmp(tag, "TGA_OPSS") ||
               !strcmp(tag, "BMP_OPSS")) {
        /* OpenGLLoad{TGA,BMP}Data[Lzss]: show_w show_h w h (i32), then w*h pixels or size-8 LZSS */
        int bpp = tag[0] == 'T' ? 4 : 2, lz = tag[6] == 'S';
        uint32_t w, h;

        if (!need(b, o, 16)) {
            igsa_fail(c, "%s at 0x%zx: truncated", tag, start - 12);
            return 0;
        }
        w = igsa_le32(d + o + 8);
        h = igsa_le32(d + o + 12);
        k = lz ? (size >= 8 ? size - 8 : 0) : (size_t)w * h * bpp;
        if (w > 16384 || h > 16384 || (lz && size < 8) || !need(b, o + 16, k)) {
            igsa_fail(c, "%s at 0x%zx: %ux%u, %zu bytes past the end", tag, start - 12, w, h, k);
            return 0;
        }
        if (two) {
            igsa_jint(c, "show_w", (int32_t)igsa_le32(d + o));
            igsa_jint(c, "show_h", (int32_t)igsa_le32(d + o + 4));
            igsa_jint(c, "w", w);
            igsa_jint(c, "h", h);
            if (lz) {
                uint8_t *u;
                size_t un;

                if (unlz(b, idx, d + o + 16, k, &u, &un))
                    return 0;
                r = emit_rgba(b, idx, w, h, u, un, (size_t)bpp, bpp == 4 ? abgr : rgba5551);
                free(u);
            } else
                r = emit_rgba(b, idx, w, h, d + o + 16, k, (size_t)bpp, bpp == 4 ? abgr : rgba5551);
            if (r)
                return 0;
        }
        o += 16 + k;
    } else {
        /* DDS_OPGL / DDS_OPSS (OpenGLLoadDDSData): a DDS file, raw or LZSS */
        if (!need(b, o, size)) {
            igsa_fail(c, "%s at 0x%zx: size %u past the end", tag, start - 12, size);
            return 0;
        }
        if (two) {
            char name[64];
            const uint8_t *p = d + o;
            uint8_t *u = NULL;
            size_t pn = size;

            if (tag[6] == 'S') {
                if (unlz(b, idx, d + o, size, &u, &pn))
                    return 0;
                p = u;
            }
            if (pn >= 128 && !memcmp(p, "DDS ", 4)) {
                igsa_jint(c, "w", igsa_le32(p + 16));
                igsa_jint(c, "h", igsa_le32(p + 12));
                igsa_jhex(c, "fourcc", igsa_le32(p + 84));
                igsa_jint(c, "mipmaps", igsa_le32(p + 28));
            }
            snprintf(name, sizeof(name), "images/%04d.png", idx);
            r = igsa_emit_dds(c, name, p, pn);
            free(u);
            if (r)
                return 0;
        }
        o += size;
    }
    b->nimg++;
    return o - start;
}

/* ---- actions ------------------------------------------------------------------------------- */

static void dump_steps(igsa_ctx *c, const uint8_t *p, size_t n)
{
    size_t i;

    igsa_jarr(c, "steps");
    for (i = 0; i + 10 <= n; i += 10) {
        const uint8_t *s = p + i;

        igsa_jobj(c, NULL);
        switch (s[0]) {     /* ActionLoadNextStep */
        case 0:
            igsa_jstr(c, "op", "show");
            igsa_jint(c, "time", s[2]);
            igsa_jint(c, "picture", s[3]);
            igsa_jint(c, "layer", s[4] & 31);
            igsa_jint(c, "palette", s[5] & 31);
            igsa_jbool(c, "hidden", s[5] >> 5 & 1);
            igsa_jbool(c, "flip_v", s[5] >> 6 & 1);
            igsa_jbool(c, "flip_h", s[5] >> 7 & 1);
            igsa_jint(c, "dx", (int16_t)igsa_le16(s + 6));
            igsa_jint(c, "dy", (int16_t)igsa_le16(s + 8));
            break;
        case 1:
            igsa_jstr(c, "op", "loop");
            igsa_jint(c, "count", igsa_le16(s + 2));
            break;
        case 2:
            igsa_jstr(c, "op", "next");
            break;
        case 3:
            igsa_jstr(c, "op", "shadow");
            igsa_jint(c, "count", s[2]);
            igsa_jint(c, "palette", igsa_le16(s + 4));
            break;
        case 4:
            igsa_jstr(c, "op", "shadow_off");
            break;
        case 6:
            igsa_jstr(c, "op", "layer");
            igsa_jint(c, "value", igsa_le16(s + 8));
            break;
        case 9:
            igsa_jstr(c, "op", "sound");
            igsa_jint(c, "sound", (int16_t)igsa_le16(s + 6));
            break;
        case 10:
            igsa_jstr(c, "op", "spawn");
            igsa_jint(c, "action", s[2] - 1);
            igsa_jint(c, "group", s[3]);
            igsa_jint(c, "layer", (int)igsa_le16(s + 4) - 32000);
            igsa_jint(c, "dx", (int16_t)igsa_le16(s + 6));
            igsa_jint(c, "dy", (int16_t)igsa_le16(s + 8));
            break;
        case 12:
            igsa_jstr(c, "op", "callback");
            igsa_jint(c, "value", igsa_le16(s + 2));
            break;
        case 14:
            igsa_jstr(c, "op", "alpha");
            igsa_jint(c, "alpha", s[2] == 0x80 ? 255 : (uint8_t)(2 * s[2]));
            break;
        case 15:
            igsa_jstr(c, "op", "zoom");
            igsa_jint(c, "sx", igsa_le16(s + 2));     /* 32 = 1.0 */
            igsa_jint(c, "sy", igsa_le16(s + 4));
            break;
        case 16:
            igsa_jstr(c, "op", "rotate");
            igsa_jint(c, "degrees", (int16_t)igsa_le16(s + 2));
            break;
        default:
            igsa_jint(c, "op", s[0]);
            igsa_jbytes(c, "args", s + 1, 9);
        }
        igsa_jend(c);
    }
    if (i < n)
        igsa_jbytes(c, NULL, p + i, n - i);
    igsa_jend(c);
}

/* ACT_DATA at o (the head chunk's payload, size bytes).  Returns the bytes after o it takes, or 0. */
static size_t action(bank *b, size_t o, uint32_t size)
{
    igsa_ctx *c = b->c;
    const uint8_t *d = c->d;
    size_t start = o, fr0;
    uint8_t head[0x2C];
    int32_t frames, pics, i;
    size_t nl;
    int two = b->pass == 2;
    int *picimg = NULL;

    if (!need(b, o, size)) {
        igsa_fail(c, "ACT_DATA at 0x%zx: size %u past the end", o - 12, size);
        return 0;
    }
    memset(head, 0, sizeof(head));
    memcpy(head, d + o, size < sizeof(head) ? size : sizeof(head));
    o += size;
    frames = (int32_t)igsa_le32(head + 12);
    pics = (int32_t)igsa_le32(head + 16);
    if (frames < 0 || pics < 0 || frames > 256 || pics > 256) {    /* 256 pointers each in the loaded action */
        igsa_fail(c, "action %d: %d frames, %d pictures", b->nact, frames, pics);
        return 0;
    }
    for (nl = 0; 21 + nl < sizeof(head) && head[21 + nl]; nl++)
        ;
    if (two) {
        igsa_jint(c, "action", b->nact);
        igsa_jbig5(c, "name", (const char *)head + 21, nl);
        igsa_juint(c, "kind", igsa_le32(head));
        igsa_jint(c, "x", (int32_t)igsa_le32(head + 4));
        igsa_jint(c, "y", (int32_t)igsa_le32(head + 8));
        igsa_jint(c, "flag", head[20]);     /* nonzero: display flag 0x40 (action_vActionActive) */
        if (c->mode == IGSA_LIST) {
            char info[64];

            snprintf(info, sizeof(info), "%d frames, %d pictures", frames, pics);
            fprintf(c->out, "%-48s %-5s %s\n", "(action)", "act", info);
        }
        igsa_jarr(c, "frames");
    }
    fr0 = o;
    for (i = 0; i < frames; i++) {
        uint32_t len;

        if (!need(b, o, 12) || !need(b, o + 12, len = igsa_le32(d + o + 8))) {
            igsa_fail(c, "action %d frame %d: truncated", b->nact, i);
            return 0;
        }
        if (two) {
            char tag[9];

            igsa_str(tag, d + o, 8);
            igsa_jobj(c, NULL);
            igsa_jstr(c, "tag", tag);
            dump_steps(c, d + o + 12, len);
            igsa_jend(c);
        }
        o += 12 + len;
    }
    if (two) {
        igsa_jend(c);
        igsa_jarr(c, "pictures");
    }
    if (pics && !(picimg = malloc(sizeof(int) * (size_t)pics))) {
        igsa_fail(c, "out of memory");
        return 0;
    }
    for (i = 0; i < pics; i++) {
        uint32_t len, len2;
        uint8_t ph[16];
        unsigned nh, k;

        if (!need(b, o, 12) || !need(b, o + 12, len = igsa_le32(d + o + 8))) {
            free(picimg);
            igsa_fail(c, "action %d picture %d: truncated", b->nact, i);
            return 0;
        }
        memset(ph, 0, sizeof(ph));
        memcpy(ph, d + o + 12, len < sizeof(ph) ? len : sizeof(ph));
        o += 12 + len;
        if (!need(b, o, 12) || !need(b, o + 12, len2 = igsa_le32(d + o + 8))) {
            free(picimg);
            igsa_fail(c, "action %d picture %d: hit chunk truncated", b->nact, i);
            return 0;
        }
        picimg[i] = igsa_le16(ph + 6);
        nh = ph[8];
        if (nh * 10u > len2) {
            free(picimg);
            igsa_fail(c, "action %d picture %d: %u hits in %u bytes", b->nact, i, nh, len2);
            return 0;
        }
        if (two) {
            /* PictureHead: hot x, hot y, flips, image, hit count */
            igsa_jobj(c, NULL);
            igsa_jint(c, "hot_x", (int16_t)igsa_le16(ph));
            igsa_jint(c, "hot_y", (int16_t)igsa_le16(ph + 2));
            igsa_jint(c, "flip_h", ph[4]);
            igsa_jint(c, "flip_v", ph[5]);
            igsa_jint(c, "image", igsa_le16(ph + 6));
            if (len > 9)
                igsa_jbytes(c, "rest", ph + 9, (len < sizeof(ph) ? len : sizeof(ph)) - 9);
            igsa_jarr(c, "hits");   /* ActionCrash: x, y, w, h, kind */
            for (k = 0; k < nh; k++) {
                const uint8_t *q = d + o + 12 + 10 * k;

                igsa_jobj(c, NULL);
                igsa_jint(c, "x", (int16_t)igsa_le16(q));
                igsa_jint(c, "y", (int16_t)igsa_le16(q + 2));
                igsa_jint(c, "w", (int16_t)igsa_le16(q + 4));
                igsa_jint(c, "h", (int16_t)igsa_le16(q + 6));
                igsa_jint(c, "kind", (int16_t)igsa_le16(q + 8));
                igsa_jend(c);
            }
            igsa_jend(c);
            igsa_jend(c);
        }
        o += 12 + len2;
    }
    if (two)
        igsa_jend(c);
    else {
        /* pkids ActionGroupLoad: an 8-bit image takes the palette of the first "show" step that draws it */
        size_t q = fr0;

        for (i = 0; i < frames; i++) {
            uint32_t len = igsa_le32(d + q + 8);
            size_t s;

            for (s = 0; s + 10 <= len; s += 10) {
                const uint8_t *st = d + q + 12 + s;

                if (st[0])
                    continue;
                if (st[3] < pics) {
                    int img = picimg[st[3]];

                    if (ensure(b, img)) {
                        free(picimg);
                        return 0;
                    }
                    if (b->imgpal[img] < 0)
                        b->imgpal[img] = (signed char)(st[5] & 31);
                }
                break;
            }
            q += 12 + len;
        }
    }
    free(picimg);
    b->nact++;
    c->entries++;
    return o - start;
}

/* ---- the bank ------------------------------------------------------------------------------ */

static int walk(bank *b)
{
    igsa_ctx *c = b->c;
    const uint8_t *d = c->d;
    size_t o = 12;
    int ended = 0, two = b->pass == 2;
    char ver[13] = "";

    b->nimg = b->npal = b->nact = 0;
    b->versioned = 0;
    if (need(b, o, 12) && !memcmp(d + o, " VERSION", 8)) {
        if (!need(b, o, 28))
            return igsa_fail(c, "VERSION chunk truncated");
        memcpy(ver, d + o + 12, 12);
        ver[12] = 0;
        b->versioned = 1;
        o += 28;
    }
    if (two) {
        igsa_jobj(c, NULL);
        igsa_jstr(c, "format", "TSGROM01");
        igsa_juint(c, "header_size", igsa_le32(d + 8));
        if (b->versioned)
            igsa_jstr(c, "version", ver);
        igsa_jarr(c, "chunks");
    }
    while (need(b, o, 12)) {
        char tag[9];
        uint32_t size;
        size_t at = o, k;

        memcpy(tag, d + o, 8);
        tag[8] = 0;
        size = igsa_le32(d + o + 8);
        o += 12;
        if (two) {
            igsa_jobj(c, NULL);
            igsa_jstr(c, "tag", tag);
            igsa_jhex(c, "offset", at);
            igsa_juint(c, "size", size);
        }
        if (!strcmp(tag, "FILE_END")) {
            if (two)
                igsa_jend(c);
            ended = 1;
            break;
        } else if (!strcmp(tag, "BASEDATA")) {
            if (!need(b, o, size))
                return igsa_fail(c, "BASEDATA at 0x%zx: size %u", at, size);
            if (two && size >= 12) {
                igsa_juint(c, "palettes", igsa_le32(d + o));
                igsa_juint(c, "images", igsa_le32(d + o + 4));
                igsa_juint(c, "actions", igsa_le32(d + o + 8));
            }
            o += size;
        } else if (!strcmp(tag, "ACTINDEX")) {
            if (size < 2 || !need(b, o, size))
                return igsa_fail(c, "ACTINDEX at 0x%zx: size %u", at, size);
            if (two)
                igsa_jint(c, "id", igsa_le16(d + o));   /* the group id, < 2000 */
            o += size;
        } else if (!strcmp(tag, "PALETTE1")) {
            /* 256 x RGB8 (the loader reads 768 bytes whatever the size) */
            if (!need(b, o, 768))
                return igsa_fail(c, "PALETTE1 at 0x%zx: truncated", at);
            if (b->npal >= b->palcap) {
                int nc = b->palcap ? b->palcap * 2 : 16;
                size_t *t = realloc(b->pals, sizeof(size_t) * (size_t)nc);

                if (!t)
                    return igsa_fail(c, "out of memory");
                b->pals = t;
                b->palcap = nc;
            }
            b->pals[b->npal] = o;
            if (two) {
                int i;

                igsa_jint(c, "palette", b->npal);
                igsa_jarr(c, "rgb");
                for (i = 0; i < 256; i++)
                    igsa_jhex(c, NULL, (unsigned)d[o + 3 * i] << 16 | d[o + 3 * i + 1] << 8 | d[o + 3 * i + 2]);
                igsa_jend(c);
            }
            b->npal++;
            o += 768;
        } else if (strlen(tag) == 8 && tag[3] == '_' &&
                   (!memcmp(tag, "PCX", 3) || !memcmp(tag, "PK0", 3) || !memcmp(tag, "TGA", 3) ||
                    !memcmp(tag, "BMP", 3) || !memcmp(tag, "DDS", 3)) &&
                   (!strcmp(tag + 4, "DATA") || !strcmp(tag + 4, "LZSS") || !strcmp(tag + 4, "OPGL") ||
                    !strcmp(tag + 4, "OPSS"))) {
            if (!strcmp(tag, "PCX_OPGL") || !strcmp(tag, "PCX_OPSS") || !strcmp(tag, "PK0_DATA") ||
                !strcmp(tag, "PK0_OPGL") || !strcmp(tag, "PK0_OPSS") || !strcmp(tag, "DDS_DATA") ||
                !strcmp(tag, "DDS_LZSS")) {
                /* not an image tag of the loader: skipped */
                if (!need(b, o, size))
                    return igsa_fail(c, "%s at 0x%zx: size %u past the end", tag, at, size);
                o += size;
            } else {
                if (!(k = image(b, tag, o, size)))
                    return -1;
                o += k;
            }
        } else if (!strcmp(tag, "ACT_DATA")) {
            if (!(k = action(b, o, size)))
                return -1;
            o += k;
        } else if (!strcmp(tag, "MTV_INAC")) {
            /* pkids: read into intact_rMtvData (the MTV interaction table) */
            if (!need(b, o, size))
                return igsa_fail(c, "MTV_INAC at 0x%zx: size %u", at, size);
            if (two) {
                uint32_t i;

                igsa_jarr(c, "words");
                for (i = 0; i + 4 <= size; i += 4)
                    igsa_jint(c, NULL, (int32_t)igsa_le32(d + o + i));
                igsa_jend(c);
                if (size % 4)
                    igsa_jbytes(c, "tail", d + o + size - size % 4, size % 4);
            }
            o += size;
        } else {
            /* "  SOUND" (the tool's sound list path) and anything else: skipped by the loader */
            if (!need(b, o, size))
                return igsa_fail(c, "%s at 0x%zx: size %u past the end", tag, at, size);
            if (two) {
                if (!strcmp(tag, "   SOUND") || !strcmp(tag, "  SOUND ")) {
                    size_t l = 0;

                    while (l < size && d[o + l])
                        l++;
                    igsa_jbig5(c, "path", (const char *)d + o, l);
                } else if (size)
                    igsa_jbytes(c, "data", d + o, size > 256 ? 256 : size);
            }
            o += size;
        }
        if (two)
            igsa_jend(c);
    }
    if (two) {
        igsa_jend(c);
        igsa_jint(c, "images", b->nimg);
        igsa_jint(c, "palettes", b->npal);
        igsa_jint(c, "actions", b->nact);
        igsa_jbool(c, "file_end", ended);
        igsa_juint(c, "trailing", c->n - o);
        igsa_jend(c);
    }
    if (!ended)
        return igsa_fail(c, "no FILE_END (stopped at 0x%zx)", o);
    return 0;
}

static int parse(igsa_ctx *c)
{
    bank b;
    int r, e;

    memset(&b, 0, sizeof(b));
    b.c = c;
    b.pass = 1;
    e = c->entries;
    r = walk(&b);
    if (!r) {
        c->entries = e;
        b.pass = 2;
        r = walk(&b);
    }
    free(b.imgpal);
    free(b.pals);
    return r;
}

const igsa_format igsa_fmt_tsgrom = { "tsgrom", "TSGROM01 image/action bank (E2000/E1000 2D engine)", probe, parse };
