/*
 * The E2000/E1000 2D engine's 1-bit fonts (system/font_*.rom, font_l24.gb,
 * STDFONT.15): headerless glyph arrays, rows top-down, MSB = leftmost pixel.
 * TextInit / TextChangeFont / TextChangAsciiFont / TextReadWord / Big5Code
 * (sship 0x80595A0..0x805A100).  wiki Format-TSGFONT.
 *
 *   font_[mkls]24.rom, font_l24.gb  24x24, 72 bytes, Big5 lead 0xA4.. (13094 glyphs)
 *   font_e24.rom                    24x24, Big5 lead 0xA1..0xA3 (symbols, 408 glyphs)
 *   font_a24.rom                    16x24, 48 bytes, single-byte codes 0..255
 *   font_a15.rom                    8x15, 15 bytes, single-byte codes 0..255
 *   STDFONT.15                      16x15, 30 bytes, Big5 lead 0xA4.. (no loader references it)
 *
 * Big5 glyph index = 157 * (lead - base) + (trail < 0x7F ? trail - 0x40 : trail - 0x62).
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"


enum { STD24, SPC24, ASC24, ASC15, STD15 };

static const struct kind {
    const char *name;
    int w, h, bytes, base;          /* base: Big5 lead of glyph 0, 0 = single-byte codes */
} kinds[] = {
    [STD24] = { "big5 24x24", 24, 24, 72, 0xA4 },
    [SPC24] = { "big5 symbols 24x24", 24, 24, 72, 0xA1 },
    [ASC24] = { "ascii 16x24", 16, 24, 48, 0 },
    [ASC15] = { "ascii 8x15", 8, 15, 15, 0 },
    [STD15] = { "big5 16x15", 16, 15, 30, 0xA4 },
};

static int lc_eq(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        int x = a[i], y = b[i];

        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
    }
    return 1;
}

static int kind_of(const char *path, size_t n)
{
    const char *b = path, *p;
    size_t l;
    int k = -1;

    if (!path)
        return -1;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    l = strlen(b);
    if (l == 10 && lc_eq(b, "stdfont.15", 10))
        k = STD15;
    else if (l == 12 && lc_eq(b, "font_", 5) && lc_eq(b + 8, ".rom", 4)) {
        if (lc_eq(b + 5, "a15", 3))
            k = ASC15;
        else if (lc_eq(b + 5, "a24", 3))
            k = ASC24;
        else if (lc_eq(b + 5, "e24", 3))
            k = SPC24;
        else if (b[6] == '2' && b[7] == '4')
            k = STD24;
    } else if (l == 11 && lc_eq(b, "font_", 5) && lc_eq(b + 6, "24.gb", 5))
        k = STD24;      /* TextOutRomFile's output, the same layout */
    if (k < 0 || !n || n % (size_t)kinds[k].bytes)
        return -1;
    if (kinds[k].base == 0 && n / (size_t)kinds[k].bytes > 256)
        return -1;
    return k;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)d;
    /* headerless: the name and a whole number of glyphs */
    return kind_of(path, n) >= 0 ? 60 : 0;
}

static int parse(igsa_ctx *c)
{
    int k = kind_of(c->path, c->n), cols, rows, i, x, y;
    const struct kind *kd;
    size_t cnt;
    uint8_t *sheet;
    int sw, sh, rb, r;

    if (k < 0)
        return igsa_fail(c, "not a font size/name (%zu bytes)", c->n);
    kd = &kinds[k];
    cnt = c->n / (size_t)kd->bytes;
    cols = kd->base ? 157 : 16;     /* Big5: one lead byte per sheet row */
    rows = (int)((cnt + (size_t)cols - 1) / (size_t)cols);
    sw = cols * kd->w;
    sh = rows * kd->h;
    rb = kd->bytes / kd->h;
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "TSG 1-bit font");
    igsa_jstr(c, "kind", kd->name);
    igsa_jint(c, "glyph_w", kd->w);
    igsa_jint(c, "glyph_h", kd->h);
    igsa_jint(c, "glyph_bytes", kd->bytes);
    igsa_juint(c, "glyphs", cnt);
    igsa_jstr(c, "sheet", "glyphs.png");
    igsa_jint(c, "sheet_columns", cols);
    if (kd->base)
        igsa_jhex(c, "first_lead", (unsigned)kd->base);
    igsa_jarr(c, "glyph");
    for (i = 0; i < (int)cnt; i++) {
        igsa_jobj(c, NULL);
        if (kd->base) {
            unsigned lead = (unsigned)(kd->base + i / 157), col = (unsigned)(i % 157);
            unsigned trail = col < 63 ? 0x40 + col : 0x62 + col;
            char mb[2];

            mb[0] = (char)lead;
            mb[1] = (char)trail;
            igsa_jhex(c, "code", lead << 8 | trail);
            if (lead < 0xFF)
                igsa_jbig5(c, "char", mb, 2);
        } else
            igsa_jhex(c, "code", (unsigned)i);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    if (!(sheet = calloc((size_t)sw * sh, 4)))
        return igsa_fail(c, "out of memory");
    for (i = 0; i < (int)cnt; i++) {
        const uint8_t *g = c->d + (size_t)i * kd->bytes;
        int ox = (i % cols) * kd->w, oy = (i / cols) * kd->h;

        for (y = 0; y < kd->h; y++)
            for (x = 0; x < kd->w; x++) {
                uint8_t *p = sheet + (((size_t)(oy + y) * sw) + ox + x) * 4;

                p[0] = p[1] = p[2] = 255;
                p[3] = g[y * rb + x / 8] >> (7 - x % 8) & 1 ? 255 : 0;
            }
    }
    r = igsa_emit_png(c, "glyphs.png", sw, sh, sheet);
    free(sheet);
    return r;
}

const igsa_format igsa_fmt_tsgfont = { "tsgfont", "TSG 1-bit font (font_*.rom, font_l24.gb, STDFONT.15): Big5/ASCII glyphs", probe, parse };
