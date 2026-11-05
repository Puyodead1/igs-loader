/*
 * zl .emi particle systems (We Dance Online 2 model/, scene/).  Chunks of
 * u32 id, u32 size; containers end with id -1.  Reversed from wedo:
 * LoadEmi(char*) 0x8332D10, LoadEmi(zlParticle&) 0x83329C0, the
 * zlEmitter*::Load (0x83A07F0..0x83A1F40) and zlForce*::Load
 * (0x83A5410..0x83A6750) functions; a Mesh emitter holds a zlMesh
 * (LoadZ(zlMesh&) 0x831E700, LoadZ(zlMeshBase*) 0x831E250,
 * LoadZ_MESH_HEAD 0x831E310 / HEAD2 0x831E500, zlMeshBase::SetFVF
 * 0x830D240).  Chunk names are the engine's Z_CHUNKID_* strings.
 * .Z models / scenes and .cam are fmt_zl.c's.  wiki Format-EMI.
 */#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

enum {
    Z_MODEL = 0x10000, Z_MODEL_HEAD = 0x10001, Z_MESH = 0x10010, Z_MESH_DATA = 0x10011, Z_MESH_HEAD = 0x10013,
    Z_MESH_HEAD2 = 0x10014, Z_SKELETON = 0x10020, Z_BONE = 0x10021, Z_BONE2 = 0x10022, Z_ANIMATION = 0x10030,
    Z_TRACK = 0x10031, Z_TRACK_POS = 0x10032, Z_TRACK_ROT = 0x10033, Z_TRACK_SCALE = 0x10034, Z_ANIMATION2 = 0x10035,
    Z_SKIN = 0x10040, Z_SKINMESH = 0x10041, Z_SKINMESH_BASIS = 0x10042, Z_SKINMESH_BONE = 0x10043, Z_MORPH = 0x10050,
    Z_MORPHTRACK = 0x10051, Z_MORPH_TIMES = 0x10052, Z_MORPH_KEYS = 0x10053, Z_MORPH_KEYS2 = 0x10054,
    Z_SHADOWMESH = 0x10070, Z_SHADOW_MAP = 0x10072, Z_CAMERA = 0x10200, Z_PARTICLE = 0x20000,
    Z_PARTICLE_DATA = 0x20001, Z_EMITTER = 0x20100, Z_FORCE = 0x20200, Z_SCENE = 0x100000, Z_OCT = 0x100100,
    Z_STOCK = 0x100200, Z_STOCK_NAMES = 0x100202, Z_POLYMESH = 0x100203, Z_GEOMMOV_REF = 0x100204,
    Z_TRIMESH = 0x100205, Z_GEOMMOV = 0x100300,
};

/* a growing text buffer (the OBJ outputs) */
typedef struct sb { char *p; size_t n, cap; } sb;

static void sbf(sb *b, const char *fmt, ...)
{
    va_list ap;
    int k;

    if (!b)
        return;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0)
            return;
        if (b->p && b->n + (size_t)k < b->cap) {
            b->n += (size_t)k;
            return;
        }
        {
            size_t nc = b->cap ? b->cap * 2 : 65536;
            char *q;

            while (nc < b->n + (size_t)k + 1)
                nc *= 2;
            if (!(q = realloc(b->p, nc)))
                return;
            b->p = q;
            b->cap = nc;
        }
    }
}

typedef struct zr {
    igsa_ctx *c;
    const uint8_t *d;
    size_t n, o;
    jmp_buf jb;
    sb *obj, *mtl;              /* EXTRACT only */
    size_t vbase;               /* OBJ vertex numbering */
    int meshes, bases, verts, tris;
    char mtls[256][64];
    int nmtl;
} zr;

static void zfail(zr *r, const char *fmt, ...)
{
    va_list ap;
    char m[400];

    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    igsa_fail(r->c, "%s", m);
    longjmp(r->jb, 1);
}

static void need(zr *r, size_t k, const char *what)
{
    if (k > r->n || r->o > r->n - k)
        zfail(r, "%s at 0x%zx: %zu bytes past the end", what, r->o, k);
}

