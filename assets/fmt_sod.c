/*
 * SOD stage scripts (E2000 shooters: Pirate Revenge, Gold Captain, the .sod files under sod/):
 * the stage's objects (enemies, items, traps), their paths and summons, the
 * war zones.  Sections of name[32] (right-aligned, NUL-terminated),
 * length[6] (decimal ASCII), data.  sodReader_LoadFile / sodReader_ReadSODData
 * and the sodReader_ReadSOD3x/4x readers (sship 0x80C4310..0x80C5D4C).
 * wiki Format-SOD.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static const char vhdr[32] = "                        Version";

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 38 && !memcmp(d, vhdr, 32) ? 100 : 0;
}

/* the object record layouts: what each reader copies into LoadedObjects (1528-byte entries) */
static const struct cod {
    const char *name;
    int rec, nparam, param, id;     /* id: offset, -1 none, -2 = the record index */
    int path, pstride, pfields;     /* pfields 4: the Sod37 reader leaves "type" 0 */
    int sum, sstride, sfields;      /* sum < 0: no summons */
    int frames, summoner;           /* -1: not in the record */
} cods[] = {
    { "Sod37_Cod", 580, 20, 20, -1, 100, 16, 4, -1, 0, 0, -1, -1 },
    { "Sod38_Cod", 700, 20, 20, -1, 100, 20, 5, -1, 0, 0, -1, -1 },
    { "Sod39_Cod", 1420, 20, 20, -2, 100, 20, 5, 700, 24, 2, -1, -1 },
    { "Sod40_Cod", 1404, 15, 24, 20, 84, 20, 5, 684, 24, 2, -1, -1 },
    { "Sod42_Cod", 1404, 15, 24, 20, 84, 20, 5, 684, 24, 6, -1, -1 },
    { "Sod43_Cod", 1528, 15, 28, 20, 88, 20, 5, 688, 28, 7, 12, 24 },   /* read raw */
};

static const char *const cls[] = { "enemy", "item", "trap" };
static const char *const pf[] = { "frames", "dx", "dz", "delay", "type" };
static const char *const sf[] = { "frames", "id", "dx", "dz", "a", "b", "index" };

static int32_t i32(const uint8_t *p) { return (int32_t)igsa_le32(p); }

