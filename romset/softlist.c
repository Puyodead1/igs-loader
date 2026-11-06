/*
 * The software lists, hash/<machine>.xml: a small strict reader for the
 * subset of MAME's softlist format the loader uses.
 *
 *   <softwarelist name="e2100" description="...">
 *     <software name="id" [cloneof="parent"] [supported="no"]>
 *       <description>..</description> <year>..</year> <publisher>..</publisher>
 *       <info name="version" value=".."/>  (every <info> is kept; "tunable" is parsed)
 *       <sharedfeat name=".." value=".."/>
 *       <part name="hdd" interface="..">
 *         <feature name=".." value=".."/>
 *         <diskarea name="harddriv"><disk name=".." sha1=".."/></diskarea>
 *         <dataarea name="files"><rom name="/cabinet/path" size="n" sha1=".."/></dataarea>
 *       </part>
 *     </software>
 *   </softwarelist>
 *
 * Comments, <?xml ?> and <!DOCTYPE> are skipped; the five named entities
 * and &#n; / &#xn; are decoded.  Unknown elements are ignored.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "romset.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

static struct sw_info *sws;
static int nsw, capsw;

int sw_count(void) { return nsw; }
const struct sw_info *sw_at(int i) { return i >= 0 && i < nsw ? &sws[i] : NULL; }

const struct sw_info *sw_find(const char *name)
{
    int i;

    for (i = 0; i < nsw; i++)
        if (!strcmp(sws[i].name, name))
            return &sws[i];
    return NULL;
}

const char *sw_feature(const struct sw_info *sw, const char *name, const char *def)
{
    int i;

    for (i = 0; sw && i < sw->nfeat; i++)
        if (!strcmp(sw->feat[i].name, name))
            return sw->feat[i].value;
    return def;
}

const char *sw_info_value(const struct sw_info *sw, const char *name, const char *def)
{
    int i;

    for (i = 0; sw && i < sw->ninfo; i++)
        if (!strcmp(sw->info[i].name, name))
            return sw->info[i].value;
    return def;
}

int sw_runnable(const struct sw_info *sw)
{
    return !sw || !sw->supported || strcmp(sw->supported, "no");
}

void sw_free(void)
{
    int i, j;

    for (i = 0; i < nsw; i++) {
        struct sw_info *s = &sws[i];

        free(s->list);
        free(s->name);
        free(s->cloneof);
        free(s->supported);
        free(s->description);
        free(s->year);
        free(s->publisher);
        free(s->version);
        free(s->disk);
        free(s->disk_sha1);
        for (j = 0; j < s->nfeat; j++) {
            free(s->feat[j].name);
            free(s->feat[j].value);
        }
        for (j = 0; j < s->ninfo; j++) {
            free(s->info[j].name);
            free(s->info[j].value);
        }
        for (j = 0; j < s->nrom; j++) {
            free(s->rom[j].name);
            free(s->rom[j].sha1);
        }
        for (j = 0; j < s->ntun; j++)
            free(s->tun[j].name);       /* one block: name type def group text */
        free(s->feat);
        free(s->info);
        free(s->rom);
        free(s->tun);
    }
    free(sws);
    sws = NULL;
    nsw = capsw = 0;
}

/* ---- the parser ------------------------------------------------------------------------- */

struct xp {
    const char *file, *p, *end;
    int line;
};

#define MAXATTR 8
struct attrs { int n; char *k[MAXATTR], *v[MAXATTR]; };

static int fail(struct xp *x, const char *what)
{
    snprintf(rs_error, sizeof(rs_error), "%s:%d: %s", x->file, x->line, what);
    return -1;
}