static uint32_t u32(zr *r)
{
    need(r, 4, "u32");
    r->o += 4;
    return igsa_le32(r->d + r->o - 4);
}

static float f32(zr *r)
{
    need(r, 4, "float");
    r->o += 4;
    return igsa_lef(r->d + r->o - 4);
}

static const char *cstr(zr *r)
{
    const uint8_t *e;
    const char *s;

    if (r->o >= r->n || !(e = memchr(r->d + r->o, 0, r->n - r->o)))
        zfail(r, "string at 0x%zx: no NUL", r->o);
    s = (const char *)r->d + r->o;
    r->o = (size_t)(e - r->d) + 1;
    return s;
}

static void jfloats(zr *r, const char *key, int k)
{
    int i;

    igsa_jarr(r->c, key);
    for (i = 0; i < k; i++)
        igsa_jnum(r->c, NULL, f32(r));
    igsa_jend(r->c);
}

static void jstrz(zr *r, const char *key, const char *s)
{
    igsa_jbig5(r->c, key, s, strlen(s));
}

/* the chunk at r->o: id, size; returns the end of its data */
static size_t chunk(zr *r, uint32_t *id)
{
    uint32_t sz;

    *id = u32(r);
    if (*id == 0xFFFFFFFFu)
        return r->o;
    sz = u32(r);
    if (sz > r->n - r->o)
        zfail(r, "chunk 0x%x at 0x%zx: size %u past the end", *id, r->o - 8, sz);
    return r->o + sz;
}

static void at_end(zr *r, size_t end, uint32_t id)
{
    if (r->o != end)
        zfail(r, "chunk 0x%x: data ends at 0x%zx, size says 0x%zx", id, r->o, end);
}

/* ---- meshes ---------------------------------------------------------------------------- */

static const char *mtl_name(zr *r, const char *m)
{
    int i;

    if (!*m)
        m = "default";
    for (i = 0; i < r->nmtl; i++)
        if (!strcmp(r->mtls[i], m))
            return r->mtls[i];
    if (r->nmtl < 256) {
        snprintf(r->mtls[r->nmtl], sizeof(r->mtls[0]), "%s", m);
        sbf(r->mtl, "newmtl %s\nKd 1 1 1\n\n", r->mtls[r->nmtl]);
        return r->mtls[r->nmtl++];
    }
    return m;
}

