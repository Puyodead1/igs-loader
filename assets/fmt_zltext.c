/*
 * The zl engine's text scripts (Speed Driver 2; Fish Hunter 4 M107CN has the
 * same loaders with symbols): model descriptions (*.zxp, LoadZXP), shader
 * libraries (*.shader, LoadShader / ParseShaderPass), scene descriptions
 * (*.map, LoadMap, LoadSky, LoadFog).  wiki Format-ZXP, Format-ZLSHADER,
 * Format-ZLMAP.  In sd2: LoadZXP sub_81AF4B0, LoadMap sub_82080E0.
 *
 * Lines end in CR LF; a line is a keyword and arguments split by spaces and
 * tabs; lines starting "//" or "\\" are comments, lines from one starting
 * slash-star to one starting star-slash a comment block; "{" and "}" on their own lines open and close the block of
 * the statement before them (anonymous when that statement takes no block,
 * as a shader's passes).
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

typedef struct tok { const char *s; size_t n; } tok;

typedef struct line { tok t[32]; int nt, no; tok comment; } line;

/* the next non-comment line from *o; 0 at the end */
static int next_line(const uint8_t *d, size_t n, size_t *o, line *l, int *no, int *incomment)
{
    while (*o < n) {
        size_t b = *o, e;
        int i;

        while (*o < n && d[*o] != '\n')
            (*o)++;
        e = *o;
        if (*o < n)
            (*o)++;
        ++*no;
        if (e > b && d[e - 1] == '\r')
            e--;
        l->nt = 0;
        l->no = *no;
        l->comment.s = NULL;
        l->comment.n = 0;
        for (i = (int)b; (size_t)i < e;) {
            size_t s;

            while ((size_t)i < e && (d[i] == ' ' || d[i] == '\t'))
                i++;
            if ((size_t)i >= e)
                break;
            s = (size_t)i;
            if (l->nt && e - s >= 2 && d[s] == '/' && d[s + 1] == '/') {   /* a trailing comment */
                l->comment.s = (const char *)d + s + 2;
                l->comment.n = e - s - 2;
                break;
            }
            while ((size_t)i < e && d[i] != ' ' && d[i] != '\t')
                i++;
            if (l->nt < 32) {
                l->t[l->nt].s = (const char *)d + s;
                l->t[l->nt].n = (size_t)i - s;
                l->nt++;
            }
        }
        if (!l->nt)
            continue;
        if (l->t[0].n >= 2 && !memcmp(l->t[0].s, "/*", 2)) {
            *incomment = 1;
            continue;
        }
        if (l->t[0].n >= 2 && !memcmp(l->t[0].s, "*/", 2)) {
            *incomment = 0;
            continue;
        }
        if (*incomment || (l->t[0].n >= 2 && (!memcmp(l->t[0].s, "//", 2) || !memcmp(l->t[0].s, "\\\\", 2))))
            continue;
        return 1;
    }
    return 0;
}

static int teq(const tok *t, const char *s)
{
    return t->n == strlen(s) && !strncasecmp(t->s, s, t->n);
}

/* a value: number, True/False, RGBA(r,g,b,a), else text */
static void jvalue(igsa_ctx *c, const tok *t)
{
    char buf[64], *e;
    double v;

    if (t->n < sizeof(buf)) {
        memcpy(buf, t->s, t->n);
        buf[t->n] = 0;
        v = strtod(buf, &e);
        if (e != buf && !*e) {
            igsa_jnum(c, NULL, v);
            return;
        }
        if (!strcasecmp(buf, "true") || !strcasecmp(buf, "false")) {
            igsa_jbool(c, NULL, !strcasecmp(buf, "true"));
            return;
        }
        if (!strncasecmp(buf, "RGBA(", 5) && buf[t->n - 1] == ')') {
            int r, g, b, a;

            if (sscanf(buf + 5, "%d,%d,%d,%d", &r, &g, &b, &a) == 4) {
                igsa_jobj(c, NULL);
                igsa_jint(c, "r", r);
                igsa_jint(c, "g", g);
                igsa_jint(c, "b", b);
                igsa_jint(c, "a", a);
                igsa_jend(c);
                return;
            }
        }
    }
    igsa_jgbk(c, NULL, t->s, t->n);
}

/* statements that take a "{" block */
static const char *const blocks[] = { "Shader", "Sky", "Fog", "Raytail", "RayElec", "Light", "Emitter", "Force", NULL };

static int takes_block(const line *l)
{
    int i;

    for (i = 0; blocks[i]; i++)
        if (teq(&l->t[0], blocks[i]))
            return 1;
    return 0;
}

typedef struct doc {
    igsa_ctx *c;
    const uint8_t *d;
    size_t n, o;
    int no, incomment, depth, maxdepth, statements, unclosed, stray;
    line pend;
    int have;
} doc;

static int get(doc *x, line *l)
{
    if (x->have) {
        *l = x->pend;
        x->have = 0;
        return 1;
    }
    return next_line(x->d, x->n, &x->o, l, &x->no, &x->incomment);
}

