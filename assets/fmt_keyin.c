/*
 * The Zhuyin (Bopomofo) input method's data, keyindat/ (Gold Captain and
 * Pirate Revenge carry the directory, unused; Go Go Ball 2's name entry
 * reads keyindat/table.dat: sub_80801A0 loads it, sub_80808C0 looks a
 * syllable up), and the store name it edits.  wiki Format-KEYIN,
 * Format-STORENAME.
 *
 * table.dat, PHONETIC.TAB ("keyintab"): u16 offsets[111] (offsets[0] = 222,
 *   offsets[110] = file size); section initial * 5 + tone spans
 *   offsets[s] .. offsets[s + 1]: groups of a code byte (medial << 4 | final)
 *   followed by the Big5 characters (lead byte >= 0x80) of that syllable.
 * ROMAN.BOX ("keyinbox"): "<pinyin>\t<Big5 zhuyin>\r\n" lines.
 * DIR.DOC ("keyindir"): "\\ <Big5 name>\r\n" lines.
 * storename.dat ("storename"): u32 length, the Big5 name, NUL (sub_807D580 /
 *   sub_8093440 in pitch).
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

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return 0;
    return !*a && !*b;
}

/* Big5 Bopomofo: initials ㄅ..ㄙ, medials ㄧ ㄨ ㄩ, finals ㄚ..ㄦ */
static const uint8_t initials[21] = { 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E,
                                      0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA };
static const uint8_t medials[3] = { 0xB8, 0xB9, 0xBA };
/* the tone slots in section order: 1st, neutral, 2nd, 3rd, 4th */
static const int tones[5] = { 1, 0, 2, 3, 4 };

/* ---- table.dat / PHONETIC.TAB ---------------------------------------------------------------- */

#define SECT 110

static int tab_check(const uint8_t *d, size_t n, size_t *bad)
{
    int s;

    if (n < 2 * (SECT + 1) || igsa_le16(d) != 2 * (SECT + 1) || igsa_le16(d + 2 * SECT) != n)
        return 0;
    for (s = 0; s < SECT; s++) {
        size_t o = igsa_le16(d + 2 * s), e = igsa_le16(d + 2 * s + 2);

        if (e < o)
            return 0;
        while (o < e) {
            uint8_t k = d[o++];

            if (k & 0x80 || (k >> 4) > 3 || (k & 15) > 13) {
                if (bad)
                    *bad = o - 1;
                return 0;
            }
            while (o < e && d[o] & 0x80) {
                if (o + 2 > e) {
                    if (bad)
                        *bad = o;
                    return 0;
                }
                o += 2;
            }
        }
    }
    return 1;
}

static int tab_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return tab_check(d, n, NULL) ? 100 : 0;
}

/* the syllable as Big5 bytes */
static size_t zhuyin(uint8_t *z, int ini, int code)
{
    size_t l = 0;

    if (ini) {
        z[l++] = 0xA3;
        z[l++] = initials[ini - 1];
    }
    if (code >> 4) {
        z[l++] = 0xA3;
        z[l++] = medials[(code >> 4) - 1];
    }
    if (code & 15) {
        z[l++] = 0xA3;
        z[l++] = (uint8_t)(0xAA + (code & 15));
    }
    return l;
}

static int tab_parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t bad = 0;
    int s, chars = 0;

    if (!tab_check(d, c->n, &bad)) {
        if (bad)
            return igsa_fail(c, "byte 0x%zx: not a syllable code or a whole Big5 character", bad);
        return igsa_fail(c, "no 111-offset section index ending at the file size");
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Zhuyin table");
    igsa_jarr(c, "sections");
    for (s = 0; s < SECT; s++) {
        size_t o = igsa_le16(d + 2 * s), e = igsa_le16(d + 2 * s + 2);
        int ini = s / 5, tone = tones[s % 5];
        uint8_t z[2];

        igsa_jobj(c, NULL);
        igsa_jint(c, "section", s);
        igsa_juint(c, "offset", o);
        igsa_jint(c, "initial", ini);
        if (ini) {
            z[0] = 0xA3;
            z[1] = initials[ini - 1];
            igsa_jbig5(c, "initial_zhuyin", (const char *)z, 2);
        }
        igsa_jint(c, "tone", tone);
        igsa_jarr(c, "syllables");
        while (o < e) {
            uint8_t code = d[o++], zy[6];
            size_t c0 = o, zl;

            while (o < e && d[o] & 0x80)
                o += 2;
            zl = zhuyin(zy, ini, code);
            igsa_jobj(c, NULL);
            igsa_jhex(c, "code", code);
            igsa_jint(c, "medial", code >> 4);
            igsa_jint(c, "final", code & 15);
            igsa_jbig5(c, "zhuyin", (const char *)zy, zl);
            igsa_jint(c, "count", (long long)(o - c0) / 2);
            igsa_jbig5(c, "chars", (const char *)d + c0, o - c0);
            igsa_jend(c);
            chars += (int)(o - c0) / 2;
            c->entries++;
        }
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jint(c, "chars", chars);
    igsa_jend(c);
    return 0;
}

/* ---- ROMAN.BOX ------------------------------------------------------------------------------- */

/* "<a-z>+\t(A3 74..BA)+\r\n" */
static int box_line(const uint8_t *p, size_t n, size_t *py, size_t *zl, size_t *len)
{
    size_t i = 0;

    while (i < n && p[i] >= 'a' && p[i] <= 'z')
        i++;
    if (!i || i >= n || p[i] != '\t')
        return 0;
    *py = i++;
    if (i + 2 > n)
        return 0;
    while (i + 1 < n && p[i] == 0xA3 && ((p[i + 1] >= 0x74 && p[i + 1] <= 0x7E) || (p[i + 1] >= 0xA1 && p[i + 1] <= 0xBA)))
        i += 2;
    *zl = i - *py - 1;
    while (i < n && p[i] == ' ')    /* one line ("zang") has a trailing blank */
        i++;
    if (!*zl || i + 2 > n || p[i] != '\r' || p[i + 1] != '\n')
        return 0;
    *len = i + 2;
    return 1;
}

static int box_valid(const uint8_t *d, size_t n, size_t *bad)
{
    size_t o = 0, py, zl, len;

    if (!n)
        return 0;
    while (o < n) {
        if (!box_line(d + o, n - o, &py, &zl, &len)) {
            if (bad)
                *bad = o;
            return 0;
        }
        o += len;
    }
    return 1;
}

static int box_probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return box_valid(d, n, NULL) ? 90 : 0;
}

