/*
 * ACT_ZL3D: the zl engine's action groups (the E2100 Fish engine's action/<name>.rom,
 * We Dance texture/rom, Percussion Master 3 media/rom).  Chunks of
 * tag[8] size(u32); CActionGroup::vLoad.  wiki Format-ACT.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

typedef struct rd { const uint8_t *d; size_t n, o; } rd;

static int need(rd *r, size_t k) { return r->o + k <= r->n; }

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 12 && !memcmp(d, "ACT_ZL3D", 8) ? 100 : 0;
}

/* ActionHead (vLoad): kind, x, y, frames, pictures, name[21], mode */
struct head { uint32_t kind; int32_t x, y, frames, pics; char name[22]; uint8_t mode; };

static const char *const ops[] = {
    [0] = "show", [1] = "loop", [2] = "next", [6] = "layer", [9] = "sound",
    [14] = "alpha", [15] = "scale", [16] = "rotate",
};

/* a frame: 10-byte steps (u16 op, 8 bytes of arguments) up to the first "show" */
static void dump_steps(igsa_ctx *c, const uint8_t *p, size_t n)
{
    size_t i;

    igsa_jarr(c, "steps");
    for (i = 0; i + 10 <= n; i += 10) {
        unsigned op = igsa_le16(p + i);

        igsa_jobj(c, NULL);
        if (op < sizeof(ops) / sizeof(ops[0]) && ops[op])
            igsa_jstr(c, "op", ops[op]);
        else
            igsa_jint(c, "op", op);
        switch (op) {
        case 0:
            igsa_jint(c, "time", p[i + 2]);
            igsa_jint(c, "picture", p[i + 3]);
            igsa_jint(c, "layer", p[i + 4] & 31);
            igsa_jbool(c, "hidden", p[i + 5] >> 5 & 1);
            igsa_jbool(c, "flip_v", p[i + 5] >> 6 & 1);
            igsa_jbool(c, "flip_h", p[i + 5] >> 7 & 1);
            igsa_jint(c, "dx", (int16_t)igsa_le16(p + i + 6));
            igsa_jint(c, "dy", (int16_t)igsa_le16(p + i + 8));
            break;
        case 1:
            igsa_jint(c, "count", igsa_le16(p + i + 2));
            break;
        case 6:
            igsa_jint(c, "value", igsa_le16(p + i + 8));
            break;
        case 9:
            igsa_jint(c, "sound", (int16_t)igsa_le16(p + i + 6));
            break;
        case 14:
            igsa_jint(c, "alpha", igsa_le16(p + i + 2) == 0x80 ? 255 : (uint8_t)(2 * p[i + 2]));
            break;
        case 15:
            igsa_jint(c, "sx", igsa_le16(p + i + 2));   /* 64 = 1.0 */
            igsa_jint(c, "sy", igsa_le16(p + i + 4));
            break;
        case 16:
            igsa_jint(c, "degrees", igsa_le16(p + i + 2));
            break;
        default:
            igsa_jbytes(c, "args", p + i + 2, 8);
        }
        igsa_jend(c);
    }
    igsa_jend(c);
}

static int image(igsa_ctx *c, int idx, const char *tag, const uint8_t *p, size_t n, unsigned w, unsigned h,
                 int abgr)
{
    char name[64];
    uint8_t *rgba;
    size_t i;
    int r;

    snprintf(name, sizeof(name), "images/%04d.png", idx);
    if (!abgr)
        return igsa_emit_dds(c, name, p, n);
    if ((size_t)w * h * 4 > n || !w || !h)
        return igsa_fail(c, "%s %d: %ux%u wants %zu bytes, has %zu", tag, idx, w, h, (size_t)w * h * 4, n);
    if (!(rgba = malloc((size_t)w * h * 4)))
        return igsa_fail(c, "out of memory");
    for (i = 0; i < (size_t)w * h; i++) {   /* GL_RGBA, GL_UNSIGNED_INT_8_8_8_8: bytes A B G R */
        rgba[4 * i] = p[4 * i + 3];
        rgba[4 * i + 1] = p[4 * i + 2];
        rgba[4 * i + 2] = p[4 * i + 1];
        rgba[4 * i + 3] = p[4 * i];
    }
    r = igsa_emit_png(c, name, (int)w, (int)h, rgba);
    free(rgba);
    return r;
}

