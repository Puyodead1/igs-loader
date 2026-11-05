/*
 * We Dance Online 2's binary settings in /WeDo/linux/info (wiki Format-WEDO-INFO):
 *
 *   wedobook   bookEnc.bin (byte i XOR (i - 20)), book.bin (plain, an older copy):
 *              31 days x 28, 12 months x 12, 7 u32.  book_iIgs036Read 0x809D760,
 *              book_iIgs036Write 0x809D4E0, book_vNewDay 0x809CEE0, book_vNewMonth
 *              0x809CF50, the adders 0x809D940..0x809E580, book_iDayShow 0x809E8F0,
 *              book_iMonthShow 0x809F420
 *   wedosound  soundset.bin: 8 u32.  sdlmixer_iIgs036Read 0x8060D20, sdlmixer_vVolumeCheck
 *              0x8060C50
 *   wedostore  storename.dat: 32 bytes.  ime_iStoreNameInit 0x8089FC0,
 *              ime_iRestoreDefaultStoreName 0x808BF70, ime_iSetStoreVoice 0x8089B20
 *   wedonet    netsetting.dat: 111 bytes.  system/net/net OpenSetFile 0x8048564,
 *              Select_Connect 0x8048B44
 *   wedogameset gameset.bin: 6 u32 that nothing in the set reads
 */
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

/* ---- book -------------------------------------------------------------------------------- */

#define BOOKN 1040

static void unxor(uint8_t *o, const uint8_t *d)
{
    int i;

    for (i = 0; i < BOOKN; i++)
        o[i] = (uint8_t)(d[i] ^ (uint8_t)(i - 20));
}

static int book_ok(const uint8_t *b)
{
    int i;
    uint32_t day = igsa_le32(b + 1012), mon = igsa_le32(b + 1016);

    if (day > 31 || mon > 12)
        return 0;
    for (i = 0; i < 31; i++) {
        uint32_t f = igsa_le32(b + 28 * i + 8);

        if ((f & 1) && ((f >> 2 & 15) < 1 || (f >> 2 & 15) > 12 || (f >> 6 & 31) != (uint32_t)i + 1))
            return 0;
    }
    for (i = 0; i < 12; i++)
        if ((b[868 + 12 * i] & 1) && b[868 + 12 * i + 1] != i + 1)
            return 0;
    return 1;
}

static int book_probe(const uint8_t *d, size_t n, const char *path)
{
    uint8_t b[BOOKN];

    (void)path;
    if (n != BOOKN)
        return 0;
    if (book_ok(d))
        return 85;
    unxor(b, d);
    return book_ok(b) ? 85 : 0;
}