static int box_parse(igsa_ctx *c)
{
    size_t o = 0, py, zl, len, bad = 0;

    if (!box_valid(c->d, c->n, &bad))
        return igsa_fail(c, "line at 0x%zx: not \"<pinyin>\\t<Big5 zhuyin>\\r\\n\"", bad);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Zhuyin romanization box");
    igsa_jarr(c, "syllables");
    while (o < c->n) {
        box_line(c->d + o, c->n - o, &py, &zl, &len);
        igsa_jobj(c, NULL);
        igsa_jstrn(c, "pinyin", (const char *)c->d + o, py);
        igsa_jbig5(c, "zhuyin", (const char *)c->d + o + py + 1, zl);
        igsa_jend(c);
        c->entries++;
        o += len;
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

/* ---- DIR.DOC --------------------------------------------------------------------------------- */

static int dir_valid(const uint8_t *d, size_t n)
{
    size_t o = 0;

    if (n < 5)
        return 0;
    while (o < n) {
        size_t e = o;

        if (o + 2 > n || d[o] != '\\' || d[o + 1] != '\\')
            return 0;
        while (e < n && d[e] != '\n')
            if (!d[e++])
                return 0;
        if (e >= n || e < o + 3 || d[e - 1] != '\r')
            return 0;
        o = e + 1;
    }
    return 1;
}

static int dir_probe(const uint8_t *d, size_t n, const char *path)
{
    if (!dir_valid(d, n))
        return 0;
    return ieq(base(path), "DIR.DOC") ? 90 : 20;
}

static int dir_parse(igsa_ctx *c)
{
    size_t o = 0;

    if (!dir_valid(c->d, c->n))
        return igsa_fail(c, "not \"\\\\ <name>\\r\\n\" lines");
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "keyindat DIR.DOC");
    igsa_jarr(c, "entries");
    while (o < c->n) {
        size_t e = o, s = o + 2;

        while (c->d[e] != '\n')
            e++;
        while (s < e - 1 && c->d[s] == ' ')
            s++;
        igsa_jobj(c, NULL);
        igsa_jbig5(c, "name", (const char *)c->d + s, e - 1 - s);
        igsa_jend(c);
        c->entries++;
        o = e + 1;
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

/* ---- storename.dat --------------------------------------------------------------------------- */

static int store_valid(const uint8_t *d, size_t n)
{
    uint32_t l;

    if (n < 5)
        return 0;
    l = igsa_le32(d);
    return l == n - 5 && !d[n - 1] && memchr(d + 4, 0, l) == NULL;
}

static int store_probe(const uint8_t *d, size_t n, const char *path)
{
    if (!store_valid(d, n))
        return 0;
    return ieq(base(path), "storename.dat") ? 90 : 40;
}

static int store_parse(igsa_ctx *c)
{
    if (!store_valid(c->d, c->n))
        return igsa_fail(c, "%zu bytes: not u32 length + name + NUL", c->n);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "store name");
    igsa_juint(c, "length", igsa_le32(c->d));
    igsa_jbig5(c, "name", (const char *)c->d + 4, igsa_le32(c->d));
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_keyintab = { "keyintab", "Zhuyin input table (table.dat, PHONETIC.TAB): syllable -> Big5 characters", tab_probe, tab_parse };
const igsa_format igsa_fmt_keyinbox = { "keyinbox", "Zhuyin romanization (ROMAN.BOX): pinyin -> zhuyin", box_probe, box_parse };
const igsa_format igsa_fmt_keyindir = { "keyindir", "keyindat DIR.DOC: Big5 directory names", dir_probe, dir_parse };
const igsa_format igsa_fmt_storename = { "storename", "storename.dat: the store name (u32 length, Big5, NUL)", store_probe, store_parse };