static void objects(igsa_ctx *c, const struct cod *k, const uint8_t *p, size_t n)
{
    size_t i, cnt = n / (size_t)k->rec;

    igsa_jstr(c, "layout", k->name);
    igsa_jarr(c, "objects");
    for (i = 0; i < cnt; i++) {
        const uint8_t *r = p + i * (size_t)k->rec;
        int cl = (int8_t)r[16], j, f;

        igsa_jobj(c, NULL);
        igsa_juint(c, "index", i);
        if (k->id >= 0)
            igsa_jint(c, "id", i32(r + k->id));
        else if (k->id == -2)
            igsa_juint(c, "id", i);
        igsa_jint(c, "class", cl);
        if (cl >= 0 && cl <= 2)
            igsa_jstr(c, "class_name", cls[cl]);
        else
            igsa_jbool(c, "empty", 1);  /* sodReader_IsCoreObjEmpty */
        igsa_jint(c, "type", (int8_t)r[17]);
        igsa_jint(c, "x", i32(r));
        igsa_jint(c, "y", i32(r + 4));
        igsa_jint(c, "z", i32(r + 8));
        if (k->frames >= 0)
            igsa_jint(c, "frames", i32(r + k->frames));
        if (k->summoner >= 0)
            igsa_jint(c, "summoner", i32(r + k->summoner));
        igsa_jarr(c, "params");
        for (j = 0; j < k->nparam; j++)
            igsa_jint(c, NULL, i32(r + k->param + 4 * j));
        igsa_jend(c);
        igsa_jarr(c, "paths");
        for (j = 0; j < 30; j++) {
            /* the Sod37 reader takes the 4 dwords after each 16-byte entry's first */
            const uint8_t *q = r + k->path + k->pstride * j + (k->pfields == 4 ? 4 : 0);
            int32_t fr = i32(q), dl = i32(q + 12);

            if (fr <= 0 || dl < 0)      /* sodReader_IsPathEmpty */
                continue;
            igsa_jobj(c, NULL);
            igsa_jint(c, "slot", j);
            for (f = 0; f < k->pfields; f++)
                igsa_jint(c, pf[f], i32(q + 4 * f));
            igsa_jend(c);
        }
        igsa_jend(c);
        if (k->sum >= 0) {
            igsa_jarr(c, "summons");
            for (j = 0; j < 30; j++) {
                const uint8_t *q = r + k->sum + k->sstride * j;

                if (i32(q) < 0 || i32(q + 4) < 0)     /* sodReader_IsSummonObjEmpty */
                    continue;
                igsa_jobj(c, NULL);
                igsa_jint(c, "slot", j);
                for (f = 0; f < k->sfields; f++)
                    igsa_jint(c, sf[f], i32(q + 4 * f));
                igsa_jend(c);
            }
            igsa_jend(c);
        }
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
}

static int parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t o = 0, n = c->n;
    int nsec = 0;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "SOD");
    igsa_jarr(c, "sections");
    while (o < n) {
        char name[33], len[7], *e;
        const char *nm;
        long l;
        size_t k;

        if (o + 38 > n)
            return igsa_fail(c, "section at 0x%zx: header truncated", o);
        memcpy(name, d + o, 32);
        name[32] = 0;
        memcpy(len, d + o + 32, 6);
        len[6] = 0;
        l = strtol(len, &e, 10);
        if (e == len || l < 0 || (size_t)l > n - o - 38)
            return igsa_fail(c, "section at 0x%zx: length \"%s\"", o, len);
        for (nm = name; *nm == ' '; nm++)
            ;
        igsa_jobj(c, NULL);
        igsa_jstr(c, "name", nm);
        igsa_jhex(c, "offset", o);
        igsa_jint(c, "length", l);
        o += 38;
        if (!strcmp(nm, "Version"))
            igsa_jstrn(c, "version", (const char *)d + o, strnlen((const char *)d + o, (size_t)l));
        else if (!strcmp(nm, "Sod37_OASetting") || !strcmp(nm, "Sod40G_OASetting") || !strcmp(nm, "Sod42_OASetting")) {
            /* LoadedObjectsAppSetting, 12 bytes per object; the Sod37/40G readers keep bytes 0, 4-7, 8 */
            int raw = nm[3] == '4' && nm[4] == '2';

            if (l % 12)
                return igsa_fail(c, "%s: %ld bytes, not 12-byte records", nm, l);
            igsa_jarr(c, "settings");
            for (k = 0; k + 12 <= (size_t)l; k += 12) {
                const uint8_t *q = d + o + k;

                igsa_jobj(c, NULL);
                igsa_jint(c, "b0", q[0]);
                igsa_jint(c, "frames", i32(q + 4));  /* sodReader_GetWarZoneFramesOfAppSetting */
                igsa_jint(c, "b8", q[8]);
                if (raw) {
                    igsa_jint(c, "b9", q[9]);
                    igsa_jint(c, "b10", q[10]);
                }
                igsa_jend(c);
            }
            igsa_jend(c);
        } else if (!strcmp(nm, "Sod37_SODSetting")) {
            if (l < 12)
                return igsa_fail(c, "%s: %ld bytes", nm, l);
            igsa_jint(c, "whole_distance", i32(d + o));     /* sodReader_GetSODWholeDistance */
            igsa_jint(c, "elapsed_time", i32(d + o + 4));   /* sodReader_GetSODElapsedTime */
            igsa_jint(c, "speed", i32(d + o + 8));          /* sodReader_GetSODSpeed */
            if (l > 12)
                igsa_jbytes(c, "rest", d + o + 12, (size_t)l - 12);
        } else if (!strcmp(nm, "Sod37_WarZone")) {
            if (l % 12)
                return igsa_fail(c, "%s: %ld bytes, not 12-byte records", nm, l);
            igsa_jarr(c, "zones");
            for (k = 0; k + 12 <= (size_t)l; k += 12) {
                igsa_jobj(c, NULL);
                igsa_jint(c, "z", i32(d + o + k));                  /* sodReader_GetWarZoneIndexByZ */
                igsa_jint(c, "length", i32(d + o + k + 4));
                igsa_jint(c, "forward_speed", i32(d + o + k + 8));  /* sodReader_GetForwardSpeed */
                igsa_jend(c);
            }
            igsa_jend(c);
        } else {
            size_t i;

            for (i = 0; i < sizeof(cods) / sizeof(cods[0]) && strcmp(nm, cods[i].name); i++)
                ;
            if (i == sizeof(cods) / sizeof(cods[0]))
                return igsa_fail(c, "section \"%s\" at 0x%zx: not one sodReader_ReadSODData knows", nm, o - 38);
            if (l % cods[i].rec)
                return igsa_fail(c, "%s: %ld bytes, not %d-byte records", nm, l, cods[i].rec);
            if ((size_t)l / (size_t)cods[i].rec > 640)
                return igsa_fail(c, "%s: %ld objects (640 max)", nm, l / cods[i].rec);
            objects(c, &cods[i], d + o, (size_t)l);
        }
        igsa_jend(c);
        o += (size_t)l;
        nsec++;
    }
    igsa_jend(c);
    igsa_jint(c, "section_count", nsec);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_sod = { "sod", "SOD stage script: objects, paths, summons, war zones (E2000 shooters)", probe, parse };
