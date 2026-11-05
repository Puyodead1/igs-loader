/*
 * We Dance Online 2's text tables, decoded to named fields (wiki
 * Format-WEDO-CHART, -EFF, -SONGLIST, -TABLES, -INFO (keyerror.dat), Format-ZHL):
 *
 *   wedochart  sound/music/csv/<n>.csv, a song's chart: cursor_iCsvLoad 0x80AE580
 *   wedoeff    sound/music/csv/<n>.eff, its stage: effect_iCsvLoad 0x8177570
 *   wedolist   sound/music/csvlist*.csv/.ini/.bak, the song list (header row names
 *              the columns): csvlist_iLoad 0x80CA5F0
 *   wedotable  model/<part>.mlt, clothes_probability.ini, npc_list.lst: Big5 tables,
 *              every byte + 5 (cardinfo_iTransDecode 0x8153140), header row, "END";
 *              cardinfo_iAvatarTableInit 0x8142B50
 *   wedoavset  model/avatar_setting.set: Big5 "label<TAB>count" lines (the same)
 *   wedokeyerr info/keyerror.dat: Igs036_iKeyTestInit 0x807DC20
 *   wedozhl    .zhl GLSL effects: zlEffect::Load 0x82FECD0, zlEffectMgr::LoadZHL 0x8300900
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

/* ---- lines and tokens -------------------------------------------------------------------- */

typedef struct lines { const char *p, *e; char buf[4096]; int no; } lines;

static void lines_init(lines *l, const uint8_t *d, size_t n)
{
    l->p = (const char *)d;
    l->e = (const char *)d + n;
    l->no = 0;
}

/* the next line without CR/LF (cut at 4095), NULL at the end */
static char *next_line(lines *l)
{
    const char *q;
    size_t k;

    if (l->p >= l->e)
        return NULL;
    q = memchr(l->p, '\n', (size_t)(l->e - l->p));
    if (!q)
        q = l->e;
    k = (size_t)(q - l->p);
    if (k > sizeof(l->buf) - 1)
        k = sizeof(l->buf) - 1;
    memcpy(l->buf, l->p, k);
    l->buf[k] = 0;
    while (k && (l->buf[k - 1] == '\r'))
        l->buf[--k] = 0;
    l->p = q < l->e ? q + 1 : q;
    l->no++;
    return l->buf;
}

/* strtok without the hidden state */
static char *tok(char **s, const char *delim)
{
    char *p = *s, *t;

    if (!p)
        return NULL;
    p += strspn(p, delim);
    if (!*p) {
        *s = NULL;
        return NULL;
    }
    t = p;
    p += strcspn(p, delim);
    if (*p)
        *p++ = 0;
    *s = p;
    return t;
}

static int isnum(const char *s)
{
    char *e;

    if (!s || !*s)
        return 0;
    (void)strtod(s, &e);
    return !*e;
}

static const char *base(const char *path)
{
    const char *b = path, *p;

    if (!path)
        return "";
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static int ext_is(const char *path, const char *ext)
{
    const char *b = base(path), *d = strrchr(b, '.');

    return d && !strcasecmp(d, ext);
}

static int text_ok(const uint8_t *d, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (d[i] < 9 || (d[i] > 13 && d[i] < 32))
            return 0;
    return 1;
}

static void jtext(igsa_ctx *c, const char *key, const char *s)
{
    igsa_jbig5(c, key, s, strlen(s));
}

static void jval(igsa_ctx *c, const char *key, const char *s)
{
    if (isnum(s) && strchr(s, '.'))
        igsa_jnum(c, key, strtod(s, NULL));
    else if (isnum(s))
        igsa_jint(c, key, strtoll(s, NULL, 10));
    else
        jtext(c, key, s);
}

static const char *const SEP = " ,\n\t";

/* ---- wedochart: <n>.csv --------------------------------------------------------------------- */

static int chart_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n > 16 && text_ok(d, n < 512 ? n : 512) && !strncasecmp((const char *)d, "Start,", 6) ? 85 : 0;
}

static const char *const ntype[8] = { "none", "tap", "link", "3", "press", "5", "6", "7" };

