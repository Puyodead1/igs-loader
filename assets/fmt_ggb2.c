/*
 * Go Go Ball 2 DX files in /rf_data (pitch, stripped).  wiki Format-GGBBOOK,
 * Format-GGBINFO, Format-GGBTEST.
 *
 * <year>.bin (1033 bytes): the yearly copy of the bookkeeping block (the
 *   block itself lives in the IGS027A NVRAM, command 48, 1044 bytes from
 *   0x8540380): 31 days x 28, 12 months x 12, trailer.  Written only, by
 *   sub_8074400 after gameinfo.txt; day / month init sub_8073DE0 / sub_8073E50,
 *   counters sub_8075100 (coin, service), sub_8075380 (continue),
 *   sub_80753C0 (players), sub_8075310 (play time), sub_8074340 (tickets),
 *   sub_8085510 / sub_80854E0 (test-data credits and time).
 * gameinfo.dat (1860 bytes): u32 style-select count, then per level (29),
 *   stage (4), players (2): u32 times, u32 passes.  sub_8074230 (load),
 *   sub_8074160 (save).
 * testdata.bin (19504 bytes): 4 u32, then per players (2), stage (4),
 *   level (29) 21 u32.  sub_8083730 (load), sub_8085410 (save), the adders
 *   sub_8085530..sub_8085980 (c/testdata.c), text report sub_8083800.
 */
#include <stdio.h>
#include <string.h>

#include "igsa.h"

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

/* ---- <year>.bin ------------------------------------------------------------------------------ */

#define BOOK 1033

static int book_valid(const uint8_t *d, size_t n)
{
    int i;

    if (n != BOOK || d[1012] < 1 || d[1012] > 31 || d[1013] < 1 || d[1013] > 12 || d[1014] > 99)
        return 0;
    for (i = 0; i < 31; i++) {
        uint32_t f = igsa_le32(d + 28 * i + 8);

        if ((f & 1) && ((f >> 6 & 31) != (uint32_t)i + 1 || (f >> 2 & 15) < 1 || (f >> 2 & 15) > 12))
            return 0;
    }
    for (i = 0; i < 12; i++) {
        const uint8_t *m = d + 868 + 12 * i;

        if ((m[0] & 1) && m[1] != i + 1)
            return 0;
    }
    return 1;
}

static int book_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return book_valid(d, n) ? 90 : 0;
}

