/*
 * Percussion Master 3 charts and song list: media/script_AES/<song>_<level>.enccut,
 * SongList.enc, rewrite/script_download/<name>.enc (and the plaintext .txt
 * leftovers there).  FPGA cipher (wiki Sound Crypto) with the chip's chart
 * keys; the .enccut files lack their first 16 bytes (4-byte key-id header and
 * the first 12 cipher bytes), whose plaintext is every chart's first line.
 * AES::iAESDecrypt, PMGAME::BandScript::iLoadBandFile / iLoadList.
 * wiki Format-PM3CHART.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

/* the masks of PowerOn's ten keys (chart key ids 0..9) and of pm3's own list key (wiki Keys) */
static const uint8_t chart_mask[10][16] = {
    { 0x32, 0x58, 0x21, 0x7a, 0x79, 0xe8, 0x82, 0x61, 0xf1, 0x39, 0xe5, 0xad, 0xc7, 0x3d, 0x61, 0x1a },
    { 0xd8, 0x82, 0x84, 0x5f, 0xd9, 0xc6, 0x84, 0xab, 0x15, 0x8f, 0xf1, 0x28, 0x1d, 0xf4, 0x4a, 0x03 },
    { 0x89, 0x0f, 0xe5, 0x18, 0x07, 0x3e, 0xa7, 0x28, 0x35, 0x32, 0x72, 0x0c, 0x56, 0x96, 0xda, 0x58 },
    { 0x16, 0x1d, 0xe4, 0xf0, 0xb2, 0x10, 0xbc, 0x08, 0xd6, 0x22, 0x75, 0x28, 0xd6, 0xff, 0xa9, 0x52 },
    { 0x95, 0x80, 0x66, 0xea, 0x68, 0x8c, 0xe5, 0xd9, 0xee, 0xce, 0x58, 0x2a, 0x5f, 0xe9, 0x5a, 0x51 },
    { 0x3a, 0x0e, 0x2d, 0x44, 0xe5, 0x78, 0xa0, 0xa8, 0x13, 0x70, 0xdd, 0x14, 0x7c, 0xbf, 0x2e, 0x19 },
    { 0x18, 0x03, 0xa7, 0x3f, 0xa9, 0xfe, 0xe3, 0xe0, 0x28, 0x2d, 0xf9, 0x20, 0x9d, 0x24, 0x93, 0x92 },
    { 0x9a, 0xda, 0xb8, 0x2c, 0x2d, 0xa8, 0xa0, 0xea, 0x30, 0xbf, 0x41, 0xa3, 0x66, 0x3c, 0x63, 0x7a },
    { 0x98, 0xcf, 0xef, 0x1a, 0xc9, 0xb6, 0x89, 0xa4, 0x52, 0xec, 0xe2, 0x2a, 0x53, 0x94, 0x5a, 0x55 },
    { 0xcf, 0x97, 0xd6, 0xc8, 0x27, 0x69, 0x98, 0x47, 0x90, 0xa0, 0xe1, 0x70, 0x72, 0x9b, 0x1b, 0x31 },
};
static const uint8_t list_mask[16] = {
    0x16, 0x18, 0xa1, 0xda, 0x69, 0xc8, 0x20, 0x69, 0xa5, 0x3b, 0xc9, 0x2c, 0xc6, 0x78, 0x25, 0x52,
};
static const char chart_head[] = "#------ BASE DATA";   /* every chart's first line; 16 bytes are cut */
static const char list_head[] = "#---- [BPM]";

/* the key id in a header word: bits 4, 10, 17, 29 (AES::iAESDecrypt) */
static int head_kid(uint32_t w)
{
    return (w >> 4 & 1) | (w >> 10 & 1) << 1 | (w >> 17 & 1) << 2 | (w >> 29 & 1) << 3;
}

/* the trailer block's key-id word under mask m: {random, size, kid} */
static int trailer_ok(const uint8_t *d, size_t n, const uint8_t *m, int kid, size_t body)
{
    uint8_t b[16];

    memcpy(b, d + n - 16, 16);
    fpga_decrypt(m, b, 16);
    return igsa_le32(b + 12) == (uint32_t)kid && igsa_le32(b + 8) < body && (int32_t)igsa_le32(b + 4) > 0;
}

enum { K_CUT, K_ENC, K_LISTENC, K_TXT, K_LISTTXT };