/* Z_MESH_DATA (SaveZ_MESH_DATA): u32 fvf, u32 vertices, u32 indices, u32 vbytes, vertices, u32 ibytes, indices */
static void mesh_data(zr *r, const char *name, const char *mat)
{
    uint32_t fvf = u32(r), nv = u32(r), ni = u32(r), vb, ib;
    unsigned stride = 0, off_n = 0, off_t = 0, ntex = (fvf >> 8) & 15, tsz0 = 0, i, isz = fvf & 0x2000 ? 4 : 2;
    const uint8_t *vp, *ip;
    static const unsigned tsz[4] = { 8, 12, 16, 4 };

    igsa_jobj(r->c, NULL);
    igsa_jhex(r->c, "fvf", fvf);
    if (fvf & 2)
        stride += 12;
    if (fvf & 0xC)
        stride += 32;           /* blend weights, blend indices */
    if (fvf & 0x10) {
        off_n = stride;
        stride += 12;
    }
    if (fvf & 0x20)
        stride += 4;
    if (fvf & 0x40)
        stride += 4;            /* diffuse */
    if (fvf & 0x80)
        stride += 4;            /* specular */
    off_t = stride;
    for (i = 0; i < ntex; i++) {
        unsigned s = tsz[(fvf >> (16 + 2 * i)) & 3];

        if (!i)
            tsz0 = s;
        stride += s;
    }
    igsa_jbool(r->c, "position", fvf & 2);
    igsa_jbool(r->c, "blend", (fvf & 0xC) != 0);
    igsa_jbool(r->c, "normal", (fvf & 0x10) != 0);
    igsa_jbool(r->c, "diffuse", (fvf & 0x40) != 0);
    igsa_jbool(r->c, "specular", (fvf & 0x80) != 0);
    igsa_jint(r->c, "texcoords", ntex);
    igsa_jint(r->c, "stride", stride);
    igsa_jint(r->c, "index_size", isz);
    igsa_juint(r->c, "vertices", nv);
    igsa_juint(r->c, "indices", ni);
    if (!nv || !ni) {           /* the loader reads nothing more then */
        igsa_jend(r->c);
        return;
    }
    vb = u32(r);
    need(r, vb, "vertex data");
    if ((uint64_t)stride * nv != vb)
        zfail(r, "mesh %s: %u vertices of %u bytes, %u bytes of vertex data", name, nv, stride, vb);
    vp = r->d + r->o;
    r->o += vb;
    ib = u32(r);
    need(r, ib, "index data");
    if ((uint64_t)isz * ni != ib)
        zfail(r, "mesh %s: %u indices of %u bytes, %u bytes", name, ni, isz, ib);
    ip = r->d + r->o;
    r->o += ib;
    for (i = 0; i < ni; i++) {
        uint32_t x = isz == 4 ? igsa_le32(ip + 4 * i) : igsa_le16(ip + 2 * i);

        if (x >= nv)
            zfail(r, "mesh %s: index %u is %u of %u vertices", name, i, x, nv);
    }
    igsa_jend(r->c);
    r->bases++;
    r->verts += (int)nv;
    r->tris += (int)(ni / 3);
    if (r->obj && (fvf & 2)) {
        const char *m = mtl_name(r, mat);

        sbf(r->obj, "g %s_%d\nusemtl %s\n", *name ? name : "mesh", r->bases, m);
        for (i = 0; i < nv; i++) {
            const uint8_t *v = vp + (size_t)stride * i;

            sbf(r->obj, "v %g %g %g\n", igsa_lef(v), igsa_lef(v + 4), igsa_lef(v + 8));
            if (ntex && tsz0 >= 8)
                sbf(r->obj, "vt %g %g\n", igsa_lef(v + off_t), 1.0 - igsa_lef(v + off_t + 4));
            if (fvf & 0x10)
                sbf(r->obj, "vn %g %g %g\n", igsa_lef(v + off_n), igsa_lef(v + off_n + 4), igsa_lef(v + off_n + 8));
        }
        for (i = 0; i + 2 < ni; i += 3) {
            size_t f[3], k;

            for (k = 0; k < 3; k++)
                f[k] = r->vbase + 1 + (isz == 4 ? igsa_le32(ip + 4 * (i + k)) : igsa_le16(ip + 2 * (i + k)));
            if (ntex && tsz0 >= 8 && (fvf & 0x10))
                sbf(r->obj, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", f[0], f[0], f[0], f[1], f[1], f[1], f[2], f[2], f[2]);
            else if (ntex && tsz0 >= 8)
                sbf(r->obj, "f %zu/%zu %zu/%zu %zu/%zu\n", f[0], f[0], f[1], f[1], f[2], f[2]);
            else if (fvf & 0x10)
                sbf(r->obj, "f %zu//%zu %zu//%zu %zu//%zu\n", f[0], f[0], f[1], f[1], f[2], f[2]);
            else
                sbf(r->obj, "f %zu %zu %zu\n", f[0], f[1], f[2]);
        }
        r->vbase += nv;
    }
}

/* Z_MESH_HEAD (LoadZ_MESH_HEAD): name, material, u32 id, u32 parent, u32 parts, vec3 min, vec3 max; HEAD2 + 16 bytes */
static void mesh_head(zr *r, int two, char *name, char *mat)
{
    const char *s = cstr(r);

    snprintf(name, 128, "%s", s);
    jstrz(r, "name", s);
    s = cstr(r);
    snprintf(mat, 128, "%s", s);
    jstrz(r, "material", s);
    igsa_juint(r->c, "id", u32(r));
    igsa_jint(r->c, "parent", (int32_t)u32(r));
    igsa_juint(r->c, "parts", u32(r));
    jfloats(r, "min", 3);
    jfloats(r, "max", 3);
    if (two) {
        jfloats(r, "extra", 3);
        igsa_jhex(r->c, "flags", u32(r));   /* zlMesh+4 */
    }
}

static void skin_lists(zr *r, uint32_t id)
{
    uint32_t n = u32(r), i, k, j;

    igsa_jobj(r->c, NULL);
    igsa_jstr(r->c, "chunk", id == Z_SKINMESH ? "SKINMESH" : id == Z_SKINMESH_BASIS ? "SKINMESH_BASIS" : "SKINMESH_BONE");
    igsa_juint(r->c, "count", n);
    if (n > r->n)
        zfail(r, "skin group: %u entries", n);
    igsa_jarr(r->c, id == Z_SKINMESH_BONE ? "bones" : "vertices");
    for (i = 0; i < n; i++) {
        igsa_jobj(r->c, NULL);
        if (id == Z_SKINMESH_BONE) {    /* zlBoneInfluences: pos, normal, tangent, binormal, u32 n, {u32 vertex, f weight} */
            jfloats(r, "pos", 3);
            jfloats(r, "normal", 3);
            jfloats(r, "tangent", 3);
            jfloats(r, "binormal", 3);
            k = u32(r);
            need(r, (size_t)k * 8, "bone influences");
            igsa_jarr(r->c, "influences");
            for (j = 0; j < k; j++) {
                igsa_jarr(r->c, NULL);
                igsa_juint(r->c, NULL, u32(r));
                igsa_jnum(r->c, NULL, f32(r));
                igsa_jend(r->c);
            }
            igsa_jend(r->c);
        } else {                /* defSkinInfluence: u32 bone, f weight, pos, normal[, tangent, binormal] */
            size_t sz = id == Z_SKINMESH ? 32 : 56;

            k = u32(r);
            need(r, k * sz, "skin influences");
            igsa_jarr(r->c, "influences");
            for (j = 0; j < k; j++) {
                const uint8_t *q = r->d + r->o + j * sz;

                igsa_jarr(r->c, NULL);
                igsa_juint(r->c, NULL, igsa_le32(q));
                igsa_jnum(r->c, NULL, igsa_lef(q + 4));
                igsa_jend(r->c);
            }
            igsa_jend(r->c);
            r->o += k * sz;
        }
        igsa_jend(r->c);
    }
    igsa_jend(r->c);
    igsa_jend(r->c);
}

static void morph_track(zr *r)
{
    uint32_t frames = u32(r), id;
    size_t end;

    igsa_jobj(r->c, NULL);
    igsa_juint(r->c, "frames", frames);
    igsa_jarr(r->c, "chunks");
    for (;;) {
        end = chunk(r, &id);
        if (id == 0xFFFFFFFFu)
            break;
        igsa_jobj(r->c, NULL);
        if (id == Z_MORPH_TIMES) {
            igsa_jstr(r->c, "chunk", "MORPH_TIMES");
            igsa_juint(r->c, "bytes", end - r->o);
            r->o = end;
        } else if (id == Z_MORPH_KEYS || id == Z_MORPH_KEYS2) {
            uint32_t k = u32(r);
            size_t sz = id == Z_MORPH_KEYS ? 24 : 48;

            igsa_jstr(r->c, "chunk", id == Z_MORPH_KEYS ? "MORPH_KEYS" : "MORPH_KEYS2");
            igsa_juint(r->c, "vertices", k);
            if ((uint64_t)k * sz * frames > r->n)
                zfail(r, "morph keys: %u x %u", k, frames);
            need(r, (size_t)k * sz * frames, "morph keys");
            r->o += (size_t)k * sz * frames;
        } else
            zfail(r, "morph track: unknown chunk 0x%x at 0x%zx", id, r->o - 8);
        at_end(r, end, id);
        igsa_jend(r->c);
    }
    igsa_jend(r->c);
    igsa_jend(r->c);
}

/* Z_MESH, Z_SKIN, Z_MORPH, Z_SHADOWMESH: sub-chunks to -1 */
static void mesh(zr *r, uint32_t kind, size_t cend)
{
    char name[128] = "", mat[128] = "";
    uint32_t id;
    size_t end;

    igsa_jstr(r->c, "kind", kind == Z_MESH ? "mesh" : kind == Z_SKIN ? "skin" : kind == Z_MORPH ? "morph" : "shadow");
    igsa_jarr(r->c, "chunks");
    for (;;) {
        if (r->o >= cend)
            zfail(r, "mesh at 0x%zx: no end mark", r->o);
        end = chunk(r, &id);
        if (id == 0xFFFFFFFFu)
            break;
        if (id == Z_MESH_DATA) {
            mesh_data(r, name, mat);
        } else if (id == Z_MESH_HEAD || id == Z_MESH_HEAD2) {
            if (kind == Z_SHADOWMESH)
                zfail(r, "shadow mesh with a head");
            igsa_jobj(r->c, NULL);
            igsa_jstr(r->c, "chunk", id == Z_MESH_HEAD ? "MESH_HEAD" : "MESH_HEAD2");
            mesh_head(r, id == Z_MESH_HEAD2, name, mat);
            igsa_jend(r->c);
        } else if (kind == Z_SKIN && (id == Z_SKINMESH || id == Z_SKINMESH_BASIS || id == Z_SKINMESH_BONE)) {
            skin_lists(r, id);
        } else if (kind == Z_MORPH && id == Z_MORPHTRACK) {
            morph_track(r);
        } else if (kind == Z_SHADOWMESH && id == Z_SHADOW_MAP) {
            uint32_t n = u32(r), i, k;

            igsa_jobj(r->c, NULL);
            igsa_jstr(r->c, "chunk", "SHADOW_MAP");
            igsa_juint(r->c, "vertices", n);
            for (i = 0; i < n; i++) {
                k = u32(r);
                need(r, (size_t)k * 4, "shadow map");
                r->o += (size_t)k * 4;
                k = u32(r);
                need(r, (size_t)k * 16, "shadow map");
                r->o += (size_t)k * 16;
            }
            igsa_jend(r->c);
        } else
            zfail(r, "mesh: unknown chunk 0x%x at 0x%zx", id, r->o - 8);
        at_end(r, end, id);
    }
    igsa_jend(r->c);
    r->meshes++;
}

static const struct { const char *name; int f; } emitters[] = {   /* zlEmitter*::Load: floats after the name */
    { "Line", 3 }, { "Rect", 6 }, { "Plane", 8 }, { "Disk", 5 }, { "Sphere", 2 }, { "Points", -1 }, { "Mesh", -2 },
}, forces[] = {                                                     /* zlForce*::Load */
    { "Acceler", 3 }, { "Push", 4 }, { "Gravity", 4 }, { "Twirl", 7 }, { "Rotate", 7 }, { "Wave", 5 },
};

static void particle(zr *r, size_t cend)
{
    uint32_t id;
    size_t end, i;

    igsa_jarr(r->c, "chunks");
    for (;;) {
        if (r->o >= cend)
            zfail(r, "particle at 0x%zx: no end mark", r->o);
        end = chunk(r, &id);
        if (id == 0xFFFFFFFFu)
            break;
        igsa_jobj(r->c, NULL);
        if (id == Z_PARTICLE_DATA) {    /* vec3, vec3, u32, u32, u32 max, texture, 140 bytes (16 colours at +68) */
            igsa_jstr(r->c, "chunk", "PARTICLE_DATA");
            jfloats(r, "v0", 3);
            jfloats(r, "v1", 3);
            igsa_jint(r->c, "a", (int32_t)u32(r));
            igsa_jint(r->c, "b", (int32_t)u32(r));
            igsa_jint(r->c, "count", (int32_t)u32(r));
            jstrz(r, "texture", cstr(r));
            jfloats(r, "params", 17);
            igsa_jarr(r->c, "colors");
            for (i = 0; i < 16; i++)
                igsa_jhex(r->c, NULL, u32(r));
            igsa_jend(r->c);
            jfloats(r, "params2", 2);
        } else if (id == Z_EMITTER || id == Z_FORCE) {
            const char *t = cstr(r);
            int f = -3;
            size_t ne = id == Z_EMITTER ? sizeof(emitters) / sizeof(emitters[0]) : sizeof(forces) / sizeof(forces[0]);

            igsa_jstr(r->c, "chunk", id == Z_EMITTER ? "EMITTER" : "FORCE");
            igsa_jstr(r->c, "type", t);
            for (i = 0; i < ne; i++)
                if (!strcasecmp(t, id == Z_EMITTER ? emitters[i].name : forces[i].name))
                    f = id == Z_EMITTER ? emitters[i].f : forces[i].f;
            if (f == -3)
                zfail(r, "%s type '%s' unknown", id == Z_EMITTER ? "emitter" : "force", t);
            if (f >= 0)
                jfloats(r, "params", f);
            else if (f == -1) {
                uint32_t k = u32(r);

                if ((uint64_t)k * 12 > r->n)
                    zfail(r, "points: %u", k);
                jfloats(r, "points", (int)k * 3);
            } else {
                need(r, 8, "mesh emitter");
                r->o += 8;
                igsa_jobj(r->c, "mesh");
                mesh(r, Z_MESH, end);
                igsa_jend(r->c);
            }
        } else if (id == Z_PARTICLE) {
            igsa_jstr(r->c, "chunk", "PARTICLE");
            particle(r, end);
        } else
            zfail(r, "particle: unknown chunk 0x%x at 0x%zx", id, r->o - 8);
        igsa_jend(r->c);
        at_end(r, end, id);
    }
    igsa_jend(r->c);
}

/* the top chunk is Z_PARTICLE (LoadEmi(char*) skips anything before it) */
static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 16 && igsa_le32(d) == Z_PARTICLE && igsa_le32(d + 4) <= n - 8 && igsa_le32(d + 8) == Z_PARTICLE_DATA ? 90 : 0;
}

