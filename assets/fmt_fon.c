/*
 * zl bitmap fonts (*.fon: E2100 Fish engine system/, Speed Driver 2, We
 * Dance): zlFontFile::LoadFont.  wiki Format-FON.
 *
 *   u32 count; count x { u8 lead, trail, w, h; u8 alpha[w*h] }
 *   i32 cell_w, cell_h; i32 width[256] (advance of single-byte codes)
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

/* walks the glyphs; the offset of the trailer or 0 */
static size_t walk(const uint8_t *d, size_t n)
{
    size_t o = 4, i, cnt;

    if (n < 4 + 8 + 1024)
        return 0;
    cnt = igsa_le32(d);
    if (!cnt || cnt > 65536)
        return 0;
    for (i = 0; i < cnt; i++) {
        if (o + 4 > n)
            return 0;
        o += 4 + (size_t)d[o + 2] * d[o + 3];
    }
    return o + 8 + 1024 == n ? o : 0;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return walk(d, n) ? 90 : 0;
}

static int parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t t = walk(d, c->n), o = 4, i, cnt;
    int cw, ch, cell = 1, cols = 64, rows, k;
    uint8_t *sheet;

    if (!t)
        return igsa_fail(c, "glyphs do not end at the trailer");
    cnt = igsa_le32(d);
    cw = (int32_t)igsa_le32(d + t);
    ch = (int32_t)igsa_le32(d + t + 4);
    for (i = 0; i < cnt; i++, o += 4 + (size_t)d[o + 2] * d[o + 3]) {
        if (d[o + 2] > cell)
            cell = d[o + 2];
        if (d[o + 3] > cell)
            cell = d[o + 3];
    }
    rows = (int)((cnt + cols - 1) / cols);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "zl font");
    igsa_juint(c, "glyphs", cnt);
    igsa_jint(c, "cell_w", cw);
    igsa_jint(c, "cell_h", ch);
    igsa_jstr(c, "sheet", "glyphs.png");
    igsa_jint(c, "sheet_cell", cell);
    igsa_jint(c, "sheet_columns", cols);
    igsa_jarr(c, "widths");
    for (k = 0; k < 256; k++)
        igsa_jint(c, NULL, (int32_t)igsa_le32(d + t + 8 + 4 * k));
    igsa_jend(c);
    igsa_jarr(c, "glyph");
    if (!(sheet = calloc((size_t)cols * cell * rows * cell, 4)))
        return igsa_fail(c, "out of memory");
    for (i = 0, o = 4; i < cnt; i++) {
        int w = d[o + 2], h = d[o + 3], x, y, sx = (int)(i % cols) * cell, sy = (int)(i / cols) * cell;
        const uint8_t *a = d + o + 4;

        igsa_jobj(c, NULL);
        /* the code as the game looks it up: lead byte, then trail (0 for single-byte codes) */
        igsa_jhex(c, "code", d[o + 1] ? (unsigned)d[o] << 8 | d[o + 1] : d[o]);
        igsa_jint(c, "w", w);
        igsa_jint(c, "h", h);
        igsa_jend(c);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint8_t *p = sheet + (((size_t)(sy + y) * cols * cell) + sx + x) * 4;

                p[0] = p[1] = p[2] = 255;
                p[3] = a[y * w + x];
            }
        o += 4 + (size_t)w * h;
    }
    igsa_jend(c);
    igsa_jend(c);
    k = igsa_emit_png(c, "glyphs.png", cols * cell, rows * cell, sheet);
    free(sheet);
    return k;
}

const igsa_format igsa_fmt_fon = { "fon", "zl bitmap font: GBK glyphs (8-bit alpha) and a width table", probe, parse };