static int chart_parse(igsa_ctx *c)
{
    lines l;
    char *s, *t, *p;
    int block = 0, nodes = -1, got = 0, ignored = 0;

    lines_init(&l, c->d, c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance chart");
    while ((s = next_line(&l))) {
        while (*s == ' ')
            s++;
        if (*s == ';' || *s == '#' || !*s)
            continue;
        if (block) {
            if (s[0] == '*' && s[1] == '/')
                block = 0;
            continue;
        }
        if (s[0] == '/' && s[1] == '/')
            continue;
        if (s[0] == '/' && s[1] == '*') {
            block = 1;
            continue;
        }
        p = s;
        while ((t = tok(&p, SEP))) {
            if (!strcasecmp(t, "Start")) {
                char *v = tok(&p, SEP);

                igsa_jint(c, "start", v ? atol(v) : 0);
            } else if (!strcasecmp(t, "ChangeTempo") || !strcasecmp(t, "ChangeRhythm")) {
                int tempo = t[6] == 'T' || t[6] == 't', k, i;
                char *v = tok(&p, SEP);

                k = v ? atoi(v) & 255 : 0;
                igsa_jarr(c, tempo ? "tempo_changes" : "rhythm_changes");
                for (i = 0; i < k; i++) {
                    char *q, *a, *b;

                    if (!(q = next_line(&l)))
                        return igsa_fail(c, "line %d: %s %d of %d missing", l.no, tempo ? "tempo" : "rhythm", i, k);
                    a = tok(&q, SEP);
                    b = tok(&q, SEP);
                    if (!isnum(a) || !isnum(b))
                        return igsa_fail(c, "line %d: %s entry '%s,%s'", l.no, tempo ? "tempo" : "rhythm", a ? a : "",
                                         b ? b : "");
                    igsa_jobj(c, NULL);
                    igsa_jint(c, "section", atoi(a) & 255);
                    if (tempo)
                        igsa_jnum(c, "bpm", strtod(b, NULL));
                    else
                        igsa_jint(c, "beats", atoi(b) & 255);
                    igsa_jend(c);
                }
                igsa_jend(c);
            } else if (!strcasecmp(t, "BgWav") || !strcasecmp(t, "PulseWav") || !strcasecmp(t, "HitsWav") ||
                       !strcasecmp(t, "PressWav")) {
                char *v = tok(&p, SEP);

                jtext(c, !strcasecmp(t, "BgWav") ? "music" : !strcasecmp(t, "PulseWav") ? "pulse_wav" :
                          !strcasecmp(t, "HitsWav") ? "hits_wav" : "press_wav", v ? v : "");
            } else if (!strcasecmp(t, "TotalNode")) {
                char *v = tok(&p, SEP);
                int i;

                nodes = v ? atoi(v) & 0xFFFF : 0;
                igsa_jint(c, "total_nodes", nodes);
                igsa_jarr(c, "nodes");
                for (i = 0; i < nodes; i++) {
                    char *q, *a, *b, *f;
                    unsigned long fl;
                    int lv;

                    if (!(q = next_line(&l)))
                        break;
                    a = tok(&q, SEP);
                    b = tok(&q, SEP);
                    f = tok(&q, SEP);
                    if (!isnum(a) || !isnum(b) || !isnum(f))
                        return igsa_fail(c, "line %d: node %d '%s,%s,%s'", l.no, i, a ? a : "", b ? b : "", f ? f : "");
                    fl = strtoul(f, NULL, 10);
                    igsa_jobj(c, NULL);
                    igsa_jint(c, "tick", atoi(a) & 0xFFFF);
                    igsa_jint(c, "ms", atoi(b));
                    igsa_juint(c, "flags", fl);
                    igsa_jbool(c, "beat", fl >> 1 & 1);
                    igsa_jarr(c, "levels");     /* 3 bits per difficulty from bit 2 */
                    for (lv = 0; lv < 10; lv++)
                        igsa_jstr(c, NULL, ntype[fl >> (3 * lv + 2) & 7]);
                    igsa_jend(c);
                    igsa_jend(c);
                    got++;
                }
                igsa_jend(c);
                if (got != nodes)
                    return igsa_fail(c, "%d of %d nodes", got, nodes);
                c->entries++;
            } else
                ignored++;
        }
    }
    if (nodes < 0)
        return igsa_fail(c, "no TotalNode");
    igsa_jint(c, "ignored_tokens", ignored);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %d nodes\n", base(c->path), "chart", got);
    return 0;
}

/* ---- wedoeff: <n>.eff --------------------------------------------------------------------- */

static int eff_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n > 16 && text_ok(d, n < 512 ? n : 512) && !strncasecmp((const char *)d, "StageMap,", 9) ? 85 : 0;
}

