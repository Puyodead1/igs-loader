/*
 * E2000 sound lists (wiki Format-SNDTAB):
 *
 *   sound.cmd   the TSG tool's list of a bank's sounds, one .wav name per line
 *               (a TSGROM01 "   SOUND" chunk names it); no game binary reads it
 *   sound.txt and the other tables under sound/: "load <file.wav> <loop>" lines,
 *               // comments (Big5).  ActionGroupLoadSoundTable (sship 0x805EDF0):
 *               fgets(80), strtok(" \n\t"); after "load", the file (relative to the
 *               table's directory) and a flag whose first character '1' makes
 *               SoundPlay loop it; sound i of the group is the i-th "load".
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"


static int is_ws(int ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

/* strtok(" \n\t") */
static char *tok(char **sv)
{
    char *s = *sv, *t;

    while (*s == ' ' || *s == '\n' || *s == '\t')
        s++;
    if (!*s) {
        *sv = s;
        return NULL;
    }
    for (t = s; *t && *t != ' ' && *t != '\n' && *t != '\t'; t++)
        ;
    if (*t)
        *t++ = 0;
    *sv = t;
    return s;
}

static int lc_suffix(const char *s, size_t n, const char *suf)
{
    size_t k = strlen(suf), i;

    if (n < k)
        return 0;
    for (i = 0; i < k; i++) {
        int a = s[n - k + i];

        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (a != suf[i])
            return 0;
    }
    return 1;
}

/* 1: every non-blank line is one *.wav name; 2: every non-blank line starts with "load" or "//"; 0 */
static int kind(const uint8_t *d, size_t n)
{
    size_t o = 0;
    int lists = 1, loads = 1, lines = 0;

    if (!n)
        return 0;
    while (o < n) {
        const uint8_t *l = d + o, *e = memchr(l, '\n', n - o);
        size_t ln = e ? (size_t)(e - l) : n - o, a = 0, b;

        o += ln + 1;
        while (a < ln && is_ws(l[a]))
            a++;
        b = ln;
        while (b > a && is_ws(l[b - 1]))
            b--;
        if (a == b)
            continue;
        size_t i;

        for (i = a; i < b; i++)
            if (l[i] < 0x20 && l[i] != '\t')
                return 0;
        lines++;
        {
            size_t t = a;

            while (t < b && !is_ws(l[t]))
                t++;
            if (t != b || !lc_suffix((const char *)l + a, b - a, ".wav"))
                lists = 0;
            if (!((t - a == 4 && !memcmp(l + a, "load", 4)) || (b - a >= 2 && l[a] == '/' && l[a + 1] == '/')))
                loads = 0;
        }
    }
    if (!lines)
        return 0;
    return lists ? 1 : loads ? 2 : 0;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    int k = kind(d, n);

    if (k == 1)
        return path && lc_suffix(path, strlen(path), ".cmd") ? 80 : 30;
    if (k == 2)
        return 70;
    return 0;
}

static int parse(igsa_ctx *c)
{
    const char *d = (const char *)c->d;
    size_t n = c->n, o = 0;
    int k = kind(c->d, n), idx = 0;

    if (!k)
        return igsa_fail(c, "not a sound list");
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", k == 1 ? "TSG sound list (sound.cmd)" : "sound table (ActionGroupLoadSoundTable)");
    igsa_jarr(c, "sounds");
    if (k == 1) {
        while (o < n) {
            const char *l = d + o, *e = memchr(l, '\n', n - o);
            size_t ln = e ? (size_t)(e - l) : n - o, a = 0, b = ln;

            o += ln + 1;
            while (a < b && is_ws(l[a]))
                a++;
            while (b > a && is_ws(l[b - 1]))
                b--;
            if (a == b)
                continue;
            igsa_jobj(c, NULL);
            igsa_jint(c, "index", idx++);
            igsa_jbig5(c, "file", l + a, b - a);
            igsa_jend(c);
            c->entries++;
        }
    } else {
        /* the loader's loop: fgets(80) pieces; a last piece that ends at EOF is dropped */
        int line = 1;

        while (o < n) {
            char buf[81], *t, *sv = buf;
            size_t m = 0, start = o;
            int eof, nl;

            while (m < 79 && o < n) {
                buf[m++] = d[o];
                if (d[o++] == '\n')
                    break;
            }
            buf[m] = 0;
            eof = o >= n && buf[m - 1] != '\n';
            if (eof)
                break;      /* fgets hit EOF: feof() stops the loop before this piece is used */
            nl = buf[m - 1] == '\n';
            for (t = tok(&sv); t; t = tok(&sv)) {
                char *f, *fl;

                if (strcmp(t, "load"))
                    continue;
                if (!(f = tok(&sv)) || !(fl = tok(&sv)))
                    return igsa_fail(c, "line %d: \"load\" without file and flag (the loader would crash)", line);
                igsa_jobj(c, NULL);
                igsa_jint(c, "index", idx++);
                igsa_jbig5(c, "file", f, strlen(f));
                igsa_jbool(c, "loop", *fl == '1');
                igsa_jint(c, "line", line);
                {
                    /* the // comment of this piece, if any (the loader skips its tokens) */
                    const char *cm = NULL, *pe = d + o;
                    const char *q;

                    for (q = d + start; q + 1 < pe; q++)
                        if (q[0] == '/' && q[1] == '/') {
                            cm = q + 2;
                            break;
                        }
                    if (cm) {
                        while (cm < pe && (*cm == ' ' || *cm == '\t'))
                            cm++;
                        while (pe > cm && is_ws(pe[-1]))
                            pe--;
                        igsa_jbig5(c, "comment", cm, (size_t)(pe - cm));
                    }
                }
                igsa_jend(c);
                c->entries++;
            }
            if (nl)
                line++;
        }
        if (d[n - 1] != '\n')
            igsa_jbool(c, "last_line_ignored", 1);
    }
    igsa_jend(c);
    igsa_jint(c, "count", idx);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_sndtab = { "sndtab", "E2000 sound list (sound.cmd) or sound table (load <wav> <loop>)", probe, parse };
