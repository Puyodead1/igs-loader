/*
 * Panda Family demo-play key logs (demoplay/[MMDD_HHMMSS.]sSStTdD.keylog):
 * the KeyLogger list written as is, 16 bytes per node.  KeyLogger_Add
 * (PF 0x806F4B0), _WriteLogFile (0x80F0E30), _LoadStageKeyLog (0x80F0C20).
 * wiki Format-KEYLOG.
 *
 *   u32 key      game key index 0..7 (Setting_IsGameKeyPressed); only the low byte is set
 *   u32 pressed  1
 *   u32 frame    the logger's frame counter (replay ORs 1 << key into frame's key mask)
 *   u32 next     the node's heap pointer at record time (meaningless on disk)
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static int named(const char *path)
{
    size_t l = path ? strlen(path) : 0;

    return l >= 7 && !strcmp(path + l - 7, ".keylog");
}

static int valid(const uint8_t *d, size_t n)
{
    size_t i;

    if (n % 16)
        return 0;
    for (i = 0; i < n; i += 16)
        if (d[i] > 11 || igsa_le32(d + i + 4) > 1 || (i && igsa_le32(d + i + 8) < igsa_le32(d + i - 8)))
            return 0;
    return 1;
}

static int probe(const uint8_t *d, size_t n, const char *path)
{
    if (!valid(d, n))
        return 0;
    /* empty logs exist (a stage played without input): the name decides */
    return named(path) ? (n ? 90 : 50) : 0;
}

static int parse(igsa_ctx *c)
{
    const char *b = c->path, *p;
    size_t i;
    int st, tb, df, mo, dy, hh, mi, ss;

    if (!valid(c->d, c->n))
        return igsa_fail(c, "%zu bytes: not 16-byte key records in frame order", c->n);
    for (p = c->path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "PF keylog");
    /* the name: [MMDD_HHMMSS.]sSStTdD.keylog: stage, table (arrangement), difficulty */
    if (sscanf(b, "%2d%2d_%2d%2d%2d.s%2dt%1dd%1d.keylog", &mo, &dy, &hh, &mi, &ss, &st, &tb, &df) == 8) {
        igsa_jint(c, "month", mo);
        igsa_jint(c, "day", dy);
        igsa_jint(c, "hour", hh);
        igsa_jint(c, "minute", mi);
        igsa_jint(c, "second", ss);
        igsa_jint(c, "stage", st);
        igsa_jint(c, "table", tb);
        igsa_jint(c, "difficulty", df);
    } else if (sscanf(b, "s%2dt%1dd%1d.keylog", &st, &tb, &df) == 3) {
        igsa_jint(c, "stage", st);
        igsa_jint(c, "table", tb);
        igsa_jint(c, "difficulty", df);
    }
    igsa_juint(c, "records", c->n / 16);
    igsa_jarr(c, "keys");
    for (i = 0; i < c->n; i += 16) {
        const uint8_t *r = c->d + i;

        igsa_jobj(c, NULL);
        igsa_juint(c, "frame", igsa_le32(r + 8));
        igsa_jint(c, "key", r[0]);
        if (igsa_le32(r) >> 8)
            igsa_jhex(c, "key_word", igsa_le32(r));
        igsa_juint(c, "pressed", igsa_le32(r + 4));
        igsa_jhex(c, "next", igsa_le32(r + 12));
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_keylog = { "keylog", "Panda Family demo-play key log (16-byte key records)", probe, parse };