static int book_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d, *t = d + 1012;
    int i, y;

    if (!book_valid(d, c->n))
        return igsa_fail(c, "%zu bytes: not a 1033-byte bookkeeping year (day/month records or indices out of range)", c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Go Go Ball 2 bookkeeping year");
    igsa_jint(c, "year", 2000 + t[2]);
    if (sscanf(base(c->path), "%d.bin", &y) == 1)
        igsa_jbool(c, "year_matches_name", y == 2000 + t[2]);
    igsa_jint(c, "day_index", t[0]);
    igsa_jint(c, "month_index", t[1]);
    igsa_juint(c, "all_coins", igsa_le32(t + 3));
    igsa_juint(c, "all_service", igsa_le32(t + 7));
    igsa_juint(c, "all_work_s", igsa_le32(t + 11));
    igsa_juint(c, "serial", igsa_le16(t + 15));
    igsa_juint(c, "word_0", igsa_le16(t + 17));
    igsa_juint(c, "word_1", igsa_le16(t + 19));
    igsa_jarr(c, "days");
    for (i = 0; i < 31; i++) {
        const uint8_t *r = d + 28 * i;
        uint32_t f = igsa_le32(r + 8);

        igsa_jobj(c, NULL);
        igsa_jint(c, "day", i + 1);
        igsa_jbool(c, "active", f & 1);
        igsa_jbool(c, "motion_open", f >> 1 & 1);
        igsa_juint(c, "month", f >> 2 & 15);
        igsa_juint(c, "mday", f >> 6 & 31);
        igsa_juint(c, "continues", f >> 11 & 0x3FF);
        igsa_juint(c, "service", f >> 21);
        igsa_juint(c, "coins", igsa_le16(r + 12));
        igsa_juint(c, "players", igsa_le16(r + 14));
        igsa_juint(c, "open_s", igsa_le32(r));
        igsa_juint(c, "idle_s", igsa_le32(r + 4));
        igsa_juint(c, "play_s", igsa_le32(r + 16));
        igsa_juint(c, "tickets", igsa_le32(r + 20));
        igsa_juint(c, "test_credits", r[24]);
        igsa_juint(c, "byte_25", r[25]);
        igsa_juint(c, "test_play_s", igsa_le16(r + 26));
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
    igsa_jarr(c, "months");
    for (i = 0; i < 12; i++) {
        const uint8_t *m = d + 868 + 12 * i;

        igsa_jobj(c, NULL);
        igsa_jint(c, "index", i + 1);
        igsa_jbool(c, "active", m[0] & 1);
        igsa_jint(c, "year", 2000 + (m[0] >> 1));
        igsa_jint(c, "month", m[1]);
        igsa_juint(c, "coins", igsa_le16(m + 2));
        igsa_juint(c, "service", igsa_le16(m + 4));
        igsa_juint(c, "players", igsa_le16(m + 6));
        igsa_juint(c, "tickets", igsa_le32(m + 8));
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

/* ---- gameinfo.dat ---------------------------------------------------------------------------- */

#define INFO (4 + 29 * 4 * 2 * 8)

static int info_valid(const uint8_t *d, size_t n)
{
    size_t i;

    if (n != INFO)
        return 0;
    for (i = 4; i < n; i += 8)
        if (igsa_le32(d + i + 4) > igsa_le32(d + i))
            return 0;
    return 1;
}

static int info_probe(const uint8_t *d, size_t n, const char *path)
{
    if (!info_valid(d, n))
        return 0;
    return !strcmp(base(path), "gameinfo.dat") ? 90 : 40;
}

static int info_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    int l, s, p;

    if (!info_valid(d, c->n))
        return igsa_fail(c, "%zu bytes: not 4 + 232 x {times, passes} with passes <= times", c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Go Go Ball 2 gameinfo");
    igsa_juint(c, "style_selects", igsa_le32(d));
    igsa_jarr(c, "levels");
    for (l = 0; l < 29; l++) {
        igsa_jobj(c, NULL);
        igsa_jint(c, "level", l + 1);
        igsa_jarr(c, "stages");
        for (s = 0; s < 4; s++) {
            igsa_jobj(c, NULL);
            igsa_jint(c, "stage", s + 1);
            for (p = 0; p < 2; p++) {
                const uint8_t *e = d + 4 + 8 * ((l * 4 + s) * 2 + p);

                igsa_jobj(c, p ? "players_2" : "players_1");
                igsa_juint(c, "times", igsa_le32(e));
                igsa_juint(c, "passes", igsa_le32(e + 4));
                igsa_jend(c);
                c->entries++;
            }
            igsa_jend(c);
        }
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

/* ---- testdata.bin ---------------------------------------------------------------------------- */

#define TREC 84
#define TEST (16 + 2 * 4 * 29 * TREC)

static const char *const ranks = "SABCDE";
static const char *const abil[] = { "speed", "stability", "accuracy", "reaction" };

/* d6 is the sum of the ranks (sub_8085980 adds the rank and counts it) */
static int test_check(const uint8_t *d, size_t n, size_t *bad)
{
    size_t i;

    if (n != TEST)
        return 0;
    for (i = 16; i < n; i += TREC) {
        uint32_t sum = 0;
        int r;

        for (r = 0; r < 6; r++)
            sum += (uint32_t)r * igsa_le32(d + i + 28 + 4 * r);
        if (sum != igsa_le32(d + i + 24)) {
            if (bad)
                *bad = i;
            return 0;
        }
    }
    return 1;
}

static int test_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return test_check(d, n, NULL) ? 90 : 0;
}

static int test_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t bad = 0;
    int p, s, l, k;

    if (c->n != TEST)
        return igsa_fail(c, "%zu bytes: not 16 + 232 x 84", c->n);
    if (!test_check(d, c->n, &bad))
        return igsa_fail(c, "record at 0x%zx: rank sum %u is not the sum of the rank counts", bad, igsa_le32(d + bad + 24));
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Go Go Ball 2 testdata");
    igsa_juint(c, "modes_1p", igsa_le32(d));
    igsa_juint(c, "modes_2p", igsa_le32(d + 4));
    igsa_juint(c, "play_ms", igsa_le32(d + 8));
    igsa_juint(c, "credits", igsa_le32(d + 12));
    igsa_jarr(c, "players");
    for (p = 0; p < 2; p++) {
        igsa_jobj(c, NULL);
        igsa_jint(c, "players", p + 1);
        igsa_jarr(c, "stages");
        for (s = 0; s < 4; s++) {
            igsa_jobj(c, NULL);
            igsa_jint(c, "stage", s + 1);
            igsa_jarr(c, "levels");
            for (l = 0; l < 29; l++) {
                const uint8_t *r = d + 16 + (size_t)TREC * ((p * 4 + s) * 29 + l);
                char key[24];

                igsa_jobj(c, NULL);
                igsa_jint(c, "level", l + 1);
                igsa_juint(c, "selected", igsa_le32(r));
                igsa_juint(c, "passed", igsa_le32(r + 4));
                igsa_juint(c, "failed", igsa_le32(r + 8));
                igsa_juint(c, "continued", igsa_le32(r + 12));
                igsa_juint(c, "play_ms", igsa_le32(r + 16));
                igsa_juint(c, "score", igsa_le32(r + 20));
                igsa_juint(c, "rank_sum", igsa_le32(r + 24));
                igsa_jobj(c, "ranks");
                for (k = 0; k < 6; k++) {
                    key[0] = ranks[k];
                    key[1] = 0;
                    igsa_juint(c, key, igsa_le32(r + 28 + 4 * k));
                }
                igsa_jend(c);
                igsa_jobj(c, "preset");
                for (k = 0; k < 4; k++)
                    igsa_juint(c, abil[k], igsa_le32(r + 52 + 4 * k));
                igsa_jend(c);
                igsa_jobj(c, "ability");
                for (k = 0; k < 4; k++)
                    igsa_juint(c, abil[k], igsa_le32(r + 68 + 4 * k));
                igsa_jend(c);
                igsa_jend(c);
                c->entries++;
            }
            igsa_jend(c);
            igsa_jend(c);
        }
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_ggbbook = { "ggbbook", "Go Go Ball 2 <year>.bin: yearly bookkeeping (days, months, totals)", book_probe, book_parse };
const igsa_format igsa_fmt_ggbinfo = { "ggbinfo", "Go Go Ball 2 gameinfo.dat: plays and passes per level, stage, players", info_probe, info_parse };
const igsa_format igsa_fmt_ggbtest = { "ggbtest", "Go Go Ball 2 testdata.bin: test-data statistics per players, stage, level", test_probe, test_parse };