/* statements up to "}" (depth > 0) or the end */
static int block(doc *x)
{
    igsa_ctx *c = x->c;
    line l, nx;
    int i;

    if (++x->depth > x->maxdepth)
        x->maxdepth = x->depth;
    if (x->depth > 16)
        return igsa_fail(c, "line %d: blocks nested deeper than 16", x->no);
    while (get(x, &l)) {
        if (teq(&l.t[0], "}")) {
            if (x->depth == 1) {        /* the loaders ignore it */
                x->stray++;
                continue;
            }
            x->depth--;
            return 0;
        }
        igsa_jobj(c, NULL);
        if (teq(&l.t[0], "{")) {
            igsa_jint(c, "line", l.no);
            igsa_jarr(c, "block");
            if (block(x))
                return -1;
            igsa_jend(c);
            igsa_jend(c);
            continue;
        }
        x->statements++;
        igsa_jgbk(c, "key", l.t[0].s, l.t[0].n);
        igsa_jint(c, "line", l.no);
        igsa_jarr(c, "args");
        for (i = 1; i < l.nt; i++)
            jvalue(c, &l.t[i]);
        igsa_jend(c);
        if (l.comment.s)
            igsa_jgbk(c, "comment", l.comment.s, l.comment.n);
        if (takes_block(&l) && get(x, &nx)) {
            if (teq(&nx.t[0], "{")) {
                igsa_jarr(c, "block");
                if (block(x))
                    return -1;
                igsa_jend(c);
            } else {
                x->pend = nx;
                x->have = 1;
            }
        }
        igsa_jend(c);
    }
    if (x->depth > 1 && !x->unclosed)     /* the loaders stop at the end as well */
        x->unclosed = x->depth - 1;
    x->depth--;
    return 0;
}

/* text the loaders take: no NUL, no control bytes but tab, CR, LF */
static int is_text(const uint8_t *d, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (d[i] < 0x20 && d[i] != '\t' && d[i] != '\r' && d[i] != '\n' && !(d[i] == 0x1A && i + 1 == n))
            return 0;
    return n > 0;
}

/* the first two statements' keywords */
static int first2(const uint8_t *d, size_t n, line *a, line *b)
{
    size_t o = 0;
    int no = 0, ic = 0;

    if (!next_line(d, n, &o, a, &no, &ic))
        return 0;
    return next_line(d, n, &o, b, &no, &ic) ? 2 : 1;
}

static const char *ext(const char *p)
{
    const char *e = p ? strrchr(p, '.') : NULL;

    return e ? e : "";
}

static int probe_zxp(const uint8_t *d, size_t n, const char *path)
{
    size_t o = 0;
    int no = 0, ic = 0, k;
    line l;

    if (n > (1u << 20) || !is_text(d, n))
        return 0;
    /* a Scale or Model statement among the first lines (LoadZXP ignores words it does not know) */
    for (k = 0; k < 8 && next_line(d, n, &o, &l, &no, &ic); k++) {
        if (teq(&l.t[0], "{"))
            return 0;
        if (teq(&l.t[0], "Scale") || teq(&l.t[0], "Model"))
            return strcasecmp(ext(path), ".zxp") ? 40 : 85;
    }
    return 0;
}
static int probe_shader(const uint8_t *d, size_t n, const char *path)
{
    line a, b;

    if (n > (16u << 20) || !is_text(d, n) || first2(d, n, &a, &b) != 2)
        return 0;
    if (teq(&a.t[0], "Shader") && a.nt == 2 && teq(&b.t[0], "{"))
        return strcasecmp(ext(path), ".shader") ? 40 : 85;
    return 0;
}

static int probe_map(const uint8_t *d, size_t n, const char *path)
{
    line a, b;

    if (n > (1u << 20) || !is_text(d, n) || first2(d, n, &a, &b) != 2)
        return 0;
    if (teq(&a.t[0], "Shader") && (teq(&b.t[0], "SceneZ") || teq(&b.t[0], "Scene")))
        return strcasecmp(ext(path), ".map") ? 40 : 85;
    return 0;
}

static int parse_doc(igsa_ctx *c, const char *what)
{
    doc x;

    memset(&x, 0, sizeof(x));
    x.c = c;
    x.d = c->d;
    x.n = c->n;
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", what);
    igsa_jarr(c, "statements");
    if (block(&x))
        return -1;
    igsa_jend(c);
    igsa_jint(c, "lines", x.no);
    igsa_jint(c, "count", x.statements);
    if (x.unclosed)
        igsa_jint(c, "unclosed_blocks", x.unclosed);
    if (x.stray)
        igsa_jint(c, "stray_closes", x.stray);
    igsa_jend(c);
    c->entries++;
    return 0;
}

static int parse_zxp(igsa_ctx *c) { return parse_doc(c, "zl model description (.zxp)"); }
static int parse_shader(igsa_ctx *c) { return parse_doc(c, "zl shader library (.shader)"); }
static int parse_map(igsa_ctx *c) { return parse_doc(c, "zl scene description (.map)"); }

const igsa_format igsa_fmt_zxp = { "zxp", "zl model description (text): scale, model, shader, attachments", probe_zxp,
                                   parse_zxp };
const igsa_format igsa_fmt_zlshader = { "zlshader", "zl shader library (text): materials and passes", probe_shader,
                                        parse_shader };
const igsa_format igsa_fmt_zlmap = { "zlmap", "zl scene description (text): shader, scene, sky, fog", probe_map,
                                     parse_map };
