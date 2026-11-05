/*
 * Percussion Master 3 rewrite/ files and the IME table: Book.dat, SettingInfo.dat,
 * SelTime.dat, Total.dat (SettingData), RankList_*.dat (RankList), SongTopScore.dat,
 * hotlist.dat, songvolume.dat, ccdctrl.dat (CCDDataCtrl), filter_text.dat, store.csv, store_list.csv (MD5-headed lists;
 * media/ime/phonetic.tab is keyintab, fmt_keyin.c).  wiki Format-PM3SAVE.
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

/* ---- Book.dat: SettingData day / month records ---------------------------------------- */

static int p_book(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == 1354 && igsa_le32(d + 1346) == 960 && igsa_le32(d + 1350) == 384 && d[0] <= 30 && d[1] <= 12 ? 80 : 0;
}

/* a 32-byte record (SettingData::vAddRound etc., vNetSendDayIncome) */
static void book_rec(igsa_ctx *c, const uint8_t *r, int day)
{
    igsa_jobj(c, NULL);
    igsa_jint(c, "age", igsa_le16(r));
    igsa_jint(c, "year", igsa_le16(r + 2));
    igsa_jint(c, "month", igsa_le16(r + 4));
    if (day)
        igsa_jint(c, "day", igsa_le16(r + 6));
    igsa_jint(c, "coins", igsa_le16(r + 8));
    igsa_jint(c, "rounds", igsa_le16(r + 10));
    igsa_jint(c, "service", igsa_le16(r + 12));
    igsa_jint(c, "real_coins", igsa_le16(r + 14));
    igsa_jint(c, "real_service", igsa_le16(r + 16));
    igsa_jint(c, "account_rounds", igsa_le16(r + 18));
    igsa_juint(c, "open_s", igsa_le32(r + 20));
    igsa_juint(c, "play_s", igsa_le32(r + 24));
    igsa_jint(c, "word28", igsa_le16(r + 28));
    igsa_jint(c, "unsent", r[30]);
    igsa_jend(c);
}

static int f_book(igsa_ctx *c)
{
    int i;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3book");
    igsa_jint(c, "day_index", c->d[0]);
    igsa_jint(c, "month_index", c->d[1]);
    igsa_jarr(c, "days");
    for (i = 0; i < 30; i++)
        book_rec(c, c->d + 2 + 32 * i, 1);
    igsa_jend(c);
    igsa_jarr(c, "months");
    for (i = 0; i < 12; i++)
        book_rec(c, c->d + 962 + 32 * i, 0);
    igsa_jend(c);
    igsa_jend(c);
    list1(c, "book", "30 days, 12 months");
    c->entries++;
    return 0;
}

/* ---- SettingInfo.dat (84 bytes, SettingData::vLoadInfo / vResetInfo) ------------------- */

static int p_setting(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == 84 && igsa_le32(d + 80) == 84 ? 80 : 0;
}

static int f_setting(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    char ip[3][20], mac[20], pw[5], mpw[5];
    int i;

    for (i = 0; i < 3; i++)
        snprintf(ip[i], sizeof(ip[i]), "%u.%u.%u.%u", d[4 * i], d[4 * i + 1], d[4 * i + 2], d[4 * i + 3]);
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", d[12], d[13], d[14], d[15], d[16], d[17]);
    for (i = 0; i < 4; i++) {
        pw[i] = (char)('0' + d[49 + i] % 10);
        mpw[i] = (char)('0' + d[53 + i] % 10);
    }
    pw[4] = mpw[4] = 0;
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3setting");
    igsa_jstr(c, "ip", ip[0]);
    igsa_jstr(c, "netmask", ip[1]);
    igsa_jstr(c, "gateway", ip[2]);
    igsa_jstr(c, "mac", mac);
    igsa_jint(c, "country", (int8_t)d[18]);
    igsa_jint(c, "province", (int8_t)d[19]);
    igsa_jint(c, "region", igsa_le16(d + 20));
    igsa_juint(c, "store", igsa_le32(d + 24));
    igsa_jbig5(c, "store_name", (const char *)d + 28, strnlen((const char *)d + 28, 21));
    igsa_jstr(c, "password", pw);
    igsa_jstr(c, "master_password", mpw);
    igsa_jint(c, "coins_per_credit", d[57]);
    igsa_jbool(c, "free_play", d[58]);
    igsa_jint(c, "day_from_min", (int32_t)igsa_le32(d + 60));
    igsa_jint(c, "day_game_volume", d[64]);
    igsa_jint(c, "day_demo_volume", d[65]);
    igsa_jint(c, "night_from_min", (int32_t)igsa_le32(d + 68));
    igsa_jint(c, "night_game_volume", d[72]);
    igsa_jint(c, "night_demo_volume", d[73]);
    igsa_jint(c, "game_level", d[74]);
    igsa_jint(c, "store_changed", d[75]);
    igsa_jint(c, "ip_mode", d[76]);
    igsa_jint(c, "voice_language", d[77]);
    igsa_jend(c);
    list1(c, "set", "settings");
    c->entries++;
    return 0;
}

