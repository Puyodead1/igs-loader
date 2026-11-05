/*
 * Hero of Robots media/cache/<script>.sbin: Ogre scripts (.material, .program, .compositor,
 * .particle, .pu, .os) as compiled abstract syntax trees, written by the game's
 * Plugin_ScriptSerializer (data/so, configured by data/ScriptCache.cfg) and read back
 * instead of the text (Ogre::ScriptSerializer::deserialize, ScriptSerializer.cpp).
 * The parser rebuilds the script text.  wiki Format-SBIN.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

#define MAGIC 0x4552474Fu               /* "OGRE" */

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 28 && igsa_le32(d) == MAGIC && igsa_le16(d + 4) == 1 && igsa_le32(d + 12) >= 16 &&
                   igsa_le32(d + 12) + 16 <= n && d[igsa_le32(d + 12)] == 3
               ? 100
               : 0;
}

enum { N_ATOM = 1, N_OBJECT = 2, N_PROPERTY = 3 };

struct node {
    int type, line, id, abstract;
    uint32_t name, cls, value;
    int nbase, nenv;
    uint32_t *base, *env;               /* env: key, value pairs */
    struct node *kids[3];               /* children / values / overrides (object), values (property) */
    struct node *next, *parent;
    struct node *last[3];
};

typedef struct sb {
    igsa_ctx *c;
    const uint8_t *d;
    size_t n, o;
    uint32_t nstr;
    uint32_t *sid;
    const char **str;
    uint32_t *slen;
    struct node **all;
    size_t nall, capall;
} sb;

static struct node *mk(sb *s)
{
    struct node *x = calloc(1, sizeof(*x));

    if (!x)
        return NULL;
    if (s->nall == s->capall) {
        struct node **a = realloc(s->all, (s->capall = s->capall ? s->capall * 2 : 256) * sizeof(*a));

        if (!a) {
            free(x);
            return NULL;
        }
        s->all = a;
    }
    return s->all[s->nall++] = x;
}

/* the string with id k */
static int sget(const sb *s, uint32_t k, const char **p, uint32_t *l)
{
    uint32_t i;

    for (i = 0; i < s->nstr; i++)
        if (s->sid[i] == k) {
            *p = s->str[i];
            *l = s->slen[i];
            return 1;
        }
    *p = "";
    *l = 0;
    return 0;
}

/* ---- text ----------------------------------------------------------------------------------- */

typedef struct tb { char *p; size_t n, cap; int bad; } tb;

static void tput(tb *t, const char *s, size_t l)
{
    if (t->bad)
        return;
    if (t->n + l + 1 > t->cap) {
        size_t nc = (t->cap + l + 1) * 2;
        char *q = realloc(t->p, nc);

        if (!q) {
            t->bad = 1;
            return;
        }
        t->p = q;
        t->cap = nc;
    }
    memcpy(t->p + t->n, s, l);
    t->n += l;
}

static void tstr(tb *t, const sb *s, uint32_t k)
{
    const char *p;
    uint32_t l;

    sget(s, k, &p, &l);
    tput(t, p, l);
}

static void tind(tb *t, int d)
{
    int i;

    for (i = 0; i < d; i++)
        tput(t, "\t", 1);
}

static void text(tb *t, const sb *s, const struct node *x, int d)
{
    const struct node *k;
    int i;

    for (; x; x = x->next) {
        if (x->type == N_ATOM) {
            tind(t, d);
            tstr(t, s, x->value);
            tput(t, "\n", 1);
        } else if (x->type == N_PROPERTY) {
            tind(t, d);
            tstr(t, s, x->name);
            for (k = x->kids[0]; k; k = k->next) {
                tput(t, " ", 1);
                if (k->type == N_ATOM)
                    tstr(t, s, k->value);
            }
            tput(t, "\n", 1);
        } else {
            tind(t, d);
            if (x->abstract)
                tput(t, "abstract ", 9);
            tstr(t, s, x->cls);
            {
                const char *p;
                uint32_t l;

                if (sget(s, x->name, &p, &l) && l) {
                    tput(t, " ", 1);
                    tput(t, p, l);
                }
            }
            for (k = x->kids[1]; k; k = k->next)
                if (k->type == N_ATOM) {
                    tput(t, " ", 1);
                    tstr(t, s, k->value);
                }
            for (i = 0; i < x->nbase; i++) {
                tput(t, i ? " " : " : ", i ? 1 : 3);
                tstr(t, s, x->base[i]);
            }
            tput(t, "\n", 1);
            tind(t, d);
            tput(t, "{\n", 2);
            for (i = 0; i < x->nenv; i++) {
                tind(t, d + 1);
                tput(t, "set ", 4);
                tstr(t, s, x->env[2 * i]);
                tput(t, " ", 1);
                tstr(t, s, x->env[2 * i + 1]);
                tput(t, "\n", 1);
            }
            text(t, s, x->kids[0], d + 1);
            text(t, s, x->kids[2], d + 1);
            tind(t, d);
            tput(t, "}\n", 2);
        }
    }
}

