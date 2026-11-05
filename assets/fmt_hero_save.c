/*
 * Hero of Robots exe/rw_data/ records and the UI font tables: book_combine.bin
 * (Bookkeeping, 0x684 bytes), card_use.bin (cards used per kind), timereset.bin
 * (Game::SaveResetTime), media/localization/ui/<name>.font (CIgsFontType::InitFontData).
 * wiki Format-HEROSAVE.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static const char *base(const char *p)
{
    const char *a, *b;

    if (!p)
        return "";
    a = strrchr(p, '/');
    b = strrchr(p, '\\');
    if (b > a)
        a = b;
    return a ? a + 1 : p;
}

static int named(const char *path, const char *prefix)
{
    const char *b = base(path);
    size_t i;

    for (i = 0; prefix[i]; i++)
        if (!b[i] || (b[i] | 32) != (prefix[i] | 32))
            return 0;
    return 1;
}

static void list1(igsa_ctx *c, const char *kind, const char *info)
{
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %s\n", base(c->path), kind, info);
}

/* ---- book_combine.bin ---------------------------------------------------------------------- */

static int book_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == 0x684 && d[1594] < 60 && d[1595] < 15 && d[1592] <= 12 && d[1593] <= 31 ? 80 : 0;
}

/* day records 60 x 20 at 0 (Bookkeeping::updateIndex, addCoinCnt, addPowerOnTime, ...) */
static void day(igsa_ctx *c, const uint8_t *r)
{
    igsa_jobj(c, NULL);
    igsa_jint(c, "month", r[8] >> 1);
    igsa_jint(c, "day", r[9]);
    igsa_juint(c, "power_on_s", igsa_le32(r));
    igsa_juint(c, "play_s", igsa_le32(r + 4));
    igsa_juint(c, "coins", igsa_le16(r + 10));
    igsa_juint(c, "service", igsa_le16(r + 12));
    igsa_juint(c, "plays", igsa_le16(r + 14));
    igsa_juint(c, "payouts", igsa_le16(r + 16));
    igsa_juint(c, "word18", igsa_le16(r + 18));
    igsa_jend(c);
}

/* month records 15 x 20 at 1200 */
static void month(igsa_ctx *c, const uint8_t *r)
{
    igsa_jobj(c, NULL);
    igsa_jint(c, "year", 2000 + (r[12] >> 1));
    igsa_jint(c, "month", r[13]);
    igsa_juint(c, "power_on_s", igsa_le32(r));
    igsa_juint(c, "play_s", igsa_le32(r + 4));
    igsa_juint(c, "coins", igsa_le32(r + 8));
    igsa_juint(c, "service", igsa_le16(r + 14));
    igsa_juint(c, "plays", igsa_le16(r + 16));
    igsa_juint(c, "payouts", igsa_le16(r + 18));
    igsa_jend(c);
}

static void u32s(igsa_ctx *c, const char *k, const uint8_t *p, int n)
{
    igsa_jarr(c, k);
    while (n-- > 0) {
        igsa_juint(c, NULL, igsa_le32(p));
        p += 4;
    }
    igsa_jend(c);
}

static int book_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    int i;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "herobook");
    igsa_jint(c, "year", 2000 + (int)igsa_le32(d + 1588));
    igsa_jint(c, "month", d[1592]);
    igsa_jint(c, "day", d[1593]);
    igsa_jint(c, "day_index", d[1594]);
    igsa_jint(c, "month_index", d[1595]);
    igsa_jarr(c, "days");
    for (i = 0; i < 60; i++)
        day(c, d + 20 * i);
    igsa_jend(c);
    igsa_jarr(c, "months");
    for (i = 0; i < 15; i++)
        month(c, d + 1200 + 20 * i);
    igsa_jend(c);
    igsa_jobj(c, "total");
    igsa_juint(c, "coins", igsa_le32(d + 1500));
    igsa_juint(c, "plays", igsa_le32(d + 1504));
    igsa_juint(c, "service", igsa_le32(d + 1508));
    igsa_juint(c, "free_plays", igsa_le32(d + 1512));
    igsa_juint(c, "payouts", igsa_le32(d + 1516));
    igsa_juint(c, "operator", igsa_le32(d + 1520));
    u32s(c, "mode_plays", d + 1524, 8);
    u32s(c, "level_wins", d + 1556, 4);
    u32s(c, "death_wins", d + 1572, 4);
    igsa_jend(c);
    u32s(c, "words_1596", d + 1596, 4);
    /* bytes 1612.. follow the 1612 the IGS036 keeps (message 11) */
    igsa_jobj(c, "total2");
    igsa_juint(c, "coins", igsa_le32(d + 1612));
    igsa_juint(c, "plays", igsa_le32(d + 1616));
    igsa_juint(c, "cards_bought", igsa_le32(d + 1620));
    igsa_juint(c, "operator", igsa_le32(d + 1624));
    igsa_juint(c, "payouts", igsa_le32(d + 1628));
    igsa_juint(c, "service", igsa_le32(d + 1632));
    igsa_juint(c, "free_plays", igsa_le32(d + 1636));
    igsa_juint(c, "cards_bought_all", igsa_le32(d + 1640));
    u32s(c, "words_1644", d + 1644, 6);
    igsa_jend(c);
    igsa_jend(c);
    list1(c, "book", "bookkeeping");
    c->entries++;
    return 0;
}