/* ---- SelTime.dat, Total.dat -------------------------------------------------------------- */

static int p_seltime(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == 84 && igsa_le32(d + 80) == 80 ? 80 : 0;
}

static int f_seltime(igsa_ctx *c)
{
    static const char *const m[4] = { "solo", "couple", "versus", "skill" };
    int i, k;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3seltime");
    for (k = 0; k < 4; k++) {
        igsa_jarr(c, m[k]);
        for (i = 0; i < 5; i++)
            igsa_juint(c, NULL, igsa_le32(c->d + 20 * k + 4 * i));
        igsa_jend(c);
    }
    igsa_jend(c);
    list1(c, "sel", "mode selections");
    c->entries++;
    return 0;
}

static int p_total(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == 16 && igsa_le32(d + 12) == 12 ? 70 : 0;
}

static int f_total(igsa_ctx *c)
{
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3total");
    igsa_juint(c, "coins", igsa_le32(c->d));
    igsa_juint(c, "rounds", igsa_le32(c->d + 4));
    igsa_juint(c, "service", igsa_le32(c->d + 8));
    igsa_jend(c);
    list1(c, "total", "totals");
    c->entries++;
    return 0;
}

/* ---- RankList_<Normal|Nostop>_<level>.dat: 99 x 36 bytes -------------------------------- */

static int p_rank(const uint8_t *d, size_t n, const char *path)
{
    int i;

    if (n != 3564)
        return 0;
    for (i = 0; i < 99; i++)
        if (d[36 * i + 32])
            return 0;
    return named(path, "RankList") ? 80 : 20;
}

