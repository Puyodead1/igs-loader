/*
 * TSG_DEMO screen recordings (Pirate Revenge preview .dem, Panda Family
 * preview .demo; SaveStart writes them to /fec_data): "TSG_DEMO", then pages
 * of tag[8] size(u32).  GamePreviewActive / GamePreviewProcess (sship
 * 0x806F260 / 0x806F610, PF 0x806D4D0 / 0x806D8B0).  wiki Format-TSGDEMO.
 *
 *   PAGEDATA  runs on the previous frame: u16 n; bit 15 set: skip n & 0x7FFF
 *             pixels, else n XRGB1555 pixels follow
 *   LZSSPAGE  u32 unpacked size, LZSS of the same runs
 *   " FILEEND" end (the player rewinds)
 *
 * The frame size is the caller's (320x240 or 640x480 in Pirate Revenge,
 * 170x207 or 340x415 in Panda Family); it is taken from the pixels a page covers.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 8 && !memcmp(d, "TSG_DEMO", 8) ? 100 : 0;
}

static const int sizes[][2] = { { 320, 240 }, { 640, 480 }, { 170, 207 }, { 340, 415 } };

/* applies runs to fb (px pixels; NULL: count only); the pixels covered, or -1 */
static long runs(const uint8_t *p, size_t n, uint16_t *fb, size_t px)
{
    size_t i = 0, pos = 0;

    while (i + 2 <= n) {
        unsigned v = igsa_le16(p + i);

        i += 2;
        if (v & 0x8000)
            pos += v & 0x7FFF;
        else {
            if (i + 2 * (size_t)v > n)
                return -1;
            if (fb) {
                size_t k;

                for (k = 0; k < v && pos + k < px; k++)
                    fb[pos + k] = igsa_le16(p + i + 2 * k);
            }
            pos += v;
            i += 2 * (size_t)v;
        }
    }
    return i == n ? (long)pos : -1;
}

struct page { size_t off; uint32_t size; int lz; };

static int parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t o = 8, np = 0, cap = 0, i;
    struct page *pg = NULL;
    int ended = 0, w = 0, h = 0, r = 0;
    long maxpos = 0;
    uint16_t *fb = NULL;
    uint8_t *rgba = NULL;

    /* pass 1: the pages and the pixels they cover */
    while (o + 12 <= c->n) {
        uint32_t size = igsa_le32(d + o + 8);
        int lz;

        if (!memcmp(d + o, " FILEEND", 8)) {
            ended = 1;
            o += 12;
            break;
        }
        if (size > c->n - o - 12) {
            free(pg);
            return igsa_fail(c, "page at 0x%zx: size %u past the end", o, size);
        }
        lz = !memcmp(d + o, "LZSSPAGE", 8);
        if (lz || !memcmp(d + o, "PAGEDATA", 8)) {
            long pos;

            if (np >= cap) {
                struct page *t = realloc(pg, sizeof(*pg) * (cap = cap ? cap * 2 : 256));

                if (!t) {
                    free(pg);
                    return igsa_fail(c, "out of memory");
                }
                pg = t;
            }
            pg[np].off = o + 12;
            pg[np].size = size;
            pg[np].lz = lz;
            if (lz) {
                uint8_t *u;
                size_t un;

                if (size < 4 || igsa_lzss(d + o + 16, size - 4, &u, &un)) {
                    free(pg);
                    return igsa_fail(c, "LZSSPAGE at 0x%zx: size %u", o, size);
                }
                if (un < igsa_le32(d + o + 12)) {
                    free(u);
                    free(pg);
                    return igsa_fail(c, "LZSSPAGE at 0x%zx: %zu of %u bytes", o, un, igsa_le32(d + o + 12));
                }
                pos = runs(u, igsa_le32(d + o + 12), NULL, 0);
                free(u);
            } else
                pos = runs(d + o + 12, size, NULL, 0);
            if (pos < 0) {
                free(pg);
                return igsa_fail(c, "page %zu at 0x%zx: runs overrun the page", np, o);
            }
            if (pos > maxpos)
                maxpos = pos;
            np++;
        }
        o += 12 + size;
    }
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if ((long)sizes[i][0] * sizes[i][1] == maxpos) {
            w = sizes[i][0];
            h = sizes[i][1];
        }
    if (!w) {
        free(pg);
        return igsa_fail(c, "pages cover %ld pixels: no known frame size", maxpos);
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "TSG_DEMO");
    igsa_jint(c, "w", w);
    igsa_jint(c, "h", h);
    igsa_juint(c, "frames", np);
    igsa_jbool(c, "file_end", ended);
    igsa_juint(c, "trailing", c->n - o);
    igsa_jstr(c, "frame_files", "frames/%05d.png");
    igsa_jarr(c, "pages");
    fb = calloc((size_t)w * h, 2);
    rgba = malloc((size_t)w * h * 4);
    if (!fb || !rgba) {
        r = igsa_fail(c, "out of memory");
        goto out;
    }
    /* pass 2: decode every frame */
    for (i = 0; i < np; i++) {
        char name[32];
        size_t k;

        igsa_jobj(c, NULL);
        igsa_jstr(c, "tag", pg[i].lz ? "LZSSPAGE" : "PAGEDATA");
        igsa_jhex(c, "offset", pg[i].off - 12);
        igsa_juint(c, "size", pg[i].size);
        if (pg[i].lz) {
            uint8_t *u;
            size_t un;

            if (igsa_lzss(d + pg[i].off + 4, pg[i].size - 4, &u, &un)) {
                r = igsa_fail(c, "out of memory");
                goto out;
            }
            igsa_juint(c, "unpacked", igsa_le32(d + pg[i].off));
            runs(u, igsa_le32(d + pg[i].off), fb, (size_t)w * h);
            free(u);
        } else
            runs(d + pg[i].off, pg[i].size, fb, (size_t)w * h);
        igsa_jend(c);
        for (k = 0; k < (size_t)w * h; k++) {
            /* rows bottom-up (the recorder reads the GL frame buffer) */
            unsigned v = fb[k], rr = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
            uint8_t *q = rgba + 4 * ((size_t)(h - 1 - (int)(k / (size_t)w)) * w + k % (size_t)w);

            q[0] = (uint8_t)(rr << 3);      /* the players shift, no rounding */
            q[1] = (uint8_t)(g << 3);
            q[2] = (uint8_t)(b << 3);
            q[3] = 255;
        }
        snprintf(name, sizeof(name), "frames/%05zu.png", i);
        if ((r = igsa_emit_png(c, name, w, h, rgba)))
            goto out;
    }
    igsa_jend(c);
    igsa_jend(c);
out:
    free(fb);
    free(rgba);
    free(pg);
    return r;
}

const igsa_format igsa_fmt_tsgdemo = { "tsgdemo", "TSG_DEMO screen recording (.dem/.demo): XRGB1555 delta pages", probe, parse };