/* ---- card_use.bin: u32 per card (robots, powers, skills: CardInfo::GetCardSize) ----------- */

static int card_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)d;
    return n && n % 4 == 0 && named(path, "card_use") ? 70 : 0;
}

static int card_parse(igsa_ctx *c)
{
    size_t i, used = 0;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "herocards");
    igsa_juint(c, "cards", c->n / 4);
    igsa_jarr(c, "uses");
    for (i = 0; i < c->n; i += 4) {
        igsa_juint(c, NULL, igsa_le32(c->d + i));
        used += igsa_le32(c->d + i) != 0;
    }
    igsa_jend(c);
    igsa_juint(c, "cards_used", used);
    igsa_jend(c);
    list1(c, "cards", "card use counts");
    c->entries++;
    return 0;
}

/* ---- timereset.bin: year, month, day, hour, minute, second (u32 each) ---------------------- */

static int time_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == 24 && igsa_le32(d) - 2000 < 100 && igsa_le32(d + 4) - 1 < 12 && igsa_le32(d + 8) - 1 < 31 &&
                   igsa_le32(d + 12) < 24 && igsa_le32(d + 16) < 60 && igsa_le32(d + 20) < 61
               ? 60
               : 0;
}

static int time_parse(igsa_ctx *c)
{
    char b[32];

    snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u", igsa_le32(c->d), igsa_le32(c->d + 4), igsa_le32(c->d + 8),
             igsa_le32(c->d + 12), igsa_le32(c->d + 16), igsa_le32(c->d + 20));
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "herotime");
    igsa_jstr(c, "reset_time", b);
    igsa_jend(c);
    list1(c, "time", b);
    c->entries++;
    return 0;
}

/* ---- .font: header (234 bytes, "IGS_FONT_TYPE_VER_2" at 166; 166 bytes before), glyphs ----- */

static const char fver[] = "IGS_FONT_TYPE_VER_2";

static int font_hdr(const uint8_t *d, size_t n, size_t *hl, uint32_t *glyphs)
{
    if (n < 166 || memchr(d, 0, 64) == NULL || memchr(d + 64, 0, 64) == NULL)
        return 0;
    *hl = n >= 234 && !memcmp(d + 166, fver, sizeof(fver)) ? 234 : 166;
    *glyphs = igsa_le32(d + 148);
    return *glyphs < 70000 && *hl + 72ull * *glyphs == n;
}

static int font_probe(const uint8_t *d, size_t n, const char *path)
{
    size_t hl;
    uint32_t g;

    (void)path;
    return font_hdr(d, n, &hl, &g) ? 90 : 0;
}

static int font_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t hl;
    uint32_t g, i;
    int k;

    if (!font_hdr(d, c->n, &hl, &g))
        return igsa_fail(c, "header");
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "herofont");
    igsa_jstr(c, "version", hl == 234 ? fver : "1");
    igsa_jstrn(c, "font_file", (const char *)d, strnlen((const char *)d, 64));
    igsa_jstrn(c, "texture_name", (const char *)d + 64, strnlen((const char *)d + 64, 64));
    igsa_juint(c, "size", igsa_le32(d + 128));
    igsa_juint(c, "word132", igsa_le32(d + 132));
    igsa_juint(c, "texture_w", igsa_le32(d + 136));
    igsa_juint(c, "texture_h", igsa_le32(d + 140));
    igsa_juint(c, "textures", igsa_le32(d + 144));
    igsa_juint(c, "glyphs", g);
    igsa_juint(c, "word152", igsa_le32(d + 152));
    igsa_jint(c, "byte156", d[156]);
    igsa_jint(c, "byte157", d[157]);
    igsa_jnum(c, "scale", igsa_lef(d + 158));
    igsa_juint(c, "word162", igsa_le16(d + 162));
    igsa_juint(c, "word164", igsa_le16(d + 164));
    if (hl == 234)
        igsa_juint(c, "word230", igsa_le32(d + 230));
    igsa_jarr(c, "glyph");
    for (i = 0; i < g; i++) {
        const uint8_t *r = d + hl + 72 * (size_t)i;

        igsa_jobj(c, NULL);
        igsa_juint(c, "code", igsa_le32(r + 40));      /* KSCodeData: the map key */
        igsa_jarr(c, "fields");
        for (k = 0; k < 18; k++)
            if (k != 10)
                igsa_jint(c, NULL, (int32_t)igsa_le32(r + 4 * k));
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    list1(c, "font", "glyph table");
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_herobook = { "herobook", "Hero of Robots book_combine.bin: bookkeeping", book_probe, book_parse };
const igsa_format igsa_fmt_herocards = { "herocards", "Hero of Robots card_use.bin: card use counts", card_probe, card_parse };
const igsa_format igsa_fmt_herotime = { "herotime", "Hero of Robots timereset.bin", time_probe, time_parse };
const igsa_format igsa_fmt_herofont = { "herofont", "Hero of Robots .font: IGS font glyph table", font_probe, font_parse };