/* ---- JSON ----------------------------------------------------------------------------------- */

static void jstrid(igsa_ctx *c, const sb *s, const char *key, uint32_t k)
{
    const char *p;
    uint32_t l;

    sget(s, k, &p, &l);
    igsa_jstrn(c, key, p, l);
}

static void jnodes(igsa_ctx *c, const sb *s, const char *key, const struct node *x)
{
    static const char *const lists[3] = { "children", "values", "overrides" };
    int i;

    igsa_jarr(c, key);
    for (; x; x = x->next) {
        igsa_jobj(c, NULL);
        igsa_jstr(c, "type", x->type == N_ATOM ? "atom" : x->type == N_PROPERTY ? "property" : "object");
        igsa_jint(c, "line", x->line);
        igsa_jint(c, "id", x->id);
        if (x->type == N_ATOM)
            jstrid(c, s, "value", x->value);
        else
            jstrid(c, s, "name", x->name);
        if (x->type == N_OBJECT) {
            jstrid(c, s, "class", x->cls);
            igsa_jbool(c, "abstract", x->abstract);
            igsa_jarr(c, "bases");
            for (i = 0; i < x->nbase; i++)
                jstrid(c, s, NULL, x->base[i]);
            igsa_jend(c);
            igsa_jobj(c, "variables");
            for (i = 0; i < x->nenv; i++) {
                const char *p;
                uint32_t l;
                char k[128];

                sget(s, x->env[2 * i], &p, &l);
                snprintf(k, sizeof(k), "%.*s", (int)(l < 127 ? l : 127), p);
                jstrid(c, s, k, x->env[2 * i + 1]);
            }
            igsa_jend(c);
        }
        for (i = 0; i < 3; i++)
            if (x->kids[i])
                jnodes(c, s, x->type == N_PROPERTY ? "values" : lists[i], x->kids[i]);
        igsa_jend(c);
    }
    igsa_jend(c);
}

/* ---- parse ---------------------------------------------------------------------------------- */

static int need(sb *s, size_t k, const char *what)
{
    if (s->o + k > s->n)
        return igsa_fail(s->c, "%s at 0x%zx: past the end", what, s->o);
    return 0;
}

struct frame { struct node *n; uint32_t list; };

static void add(sb *s, struct node *root[1], struct node **rlast, struct frame *top, struct node *x)
{
    (void)s;
    if (!top || !top->n) {
        if (*rlast)
            (*rlast)->next = x;
        else
            root[0] = x;
        *rlast = x;
        return;
    }
    {
        struct node *p = top->n;
        int l = p->type == N_PROPERTY ? 0 : (int)top->list - 1;

        x->parent = p;
        if (p->last[l])
            p->last[l]->next = x;
        else
            p->kids[l] = x;
        p->last[l] = x;
    }
}