/* kind of file and key id (charts), -1 if not ours */
static int classify(const uint8_t *d, size_t n, int *kid)
{
    int i;

    *kid = -1;
    if (n >= 17 && !memcmp(d, chart_head, 17))
        return K_TXT;
    if (n >= 11 && !memcmp(d, list_head, 11))
        return K_LISTTXT;
    if (n < 36 || (n - 4) % 16)
        return -1;
    /* whole file: header word names the key */
    i = head_kid(igsa_le32(d));
    if (trailer_ok(d, n, list_mask, i, n - 4)) {
        *kid = i;
        return K_LISTENC;
    }
    if (i < 10 && trailer_ok(d, n, chart_mask[i], i, n - 4)) {
        *kid = i;
        return K_ENC;
    }
    /* cut file: the trailer names its key */
    for (i = 0; i < 10; i++)
        if (trailer_ok(d, n, chart_mask[i], i, n + 12)) {
            *kid = i;
            return K_CUT;
        }
    return -1;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    int kid, k = classify(d, n, &kid);

    (void)path;
    return k == K_CUT || k == K_ENC || k == K_TXT ? 90 : 0;
}

static int probe_list(const uint8_t *d, size_t n, const char *path)
{
    int kid, k = classify(d, n, &kid);

    (void)path;
    return k == K_LISTENC || k == K_LISTTXT ? 90 : 0;
}

/* plaintext of a file (malloc'd, NUL-terminated) and its text length; fails with c->err */
static char *plain(igsa_ctx *c, int list, size_t *len, int *kidp)
{
    int kid, k = classify(c->d, c->n, &kid);
    uint8_t *p;
    size_t pn, sz;

    *kidp = kid;
    if (k == K_TXT || k == K_LISTTXT) {
        if ((list != 0) != (k == K_LISTTXT))
            return igsa_fail(c, "not this kind of script"), NULL;
        if (!(p = malloc(c->n + 1)))
            return igsa_fail(c, "out of memory"), NULL;
        memcpy(p, c->d, c->n);
        p[c->n] = 0;
        *len = c->n;
        return (char *)p;
    }
    if (k < 0 || (list != 0) != (k == K_LISTENC))
        return igsa_fail(c, "no chart key fits"), NULL;
    /* the plaintext from file offset 4; a cut file's block 0 (header word, 12 cut bytes and its
       first 4 bytes) is every chart's first 16 characters */
    pn = k == K_CUT ? c->n + 12 : c->n - 4;
    if (!(p = malloc(pn + 1)))
        return igsa_fail(c, "out of memory"), NULL;
    if (k == K_CUT) {
        memcpy(p, chart_head, 16);
        memcpy(p + 16, c->d + 4, c->n - 4);
        fpga_decrypt(chart_mask[kid], p + 16, c->n - 4);
    } else {
        memcpy(p, c->d + 4, pn);
        fpga_decrypt(k == K_LISTENC ? list_mask : chart_mask[kid], p, pn);
    }
    sz = igsa_le32(p + pn - 8);
    if (sz >= pn - 12 || (int32_t)igsa_le32(p + pn - 12) <= 0 || igsa_le32(p + pn - 4) != (uint32_t)kid) {
        uint32_t w = igsa_le32(p + pn - 4);

        free(p);
        return igsa_fail(c, "trailer: size %zu of %zu, key %u", sz, pn, w), NULL;
    }
    p[sz] = 0;
    *len = sz;
    return (char *)p;
}
/* the game's line reader (AES::AES_pBufGetLine): lines up to '\n', at most 255 bytes */
static int getline_(const char *t, size_t n, size_t *o, char *line)
{
    size_t k = 0;

    if (*o >= n)
        return 0;
    while (*o < n && t[*o] != '\n') {
        if (k < 254)
            line[k++] = t[*o];
        (*o)++;
    }
    if (*o < n)
        (*o)++;
    line[k] = 0;
    return 1;
}

/* strtok_r */
static char *tok(char *s, const char *d, char **save)
{
    char *e;

    if (!s)
        s = *save;
    if (!s)
        return NULL;
    s += strspn(s, d);
    if (!*s) {
        *save = s;
        return NULL;
    }
    e = s + strcspn(s, d);
    if (*e)
        *e++ = 0;
    *save = e;
    return s;
}

static long num(char **save)
{
    char *t = tok(NULL, " \t\r\n,", save);

    return t ? strtol(t, NULL, 10) : 0;
}

/* ---- charts ---------------------------------------------------------------------------- */

#define TRACKS 24
#define EVENTS 512

struct ev { int grid, idx, vol, hsta, hend, hnum; double time; };
struct chart {
    int bpms, nbpm, rhys, nrhy, total_note, wav_num, nwav, hits;
    struct { unsigned tick, bpm; double spt, base; } bpm[256];
    struct { int section, beat, event; } rhy[256];
    struct { int idx; char name[33]; } wav[256];
    int nev[TRACKS], used[TRACKS];
    struct ev ev[TRACKS][EVENTS];
};