/* the name less its last character (the loader's strncpy(len - 1): the CR before LF) */
static void jcut(igsa_ctx *c, const char *key, const char *s)
{
    size_t k = strlen(s);

    igsa_jbig5(c, key, s, k && s[k - 1] == '\r' ? k - 1 : k);
}

static int eff_parse(igsa_ctx *c)
{
    lines l;
    char *s, *t, *p;
    int state = 0, cnt = 0, got = 0, packs = 0, open = 0, ignored = 0;
    const char *list = NULL;

    lines_init(&l, c->d, c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance stage effects");
    /* the loader reads lines whole: tokens keep their CR */
    while (l.p < l.e) {
        const char *q = memchr(l.p, '\n', (size_t)(l.e - l.p));
        size_t k = q ? (size_t)(q - l.p) : (size_t)(l.e - l.p);

        if (k > sizeof(l.buf) - 1)
            k = sizeof(l.buf) - 1;
        memcpy(l.buf, l.p, k);
        l.buf[k] = 0;
        l.p = q ? q + 1 : l.e;
        l.no++;
        p = s = l.buf;
        while ((t = tok(&p, SEP))) {
            char *v;
            static const struct { const char *kw; const char *key; int st; } kws[] = {
                { "StageDanceNum", "dances", 2 }, { "StageCameraNum", "cameras", 3 }, { "StageEffectNum", "effects", 4 },
                { "StageCameraTimer", "camera_timers", 5 }, { "StageEffectInitLoop", "init_loop", 6 },
                { "StageEffectCtlmClassNum", "ctlm_classes", 8 }, { "StageEffectCtlsClassNum", "ctls_classes", 9 },
                { "StageEffectCtlbClassNum", "ctlb_classes", 10 }, { "StageSongText", "song_text", 11 },
            };
            size_t i;
            int kw = -1;

            for (i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
                if (!strcasecmp(t, kws[i].kw))
                    kw = (int)i;
            if (!strcasecmp(t, "StageMap")) {
                if (list)
                    igsa_jend(c), list = NULL;
                v = tok(&p, SEP);
                jcut(c, "stage_map", v ? v : "");
                continue;
            }
            if (!strcasecmp(t, "StageEffectInitStop")) {
                if (list)
                    igsa_jend(c), list = NULL;
                v = tok(&p, SEP);
                igsa_jint(c, "init_stop", v ? atoi(v) : 0);
                state = 0;
                continue;
            }
            if (!strcasecmp(t, "StageActionStart")) {
                if (list)
                    igsa_jend(c), list = NULL;
                v = tok(&p, SEP);
                igsa_jint(c, "action_start", v ? atoi(v) : 0);
                igsa_jarr(c, "packs");
                list = "packs";
                state = 12;
                continue;
            }
            if (kw >= 0) {
                if (list)
                    igsa_jend(c), list = NULL;
                v = tok(&p, SEP);
                state = kws[kw].st;
                cnt = v ? atoi(v) : 0;
                got = 0;
                if (state == 5)
                    cnt = -1;           /* the timers' count is the camera count */
                igsa_jarr(c, kws[kw].key);
                list = kws[kw].key;
                if (state == 11 && !cnt)
                    state = 0;
                continue;
            }
            switch (state) {
            case 2:
            case 3:
            case 4:
                jcut(c, NULL, t);
                break;
            case 5:
            case 6:
                igsa_jint(c, NULL, atoi(t));
                break;
            case 8:             /* <index>, <class> */
                v = tok(&p, SEP);
                igsa_jint(c, NULL, v ? atoi(v) : 0);
                break;
            case 9:             /* <n>, n values */
            case 10: {          /* <id>, <n>, n values */
                int id = atoi(t), k, j;

                igsa_jobj(c, NULL);
                if (state == 10) {
                    igsa_jint(c, "id", id);
                    v = tok(&p, SEP);
                    k = v ? atoi(v) : 0;
                } else
                    k = id;
                igsa_jarr(c, "values");
                for (j = 0; j < k; j++) {
                    v = tok(&p, SEP);
                    igsa_jint(c, NULL, v ? atoi(v) : 0);
                }
                igsa_jend(c);
                igsa_jend(c);
                break;
            }
            case 11:            /* <id>, text to the line's end */
                v = tok(&p, ",\n\t");
                igsa_jobj(c, NULL);
                igsa_jint(c, "id", atoi(t));
                jcut(c, "text", v ? v : "");
                igsa_jend(c);
                break;
            case 12:
                if (!strcasecmp(t, "StagePack")) {
                    v = tok(&p, SEP);
                    if (open)
                        igsa_jend(c);
                    igsa_jobj(c, NULL);
                    igsa_jint(c, "pack", v ? atoi(v) : 0);
                    open = 1;
                    packs++;
                } else if (open && !strcasecmp(t, "Time")) {
                    v = tok(&p, SEP);
                    igsa_jint(c, "time", v ? atoi(v) : 0);
                } else if (open && !strcasecmp(t, "Dance")) {
                    v = tok(&p, SEP);
                    igsa_jint(c, "dance", v ? atoi(v) : 0);
                    v = tok(&p, SEP);
                    igsa_jnum(c, "rate", v ? strtod(v, NULL) : 0);
                } else if (open && !strcasecmp(t, "Camera")) {
                    v = tok(&p, SEP);
                    igsa_jint(c, "camera", v ? atoi(v) : 0);
                } else if (open && (!strcasecmp(t, "Ctlm") || !strcasecmp(t, "Ctls") || !strcasecmp(t, "Ctlb"))) {
                    int k = t[3] == 'b' || t[3] == 'B' ? 4 : 2, j;
                    char key[8];

                    snprintf(key, sizeof(key), "%c%c%c%c", 'c', 't', 'l', t[3] | 0x20);
                    igsa_jarr(c, key);
                    for (j = 0; j < k; j++) {
                        v = tok(&p, SEP);
                        igsa_jint(c, NULL, v ? atoi(v) : 0);
                    }
                    igsa_jend(c);
                } else if (open && !strcasecmp(t, "StagePackEnd")) {
                    igsa_jend(c);
                    open = 0;
                } else
                    ignored++;
                continue;
            default:
                ignored++;
                continue;
            }
            got++;
            if (cnt >= 0 && got >= cnt && state != 12) {
                state = 0;
                igsa_jend(c);
                list = NULL;
            }
        }
    }
    if (open)
        return igsa_fail(c, "StagePack %d without StagePackEnd", packs);
    if (list)
        igsa_jend(c);
    igsa_jint(c, "ignored_tokens", ignored);
    igsa_jend(c);
    c->entries++;
    return 0;
}

/* ---- wedolist: csvlist ------------------------------------------------------------------------ */

static int list_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n > 32 && !strncmp((const char *)d, "No,New,Hide,", 12) ? 90 : 0;
}

/* split a CSV line on commas, keeping empty fields */
static int csv_split(char *s, char **f, int max)
{
    int k = 0;

    while (k < max) {
        f[k++] = s;
        s = strchr(s, ',');
        if (!s)
            break;
        *s++ = 0;
    }
    return k;
}

static int list_parse(igsa_ctx *c)
{
    lines l;
    char *s, hdr[4096], *h[64], *f[64];
    int nh, nf, i, rows = 0;

    lines_init(&l, c->d, c->n);
    if (!(s = next_line(&l)))
        return igsa_fail(c, "empty");
    snprintf(hdr, sizeof(hdr), "%s", s);
    nh = csv_split(hdr, h, 64);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance song list");
    igsa_jarr(c, "columns");
    for (i = 0; i < nh; i++)
        igsa_jstr(c, NULL, h[i]);
    igsa_jend(c);
    igsa_jarr(c, "songs");
    while ((s = next_line(&l))) {
        if (!*s)
            continue;
        nf = csv_split(s, f, 64);
        if (nf > nh)            /* rows may stop short of the header (older lists have fewer columns) */
            return igsa_fail(c, "line %d: %d fields, the header has %d", l.no, nf, nh);
        igsa_jobj(c, NULL);
        for (i = 0; i < nf; i++)
            if (*f[i])
                jval(c, h[i], f[i]);
            else
                igsa_jstr(c, h[i], "");
        igsa_jend(c);
        rows++;
        c->entries++;
    }
    igsa_jend(c);
    igsa_jint(c, "count", rows);
    igsa_jend(c);
    return 0;
}

/* ---- wedotable: + 5 Big5 tables --------------------------------------------------------------- */

static uint8_t *undo5(const uint8_t *d, size_t n)
{
    uint8_t *u = malloc(n + 1);
    size_t i;

    if (u) {
        for (i = 0; i < n; i++)
            u[i] = (uint8_t)(d[i] - 5);
        u[n] = 0;
    }
    return u;
}

static int table_probe(const uint8_t *d, size_t n, const char *path)
{
    size_t i, k = n < 1024 ? n : 1024;
    int tabs = 0, lf = 0;

    (void)path;
    if (n < 16)
        return 0;
    for (i = 0; i < k; i++) {
        uint8_t x = (uint8_t)(d[i] - 5);

        if (x < 0x20 && x != 9 && x != 10 && x != 13)
            return 0;           /* a control character once decoded */
        tabs += x == 9;
        lf += x == 10;
    }
    return tabs >= 2 && (lf >= 1 || n < 1024) ? 75 : 0;
}
static int table_parse(igsa_ctx *c)
{
    uint8_t *u = undo5(c->d, c->n);
    lines l;
    char *s, hdr[4096], *p, *t, *h[64];
    int nh = 0, rows = 0, ended = 0;

    if (!u)
        return igsa_fail(c, "out of memory");
    lines_init(&l, u, c->n);
    if (!(s = next_line(&l))) {
        free(u);
        return igsa_fail(c, "empty");
    }
    snprintf(hdr, sizeof(hdr), "%s", s);
    p = hdr;
    while (nh < 64 && (t = tok(&p, "\t")))
        h[nh++] = t;
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance table (bytes + 5, Big5, tab-separated)");
    igsa_jarr(c, "columns");
    for (int i = 0; i < nh; i++)
        jtext(c, NULL, h[i]);
    igsa_jend(c);
    igsa_jarr(c, "rows");
    while ((s = next_line(&l))) {
        char *f[64];
        int nf = 0, i;

        p = s;
        while (*p == ' ')
            p++;
        if (!strncasecmp(p, "END", 3) && (!p[3] || p[3] == '\t' || p[3] == ' ' || p[3] == ',')) {
            ended = 1;
            break;
        }
        if (!*p)
            continue;
        while (nf < 64 && (t = tok(&p, "\t")))
            f[nf++] = t;
        igsa_jarr(c, NULL);
        for (i = 0; i < nf; i++) {
            size_t k = strlen(f[i]);

            while (k && f[i][k - 1] == ' ')
                f[i][--k] = 0;
            jval(c, NULL, f[i]);
        }
        igsa_jend(c);
        rows++;
        c->entries++;
    }
    igsa_jend(c);
    igsa_jint(c, "count", rows);
    igsa_jbool(c, "end_mark", ended);
    igsa_jend(c);
    free(u);
    if (!ended)
        return igsa_fail(c, "no END row after %d rows", rows);
    if (c->mode == IGSA_EXTRACT) {
        char nm[300];

        snprintf(nm, sizeof(nm), "%s.txt", base(c->path));
        u = undo5(c->d, c->n);
        if (!u || igsa_emit_raw(c, nm, u, c->n)) {
            free(u);
            return -1;
        }
        free(u);
    }
    return 0;
}

/* ---- wedoavset: avatar_setting.set --------------------------------------------------------- */

static int avset_probe(const uint8_t *d, size_t n, const char *path)
{
    const uint8_t *t = memchr(d, '\t', n < 64 ? n : 64);

    return n >= 8 && n < 4096 && t && t > d && d[0] >= 0x80 && text_ok(d, n) && ext_is(path, ".set") ? 80 : 0;
}

static int avset_parse(igsa_ctx *c)
{
    static const char *const keys[] = { "boy_faces", "girl_faces" };
    lines l;
    char *s, *p, *a, *b;
    int i = 0;

    lines_init(&l, c->d, c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance avatar setting");
    while ((s = next_line(&l))) {
        p = s;
        a = tok(&p, SEP);
        b = tok(&p, SEP);
        if (!a)
            continue;
        if (!isnum(b))
            return igsa_fail(c, "line %d: no count", l.no);
        if (i < 2) {
            igsa_jobj(c, keys[i]);
            jtext(c, "label", a);
            igsa_jint(c, "count", atoi(b));
            igsa_jend(c);
        }
        i++;
    }
    if (i < 2)
        return igsa_fail(c, "%d lines, the loader reads 2", i);
    igsa_jend(c);
    c->entries++;
    return 0;
}

/* ---- wedokeyerr: keyerror.dat ------------------------------------------------------------- */

static int keyerr_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n > 10 && !strncmp((const char *)d, "KeyButton", 9) && text_ok(d, n) ? 90 : 0;
}

static int keyerr_parse(igsa_ctx *c)
{
    static const struct { const char *kw, *key; int n; } kw[] = {
        { "KeyButtonActMode", "button_act_mode", 5 }, { "KeyButton", "button_errors", 5 },
        { "KeyNumberActMode", "number_act_mode", 12 }, { "KeyNumber", "number_errors", 12 },
    };
    static const char *const bnames[5] = { "ok", "up", "down", "left", "right" };
    static const char *const knames[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "star", "0", "hash" };
    lines l;
    char *s, *p, *t;
    int seen = 0;

    lines_init(&l, c->d, c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance key test");
    while ((s = next_line(&l))) {
        p = s;
        while ((t = tok(&p, " ,\n\t"))) {
            size_t i;
            int j;

            for (i = 0; i < 4; i++)
                if (!strcasecmp(t, kw[i].kw))
                    break;
            if (i == 4)
                continue;
            seen++;
            igsa_jobj(c, kw[i].key);
            for (j = 0; j < kw[i].n; j++) {
                char *v = tok(&p, " ,\n\t");

                if (!isnum(v))
                    return igsa_fail(c, "%s: value %d missing", kw[i].kw, j);
                if (kw[i].n == 5)
                    igsa_jint(c, bnames[j], atoi(v));
                else
                    igsa_jint(c, knames[j], atoi(v));
            }
            igsa_jend(c);
        }
    }
    igsa_jbool(c, "used", seen > 3);     /* the loader clears everything with 3 or fewer */
    igsa_jend(c);
    c->entries++;
    return 0;
}

/* ---- wedozhl: .zhl -------------------------------------------------------------------------- */

static const char *const zhl_kw[] = { "Name", "Vertex", "Set", "SetVec4", "SetMat4", "Alias", "Texture", "TextureID", "FX", "GLSL" };

static int zhl_probe(const uint8_t *d, size_t n, const char *path)
{
    size_t i, k;

    (void)path;
    if (n < 4 || !text_ok(d, n))
        return 0;
    for (i = 0; i < sizeof(zhl_kw) / sizeof(zhl_kw[0]); i++) {
        k = strlen(zhl_kw[i]);
        if (n > k && !strncmp((const char *)d, zhl_kw[i], k) && (d[k] == ' ' || d[k] == '\t'))
            return i >= 8 ? 80 : 40;
    }
    return 0;
}

static int zhl_parse(igsa_ctx *c)
{
    lines l;
    char *s, *p, *t, *v;
    int comment = 0, ignored = 0;

    lines_init(&l, c->d, c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "zl effect (ZHL)");
    igsa_jarr(c, "statements");
    while ((s = next_line(&l))) {
        p = s;
        t = tok(&p, " \t");
        /* zlReader::GetNextTok: lines starting // or \\ and block comments are skipped */
        if (!t || !strncmp(t, "//", 2) || !strncmp(t, "\\\\", 2))
            continue;
        if (!strncmp(t, "/*", 2)) {
            comment = 1;
            continue;
        }
        if (!strncmp(t, "*/", 2)) {
            comment = 0;
            continue;
        }
        if (comment)
            continue;
        igsa_jobj(c, NULL);
        if (!strcasecmp(t, "Name") || !strcasecmp(t, "FX") || !strcasecmp(t, "GLSL")) {
            igsa_jstr(c, "op", !strcasecmp(t, "Name") ? "name" : !strcasecmp(t, "FX") ? "fx" : "glsl");
            v = tok(&p, " \t");
            jtext(c, !strcasecmp(t, "Name") ? "name" : "file", v ? v : "");
        } else if (!strcasecmp(t, "Vertex")) {
            igsa_jstr(c, "op", "vertex");
            igsa_jarr(c, "flags");
            while ((v = tok(&p, " \t")))
                igsa_jstr(c, NULL, v);
            igsa_jend(c);
        } else if (!strcasecmp(t, "Set") || !strcasecmp(t, "SetVec4") || !strcasecmp(t, "SetMat4")) {
            igsa_jstr(c, "op", !strcasecmp(t, "Set") ? "set" : !strcasecmp(t, "SetVec4") ? "set_vec4" : "set_mat4");
            v = tok(&p, " \t");
            jtext(c, "param", v ? v : "");
            igsa_jarr(c, "values");
            while ((v = tok(&p, " \t"))) {
                if (!isnum(v))
                    return igsa_fail(c, "line %d: value '%s'", l.no, v);
                igsa_jnum(c, NULL, strtod(v, NULL));
            }
            igsa_jend(c);
        } else if (!strcasecmp(t, "Alias")) {
            igsa_jstr(c, "op", "alias");
            v = tok(&p, " \t");
            jtext(c, "param", v ? v : "");
            v = tok(&p, " \t");
            jtext(c, "semantic", v ? v : "");
        } else if (!strcasecmp(t, "Texture") || !strcasecmp(t, "TextureID")) {
            int id = t[7] != 0;

            igsa_jstr(c, "op", id ? "texture_id" : "texture");
            v = tok(&p, " \t");
            jtext(c, "param", v ? v : "");
            v = tok(&p, " \t");
            jtext(c, id ? "id" : "file", v ? v : "");
            if (!id) {
                igsa_jarr(c, "flags");      /* Cube 1D 2D 3D NoMip NoCompress */
                while ((v = tok(&p, " \t")))
                    igsa_jstr(c, NULL, v);
                igsa_jend(c);
            }
        } else {
            igsa_jstr(c, "op", "ignored");
            jtext(c, "word", t);
            ignored++;
        }
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jint(c, "ignored", ignored);
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_wedochart = { "wedochart", "We Dance song chart (.csv): tempo, rhythm, notes per level", chart_probe, chart_parse };
const igsa_format igsa_fmt_wedoeff = { "wedoeff", "We Dance stage effects (.eff): dances, cameras, effect timeline", eff_probe, eff_parse };
const igsa_format igsa_fmt_wedolist = { "wedolist", "We Dance song list (csvlist): one row per song", list_probe, list_parse };
const igsa_format igsa_fmt_wedotable = { "wedotable", "We Dance +5 Big5 tables (.mlt, npc_list.lst, clothes_probability.ini)",
                                         table_probe, table_parse };
const igsa_format igsa_fmt_wedoavset = { "wedoavset", "We Dance avatar_setting.set: open faces per sex", avset_probe, avset_parse };
const igsa_format igsa_fmt_wedokeyerr = { "wedokeyerr", "We Dance keyerror.dat: stuck-button counters", keyerr_probe, keyerr_parse };
const igsa_format igsa_fmt_wedozhl = { "wedozhl", "zl GLSL effect (.zhl): shader file, aliases, textures, constants", zhl_probe, zhl_parse };