static int parse(igsa_ctx *c)
{
    sb s;
    struct frame st[256];
    int sp = 0, r = -1, nodes = 0;
    uint32_t i, stoff;
    struct node *root = NULL, *rlast = NULL, *prev = NULL;
    tb t = { NULL, 0, 0, 0 };

    memset(&s, 0, sizeof(s));
    s.c = c;
    s.d = c->d;
    s.n = c->n;
    if (!probe(c->d, c->n, NULL))
        return igsa_fail(c, "header");
    stoff = igsa_le32(c->d + 12);
    /* the string table: block header (class 3), u32 count, {u32 id, u32 length, bytes} */
    s.o = stoff + 12;
    if (need(&s, 4, "string table"))
        return -1;
    s.nstr = igsa_le32(c->d + s.o);
    s.o += 4;
    if (s.nstr > (c->n - s.o) / 8)
        return igsa_fail(c, "%u strings", s.nstr);
    s.sid = malloc(sizeof(*s.sid) * (s.nstr + 1));
    s.slen = malloc(sizeof(*s.slen) * (s.nstr + 1));
    s.str = malloc(sizeof(*s.str) * (s.nstr + 1));
    if (!s.sid || !s.slen || !s.str) {
        igsa_fail(c, "out of memory");
        goto out;
    }
    for (i = 0; i < s.nstr; i++) {
        if (need(&s, 8, "string"))
            goto out;
        s.sid[i] = igsa_le32(c->d + s.o);
        s.slen[i] = igsa_le32(c->d + s.o + 4);
        s.o += 8;
        if (need(&s, s.slen[i], "string"))
            goto out;
        s.str[i] = (const char *)c->d + s.o;
        s.o += s.slen[i];
    }
    /* the blocks from offset 16; the string table's class-3 header ends them */
    s.o = 16;
    for (;;) {
        unsigned cls, type;
        struct node *x;

        if (s.o > stoff) {
            igsa_fail(c, "blocks run into the string table at 0x%zx", s.o);
            goto out;
        }
        if (need(&s, 12, "block header"))
            goto out;
        cls = c->d[s.o];
        type = igsa_le32(c->d + s.o + 4);
        s.o += 12;
        if (cls == 3)
            break;
        if (cls == 2) {                 /* transition: 2 push the previous node (list userData), 1 pop */
            uint32_t dir, ud;

            if (need(&s, 8, "transition"))
                goto out;
            dir = igsa_le32(c->d + s.o);
            ud = igsa_le32(c->d + s.o + 4);
            s.o += 8;
            if (dir == 2) {
                if (sp == 256) {
                    igsa_fail(c, "nesting too deep");
                    goto out;
                }
                if (prev && prev->type == N_OBJECT && (ud < 1 || ud > 3)) {
                    igsa_fail(c, "0x%zx: child list %u", s.o - 8, ud);
                    goto out;
                }
                st[sp].n = prev;
                st[sp++].list = ud;
            } else if (dir == 1) {
                if (!sp) {
                    igsa_fail(c, "0x%zx: pop of an empty stack", s.o - 8);
                    goto out;
                }
                prev = st[--sp].n;
            } else {
                igsa_fail(c, "0x%zx: direction %u", s.o - 8, dir);
                goto out;
            }
            continue;
        }
        if (cls != 1) {
            igsa_fail(c, "0x%zx: block class %u", s.o - 12, cls);
            goto out;
        }
        if (!(x = mk(&s))) {
            igsa_fail(c, "out of memory");
            goto out;
        }
        if (type == N_ATOM || type == N_PROPERTY) {
            if (need(&s, 12, "node"))
                goto out;
            x->type = (int)type;
            x->line = (int)igsa_le32(c->d + s.o);
            if (type == N_ATOM) {
                x->id = (int)igsa_le32(c->d + s.o + 4);
                x->value = igsa_le32(c->d + s.o + 8);
            } else {
                x->name = igsa_le32(c->d + s.o + 4);
                x->id = (int)igsa_le32(c->d + s.o + 8);
            }
            s.o += 12;
        } else {                        /* any other type is read as an object */
            uint32_t k;

            if (need(&s, 28, "object"))
                goto out;
            x->type = N_OBJECT;
            x->line = (int)igsa_le32(c->d + s.o);
            x->name = igsa_le32(c->d + s.o + 4);
            x->cls = igsa_le32(c->d + s.o + 8);
            x->id = (int)igsa_le32(c->d + s.o + 12);
            x->abstract = c->d[s.o + 16] != 0;
            x->nbase = (int)igsa_le32(c->d + s.o + 20);
            x->nenv = (int)igsa_le32(c->d + s.o + 24);
            s.o += 28;
            if (x->nbase < 0 || x->nenv < 0 || (size_t)x->nbase * 4 + (size_t)x->nenv * 8 > s.n - s.o) {
                igsa_fail(c, "object at 0x%zx: %d bases, %d variables", s.o - 28, x->nbase, x->nenv);
                goto out;
            }
            x->base = malloc(4 * (size_t)x->nbase + 4);
            x->env = malloc(8 * (size_t)x->nenv + 4);
            if (!x->base || !x->env) {
                igsa_fail(c, "out of memory");
                goto out;
            }
            for (k = 0; k < (uint32_t)x->nbase; k++, s.o += 4)
                x->base[k] = igsa_le32(c->d + s.o);
            for (k = 0; k < 2 * (uint32_t)x->nenv; k++, s.o += 4)
                x->env[k] = igsa_le32(c->d + s.o);
        }
        add(&s, &root, &rlast, sp ? &st[sp - 1] : NULL, x);
        prev = x;
        nodes++;
    }
    text(&t, &s, root, 0);
    if (t.bad) {
        igsa_fail(c, "out of memory");
        goto out;
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "sbin");
    igsa_juint(c, "version", igsa_le16(c->d + 4));
    igsa_juint(c, "modified", igsa_le32(c->d + 8));
    igsa_juint(c, "strings", s.nstr);
    igsa_jint(c, "nodes", nodes);
    jnodes(c, &s, "tree", root);
    igsa_jend(c);
    {
        const char *b = c->path ? c->path : "script.sbin", *p;
        char nm[512];
        size_t l;

        for (p = b; *p; p++)
            if (*p == '/' || *p == '\\')
                b = p + 1;
        l = strlen(b);
        if (l > 5 && !strcmp(b + l - 5, ".sbin"))
            l -= 5;
        snprintf(nm, sizeof(nm), "%.*s", (int)l, b);
        if (igsa_emit_raw(c, nm, t.p ? t.p : "", t.n))
            goto out;
    }
    r = 0;
out:
    for (i = 0; i < s.nall; i++) {
        free(s.all[i]->base);
        free(s.all[i]->env);
        free(s.all[i]);
    }
    free(s.all);
    free(s.sid);
    free(s.slen);
    free(s.str);
    free(t.p);
    return r;
}

const igsa_format igsa_fmt_sbin = { "sbin", "Hero of Robots .sbin: Ogre script syntax tree (ScriptSerializer), to script text", probe,
                                    parse };