static char *xstrndup(const char *s, size_t n)
{
    char *r = malloc(n + 1);

    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* decode entities in place */
static void unescape(char *s)
{
    char *o = s;

    while (*s) {
        if (*s == '&') {
            static const struct { const char *e; char c; } ents[] = {
                { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' } };
            size_t i;
            int done = 0;

            for (i = 0; i < sizeof(ents) / sizeof(ents[0]); i++)
                if (!strncmp(s + 1, ents[i].e, strlen(ents[i].e))) {
                    *o++ = ents[i].c;
                    s += 1 + strlen(ents[i].e);
                    done = 1;
                    break;
                }
            if (!done && s[1] == '#') {
                char *e;
                long c = s[2] == 'x' ? strtol(s + 3, &e, 16) : strtol(s + 2, &e, 10);

                if (*e == ';' && c > 0 && c < 0x80) {
                    *o++ = (char)c;
                    s = e + 1;
                    done = 1;
                }
            }
            if (done)
                continue;
        }
        *o++ = *s++;
    }
    *o = 0;
}

static void ws(struct xp *x)
{
    while (x->p < x->end && isspace((unsigned char)*x->p)) {
        if (*x->p == '\n')
            x->line++;
        x->p++;
    }
}

static int skip_to(struct xp *x, const char *what)
{
    size_t n = strlen(what);

    while (x->p + n <= x->end) {
        if (!memcmp(x->p, what, n)) {
            x->p += n;
            return 0;
        }
        if (*x->p == '\n')
            x->line++;
        x->p++;
    }
    return fail(x, "unterminated markup");
}

static int name_char(int c)
{
    return isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':';
}

/* ---- building the list ------------------------------------------------------------------ */

struct state {
    char *list;
    struct sw_info *cur;
    int depth, sw_depth;
    char text_tag[32];          /* collecting text for this child of <software> */
    char *text;
};

static const char *aget(const struct attrs *a, const char *k)
{
    int i;

    for (i = 0; i < a->n; i++)
        if (!strcmp(a->k[i], k))
            return a->v[i];
    return NULL;
}

static char *dupnull(const char *s) { return s ? strdup(s) : NULL; }

/* "NAME type default [Group] text": one block, split in place */
static int add_tunable(struct sw_info *sw, const char *v)
{
    size_t l = strlen(v);
    char *b = malloc(l + 2), *f[4], *p = b;
    struct sw_tunable *t;
    int i;

    memcpy(b, v, l + 1);
    b[l + 1] = 0;                       /* an empty string for "no group" */
    for (i = 0; i < 4; i++) {
        while (*p == ' ')
            p++;
        f[i] = p;
        if (i < 3) {
            while (*p && *p != ' ')
                p++;
            if (!*p || p == f[i]) {
                free(b);
                return -1;
            }
            *p++ = 0;
        }
    }
    sw->tun = realloc(sw->tun, (sw->ntun + 1) * sizeof(*sw->tun));
    t = &sw->tun[sw->ntun++];
    t->name = f[0];
    t->type = f[1];
    t->def = f[2];
    t->group = b + l + 1;
    t->text = f[3];
    if (f[3][0] == '[' && (p = strchr(f[3], ']'))) {
        *p++ = 0;
        t->group = f[3] + 1;
        while (*p == ' ')
            p++;
        t->text = p;
    }
    return 0;
}

static int start_el(struct xp *x, struct state *st, const char *tag, const struct attrs *a)
{
    struct sw_info *sw = st->cur;
    const char *n = aget(a, "name"), *v = aget(a, "value");

    if (!strcmp(tag, "softwarelist")) {
        if (!n)
            return fail(x, "<softwarelist> without name");
        free(st->list);
        st->list = strdup(n);
    } else if (!strcmp(tag, "software")) {
        if (!st->list)
            return fail(x, "<software> outside <softwarelist>");
        if (sw)
            return fail(x, "nested <software>");
        if (!n)
            return fail(x, "<software> without name");
        if (sw_find(n)) {
            char m[160];

            snprintf(m, sizeof(m), "software %s is defined twice (names are unique across lists)", n);
            return fail(x, m);
        }
        if (nsw == capsw) {
            capsw = capsw ? capsw * 2 : 16;
            sws = realloc(sws, capsw * sizeof(*sws));
        }
        sw = st->cur = &sws[nsw++];
        memset(sw, 0, sizeof(*sw));
        sw->list = strdup(st->list);
        sw->name = strdup(n);
        sw->cloneof = dupnull(aget(a, "cloneof"));
        sw->supported = dupnull(aget(a, "supported"));
        st->sw_depth = st->depth;
    } else if (sw) {
        if (!strcmp(tag, "description") || !strcmp(tag, "year") || !strcmp(tag, "publisher")) {
            snprintf(st->text_tag, sizeof(st->text_tag), "%s", tag);
            free(st->text);
            st->text = strdup("");
        } else if (!strcmp(tag, "info")) {
            if (!n || !v)
                return fail(x, "info without name or value");
            if (!strcmp(n, "version"))
                sw->version = strdup(v);
            if (!strcmp(n, "tunable") && add_tunable(sw, v))
                return fail(x, "tunable is not \"NAME type default text\"");
            sw->info = realloc(sw->info, (sw->ninfo + 1) * sizeof(*sw->info));
            sw->info[sw->ninfo].name = strdup(n);
            sw->info[sw->ninfo].value = strdup(v);
            sw->ninfo++;
        } else if (!strcmp(tag, "feature") || !strcmp(tag, "sharedfeat")) {
            if (!n || !v)
                return fail(x, "feature without name or value");
            sw->feat = realloc(sw->feat, (sw->nfeat + 1) * sizeof(*sw->feat));
            sw->feat[sw->nfeat].name = strdup(n);
            sw->feat[sw->nfeat].value = strdup(v);
            sw->nfeat++;
        } else if (!strcmp(tag, "disk")) {
            sw->disk = dupnull(n);
            sw->disk_sha1 = dupnull(aget(a, "sha1"));
        } else if (!strcmp(tag, "rom")) {
            const char *sz = aget(a, "size");

            if (!n)
                return fail(x, "<rom> without name");
            sw->rom = realloc(sw->rom, (sw->nrom + 1) * sizeof(*sw->rom));
            sw->rom[sw->nrom].name = strdup(n);
            sw->rom[sw->nrom].sha1 = dupnull(aget(a, "sha1"));
            sw->rom[sw->nrom].size = sz ? strtoll(sz, NULL, 10) : -1;
            sw->nrom++;
        }
    }
    return 0;
}

static void end_el(struct state *st, const char *tag)
{
    struct sw_info *sw = st->cur;

    if (sw && st->text && !strcmp(tag, st->text_tag)) {
        char *t = st->text, *e;

        while (isspace((unsigned char)*t))
            t++;
        for (e = t + strlen(t); e > t && isspace((unsigned char)e[-1]); e--)
            ;
        *e = 0;
        t = strdup(t);
        if (!strcmp(tag, "description"))
            sw->description = t;
        else if (!strcmp(tag, "year"))
            sw->year = t;
        else
            sw->publisher = t;
        free(st->text);
        st->text = NULL;
        st->text_tag[0] = 0;
    }
    if (sw && !strcmp(tag, "software")) {
        if (!sw->description)
            sw->description = strdup(sw->name);
        st->cur = NULL;
    }
}

static int parse(struct xp *x)
{
    struct state st;
    char stack[16][32];
    int rc = 0;

    memset(&st, 0, sizeof(st));
    while (!rc && x->p < x->end) {
        if (*x->p != '<') {
            const char *t = x->p;

            while (x->p < x->end && *x->p != '<') {
                if (*x->p == '\n')
                    x->line++;
                x->p++;
            }
            if (st.text) {
                char *chunk = xstrndup(t, x->p - t);
                size_t a = strlen(st.text);

                unescape(chunk);
                st.text = realloc(st.text, a + strlen(chunk) + 1);
                strcpy(st.text + a, chunk);
                free(chunk);
            } else if (!st.depth) {
                for (; t < x->p; t++)
                    if (!isspace((unsigned char)*t)) {
                        rc = fail(x, "text outside an element");
                        break;
                    }
            }
            continue;
        }
        if (x->end - x->p >= 4 && !memcmp(x->p, "<!--", 4)) {
            x->p += 4;
            rc = skip_to(x, "-->");
        } else if (x->end - x->p >= 2 && !memcmp(x->p, "<?", 2)) {
            rc = skip_to(x, "?>");
        } else if (x->end - x->p >= 9 && !memcmp(x->p, "<!DOCTYPE", 9)) {
            int br = 0;

            for (; x->p < x->end; x->p++) {
                if (*x->p == '\n')
                    x->line++;
                else if (*x->p == '[')
                    br++;
                else if (*x->p == ']')
                    br--;
                else if (*x->p == '>' && !br)
                    break;
            }
            x->p++;
        } else if (x->end - x->p >= 2 && x->p[1] == '/') {
            const char *n;

            x->p += 2;
            n = x->p;
            while (x->p < x->end && name_char((unsigned char)*x->p))
                x->p++;
            if (!st.depth || strlen(stack[st.depth - 1]) != (size_t)(x->p - n)
                || memcmp(stack[st.depth - 1], n, x->p - n)) {
                rc = fail(x, "mismatched end tag");
                break;
            }
            ws(x);
            if (x->p >= x->end || *x->p != '>') {
                rc = fail(x, "bad end tag");
                break;
            }
            x->p++;
            st.depth--;
            end_el(&st, stack[st.depth]);
        } else {
            struct attrs a;
            char tag[32];
            const char *n;
            int empty = 0, i;

            a.n = 0;
            x->p++;
            n = x->p;
            while (x->p < x->end && name_char((unsigned char)*x->p))
                x->p++;
            if (x->p == n || x->p - n >= (int)sizeof(tag)) {
                rc = fail(x, "bad tag name");
                break;
            }
            memcpy(tag, n, x->p - n);
            tag[x->p - n] = 0;
            for (;;) {
                const char *k, *v;
                char q;

                ws(x);
                if (x->p >= x->end) {
                    rc = fail(x, "unterminated tag");
                    break;
                }
                if (*x->p == '>') {
                    x->p++;
                    break;
                }
                if (*x->p == '/' && x->p + 1 < x->end && x->p[1] == '>') {
                    x->p += 2;
                    empty = 1;
                    break;
                }
                k = x->p;
                while (x->p < x->end && name_char((unsigned char)*x->p))
                    x->p++;
                if (x->p == k) {
                    rc = fail(x, "bad attribute");
                    break;
                }
                if (a.n == MAXATTR) {
                    rc = fail(x, "too many attributes");
                    break;
                }
                a.k[a.n] = xstrndup(k, x->p - k);
                ws(x);
                if (x->p >= x->end || *x->p != '=') {
                    free(a.k[a.n]);
                    rc = fail(x, "attribute without value");
                    break;
                }
                x->p++;
                ws(x);
                if (x->p >= x->end || (*x->p != '"' && *x->p != '\'')) {
                    free(a.k[a.n]);
                    rc = fail(x, "unquoted attribute value");
                    break;
                }
                q = *x->p++;
                v = x->p;
                while (x->p < x->end && *x->p != q && *x->p != '<')
                    x->p++;
                if (x->p >= x->end || *x->p != q) {
                    free(a.k[a.n]);
                    rc = fail(x, "unterminated attribute value");
                    break;
                }
                a.v[a.n] = xstrndup(v, x->p - v);
                unescape(a.v[a.n]);
                x->p++;
                a.n++;
            }
            if (!rc) {
                if (st.depth == 16)
                    rc = fail(x, "nested too deep");
                else {
                    snprintf(stack[st.depth++], sizeof(stack[0]), "%s", tag);
                    rc = start_el(x, &st, tag, &a);
                    if (!rc && empty) {
                        st.depth--;
                        end_el(&st, tag);
                    }
                }
            }
            for (i = 0; i < a.n; i++) {
                free(a.k[i]);
                free(a.v[i]);
            }
        }
    }
    if (!rc && st.depth)
        rc = fail(x, "unclosed element");
    if (!rc && st.cur)
        rc = fail(x, "unclosed <software>");
    free(st.list);
    free(st.text);
    return rc;
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    struct xp x;
    char *buf;
    long n;
    int rc;

    if (!f) {
        snprintf(rs_error, sizeof(rs_error), "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        snprintf(rs_error, sizeof(rs_error), "cannot read %s", path);
        return -1;
    }
    fclose(f);
    buf[n] = 0;
    x.file = path;
    x.p = buf;
    x.end = buf + n;
    x.line = 1;
    rc = parse(&x);
    free(buf);
    return rc;
}

static int load_dir(const char *dir)
{
    char path[RS_PATH];
    int rc = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(path, sizeof(path), "%s\\*.xml", dir);
    h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        if (!rc)
            rc = load_file(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    struct dirent **ents;
    int n, i;

    n = scandir(dir, &ents, NULL, alphasort);
    if (n < 0)
        return 0;
    for (i = 0; i < n; i++) {
        size_t l = strlen(ents[i]->d_name);

        if (!rc && l > 4 && !strcmp(ents[i]->d_name + l - 4, ".xml")) {
            snprintf(path, sizeof(path), "%s/%s", dir, ents[i]->d_name);
            rc = load_file(path);
        }
        free(ents[i]);
    }
    free(ents);
#endif
    return rc;
}

int sw_load(const char *hashpath)
{
    char buf[RS_PATH * 4], *save = NULL, *d;

    snprintf(buf, sizeof(buf), "%s", hashpath ? hashpath : "");
    for (d = strtok_r(buf, ";", &save); d; d = strtok_r(NULL, ";", &save))
        if (*d && load_dir(d))
            return -1;
    if (!nsw) {
        snprintf(rs_error, sizeof(rs_error), "no software lists (hash/*.xml) in %s", hashpath ? hashpath : "-");
        return -1;
    }
    return 0;
}