struct actjson { rd *r; size_t start; };

static int parse(igsa_ctx *c)
{
    rd r = { c->d, c->n, 12 };
    int nimg = 0, nact = 0, ended = 0;
    /* We Dance's texture/rom names are Big5 (made on a Taiwanese tool; the game never reads them) */
    int big5 = c->path && (strstr(c->path, "texture/rom") || strstr(c->path, "texture\\rom"));

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "ACT_ZL3D");
    igsa_jarr(c, "chunks");
    while (need(&r, 8)) {
        char tag[9];
        uint32_t size;

        memcpy(tag, r.d + r.o, 8);
        tag[8] = 0;
        r.o += 8;
        if (!need(&r, 4))
            return igsa_fail(c, "%s at 0x%zx: no size", tag, r.o - 8);
        size = igsa_le32(r.d + r.o);
        r.o += 4;
        igsa_jobj(c, NULL);
        igsa_jstr(c, "tag", tag);
        igsa_juint(c, "size", size);
        if (!strcmp(tag, "TEXTURE0")) {
            char nm[256];

            if (size < 8 || !need(&r, size))
                return igsa_fail(c, "TEXTURE0 at 0x%zx: size %u", r.o - 12, size);
            igsa_str(nm, r.d + r.o + 8, size - 8 < 255 ? size - 8 : 255);
            igsa_jint(c, "image", nimg++);
            igsa_jint(c, "w", (int32_t)igsa_le32(r.d + r.o));
            igsa_jint(c, "h", (int32_t)igsa_le32(r.d + r.o + 4));
            igsa_jstr(c, "file", nm);
            if (big5) {     /* the game loads texture/dds.<tw|cn|en>/<file with extension dds> */
                char dds[300];
                size_t k = strlen(nm);

                snprintf(dds, sizeof(dds), "texture/dds.cn/%.*sdds", (int)(k >= 3 ? k - 3 : k), nm);
                igsa_jstr(c, "dds", dds);
            }
            if (c->mode == IGSA_LIST)
                fprintf(c->out, "%-48s %-5s %s\n", "(texture)", "ref", nm);
            r.o += size;
        } else if (!strcmp(tag, "TGA_OPGL") || !strcmp(tag, "TGA_OPSS")) {
            unsigned w, h, flag;
            int lz = tag[6] == 'S';
            size_t len;

            if (!need(&r, 17))
                return igsa_fail(c, "%s at 0x%zx: truncated", tag, r.o - 12);
            flag = r.d[r.o];
            w = igsa_le32(r.d + r.o + 9);
            h = igsa_le32(r.d + r.o + 13);
            igsa_jint(c, "image", nimg);
            igsa_jint(c, "flag", flag);
            igsa_jint(c, "show_w", (int32_t)igsa_le32(r.d + r.o + 1));
            igsa_jint(c, "show_h", (int32_t)igsa_le32(r.d + r.o + 5));
            igsa_jint(c, "w", w);
            igsa_jint(c, "h", h);
            r.o += 17;
            /* the plain chunk's pixels are w*h*4 whatever the size says; the LZSS one is size-8 bytes */
            len = lz ? (size >= 8 ? size - 8 : 0) : (size_t)w * h * 4;
            if (!need(&r, len))
                return igsa_fail(c, "%s at 0x%zx: %zu bytes past the end", tag, r.o - 29, len);
            if (lz) {
                uint8_t *u;
                size_t un;

                if (igsa_lzss(r.d + r.o, len, &u, &un))
                    return igsa_fail(c, "out of memory");
                igsa_juint(c, "unpacked", un);
                if (image(c, nimg, tag, u, un, w, h, 1)) {
                    free(u);
                    return -1;
                }
                free(u);
            } else if (image(c, nimg, tag, r.d + r.o, len, w, h, 1))
                return -1;
            nimg++;
            r.o += len;
        } else if (!strcmp(tag, "DDS_OPGL") || !strcmp(tag, "DDS_OPSS")) {
            if (!need(&r, size))
                return igsa_fail(c, "%s at 0x%zx: size %u past the end", tag, r.o - 12, size);
            igsa_jint(c, "image", nimg);
            if (tag[6] == 'S') {
                uint8_t *u;
                size_t un;

                if (igsa_lzss(r.d + r.o, size, &u, &un))
                    return igsa_fail(c, "out of memory");
                igsa_juint(c, "unpacked", un);
                if (un >= 20) {
                    igsa_jint(c, "w", igsa_le32(u + 16));
                    igsa_jint(c, "h", igsa_le32(u + 12));
                }
                if (image(c, nimg, tag, u, un, 0, 0, 0)) {
                    free(u);
                    return -1;
                }
                free(u);
            } else {
                if (size >= 20) {
                    igsa_jint(c, "w", igsa_le32(r.d + r.o + 16));
                    igsa_jint(c, "h", igsa_le32(r.d + r.o + 12));
                }
                if (image(c, nimg, tag, r.d + r.o, size, 0, 0, 0))
                    return -1;
            }
            nimg++;
            r.o += size;
        } else if (!strcmp(tag, "PCX_OPGL")) {
            char name[64];

            if (size < 8 || !need(&r, size))
                return igsa_fail(c, "PCX_OPGL at 0x%zx: size %u", r.o - 12, size);
            igsa_jint(c, "image", nimg);
            igsa_jint(c, "w", (int32_t)igsa_le32(r.d + r.o));
            igsa_jint(c, "h", (int32_t)igsa_le32(r.d + r.o + 4));
            snprintf(name, sizeof(name), "images/%04d.png", nimg++);
            if (igsa_emit_pcx(c, name, r.d + r.o + 8, size - 8))
                return -1;
            r.o += size;
        } else if (!strcmp(tag, "ACT_DATA")) {
            struct head h;
            int i;

            if (size < 0x2A || !need(&r, size))
                return igsa_fail(c, "ACT_DATA at 0x%zx: head size %u", r.o - 12, size);
            h.kind = igsa_le32(r.d + r.o);
            h.x = (int32_t)igsa_le32(r.d + r.o + 4);
            h.y = (int32_t)igsa_le32(r.d + r.o + 8);
            h.frames = (int32_t)igsa_le32(r.d + r.o + 12);
            h.pics = (int32_t)igsa_le32(r.d + r.o + 16);
            igsa_str(h.name, r.d + r.o + 20, 21);
            h.mode = r.d[r.o + 41];
            r.o += size;
            if (h.frames < 0 || h.pics < 0 || h.frames > 100000 || h.pics > 100000)
                return igsa_fail(c, "action %d (%s): %d frames, %d pictures", nact, h.name, h.frames, h.pics);
            igsa_jint(c, "action", nact);
            if (big5)
                igsa_jbig5(c, "name", h.name, strlen(h.name));
            else
                igsa_jgbk(c, "name", h.name, strlen(h.name));
            igsa_juint(c, "kind", h.kind);
            igsa_jint(c, "x", h.x);
            igsa_jint(c, "y", h.y);
            igsa_jint(c, "mode", h.mode);
            if (c->mode == IGSA_LIST) {
                char info[64];

                snprintf(info, sizeof(info), "%d frames, %d pictures", h.frames, h.pics);
                fprintf(c->out, "%-48s %-5s %s\n", h.name, "act", info);
            }
            igsa_jarr(c, "frames");
            for (i = 0; i < h.frames; i++) {
                uint32_t len;

                if (!need(&r, 12) || !need(&r, 12 + (len = igsa_le32(r.d + r.o + 8))))
                    return igsa_fail(c, "action %s frame %d: truncated", h.name, i);
                igsa_jobj(c, NULL);
                dump_steps(c, r.d + r.o + 12, len);
                igsa_jend(c);
                r.o += 12 + len;
            }
            igsa_jend(c);
            igsa_jarr(c, "pictures");
            if (h.pics > 0) {
                if (!need(&r, 8))
                    return igsa_fail(c, "action %s: pictures truncated", h.name);
                r.o += 8;
                for (i = 0; i < h.pics; i++) {
                    uint32_t len, len2;
                    unsigned nh, k;

                    if (!need(&r, 4) || !need(&r, 4 + (len = igsa_le32(r.d + r.o))) || len < 7)
                        return igsa_fail(c, "action %s picture %d: truncated", h.name, i);
                    igsa_jobj(c, NULL);
                    igsa_jint(c, "ox", (int16_t)igsa_le16(r.d + r.o + 4));
                    igsa_jint(c, "oy", (int16_t)igsa_le16(r.d + r.o + 6));
                    igsa_jint(c, "image", igsa_le16(r.d + r.o + 8));
                    nh = r.d[r.o + 10];
                    r.o += 4 + len;
                    /* We Dance (CActionGroup::vLoad 0x80736C0): ACT_POOL is ox, oy, image only, no ACTBLOCK */
                    if (!need(&r, 12) || memcmp(r.d + r.o, "ACTBLOCK", 8)) {
                        igsa_jend(c);
                        if (i + 1 < h.pics) {
                            if (!need(&r, 8))
                                return igsa_fail(c, "action %s picture %d: truncated", h.name, i + 1);
                            r.o += 8;
                        }
                        continue;
                    }
                    len2 = igsa_le32(r.d + r.o + 8);
                    r.o += 12;
                    igsa_jarr(c, "hits");
                    if (nh) {       /* read only when there are hit boxes */
                        if (!need(&r, len2) || len2 < nh * 10u)
                            return igsa_fail(c, "action %s picture %d: %u hits in %u bytes", h.name, i, nh, len2);
                        for (k = 0; k < nh; k++) {
                            const uint8_t *q = r.d + r.o + 10 * k;

                            igsa_jobj(c, NULL);
                            igsa_jint(c, "x", (int16_t)igsa_le16(q));
                            igsa_jint(c, "y", (int16_t)igsa_le16(q + 2));
                            igsa_jint(c, "size", (int16_t)igsa_le16(q + 4));
                            igsa_jint(c, "a", (int16_t)igsa_le16(q + 6));
                            igsa_jint(c, "b", (int16_t)igsa_le16(q + 8));
                            igsa_jend(c);
                        }
                        r.o += len2;
                    }
                    igsa_jend(c);
                    igsa_jend(c);
                    if (i + 1 < h.pics) {
                        if (!need(&r, 8))
                            return igsa_fail(c, "action %s picture %d: truncated", h.name, i + 1);
                        r.o += 8;
                    }
                }
            }
            igsa_jend(c);
            nact++;
            c->entries++;
        } else if (!strcmp(tag, "FILE_END")) {
            igsa_jend(c);
            ended = 1;
            break;
        } else {
            if (!need(&r, size))
                return igsa_fail(c, "%s at 0x%zx: size %u past the end", tag, r.o - 12, size);
            r.o += size;
        }
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jint(c, "images", nimg);
    igsa_jint(c, "actions", nact);
    igsa_jbool(c, "file_end", ended);
    igsa_juint(c, "trailing", r.n - r.o);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_act = { "act", "ACT_ZL3D action group: images (TGA/DDS/PCX, LZSS) and actions", probe, parse };