static int book_parse(igsa_ctx *c)
{
    uint8_t b[BOOKN];
    const uint8_t *t;
    int i, enc;

    if (c->n != BOOKN)
        return igsa_fail(c, "%zu bytes, the book is %d", c->n, BOOKN);
    enc = !book_ok(c->d);
    if (enc)
        unxor(b, c->d);
    else
        memcpy(b, c->d, BOOKN);
    if (!book_ok(b))
        return igsa_fail(c, "day/month records out of range, plain or decrypted");
    t = b + 1012;
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance bookkeeping");
    igsa_jbool(c, "encrypted", enc);
    igsa_juint(c, "day_index", igsa_le32(t));
    igsa_juint(c, "month_index", igsa_le32(t + 4));
    igsa_juint(c, "year_index", igsa_le32(t + 8));
    igsa_juint(c, "internal_coins", igsa_le32(t + 12));
    igsa_juint(c, "internal_service", igsa_le32(t + 16));
    igsa_juint(c, "all_work_s", igsa_le32(t + 20));
    igsa_juint(c, "internal_serial", igsa_le32(t + 24));
    igsa_jarr(c, "days");
    for (i = 0; i < 31; i++) {
        const uint8_t *r = b + 28 * i;
        uint32_t f = igsa_le32(r + 8);

        igsa_jobj(c, NULL);
        igsa_jint(c, "day", i + 1);
        igsa_jbool(c, "valid", f & 1);
        igsa_jbool(c, "this_month", f >> 1 & 1);
        igsa_juint(c, "month", f >> 2 & 15);
        igsa_juint(c, "mday", f >> 6 & 31);
        igsa_juint(c, "continues", f >> 11 & 0x3FF);
        igsa_juint(c, "service", f >> 21);
        igsa_juint(c, "open_s", igsa_le32(r));
        igsa_juint(c, "idle_s", igsa_le32(r + 4));
        igsa_juint(c, "coins", igsa_le16(r + 12));
        igsa_juint(c, "players", igsa_le16(r + 14));
        igsa_juint(c, "work_s", igsa_le32(r + 16));
        igsa_juint(c, "card_uses", igsa_le16(r + 20));
        igsa_jbool(c, "flag_22", r[22] & 1);
        igsa_juint(c, "fights", igsa_le16(r + 22) >> 1 & 0xFF);
        igsa_juint(c, "market", igsa_le16(r + 24));
        igsa_juint(c, "simple_players", igsa_le16(r + 26));
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
    igsa_jarr(c, "months");
    for (i = 0; i < 12; i++) {
        const uint8_t *m = b + 868 + 12 * i;

        igsa_jobj(c, NULL);
        igsa_jint(c, "index", i + 1);
        igsa_jbool(c, "valid", m[0] & 1);
        igsa_jint(c, "year", 2000 + (m[0] >> 1));
        igsa_jint(c, "month", m[1]);
        igsa_juint(c, "coins", igsa_le16(m + 2));
        igsa_juint(c, "service", igsa_le16(m + 4));
        igsa_juint(c, "players", igsa_le16(m + 6));
        igsa_juint(c, "market", igsa_le16(m + 8));
        igsa_juint(c, "simple_players", igsa_le16(m + 10));
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
    igsa_jend(c);
    if (c->mode == IGSA_EXTRACT && enc && igsa_emit_raw(c, "book.bin", b, BOOKN))
        return -1;
    return 0;
}

/* ---- soundset.bin ------------------------------------------------------------------------ */

static int sound_probe(const uint8_t *d, size_t n, const char *path)
{
    int i;

    if (n != 32 || strcasecmp(base(path), "soundset.bin"))
        return 0;
    for (i = 0; i < 8; i++)
        if (igsa_le32(d + 4 * i) > 255)
            return 0;
    return 80;
}

static int sound_parse(igsa_ctx *c)
{
    static const char *const k[8] = { "setting_0", "day_start", "day_game_volume", "day_demo_volume", "night_start",
                                      "night_game_volume", "night_demo_volume", "setting_7" };
    int i;

    if (c->n != 32)
        return igsa_fail(c, "%zu bytes, soundset.bin is 32", c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance sound setting");
    for (i = 0; i < 8; i++)
        igsa_juint(c, k[i], igsa_le32(c->d + 4 * i));
    for (i = 0; i < 2; i++) {           /* the starts are half hours */
        char hm[16];
        uint32_t v = igsa_le32(c->d + (i ? 16 : 4));

        snprintf(hm, sizeof(hm), "%02u:%02u", v / 2, v % 2 * 30);
        igsa_jstr(c, i ? "night_from" : "day_from", hm);
    }
    igsa_jend(c);
    c->entries++;
    return 0;
}

/* ---- storename.dat ----------------------------------------------------------------------- */

static int store_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)d;
    return n == 32 && !strcasecmp(base(path), "storename.dat") && memchr(d, 0, 11) && memchr(d + 12, 0, 18) ? 95 : 0;
}

static int store_parse(igsa_ctx *c)
{
    char place[12], name[19];

    if (c->n != 32)
        return igsa_fail(c, "%zu bytes, storename.dat is 32", c->n);
    igsa_str(place, c->d, 11);
    igsa_str(name, c->d + 12, 18);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance store name");
    igsa_jbig5(c, "place", place, strlen(place));
    igsa_juint(c, "province", c->d[11]);
    igsa_jbig5(c, "name", name, strlen(name));
    igsa_juint(c, "store_list_index", igsa_le16(c->d + 30));
    igsa_jend(c);
    c->entries++;
    return 0;
}

/* ---- netsetting.dat ---------------------------------------------------------------------- */

static int net_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == 111 && d[0] <= 2 && memchr(d + 1, 0, 16) && memchr(d + 17, 0, 16) && memchr(d + 33, 0, 16) ? 70 : 0;
}

static int net_parse(igsa_ctx *c)
{
    static const char *const mode[3] = { "dhcp", "static", "pppoe" };
    char s[32];

    if (c->n != 111)
        return igsa_fail(c, "%zu bytes, netsetting.dat is 111", c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance network setting");
    igsa_juint(c, "type", c->d[0]);
    igsa_jstr(c, "mode", c->d[0] < 3 ? mode[c->d[0]] : "error");
    igsa_str(s, c->d + 1, 16);
    igsa_jstr(c, "ip", s);
    igsa_str(s, c->d + 17, 16);
    igsa_jstr(c, "netmask", s);
    igsa_str(s, c->d + 33, 16);
    igsa_jstr(c, "gateway", s);
    igsa_str(s, c->d + 49, 31);
    igsa_jstr(c, "pppoe_account", s);
    igsa_str(s, c->d + 80, 31);
    igsa_jstr(c, "pppoe_password", s);
    igsa_jend(c);
    c->entries++;
    return 0;
}

/* ---- gameset.bin ------------------------------------------------------------------------- */

static int gameset_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)d;
    return n == 24 && !strcasecmp(base(path), "gameset.bin") ? 80 : 0;
}

static int gameset_parse(igsa_ctx *c)
{
    int i;

    if (c->n != 24)
        return igsa_fail(c, "%zu bytes, gameset.bin is 24", c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "We Dance gameset.bin (read by nothing in the set)");
    igsa_jarr(c, "values");
    for (i = 0; i < 6; i++)
        igsa_juint(c, NULL, igsa_le32(c->d + 4 * i));
    igsa_jend(c);
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_wedobook = { "wedobook", "We Dance bookkeeping (bookEnc.bin / book.bin): days, months, counters",
                                        book_probe, book_parse };
const igsa_format igsa_fmt_wedosound = { "wedosound", "We Dance soundset.bin: day / night volumes", sound_probe, sound_parse };
const igsa_format igsa_fmt_wedostore = { "wedostore", "We Dance storename.dat: place, province, store name", store_probe, store_parse };
const igsa_format igsa_fmt_wedonet = { "wedonet", "We Dance netsetting.dat: DHCP / static / PPPoE", net_probe, net_parse };
const igsa_format igsa_fmt_wedogameset = { "wedogameset", "We Dance gameset.bin (unused)", gameset_probe, gameset_parse };