static void chart_body(igsa_ctx *c, const struct chart *h)
{
    int i, k;

    igsa_jint(c, "ChangeBPMCount", h->bpms);
    igsa_jarr(c, "ChangeBPM");
    for (i = 0; i < h->nbpm; i++) {
        igsa_jobj(c, NULL);
        igsa_juint(c, "tick", h->bpm[i].tick);
        igsa_jnum(c, "bpm", h->bpm[i].bpm / 100.0);
        igsa_jnum(c, "start_s", h->bpm[i].base);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jint(c, "ChangeRhythmCount", h->rhys);
    igsa_jarr(c, "ChangeRhythm");
    for (i = 0; i < h->nrhy; i++) {
        igsa_jobj(c, NULL);
        igsa_jint(c, "section", h->rhy[i].section);
        igsa_jint(c, "beat", h->rhy[i].beat);
        igsa_jint(c, "event", h->rhy[i].event);
        igsa_jend(c);
    }
    igsa_jend(c);
    if (h->total_note >= 0)
        igsa_jint(c, "TotalNote", h->total_note);
    if (h->wav_num >= 0)
        igsa_jint(c, "WAVNUM", h->wav_num);
    if (h->nwav) {
        igsa_jarr(c, "WAV");
        for (i = 0; i < h->nwav; i++) {
            igsa_jobj(c, NULL);
            igsa_jint(c, "index", h->wav[i].idx);
            igsa_jstr(c, "file", h->wav[i].name);
            igsa_jend(c);
        }
        igsa_jend(c);
    }
    igsa_jint(c, "hits", h->hits);
    igsa_jarr(c, "tracks");
    for (k = 0; k < TRACKS; k++) {
        if (!h->used[k])
            continue;
        igsa_jobj(c, NULL);
        igsa_jint(c, "track", k);
        igsa_jarr(c, "events");
        for (i = 0; i < h->nev[k]; i++) {
            const struct ev *e = &h->ev[k][i];

            igsa_jobj(c, NULL);
            igsa_jint(c, "grid", e->grid);
            igsa_jnum(c, "time_s", e->time);
            igsa_jint(c, "idx", e->idx);
            igsa_jint(c, "vol", e->vol);
            igsa_jint(c, "hold_start", e->hsta);
            igsa_jint(c, "hold_end", e->hend);
            igsa_jint(c, "hold_num", e->hnum);
            igsa_jend(c);
        }
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
}

static void chart_json(igsa_ctx *c, void *arg)
{
    igsa_jobj(c, NULL);
    chart_body(c, arg);
    igsa_jend(c);
}

static double tick_time(const struct chart *h, int t)
{
    int i = 0;

    if (!h->nbpm)
        return 0;
    while (i < h->nbpm - 1 && t > (int)h->bpm[i + 1].tick)
        i++;
    return (t - (int)h->bpm[i].tick) * h->bpm[i].spt + h->bpm[i].base;
}

static int parse_chart(igsa_ctx *c)
{
    struct chart *h = calloc(1, sizeof(*h));
    char *t, line[256];
    size_t len, o = 0;
    int kid, track = 0, i, k, lines = 0;

    if (!h)
        return igsa_fail(c, "out of memory");
    if (!(t = plain(c, 0, &len, &kid))) {
        free(h);
        return -1;
    }
    h->total_note = h->wav_num = -1;
    while (getline_(t, len, &o, line)) {
        char *save, *w;

        lines++;
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n' || !line[0])
            continue;
        if (line[0] == 'q')
            break;
        if (!(w = tok(line, " \t,", &save)))
            continue;
        if (!strcmp(w, "TRACK")) {
            track = (int)num(&save);
            if (track < 0 || track >= TRACKS) {
                igsa_fail(c, "line %d: track %d", lines, track);
                goto bad;
            }
            h->used[track] = 1;
            h->nev[track] = 0;
        } else if (!strcmp(w, "EVENT")) {
            struct ev e;
            int16_t g = (int16_t)((int)num(&save) << 2) >> 2;

            memset(&e, 0, sizeof(e));
            e.grid = g;
            if (g == -1) {                      /* track end */
                continue;
            }
            e.idx = (int)num(&save) & 0x3FF;
            e.vol = (uint8_t)num(&save);
            e.hsta = (int)num(&save) & 1;
            e.hend = (int)num(&save) & 1;
            e.hnum = (int)num(&save) & 0x3F;
            if (h->nev[track] >= EVENTS) {
                igsa_fail(c, "line %d: track %d has more than %d events", lines, track, EVENTS);
                goto bad;
            }
            if (track <= 5 && !e.hsta && !e.hend && !e.hnum)
                h->hits++;              /* BandScript::vEventParse's count for the first six tracks */
            h->ev[track][h->nev[track]++] = e;
            h->used[track] = 1;
        } else if (!strcmp(w, "ChangeBPMCount"))
            h->bpms = (uint8_t)num(&save);
        else if (!strcmp(w, "ChangeBPM")) {
            if (h->nbpm >= 256) {
                igsa_fail(c, "line %d: too many BPM changes", lines);
                goto bad;
            }
            h->bpm[h->nbpm].tick = (uint16_t)num(&save);
            h->bpm[h->nbpm].bpm = (uint16_t)num(&save);
            h->nbpm++;
        } else if (!strcmp(w, "ChangeRhythmCount"))
            h->rhys = (uint8_t)num(&save);
        else if (!strcmp(w, "ChangeRhythm")) {
            if (h->nrhy >= 256) {
                igsa_fail(c, "line %d: too many rhythm changes", lines);
                goto bad;
            }
            h->rhy[h->nrhy].section = (uint8_t)num(&save);
            h->rhy[h->nrhy].beat = (uint8_t)num(&save);
            h->rhy[h->nrhy].event = (uint16_t)num(&save);
            h->nrhy++;
        } else if (!strcmp(w, "TotalNote"))
            h->total_note = (int)num(&save);
        else if (!strcmp(w, "WAVNUM"))
            h->wav_num = (int)num(&save);
        else if (!strcmp(w, "WAV")) {
            char *nm;

            if (h->nwav >= 256) {
                igsa_fail(c, "line %d: too many WAV lines", lines);
                goto bad;
            }
            h->wav[h->nwav].idx = (int)num(&save);
            nm = tok(NULL, "\t\r\n,", &save);
            snprintf(h->wav[h->nwav].name, sizeof(h->wav[0].name), "%s", nm ? nm : "");
            h->nwav++;
        } else if (!strcmp(w, "END"))
            break;
    }
    if (h->bpms != h->nbpm) {
        igsa_fail(c, "ChangeBPMCount %d, %d ChangeBPM lines", h->bpms, h->nbpm);
        goto bad;
    }
    /* BandScript::vSetChangeBMP_TotalTime: seconds per tick 6000 / (12 * bpm*100) */
    for (i = 0; i < h->nbpm; i++) {
        if (!h->bpm[i].bpm) {
            igsa_fail(c, "BPM change %d: BPM 0", i);
            goto bad;
        }
        h->bpm[i].spt = 6000.0 / (12.0 * h->bpm[i].bpm);
        if (!i) {
            h->bpm[0].tick = 0;
            h->bpm[0].base = 0;
        } else
            h->bpm[i].base = ((int)h->bpm[i].tick - (int)h->bpm[i - 1].tick) * h->bpm[i - 1].spt + h->bpm[i - 1].base;
    }
    for (k = 0; k < TRACKS; k++)
        for (i = 0; i < h->nev[k]; i++)
            h->ev[k][i].time = tick_time(h, h->ev[k][i].grid);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3chart");
    igsa_jint(c, "key", kid);
    igsa_juint(c, "text_bytes", len);
    chart_body(c, h);
    igsa_jend(c);
    if (c->mode == IGSA_LIST) {
        int nt = 0, ne = 0;

        for (k = 0; k < TRACKS; k++)
            if (h->used[k]) {
                nt++;
                ne += h->nev[k];
            }
        fprintf(c->out, "%-48s %-5s %d tracks, %d events, %d BPM changes\n", "(chart)", "chart", nt, ne, h->nbpm);
    }
    if (igsa_emit_json(c, "chart.json", chart_json, h) || igsa_emit_raw(c, "chart.txt", t, len))
        goto bad;
    free(t);
    free(h);
    return 0;
bad:
    free(t);
    free(h);
    return -1;
}

/* ---- the song list --------------------------------------------------------------------- */

struct song {
    long bpm, minbpm, maxbpm, length, hits, combo;
    char wavdir[33], name[33], singer[33], file[33];
    long song_id, singer_id, style, hide, cls, lv;
};
struct songs { int n, groups; struct song *s; };

static void field_str(char *dst, char *s)
{
    while (s && *s == ' ')
        s++;
    snprintf(dst, 33, "%s", s ? s : "");
}

static void songs_body(igsa_ctx *c, const struct songs *l)
{
    static const char *const cls[] = { "easy", "normal", "hard", "special", "master" };
    int i;

    igsa_jint(c, "entries", l->n);
    igsa_jint(c, "songs", l->groups);
    igsa_jarr(c, "list");
    for (i = 0; i < l->n; i++) {
        const struct song *s = &l->s[i];

        igsa_jobj(c, NULL);
        igsa_jnum(c, "BPM", s->bpm / 100.0);
        igsa_jnum(c, "MinBPM", s->minbpm / 100.0);
        igsa_jnum(c, "MaxBPM", s->maxbpm / 100.0);
        igsa_jint(c, "Length", s->length);
        igsa_jint(c, "TotalHit", s->hits);
        igsa_jint(c, "MaxCombo", s->combo);
        igsa_jstr(c, "WavDir", s->wavdir);
        igsa_jbig5(c, "SongName", s->name, strlen(s->name));
        igsa_jbig5(c, "SingerName", s->singer, strlen(s->singer));
        igsa_jint(c, "SongID", s->song_id);
        igsa_jint(c, "SingerID", s->singer_id);
        igsa_jint(c, "MusicStyle", s->style);
        igsa_jint(c, "Hide", s->hide);
        igsa_jint(c, "Class", s->cls);
        if (s->cls >= 0 && s->cls < 5)
            igsa_jstr(c, "class_name", cls[s->cls]);
        igsa_jint(c, "Lv", s->lv);
        igsa_jstr(c, "FileName", s->file);
        igsa_jend(c);
    }
    igsa_jend(c);
}

static void songs_json(igsa_ctx *c, void *arg)
{
    igsa_jobj(c, NULL);
    songs_body(c, arg);
    igsa_jend(c);
}

static int parse_list(igsa_ctx *c)
{
    struct songs l = { 0, 0, NULL };
    char *t, line[256];
    size_t len, o = 0, cap = 0;
    int kid, lines = 0;
    long last = -1;

    if (!(t = plain(c, 1, &len, &kid)))
        return -1;
    while (getline_(t, len, &o, line)) {
        struct song s;
        char *save, *w;

        lines++;
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n' || !line[0])
            continue;
        if (line[0] == 'q')
            break;
        memset(&s, 0, sizeof(s));
        if (!(w = tok(line, " \t,", &save))) {
            free(l.s);
            free(t);
            return igsa_fail(c, "line %d: empty", lines);
        }
        s.bpm = strtol(w, NULL, 10);
        s.minbpm = num(&save);
        s.maxbpm = num(&save);
        s.length = num(&save);
        s.hits = num(&save);
        s.combo = num(&save);
        field_str(s.wavdir, tok(NULL, " \t\r\n,", &save));
        field_str(s.name, tok(NULL, "\t\r\n,", &save));
        field_str(s.singer, tok(NULL, "\t\r\n,", &save));
        s.song_id = num(&save);
        s.singer_id = num(&save);
        s.style = num(&save);
        s.hide = num(&save);
        s.cls = num(&save);
        s.lv = num(&save);
        w = tok(NULL, " \t\r\n,", &save);
        if (!w) {
            free(l.s);
            free(t);
            return igsa_fail(c, "line %d: %s", lines, "missing FileName");
        }
        field_str(s.file, w);
        if (s.song_id != last)
            l.groups++;
        last = s.song_id;
        if ((size_t)l.n >= cap) {
            struct song *ns = realloc(l.s, (cap = cap ? cap * 2 : 256) * sizeof(*ns));

            if (!ns) {
                free(l.s);
                free(t);
                return igsa_fail(c, "out of memory");
            }
            l.s = ns;
        }
        l.s[l.n++] = s;
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3songs");
    igsa_jint(c, "key", kid);
    igsa_juint(c, "text_bytes", len);
    songs_body(c, &l);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %d charts, %d songs\n", "(song list)", "list", l.n, l.groups);
    if (igsa_emit_json(c, "songlist.json", songs_json, &l) || igsa_emit_raw(c, "songlist.txt", t, len)) {
        free(l.s);
        free(t);
        return -1;
    }
    free(l.s);
    free(t);
    return 0;
}

const igsa_format igsa_fmt_pm3chart = { "pm3chart", "Percussion Master 3 chart (.enccut/.enc, FPGA cipher): notes, BPM, rhythm",
                                        probe, parse_chart };
const igsa_format igsa_fmt_pm3songs = { "pm3songs", "Percussion Master 3 song list (SongList.enc)", probe_list, parse_list };
