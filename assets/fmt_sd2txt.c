/*
 * Speed Driver 2's text tables: data/inf/<name>.inf and .ini (read with
 * fscanf / ifstream >> word by word: 2dtexture.inf sub_8058372,
 * aidatafiles_*.inf sub_813877C, tacticssetting_*.inf, soundengine.inf,
 * sound3d.inf, ...), io.inf, camerapictime.ini, the boundary tools' text
 * dumps (<course>_boundingAI.dat, <course>_boundingBD.dat: BoundaryInfo "Mesh\t%d\t%d\t%f\t%f",
 * "Tri ..." lines; "boundary data.txt").  wiki Format-SD2TXT, Format-SD2TEX.
 *
 * Lines (CR LF), "[SECTION]" headers, words split by spaces and tabs, "//"
 * to the end of the line a comment (GBK).  Numbers, "(x,y,z)" tuples and
 * words keep their type.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static const char *ext_of(const char *p)
{
    const char *a = p ? strrchr(p, '/') : NULL, *b = p ? strrchr(p, '\\') : NULL, *e;

    if (b > a)
        a = b;
    a = a ? a + 1 : p ? p : "";
    e = strrchr(a, '.');
    return e ? e : "";
}

static int is_text(const uint8_t *d, size_t n)
{
    size_t i;

    if (!n)
        return 0;
    for (i = 0; i < n; i++)
        if (d[i] < 0x20 && d[i] != '\t' && d[i] != '\r' && d[i] != '\n' && !(d[i] == 0x1A && i + 1 == n))
            return 0;
    return 1;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    const char *e = ext_of(path);

    if (n > (8u << 20) || !is_text(d, n))
        return 0;
    if (!strcasecmp(e, ".inf") || !strcasecmp(e, ".ini") || !strcasecmp(e, ".txt") || !strcasecmp(e, ".dat"))
        return 20;
    return 0;
}

static int num(const char *s, size_t n, double *v)
{
    char buf[64], *e;

    if (!n || n >= sizeof(buf))
        return 0;
    memcpy(buf, s, n);
    buf[n] = 0;
    *v = strtod(buf, &e);
    return e != buf && !*e;
}

static void value(igsa_ctx *c, const char *s, size_t n)
{
    double v;

    if (num(s, n, &v)) {
        igsa_jnum(c, NULL, v);
        return;
    }
    if (n >= 2 && s[0] == '(' && s[n - 1] == ')') {     /* (x,y,z) */
        size_t i = 1, b = 1;
        int ok = 1;

        for (i = 1; i < n && ok; i++)
            if (s[i] == ',' || i == n - 1) {
                ok = num(s + b, i - b, &v);
                b = i + 1;
            }
        if (ok) {
            igsa_jarr(c, NULL);
            for (i = 1, b = 1; i < n; i++)
                if (s[i] == ',' || i == n - 1) {
                    num(s + b, i - b, &v);
                    igsa_jnum(c, NULL, v);
                    b = i + 1;
                }
            igsa_jend(c);
            return;
        }
    }
    igsa_jgbk(c, NULL, s, n);
}

