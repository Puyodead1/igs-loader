/*
 * We Dance Online 2's player titles: model/appellation.ini and the debug
 * model/test_appellation.ini (Big5 text).  wiki Format-WEDO-TITLES.
 *
 *   wedotitle  cardinfo_iAppellationLoadFile 0x814A8F0: "Total <n>", then
 *              "Appellation <i>" "{" keyword lines "}" blocks
 *   wedotitletest  cardinfo_iTestAppellationLoadFile 0x814A060: "<Key> <int>" lines
 *
 * The game reads 127-byte pieces (fgets 128) and splits them on " ,\n\t";
 * every token not a keyword is skipped (so "//" comments and anything after
 * a keyword's arguments are ignored), a CR ends a token.  Numbers go
 * through strtol as in the game (the shipped file's "NUUL" reads as 0); an
 * operator other than AND / OR is "NULL" (-1).
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

/* ---- the game's tokenizer ---------------------------------------------------------------- */

typedef struct tk {
    const char *p, *e;
    char piece[128];
    char *save;                 /* strtok_r state in piece */
} tk;

static char *my_strtok(char *s, char **save, const char *delim)
{
    char *t;

    if (s)
        *save = s;
    if (!*save)
        return NULL;
    t = *save + strspn(*save, delim);
    if (!*t) {
        *save = NULL;
        return NULL;
    }
    s = t + strcspn(t, delim);
    if (*s)
        *s++ = 0;
    else
        s = NULL;
    *save = s;
    /* cardinfo_iAppellationStrRevise: a CR ends the token */
    if ((s = strchr(t, '\r')))
        *s = 0;
    return t;
}

/* the next fgets(128) piece's first token, or NULL at the end of the file */
static int next_piece(tk *k)
{
    size_t n = 0;

    if (k->p >= k->e)
        return 0;
    while (k->p < k->e && n < 127) {
        char ch = *k->p++;

        k->piece[n++] = ch;
        if (ch == '\n')
            break;
    }
    k->piece[n] = 0;
    k->save = NULL;
    return 1;
}

