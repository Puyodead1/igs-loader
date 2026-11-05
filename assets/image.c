/*
 * Standard image payloads found inside IGS containers: DDS (decoded here),
 * TGA (stb_image) and PCX.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

int igsa_emit_dds(igsa_ctx *c, const char *name, const uint8_t *d, size_t n)
{
    unsigned w, h, flags, fourcc, bits, rm, gm, bm, am;
    const uint8_t *px;
    size_t need, i;
    uint8_t *rgba;
    int r;

    if (n < 128 || memcmp(d, "DDS ", 4) || igsa_le32(d + 4) != 124)
        return igsa_fail(c, "%s: not a DDS", name);
    h = igsa_le32(d + 12);
    w = igsa_le32(d + 16);
    flags = igsa_le32(d + 80);
    fourcc = igsa_le32(d + 84);
    bits = igsa_le32(d + 88);
    rm = igsa_le32(d + 92);
    gm = igsa_le32(d + 96);
    bm = igsa_le32(d + 100);
    am = igsa_le32(d + 104);
    px = d + 128;
    if (!w || !h || w > 16384 || h > 16384)
        return igsa_fail(c, "%s: DDS size %ux%u", name, w, h);
    if (!(rgba = calloc((size_t)w * h, 4)))
        return igsa_fail(c, "out of memory");
    if (flags & 4) {            /* DDPF_FOURCC */
        int dxt = fourcc == 0x31545844 ? 1 : fourcc == 0x33545844 ? 3 : fourcc == 0x35545844 ? 5 : 0;

        if (!dxt) {
            free(rgba);
            return igsa_fail(c, "%s: DDS fourcc 0x%08x", name, fourcc);
        }
        if (igsa_dxt(rgba, px, n - 128, (int)w, (int)h, dxt)) {
            free(rgba);
            return igsa_fail(c, "%s: DDS %ux%u DXT%d truncated", name, w, h, dxt);
        }
    } else {
        unsigned bpp = bits / 8, sh[4], ln[4], m[4] = { rm, gm, bm, am }, k;

        need = (size_t)w * h * bpp;
        if (!bpp || bpp > 4 || need > n - 128) {
            free(rgba);
            return igsa_fail(c, "%s: DDS %ux%u %u-bit truncated", name, w, h, bits);
        }
        for (k = 0; k < 4; k++) {
            sh[k] = ln[k] = 0;
            if (m[k]) {
                while (!(m[k] >> sh[k] & 1))
                    sh[k]++;
                while (m[k] >> (sh[k] + ln[k]) & 1)
                    ln[k]++;
            }
        }
        for (i = 0; i < (size_t)w * h; i++) {
            uint32_t v = 0;

            for (k = 0; k < bpp; k++)
                v |= (uint32_t)px[i * bpp + k] << (8 * k);
            for (k = 0; k < 4; k++) {
                uint32_t x = ln[k] ? (v & m[k]) >> sh[k] : (k == 3 ? 255 : 0);
                unsigned l = ln[k];

                rgba[i * 4 + k] = (uint8_t)(l ? (l >= 8 ? x >> (l - 8) : x * 255 / ((1u << l) - 1)) : x);
            }
            if (!(flags & 1))   /* no DDPF_ALPHAPIXELS */
                rgba[i * 4 + 3] = 255;
        }
    }
    r = igsa_emit_png(c, name, (int)w, (int)h, rgba);
    free(rgba);
    return r;
}

int igsa_emit_tga(igsa_ctx *c, const char *name, const uint8_t *d, size_t n)
{
    int w, h, ch, r;
    uint8_t *p = stbi_load_from_memory(d, (int)n, &w, &h, &ch, 4);

    if (!p)
        return igsa_fail(c, "%s: TGA: %s", name, stbi_failure_reason());
    r = igsa_emit_png(c, name, w, h, p);
    stbi_image_free(p);
    return r;
}

int igsa_emit_pcx(igsa_ctx *c, const char *name, const uint8_t *d, size_t n)
{
    unsigned w, h, planes, bpl, bpp, x, y, p;
    size_t i = 128, row;
    uint8_t *line, *rgba;
    const uint8_t *pal = NULL;
    int r;

    if (n < 128 || d[0] != 10 || d[2] != 1)
        return igsa_fail(c, "%s: not an RLE PCX", name);
    bpp = d[3];
    w = igsa_le16(d + 8) - igsa_le16(d + 4) + 1;
    h = igsa_le16(d + 10) - igsa_le16(d + 6) + 1;
    planes = d[65];
    bpl = igsa_le16(d + 66);
    if (bpp != 8 || (planes != 1 && planes != 3 && planes != 4) || bpl < w || w > 16384 || h > 16384)
        return igsa_fail(c, "%s: PCX %u planes of %u bits", name, planes, bpp);
    if (planes == 1) {
        if (n < 769 || d[n - 769] != 12)
            return igsa_fail(c, "%s: PCX palette missing", name);
        pal = d + n - 768;
    }
    row = (size_t)bpl * planes;
    line = malloc(row);
    rgba = malloc((size_t)w * h * 4);
    if (!line || !rgba) {
        free(line);
        free(rgba);
        return igsa_fail(c, "out of memory");
    }
    for (y = 0; y < h; y++) {
        size_t k = 0;

        while (k < row) {
            unsigned v, cnt = 1;

            if (i >= n)
                goto trunc;
            v = d[i++];
            if ((v & 0xC0) == 0xC0) {
                cnt = v & 0x3F;
                if (i >= n)
                    goto trunc;
                v = d[i++];
            }
            while (cnt-- && k < row)
                line[k++] = (uint8_t)v;
        }
        for (x = 0; x < w; x++) {
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;

            if (pal)
                memcpy(o, pal + 3 * line[x], 3), o[3] = 255;
            else
                for (p = 0; p < 4; p++)
                    o[p] = p < planes ? line[p * bpl + x] : 255;
        }
    }
    free(line);
    r = igsa_emit_png(c, name, (int)w, (int)h, rgba);
    free(rgba);
    return r;
trunc:
    free(line);
    free(rgba);
    return igsa_fail(c, "%s: PCX truncated", name);
}
