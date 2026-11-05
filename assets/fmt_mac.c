/*
 * TSG action name headers (*.mac beside the E2000/E1000 image/ banks): the
 * TSG tool's C header for a TSGROM01 bank, Big5 text.  No game binary reads
 * them.  wiki Format-MAC.
 *
 *   a comment block: "<<<< ACTION information file of TSG >>>>", then
 *     "action 檔案 : <source>.act", "ID : <bank id>", "outname : <bank>",
 *     "title : <title>", "creator : <name>"
 *   then "#define <name> <action index>" lines, each with a C comment
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static const char banner[] = "ACTION information file of TSG";

static const char *find(const char *h, size_t hn, const char *n, size_t nn)
{
    size_t i;

    for (i = 0; i + nn <= hn; i++)
        if (!memcmp(h + i, n, nn))
            return h + i;
    return NULL;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    size_t i = 0, lim = n < 512 ? n : 512;

    (void)path;
    while (i < lim && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n'))
        i++;
    if (i + 2 > lim || d[i] != '/' || d[i + 1] != '*')
        return 0;
    for (; i + sizeof(banner) - 1 <= lim; i++)
        if (!memcmp(d + i, banner, sizeof(banner) - 1))
            return 100;
    return 0;
}

static size_t trim(const char **s, size_t n)
{
    while (n && (**s == ' ' || **s == '\t')) {
        (*s)++;
        n--;
    }
    while (n && ((*s)[n - 1] == ' ' || (*s)[n - 1] == '\t' || (*s)[n - 1] == '\r'))
        n--;
    return n;
}

static int parse(igsa_ctx *c)
{
    const char *d = (const char *)c->d, *end = d + c->n, *p = d;
    int in_comment = 0, closed = 0, defines = 0, lineno = 0;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "TSG action header");
    igsa_jobj(c, "header");
    /* the comment block: "key : value" lines */
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p)), *l = p, *k;
        size_t n = (size_t)((e ? e : end) - p);

        p = e ? e + 1 : end;
        lineno++;
        n = trim(&l, n);
        if (!in_comment) {
            if (!n)
                continue;
            if (n >= 2 && l[0] == '/' && l[1] == '*') {
                in_comment = 1;
                continue;
            }
            return igsa_fail(c, "line %d: no header comment", lineno);
        }
        if (n >= 2 && l[n - 2] == '*' && l[n - 1] == '/') {
            closed = 1;
            break;
        }
        if (!n || find(l, n, banner, sizeof(banner) - 1))
            continue;
        if (!(k = memchr(l, ':', n)))
            return igsa_fail(c, "line %d: header line without ':'", lineno);
        {
            const char *kk = l, *v = k + 1;
            size_t kn = trim(&kk, (size_t)(k - l)), vn = trim(&v, (size_t)(l + n - v));
            char key[64];

            if (!kn || kn >= sizeof(key))
                return igsa_fail(c, "line %d: header key", lineno);
            /* the source-file key is Big5 ("action 檔案"): name it */
            if (kn > 6 && !memcmp(kk, "action", 6))
                strcpy(key, "action_file");
            else {
                memcpy(key, kk, kn);
                key[kn] = 0;
            }
            if (!strcmp(key, "ID"))
                igsa_jint(c, "id", strtol(v, NULL, 10));
            else
                igsa_jbig5(c, key, v, vn);
        }
    }
    igsa_jend(c);
    if (!closed)
        return igsa_fail(c, "no header comment");
    igsa_jarr(c, "defines");
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p)), *l = p, *q, *cm;
        size_t n = (size_t)((e ? e : end) - p);
        const char *name, *val;
        size_t nn, vn;

        p = e ? e + 1 : end;
        lineno++;
        n = trim(&l, n);
        if (!n)
            continue;
        if (n < 8 || memcmp(l, "#define", 7) || (l[7] != ' ' && l[7] != '\t'))
            return igsa_fail(c, "line %d: not a #define", lineno);
        q = l + 7;
        while (q < l + n && (*q == ' ' || *q == '\t'))
            q++;
        name = q;
        while (q < l + n && *q != ' ' && *q != '\t')
            q++;
        nn = (size_t)(q - name);
        while (q < l + n && (*q == ' ' || *q == '\t'))
            q++;
        val = q;
        while (q < l + n && *q != ' ' && *q != '\t')
            q++;
        vn = (size_t)(q - val);
        if (!nn || !vn)
            return igsa_fail(c, "line %d: #define without name or value", lineno);
        igsa_jobj(c, NULL);
        igsa_jbig5(c, "name", name, nn);
        {
            char num[32];
            char *x;
            long v;

            snprintf(num, sizeof(num), "%.*s", (int)(vn < 31 ? vn : 31), val);
            v = strtol(num, &x, 0);
            if (*x)
                igsa_jbig5(c, "value", val, vn);
            else
                igsa_jint(c, "action", v);
        }
        if ((cm = find(q, (size_t)(l + n - q), "/*", 2))) {
            const char *ce = find(cm + 2, (size_t)(l + n - cm - 2), "*/", 2), *t = cm + 2;
            size_t tn = trim(&t, (size_t)((ce ? ce : l + n) - t));

            igsa_jbig5(c, "comment", t, tn);
        }
        igsa_jend(c);
        defines++;
        c->entries++;
    }
    igsa_jend(c);
    igsa_jint(c, "count", defines);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_mac = { "mac", "TSG action header (.mac): action names of a TSGROM01 bank", probe, parse };