/* the next token in the current piece (the game never reads across pieces for arguments) */
static char *next(tk *k, int first)
{
    return my_strtok(first ? k->piece : NULL, &k->save, " ,\n\t");
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;

        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* ---- appellation.ini --------------------------------------------------------------------- */

/* keyword, kind: o = AND/OR/NULL then n values, v = n values (NULL = -1), s = strings, t = TRUE, e = int + string */
static const struct kw { const char *name; char kind; int n; } kws[] = {
    { "boy_Title", 's', 1 }, { "girl_Title", 's', 1 },
    { "Level_Cond", 'o', 3 }, { "Vs_Cond", 'o', 3 }, { "VsRank_Cond", 'o', 3 }, { "VsProbability", 'o', 3 },
    { "MaxCombo", 'o', 3 }, { "FighterLevel", 'o', 4 }, { "KeepWin", 'o', 3 }, { "keepLose", 'o', 3 },
    { "Particular", 'v', 1 }, { "NpcNo", 'v', 1 }, { "Challenge", 'v', 1 }, { "SongLevel", 'v', 1 },
    { "Probability", 'v', 1 }, { "Priority", 'v', 1 }, { "SongRestrict", 'v', 4 }, { "FighterContinue", 'v', 2 },
    { "Essential_Title", 'v', 3 }, { "DateTime", 'd', 6 }, { "repeatedly", 't', 1 },
    { "boy_MessageBox", 's', 1 }, { "girl_MessageBox", 's', 1 },
    { "boy_equipment", 's', 2 }, { "girl_equipment", 's', 2 },
    { "boy_AttchEmi", 'e', 2 }, { "girl_AttchEmi", 'e', 2 },
};

static int value(igsa_ctx *c, const char *t, const char *key, int line)
{
    char *e;
    long v;

    if (!t)
        return igsa_fail(c, "line %d: %s: argument missing", line, key);
    if (ieq(t, "NULL")) {
        igsa_jint(c, NULL, -1);
        return 0;
    }
    v = strtol(t, &e, 10);      /* as the game: a typo reads as 0 */
    igsa_jint(c, NULL, v);
    return 0;
}

static int parse_titles(igsa_ctx *c)
{
    tk k = { (const char *)c->d, (const char *)c->d + c->n, "", NULL };
    int state = 0, line = 0, total = -1, blocks = 0, open = 0;
    int seen[sizeof(kws) / sizeof(kws[0])] = { 0 };

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance appellation.ini");
    while (next_piece(&k)) {
        char *t = next(&k, 1);

        line++;
        for (; t; t = next(&k, 0)) {
            if (state == 0) {               /* before "Total" */
                if (ieq(t, "Total")) {
                    char *v = next(&k, 0);

                    if (!v)
                        return igsa_fail(c, "line %d: Total without a count", line);
                    total = atoi(v);
                    igsa_jint(c, "total", total);
                    igsa_jarr(c, "titles");
                    state = 1;
                }
            } else if (state == 1) {        /* between blocks */
                if (ieq(t, "Appellation")) {
                    char *v = next(&k, 0);

                    if (!v)
                        return igsa_fail(c, "line %d: Appellation without a number", line);
                    if (atoi(v) > total || atoi(v) < 0)
                        return igsa_fail(c, "line %d: title %s past Total %d", line, v, total);
                    igsa_jobj(c, NULL);
                    igsa_jint(c, "id", atoi(v));
                    open = 1;
                    state = 2;
                }
            } else if (state == 2) {        /* the token after "Appellation n" must be "{" */
                if (!ieq(t, "{")) {   /* the game drops the block */
                    igsa_jbool(c, "dropped", 1);
                    igsa_jend(c);
                    open = 0;
                    state = 1;
                    continue;
                }
                memset(seen, 0, sizeof(seen));
                state = 3;
            } else {                        /* in a block */
                size_t i;

                if (ieq(t, "}")) {
                    igsa_jend(c);
                    open = 0;
                    blocks++;
                    c->entries++;
                    state = 1;
                    continue;
                }
                for (i = 0; i < sizeof(kws) / sizeof(kws[0]) && !ieq(t, kws[i].name); i++)
                    ;
                if (i == sizeof(kws) / sizeof(kws[0]))
                    continue;           /* comments and other tokens */
                {
                    const struct kw *w = &kws[i];
                    char key[48];
                    int j;

                    /* a keyword repeated in a block (equipment, effects, message lines): name_1, name_2 ... */
                    if (seen[i]++)
                        snprintf(key, sizeof(key), "%s_%d", w->name, seen[i] - 1);
                    else
                        snprintf(key, sizeof(key), "%s", w->name);

                    switch (w->kind) {
                    case 's':
                        if (w->n == 1) {
                            char *v = next(&k, 0);

                            if (!v)
                                return igsa_fail(c, "line %d: %s: argument missing", line, w->name);
                            igsa_jbig5(c, key, v, strlen(v));
                        } else {
                            char *a = next(&k, 0), *b = next(&k, 0);

                            if (!a || !b)
                                return igsa_fail(c, "line %d: %s: arguments missing", line, w->name);
                            igsa_jobj(c, key);
                            igsa_jstr(c, "item", a);
                            igsa_jbig5(c, "name", b, strlen(b));
                            igsa_jend(c);
                        }
                        break;
                    case 'o': {
                        char *op = next(&k, 0);

                        if (!op)
                            return igsa_fail(c, "line %d: %s: operator missing", line, w->name);
                        igsa_jobj(c, key);
                        igsa_jstr(c, "op", ieq(op, "AND") ? "AND" : ieq(op, "OR") ? "OR" : "NULL");
                        igsa_jarr(c, "values");
                        for (j = 0; j < w->n; j++)
                            if (value(c, next(&k, 0), w->name, line))
                                return -1;
                        igsa_jend(c);
                        igsa_jend(c);
                        break;
                    }
                    case 'v':
                        if (w->n == 1) {
                            char *v = next(&k, 0), *e;

                            if (!v)
                                return igsa_fail(c, "line %d: %s: argument missing", line, w->name);
                            if (ieq(v, "NULL"))
                                igsa_jint(c, key, -1);
                            else {
                                igsa_jint(c, key, strtol(v, &e, 10));
                            }
                        } else {
                            igsa_jarr(c, key);
                            for (j = 0; j < w->n; j++)
                                if (value(c, next(&k, 0), w->name, line))
                                    return -1;
                            igsa_jend(c);
                        }
                        break;
                    case 'd': {             /* up to six numbers, also split on '/'; NULL = none */
                        char *v;

                        igsa_jarr(c, key);
                        for (j = 0; j < 6 && (v = my_strtok(NULL, &k.save, "/ ,\n\t")) && !ieq(v, "NULL"); j++)
                            igsa_jint(c, NULL, atoi(v));
                        igsa_jend(c);
                        break;
                    }
                    case 't': {
                        char *v = next(&k, 0);

                        igsa_jbool(c, key, v && ieq(v, "TRUE"));
                        break;
                    }
                    case 'e': {
                        char *a = next(&k, 0), *b = next(&k, 0);

                        if (!a || !b)
                            return igsa_fail(c, "line %d: %s: arguments missing", line, w->name);
                        igsa_jobj(c, key);
                        igsa_jint(c, "bone", atoi(a));
                        igsa_jstr(c, "emi", b);
                        igsa_jend(c);
                        break;
                    }
                    }
                }
            }
        }
    }
    if (state == 0)
        return igsa_fail(c, "no Total");
    if (open)
        return igsa_fail(c, "title block not closed");
    igsa_jend(c);
    igsa_jint(c, "blocks", blocks);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%d titles (Total %d)\n", blocks, total);
    return 0;
}

static const char *const test_keys[] = {
    "NowLevel", "NowWon", "NowLost", "NowWorldRank", "NowTitle", "NowMaxCombo", "NowWinFlag", "NowWonCombo",
    "NowContinueCounter", "LastLevel", "LastWorldRank", "LastMaxCombo", "LastWin", "Challenge", "SongLevel",
    "FighterLevel", "KeepWin", "Probability",
};

static int parse_test(igsa_ctx *c)
{
    tk k = { (const char *)c->d, (const char *)c->d + c->n, "", NULL };
    int line = 0;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance test_appellation.ini");
    while (next_piece(&k)) {
        char *t;

        line++;
        for (t = next(&k, 1); t; t = next(&k, 0)) {
            size_t i;

            for (i = 0; i < sizeof(test_keys) / sizeof(test_keys[0]) && !ieq(t, test_keys[i]); i++)
                ;
            if (i < sizeof(test_keys) / sizeof(test_keys[0])) {
                char *v = next(&k, 0), *e;
                long x;

                if (!v || (x = strtol(v, &e, 10), e == v))
                    return igsa_fail(c, "line %d: %s without a number", line, test_keys[i]);
                igsa_jint(c, test_keys[i], x);
                c->entries++;
            }
        }
    }
    igsa_jend(c);
    return 0;
}

/* ---- probes ------------------------------------------------------------------------------ */

static const char *skip_ws(const uint8_t *d, size_t n, size_t *o)
{
    while (*o < n && (d[*o] == ' ' || d[*o] == '\t' || d[*o] == '\r' || d[*o] == '\n'))
        (*o)++;
    return *o < n ? (const char *)d + *o : NULL;
}

static int has(const uint8_t *d, size_t n, const char *s)
{
    size_t l = strlen(s), i;

    for (i = 0; i + l <= n; i++)
        if (!memcmp(d + i, s, l))
            return 1;
    return 0;
}

static int probe_titles(const uint8_t *d, size_t n, const char *path)
{
    size_t o = 0;
    const char *s = skip_ws(d, n, &o);

    (void)path;
    return s && n - o > 6 && !memcmp(s, "Total ", 6) && has(d, n < 4096 ? n : 4096, "Appellation") ? 80 : 0;
}

static int probe_test(const uint8_t *d, size_t n, const char *path)
{
    size_t o = 0;
    const char *s = skip_ws(d, n, &o);

    (void)path;
    return s && n - o > 9 && !memcmp(s, "NowLevel ", 9) ? 80 : 0;
}

const igsa_format igsa_fmt_wedo_titles = { "wedotitle", "We Dance appellation.ini: player titles and their conditions",
                                            probe_titles, parse_titles };
const igsa_format igsa_fmt_wedo_titletest = { "wedotitletest", "We Dance test_appellation.ini: title test values",
                                               probe_test, parse_test };