static int parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t o = 0, n = c->n;
    int no = 0, sections = 0, rows = 0, open = 0;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Speed Driver 2 text table");
    igsa_jarr(c, "sections");
    igsa_jobj(c, NULL);
    igsa_jstr(c, "name", "");
    igsa_jarr(c, "lines");
    open = 1;
    while (o < n) {
        size_t b = o, e, i, cm = (size_t)-1;
        int words = 0;

        while (o < n && d[o] != '\n')
            o++;
        e = o;
        if (o < n)
            o++;
        no++;
        if (e > b && d[e - 1] == '\r')
            e--;
        for (i = b; i + 1 < e; i++)
            if (d[i] == '/' && d[i + 1] == '/') {
                cm = i;
                break;
            }
        {
            size_t t = b;

            while (t < (cm == (size_t)-1 ? e : cm) && (d[t] == ' ' || d[t] == '\t'))
                t++;
            if (t < e && d[t] == '[') {
                size_t z = t;

                while (z < e && d[z] != ']')
                    z++;
                if (z < e) {
                    igsa_jend(c);
                    igsa_jend(c);
                    igsa_jobj(c, NULL);
                    igsa_jgbk(c, "name", (const char *)d + t + 1, z - t - 1);
                    igsa_jint(c, "line", no);
                    igsa_jarr(c, "lines");
                    sections++;
                    continue;
                }
            }
        }
        if (cm == (size_t)-1) {
            for (i = b; i < e; i++)
                if (d[i] != ' ' && d[i] != '\t')
                    break;
            if (i == e)
                continue;       /* blank */
        }
        igsa_jobj(c, NULL);
        igsa_jint(c, "line", no);
        igsa_jarr(c, "values");
        for (i = b; i < (cm == (size_t)-1 ? e : cm);) {
            size_t s;

            while (i < e && (d[i] == ' ' || d[i] == '\t'))
                i++;
            if (i >= (cm == (size_t)-1 ? e : cm))
                break;
            s = i;
            while (i < e && d[i] != ' ' && d[i] != '\t' && !(i == cm))
                i++;
            value(c, (const char *)d + s, i - s);
            words++;
        }
        igsa_jend(c);
        if (cm != (size_t)-1)
            igsa_jgbk(c, "comment", (const char *)d + cm + 2, e - cm - 2);
        igsa_jend(c);
        if (words)
            rows++;
    }
    if (open) {
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jint(c, "lines", no);
    igsa_jint(c, "rows", rows);
    igsa_jint(c, "section_headers", sections);
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_sd2txt = { "sd2txt", "Speed Driver 2 text tables (.inf, .ini, text .dat/.txt): sections, rows",
                                      probe, parse };

/*
 * data/inf/2dtexture.inf (sub_8058372, ul2dDrawTextureIndex): "[PAGE<n>]"
 * lines, then per 2D image "<texture> <w> <h> <x0> <y0> <x1> <y1> <cols> <rows>"
 * (a cols x rows grid of frames in the rectangle; the game scans with
 * "%s %d %d %d %d %d %d %d %d").  Page n is loaded by IGS036 message 0x13.
 */
static int tex_lines(const uint8_t *d, size_t n, igsa_ctx *c)
{
    size_t o = 0;
    int no = 0, images = 0, pages = 0, open = 0, idx = 0;

    while (o < n) {
        char buf[300], name[260];
        size_t b = o, l;
        int v[8], k;

        while (o < n && d[o] != '\n')
            o++;
        l = o - b < sizeof(buf) - 1 ? o - b : sizeof(buf) - 1;
        if (o < n)
            o++;
        no++;
        memcpy(buf, d + b, l);
        buf[l] = 0;
        if (sscanf(buf, "[PAGE%d]", &k) == 1) {
            if (c) {
                if (open) {
                    igsa_jend(c);
                    igsa_jend(c);
                }
                igsa_jobj(c, NULL);
                igsa_jint(c, "page", k);
                igsa_jint(c, "line", no);
                igsa_jarr(c, "images");
            }
            open = 1;
            pages++;
            continue;
        }
        k = sscanf(buf, "%259s %d %d %d %d %d %d %d %d", name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        if (k <= 0)
            continue;   /* blank */
        if (k != 9 || !open) {      /* the loader skips it */
            if (c) {
                if (open) {
                    igsa_jobj(c, NULL);
                    igsa_jint(c, "skipped_line", no);
                    igsa_jint(c, "fields", k);
                    igsa_jstrn(c, "text", buf, strlen(buf));
                    igsa_jend(c);
                }
            }
            continue;
        }
        if (v[0] <= 0 || v[1] <= 0 || v[6] < 0 || v[7] < 0)
            return c ? igsa_fail(c, "line %d: texture %dx%d, grid %dx%d", no, v[0], v[1], v[6], v[7]) : -1;
        if (c) {
            igsa_jobj(c, NULL);
            igsa_jint(c, "index", idx);
            igsa_jgbk(c, "texture", name, strlen(name));
            igsa_jint(c, "tex_w", v[0]);
            igsa_jint(c, "tex_h", v[1]);
            igsa_jint(c, "x0", v[2]);
            igsa_jint(c, "y0", v[3]);
            igsa_jint(c, "x1", v[4]);
            igsa_jint(c, "y1", v[5]);
            igsa_jint(c, "cols", v[6]);
            igsa_jint(c, "rows", v[7]);
            igsa_jend(c);
        }
        idx++;
        images++;
    }
    if (c && open) {
        igsa_jend(c);
        igsa_jend(c);
    }
    return pages && images ? images : -1;
}

static int tex_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 7 && n < (8u << 20) && !memcmp(d, "[PAGE", 5) && is_text(d, n) && tex_lines(d, n, NULL) > 0 ? 90 : 0;
}

static int tex_parse(igsa_ctx *c)
{
    int k;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Speed Driver 2 2D texture pages");
    igsa_jarr(c, "pages");
    if ((k = tex_lines(c->d, c->n, c)) < 0)
        return igsa_fail(c, "no pages");
    igsa_jend(c);
    igsa_jint(c, "images", k);
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_sd2tex = { "sd2tex", "Speed Driver 2 2D texture pages (2dtexture.inf): image rectangles and grids",
                                      tex_probe, tex_parse };