static int parse(igsa_ctx *c)
{
    zr *r = calloc(1, sizeof(zr));
    sb obj = { 0 }, mtl = { 0 };
    int ret = 0, top = 0;
    char base[256], nm[300];
    const char *b;

    if (!r)
        return igsa_fail(c, "out of memory");
    r->c = c;
    r->d = c->d;
    r->n = c->n;
    if (c->mode == IGSA_EXTRACT) {
        r->obj = &obj;
        r->mtl = &mtl;
    }
    b = c->path ? c->path : "emi";
    if (strrchr(b, '/'))
        b = strrchr(b, '/') + 1;
    if (strrchr(b, '\\'))
        b = strrchr(b, '\\') + 1;
    snprintf(base, sizeof(base), "%s", b);
    sbf(r->obj, "# %s\nmtllib %s.mtl\n", base, base);
    if (setjmp(r->jb)) {
        ret = -1;
        goto out;
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "zl EMI");
    igsa_jarr(c, "chunks");
    while (r->o < r->n) {
        uint32_t id;
        size_t end = chunk(r, &id);

        if (id == 0xFFFFFFFFu)
            break;
        igsa_jobj(c, NULL);
        if (id != Z_PARTICLE)
            zfail(r, "unknown chunk 0x%x at 0x%zx", id, r->o - 8);
        igsa_jstr(c, "chunk", "PARTICLE");
        particle(r, end);
        igsa_jend(c);
        at_end(r, end, id);
        top++;
    }
    igsa_jend(c);
    if (!top)
        zfail(r, "no particle");
    igsa_jint(c, "emitter_meshes", r->meshes);
    igsa_juint(c, "trailing", r->n - r->o);
    igsa_jend(c);
    if (c->mode == IGSA_LIST)
        fprintf(c->out, "%-48s %-5s %d particle systems\n", base, "emi", top);
    if (r->bases && r->obj) {
        snprintf(nm, sizeof(nm), "%s.obj", base);
        if (igsa_emit_raw(c, nm, obj.p, obj.n))
            ret = -1;
        snprintf(nm, sizeof(nm), "%s.mtl", base);
        if (!ret && mtl.p && igsa_emit_raw(c, nm, mtl.p, mtl.n))
            ret = -1;
    } else
        c->entries++;
out:
    free(obj.p);
    free(mtl.p);
    free(r);
    return ret;
}

const igsa_format igsa_fmt_wedoemi = { "wedoemi", "zl .emi particle systems (We Dance): emitters, forces, emitter meshes (OBJ)",
                                       probe, parse };