static int f_rank(igsa_ctx *c)
{
    int i;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3rank");
    igsa_jarr(c, "entries");
    for (i = 0; i < 99; i++) {
        const uint8_t *r = c->d + 36 * i;

        igsa_jobj(c, NULL);
        igsa_jint(c, "place", i + 1);
        igsa_jint(c, "player", (int32_t)igsa_le32(r));
        igsa_jint(c, "photo", (int32_t)igsa_le32(r + 4));
        igsa_jint(c, "photo_flag", (int32_t)igsa_le32(r + 8));
        igsa_juint(c, "score", igsa_le32(r + 12));
        igsa_jint(c, "player_info", (int32_t)igsa_le32(r + 16));
        igsa_jbig5(c, "name", (const char *)r + 20, strnlen((const char *)r + 20, 13));
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    list1(c, "rank", "99 places");
    c->entries++;
    return 0;
}

/* ---- SongTopScore.dat: 32-byte records ----------------------------------------------------- */

static int p_top(const uint8_t *d, size_t n, const char *path)
{
    size_t i, songs = 0;

    if (!n || n % 32)
        return 0;
    for (i = 0; i < n; i += 32) {
        if (igsa_le32(d + i) > 255 || igsa_le32(d + i + 4) > 4)
            return 0;
        songs += igsa_le32(d + i) != 0;
    }
    if (!songs)
        return 0;
    return named(path, "SongTopScore") ? 80 : 15;
}

static int f_top(igsa_ctx *c)
{
    size_t i;
    char info[32];

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3topscore");
    igsa_jarr(c, "records");
    for (i = 0; i < c->n; i += 32) {
        const uint8_t *r = c->d + i;

        igsa_jobj(c, NULL);
        igsa_juint(c, "song", igsa_le32(r));
        igsa_juint(c, "class", igsa_le32(r + 4));
        igsa_jint(c, "photo", (int32_t)igsa_le32(r + 8));
        igsa_juint(c, "score", igsa_le32(r + 12));
        igsa_jbig5(c, "name", (const char *)r + 16, strnlen((const char *)r + 16, 13));
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    snprintf(info, sizeof(info), "%zu records", c->n / 32);
    list1(c, "top", info);
    c->entries++;
    return 0;
}

/* ---- hotlist.dat: {u32 song, u32 plays[5]} ----------------------------------------------- */

static int p_hot(const uint8_t *d, size_t n, const char *path)
{
    size_t i;

    if (!n || n % 24)
        return 0;
    for (i = 0; i < n; i += 24)
        if (igsa_le32(d + i) - 1 > 254)
            return 0;
    return named(path, "hotlist") ? 80 : 15;
}

static int f_hot(igsa_ctx *c)
{
    size_t i;
    int k;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3hotlist");
    igsa_jarr(c, "songs");
    for (i = 0; i < c->n; i += 24) {
        igsa_jobj(c, NULL);
        igsa_juint(c, "song", igsa_le32(c->d + i));
        igsa_jarr(c, "plays");      /* per class easy..master; halved when one passes 0x4000 */
        for (k = 0; k < 5; k++)
            igsa_juint(c, NULL, igsa_le32(c->d + i + 4 + 4 * k));
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    list1(c, "hot", "song play counts");
    c->entries++;
    return 0;
}

/* ---- songvolume.dat: {u16 song, u16 volume[3]} (SongVolMgr::Read) -------------------------- */

static int p_vol(const uint8_t *d, size_t n, const char *path)
{
    size_t i;

    if (!n || n % 8 || !named(path, "songvolume"))
        return 0;
    for (i = 0; i < n; i += 8)
        if (igsa_le16(d + i) - 1u > 254)
            return 0;
    return 80;
}

static int f_vol(igsa_ctx *c)
{
    size_t i;
    int k;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3songvol");
    igsa_jarr(c, "songs");
    for (i = 0; i < c->n; i += 8) {
        igsa_jobj(c, NULL);
        igsa_juint(c, "song", igsa_le16(c->d + i));
        igsa_jarr(c, "volumes");
        for (k = 0; k < 3; k++)
            igsa_juint(c, NULL, igsa_le16(c->d + i + 2 + 2 * k));
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    list1(c, "vol", "song volumes");
    c->entries++;
    return 0;
}

/* ---- ccdctrl.dat: six camera controls {enabled, min, max, value, default} ---------------- */

static int p_ccd(const uint8_t *d, size_t n, const char *path)
{
    /* no check word: the game compares each control's min / max with the camera's and resets
       the file when they differ */
    (void)d;
    return n == 120 && named(path, "ccdctrl") ? 80 : 0;
}

static int f_ccd(igsa_ctx *c)
{
    int i;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3ccd");
    igsa_jarr(c, "controls");
    for (i = 0; i < 6; i++) {
        const uint8_t *r = c->d + 20 * i;

        igsa_jobj(c, NULL);
        igsa_jint(c, "control", 2 * i);         /* grab_iGetControl / grab_iSetControl id */
        igsa_juint(c, "enabled", igsa_le32(r));
        igsa_jint(c, "min", (int32_t)igsa_le32(r + 4));
        igsa_jint(c, "max", (int32_t)igsa_le32(r + 8));
        igsa_jint(c, "value", (int32_t)igsa_le32(r + 12));
        igsa_jint(c, "default", (int32_t)igsa_le32(r + 16));
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    list1(c, "ccd", "6 camera controls");
    c->entries++;
    return 0;
}

/* ---- filter_text.dat: MD5 line + banned words (IME::LoadFilterTable) ---------------------- */

/* MD5 (RFC 1321) */
static void md5(const uint8_t *msg, size_t len, uint8_t out[16])
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const uint8_t R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20,
        5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    uint8_t blk[64];
    size_t off, total = (len + 9 + 63) / 64 * 64;
    int i;

    for (off = 0; off < total; off += 64) {
        uint32_t w[16], a = h[0], b = h[1], cc = h[2], dd = h[3];

        for (i = 0; i < 64; i++) {
            size_t p = off + i;

            blk[i] = p < len ? msg[p] : p == len ? 0x80 : 0;
        }
        if (off + 64 == total)
            for (i = 0; i < 8; i++)
                blk[56 + i] = (uint8_t)((uint64_t)len * 8 >> (8 * i));
        for (i = 0; i < 16; i++)
            w[i] = igsa_le32(blk + 4 * i);
        for (i = 0; i < 64; i++) {
            uint32_t f, g, t;

            if (i < 16) {
                f = (b & cc) | (~b & dd);
                g = i;
            } else if (i < 32) {
                f = (dd & b) | (~dd & cc);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                f = b ^ cc ^ dd;
                g = (3 * i + 5) & 15;
            } else {
                f = cc ^ (b | ~dd);
                g = (7 * i) & 15;
            }
            t = dd;
            dd = cc;
            cc = b;
            f += a + K[i] + w[g];
            b += f << R[i] | f >> (32 - R[i]);
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += cc;
        h[3] += dd;
    }
    for (i = 0; i < 16; i++)
        out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
}

static int ishex(const uint8_t *d, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!((d[i] >= '0' && d[i] <= '9') || ((d[i] | 32) >= 'a' && (d[i] | 32) <= 'f')))
            return 0;
    return 1;
}

/* the MD5 (hex) in the first line against the rest: 1 match, 0 not, -1 no MD5 line */
static int md5_line(const uint8_t *d, size_t n, size_t *body, char hex[33])
{
    uint8_t h[16];
    size_t o = 32;
    int i;

    if (n < 34 || !ishex(d, 32) || (d[32] != '\r' && d[32] != '\n'))
        return -1;
    if (d[o] == '\r')
        o++;
    if (o < n && d[o] == '\n')
        o++;
    *body = o;
    md5(d + o, n - o, h);
    for (i = 0; i < 16; i++)
        snprintf(hex + 2 * i, 3, "%02x", h[i]);
    for (i = 0; i < 32 && (hex[i] | 32) == (d[i] | 32); i++)
        ;
    return i == 32;
}

static int p_list(const uint8_t *d, size_t n, const char *path)
{
    size_t b;
    char hex[33];
    int r = md5_line(d, n, &b, hex);

    (void)path;
    return r < 0 ? 0 : r ? 85 : 30;
}

/* downloaded lists (PowerOn 0x804A31F, IME::LoadFilterTable): filter_text.dat one word per line up
   to the first empty line; store.csv, store_list.csv comma-separated rows */
static int f_list(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t o, w0;
    char hex[33];
    int ok = md5_line(d, c->n, &o, hex), nl = 0, csv;
    const char *b = base(c->path), *x = strrchr(b, '.');

    if (ok < 0)
        return igsa_fail(c, "no MD5 line");
    csv = x && (x[1] | 32) == 'c' && (x[2] | 32) == 's' && (x[3] | 32) == 'v' && !x[4];
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "pm3list");
    igsa_jstrn(c, "md5_line", (const char *)d, 32);
    igsa_jstr(c, "md5_text", hex);
    igsa_jbool(c, "md5_ok", ok);
    igsa_jarr(c, csv ? "rows" : "words");
    while (o < c->n) {
        size_t e;

        w0 = o;
        while (o < c->n && d[o] != '\n')
            o++;
        e = o;
        if (o < c->n)
            o++;
        while (e > w0 && (d[e - 1] == '\r' || d[e - 1] == ' ' || d[e - 1] == '\t'))
            e--;
        if (e == w0) {
            if (!csv)
                break;          /* the game stops at the first empty line */
            continue;
        }
        if (csv) {
            size_t s = w0, k;

            igsa_jarr(c, NULL);
            for (k = w0; k <= e; k++)
                if (k == e || d[k] == ',') {
                    igsa_jbig5(c, NULL, (const char *)d + s, k - s);
                    s = k + 1;
                }
            igsa_jend(c);
        } else
            igsa_jbig5(c, NULL, (const char *)d + w0, e - w0);
        nl++;
    }
    igsa_jend(c);
    igsa_jint(c, "count", nl);
    igsa_jend(c);
    list1(c, "list", csv ? "rows" : "words");
    c->entries++;
    return 0;
}
const igsa_format igsa_fmt_pm3book = { "pm3book", "Percussion Master 3 Book.dat: daily / monthly bookkeeping", p_book, f_book };
const igsa_format igsa_fmt_pm3setting = { "pm3set", "Percussion Master 3 SettingInfo.dat", p_setting, f_setting };
const igsa_format igsa_fmt_pm3seltime = { "pm3sel", "Percussion Master 3 SelTime.dat: mode selections", p_seltime, f_seltime };
const igsa_format igsa_fmt_pm3total = { "pm3total", "Percussion Master 3 Total.dat", p_total, f_total };
const igsa_format igsa_fmt_pm3rank = { "pm3rank", "Percussion Master 3 RankList_*.dat", p_rank, f_rank };
const igsa_format igsa_fmt_pm3top = { "pm3top", "Percussion Master 3 SongTopScore.dat", p_top, f_top };
const igsa_format igsa_fmt_pm3hot = { "pm3hot", "Percussion Master 3 hotlist.dat", p_hot, f_hot };
const igsa_format igsa_fmt_pm3vol = { "pm3vol", "Percussion Master 3 songvolume.dat", p_vol, f_vol };
const igsa_format igsa_fmt_pm3ccd = { "pm3ccd", "Percussion Master 3 ccdctrl.dat: camera controls", p_ccd, f_ccd };
const igsa_format igsa_fmt_pm3list = { "pm3list", "Percussion Master 3 downloaded lists (MD5 line + words / CSV)", p_list, f_list };
