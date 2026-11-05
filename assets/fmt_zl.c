/*
 * The zl engine's binary chunk files (Speed Driver 2; the same code as Fish
 * Hunter 4 M107CN, whose symbols name it): models (*.z, LoadZ(zlModel&)),
 * scenes (*.map.z, LoadZ(zlScene*)), camera paths (*.cam, LoadCam).
 * In sd2: model sub_81AE380, mesh sub_81A1CF0 (heads sub_81A1A00, sub_81A1700),
 * skeleton sub_819D9C0, track sub_81A0800, animations sub_81A7AB0, morph
 * sub_81ADD50, shadow sub_81ADFF0, scene sub_8207E30 (stock sub_8206C50,
 * objects sub_8207B60, octree sub_82052B0), LoadCam sub_817B850 / sub_817B620.
 * wiki Format-ZLZ, Format-ZLCAM.
 *
 * A chunk is u32 id, u32 size, size bytes; containers end their children with
 * u32 0xFFFFFFFF (inside size).  Models and scenes also go out as Wavefront
 * OBJ + MTL (every mesh and LOD), and as JSON with every vertex and index.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "igsa.h"
#include "fmt_zl.h"

const uint8_t *(*igsa_zl_sibling)(const char *name, size_t *n);

enum {
    Z_MODEL = 0x10000, Z_MODEL_HEAD = 0x10001,
    Z_MESH = 0x10010, Z_MESH_DATA = 0x10011, Z_MESH_HEAD = 0x10013, Z_MESH_HEAD2 = 0x10014,
    Z_SKELETON = 0x10020, Z_BONE = 0x10021, Z_BONE2 = 0x10022,
    Z_ANIMATION = 0x10030, Z_TRACK = 0x10031, Z_TRACK_POS = 0x10032, Z_TRACK_ROT = 0x10033,
    Z_TRACK_SCALE = 0x10034, Z_ANIMATION2 = 0x10035,
    Z_SKIN = 0x10040, Z_SKINMESH = 0x10041, Z_SKINMESH_BASIS = 0x10042, Z_SKINMESH_BONE = 0x10043,
    Z_MORPH = 0x10050, Z_MORPHTRACK = 0x10051, Z_MORPH_TIME = 0x10052, Z_MORPH_VN = 0x10053,
    Z_MORPH_VNT = 0x10054,
    Z_SHADOW = 0x10070, Z_SHADOW_MAP = 0x10072,
    Z_CAMERA = 0x10200,
    Z_SCENE = 0x100000, Z_OCTREE = 0x100100, Z_STOCK = 0x100300, Z_STOCK_NAMES = 0x100302,
    Z_POLYMESH = 0x100303, Z_GEOMMOV_DEF = 0x100304, Z_TRIMESH = 0x100305, Z_GEOMMOV = 0x100400,
};

typedef struct rd { igsa_ctx *c; const uint8_t *d; size_t o, end; } rd;

static int need(rd *r, size_t k, const char *what)
{
    if (r->o + k > r->end || r->o + k < r->o)
        return igsa_fail(r->c, "%s at 0x%zx: %zu bytes past the end (0x%zx)", what, r->o, r->o + k - r->end, r->end);
    return 0;
}

static uint32_t u32(rd *r) { uint32_t v = igsa_le32(r->d + r->o); r->o += 4; return v; }
static float f32(rd *r) { float v = igsa_lef(r->d + r->o); r->o += 4; return v; }

static const char *cstr(rd *r, const char *what)
{
    const uint8_t *e = memchr(r->d + r->o, 0, r->end - r->o);
    const char *s = (const char *)r->d + r->o;

    if (!e) {
        igsa_fail(r->c, "%s at 0x%zx: no NUL", what, r->o);
        return NULL;
    }
    r->o = (size_t)(e - r->d) + 1;
    return s;
}

static void jvec(igsa_ctx *c, const char *key, rd *r, int n)
{
    int i;

    igsa_jarr(c, key);
    for (i = 0; i < n; i++)
        igsa_jnum(c, NULL, f32(r));
    igsa_jend(c);
}

/* ---- meshes, kept for the OBJ / JSON entries -------------------------------------------------- */

typedef struct mdata {
    uint32_t fvf, nv, ni, stride, isize;
    int off_n, off_psize, off_diffuse, off_specular, ntex, off_tex[4], tex_dim[4];
    const uint8_t *v, *ix;
} mdata;

typedef struct mesh {
    const char *name, *tex, *kind;
    int nd;
    mdata d[16];
} mesh;

typedef struct mlist { mesh *m; int n, cap; } mlist;

static mesh *mesh_new(igsa_ctx *c, mlist *l, const char *kind)
{
    if (l->n == l->cap) {
        mesh *t = realloc(l->m, sizeof(mesh) * (size_t)(l->cap = l->cap ? l->cap * 2 : 16));

        if (!t) {
            igsa_fail(c, "out of memory");
            return NULL;
        }
        l->m = t;
    }
    memset(&l->m[l->n], 0, sizeof(mesh));
    l->m[l->n].kind = kind;
    return &l->m[l->n++];
}

/* zlMeshBase::SetFVF: the vertex layout */
static int fvf_layout(mdata *m)
{
    uint32_t f = m->fvf;
    int s = 0, i;

    m->off_n = m->off_psize = m->off_diffuse = m->off_specular = -1;
    if (f & 2)
        s += 12;
    if (f & 0x10) { m->off_n = s; s += 12; }
    if (f & 0x20) { m->off_psize = s; s += 4; }
    if (f & 0x40) { m->off_diffuse = s; s += 4; }
    if (f & 0x80) { m->off_specular = s; s += 4; }
    m->ntex = f >> 8 & 15;
    if (m->ntex > 4)
        return -1;
    for (i = 0; i < m->ntex; i++) {
        static const int dim[4] = { 2, 3, 4, 1 };

        m->off_tex[i] = s;
        m->tex_dim[i] = dim[f >> (16 + 2 * i) & 3];
        s += 4 * m->tex_dim[i];
    }
    m->stride = (uint32_t)s;
    m->isize = f & 0x2000 ? 4 : 2;
    return 0;
}

static uint32_t index_at(const mdata *m, uint32_t i)
{
    return m->isize == 4 ? igsa_le32(m->ix + 4 * i) : igsa_le16(m->ix + 2 * i);
}

/* Z_MESH_DATA (LoadZ(zlMeshBase*)): fvf, vertices, indices, u32 bytes + vertex buffer, u32 bytes + index buffer */
static int mesh_data(rd *r, mdata *m, int dump)
{
    igsa_ctx *c = r->c;
    uint32_t vb, ib, i;

    memset(m, 0, sizeof(*m));
    if (need(r, 12, "mesh data"))
        return -1;
    m->fvf = u32(r);
    m->nv = u32(r);
    m->ni = u32(r);
    if (fvf_layout(m))
        return igsa_fail(c, "mesh data at 0x%zx: FVF 0x%x has %d texture sets", r->o - 12, m->fvf, m->ntex);
    if (dump) {
        igsa_jhex(c, "fvf", m->fvf);
        igsa_juint(c, "vertices", m->nv);
        igsa_juint(c, "indices", m->ni);
        igsa_juint(c, "vertex_size", m->stride);
        igsa_juint(c, "index_size", m->isize);
    }
    if (!m->nv || !m->ni)       /* the loader reads no buffers then */
        return 0;
    if (need(r, 4, "vertex buffer size"))
        return -1;
    vb = u32(r);
    if (vb != m->nv * m->stride || need(r, vb, "vertex buffer"))
        return igsa_fail(c, "mesh data at 0x%zx: vertex buffer %u bytes, %u x %u expected", r->o - 4, vb, m->nv,
                         m->stride);
    m->v = r->d + r->o;
    r->o += vb;
    if (need(r, 4, "index buffer size"))
        return -1;
    ib = u32(r);
    if (ib != m->ni * m->isize || need(r, ib, "index buffer"))
        return igsa_fail(c, "mesh data at 0x%zx: index buffer %u bytes, %u x %u expected", r->o - 4, ib, m->ni, m->isize);
    m->ix = r->d + r->o;
    r->o += ib;
    for (i = 0; i < m->ni; i++)
        if (index_at(m, i) >= m->nv)
            return igsa_fail(c, "mesh data: index %u = %u, %u vertices", i, index_at(m, i), m->nv);
    if (m->ni % 3)
        return igsa_fail(c, "mesh data: %u indices, not triangles", m->ni);
    return 0;
}

/* Z_MESH_HEAD / HEAD2 (LoadZ_MESH_HEAD, LoadZ_MESH_HEAD2) */
static int mesh_head(rd *r, mesh *m, int two)
{
    igsa_ctx *c = r->c;

    if (!(m->name = cstr(r, "mesh name")) || !(m->tex = cstr(r, "mesh shader")) || need(r, two ? 40 : 36, "mesh head"))
        return -1;
    igsa_jgbk(c, "name", m->name, strlen(m->name));
    igsa_jgbk(c, "shader", m->tex, strlen(m->tex));
    igsa_juint(c, "index", u32(r));
    igsa_juint(c, "field_44", u32(r));
    igsa_juint(c, "lods", u32(r));
    jvec(c, "min", r, 3);
    jvec(c, "max", r, 3);
    if (two)
        igsa_jhex(c, "flags", u32(r));
    return 0;
}

enum { LIST_END = -1, LIST_FILE = -2 };

static int chunk_list(rd *r, mlist *ml, int mode, mesh *cur);

/* ---- one chunk ------------------------------------------------------------------------------------ */

static int track_body(rd *r)
{
    igsa_ctx *c = r->c;
    uint32_t n, i;

    if (need(r, 8, "track"))
        return -1;
    igsa_juint(c, "bone", u32(r));
    n = u32(r);
    if (need(r, (size_t)n * 4, "track times"))
        return -1;
    igsa_jarr(c, "times");
    for (i = 0; i < n; i++)
        igsa_jnum(c, NULL, f32(r));
    igsa_jend(c);
    for (;;) {
        uint32_t id, sz;
        size_t end;
        const char *k;
        int w;

        if (need(r, 4, "track key chunk"))
            return -1;
        if ((id = u32(r)) == 0xFFFFFFFF)
            break;
        if (need(r, 4, "track key chunk size"))
            return -1;
        sz = u32(r);
        end = r->o + sz;
        if (need(r, sz, "track key chunk"))
            return -1;
        k = id == Z_TRACK_POS ? "positions" : id == Z_TRACK_ROT ? "rotations" : id == Z_TRACK_SCALE ? "scales" : NULL;
        w = id == Z_TRACK_ROT ? 4 : 3;
        if (!k)
            return igsa_fail(c, "track: chunk 0x%x", id);
        if (sz != n * 4u * w)
            return igsa_fail(c, "track %s: %u bytes for %u keys", k, sz, n);
        igsa_jarr(c, k);
        for (i = 0; i < n; i++)
            jvec(c, NULL, r, w);
        igsa_jend(c);
        r->o = end;
    }
    return 0;
}

static int chunk(rd *r, uint32_t id, mlist *ml, mesh *cur)
{
    igsa_ctx *c = r->c;
    uint32_t n, i, k;
    mesh *m;

    switch (id) {
    case Z_MODEL:
    case Z_SCENE:
    case Z_STOCK:
        return chunk_list(r, ml, LIST_END, NULL);
    case Z_MODEL_HEAD: {
        const char *nm = cstr(r, "model name");
        static const char *const cnt[] = { "animations", "meshes", "meshes_2", "morph_meshes", "skin_meshes" };

        if (!nm || need(r, 20, "model head"))
            return -1;
        igsa_jgbk(c, "name", nm, strlen(nm));
        for (i = 0; i < 5; i++)
            igsa_juint(c, cnt[i], u32(r));
        return 0;
    }
    case Z_MESH:
    case Z_SKIN:
    case Z_MORPH:
        if (!(m = mesh_new(c, ml, id == Z_MESH ? "mesh" : id == Z_SKIN ? "skin" : "morph")))
            return -1;
        return chunk_list(r, ml, LIST_END, m);
    case Z_SHADOW:
        if (!(m = mesh_new(c, ml, "shadow")))
            return -1;
        m->name = m->tex = "shadow";
        return chunk_list(r, ml, LIST_END, m);
    case Z_MESH_HEAD:
    case Z_MESH_HEAD2:
        if (!cur)
            return igsa_fail(c, "mesh head outside a mesh");
        return mesh_head(r, cur, id == Z_MESH_HEAD2);
    case Z_MESH_DATA:
        if (!cur)
            return igsa_fail(c, "mesh data outside a mesh");
        if (cur->nd == 16)
            return igsa_fail(c, "mesh %s: more than 16 LODs", cur->name ? cur->name : "");
        if (mesh_data(r, &cur->d[cur->nd], 1))
            return -1;
        igsa_jint(c, "lod", cur->nd++);
        return 0;
    case Z_SKELETON:
        if (need(r, 4, "skeleton"))
            return -1;
        igsa_juint(c, "bones", n = u32(r));
        return chunk_list(r, ml, (int)n, NULL);   /* n bone chunks, no end mark */
    case Z_BONE:
    case Z_BONE2: {
        const char *nm = cstr(r, "bone name");

        if (!nm || need(r, id == Z_BONE2 ? 112 : 48, "bone"))
            return -1;
        igsa_jgbk(c, "name", nm, strlen(nm));
        igsa_jint(c, "id", (int32_t)u32(r));
        igsa_jint(c, "parent", (int32_t)u32(r));
        jvec(c, "position", r, 3);
        jvec(c, "rotation", r, 4);
        jvec(c, "scale", r, 3);
        if (id == Z_BONE2)
            jvec(c, "matrix", r, 16);
        return 0;
    }
    case Z_ANIMATION2: {
        const char *nm = cstr(r, "animation name");

        if (!nm)
            return -1;
        igsa_jgbk(c, "name", nm, strlen(nm));
    }   /* fall through */
    case Z_ANIMATION:
        if (need(r, 8, "animation"))
            return -1;
        igsa_jnum(c, "length", f32(r));
        igsa_juint(c, "tracks", n = u32(r));
        return chunk_list(r, ml, (int)n, NULL);   /* n track chunks, no end mark */
    case Z_TRACK:
        return track_body(r);
    case Z_SKINMESH:
    case Z_SKINMESH_BASIS:
        if (need(r, 4, "skin groups"))
            return -1;
        n = u32(r);
        igsa_jarr(c, "groups");
        for (i = 0; i < n; i++) {
            uint32_t rec = id == Z_SKINMESH ? 32 : 56;

            if (need(r, 4, "skin group") || (k = u32(r), need(r, (size_t)k * rec, "skin influences")))
                return -1;
            igsa_jarr(c, NULL);
            while (k--) {       /* defSkinInfluence */
                igsa_jobj(c, NULL);
                igsa_jint(c, "bone", (int32_t)u32(r));
                igsa_jnum(c, "weight", f32(r));
                jvec(c, "position", r, 3);
                jvec(c, "normal", r, 3);
                if (rec == 56) {
                    jvec(c, "tangent", r, 3);
                    jvec(c, "binormal", r, 3);
                }
                igsa_jend(c);
            }
            igsa_jend(c);
        }
        igsa_jend(c);
        return 0;
    case Z_SKINMESH_BONE:
        if (need(r, 4, "bone influences"))
            return -1;
        n = u32(r);
        igsa_jarr(c, "bone_groups");
        for (i = 0; i < n; i++) {
            if (need(r, 52, "bone influence"))
                return -1;
            igsa_jobj(c, NULL);
            jvec(c, "position", r, 3);   /* a vertex: its bind pose, the bones weighing on it */
            jvec(c, "normal", r, 3);
            jvec(c, "tangent", r, 3);
            jvec(c, "binormal", r, 3);
            k = u32(r);
            if (need(r, (size_t)k * 8, "bone weights"))
                return -1;
            igsa_jarr(c, "weights");
            while (k--) {
                igsa_jobj(c, NULL);
                igsa_jint(c, "bone", (int32_t)u32(r));
                igsa_jnum(c, "weight", f32(r));
                igsa_jend(c);
            }
            igsa_jend(c);
            igsa_jend(c);
        }
        igsa_jend(c);
        return 0;
    case Z_MORPHTRACK:
        if (need(r, 4, "morph track"))
            return -1;
        igsa_juint(c, "frames", n = u32(r));
        for (;;) {
            uint32_t sid, sz, nv, f, rec;
            size_t end;

            if (need(r, 4, "morph chunk"))
                return -1;
            if ((sid = u32(r)) == 0xFFFFFFFF)
                break;
            if (need(r, 4, "morph chunk size") || (sz = u32(r), need(r, sz, "morph chunk")))
                return -1;
            end = r->o + sz;
            if (sid == Z_MORPH_TIME) {
                if (sz != n * 4)
                    return igsa_fail(c, "morph times: %u bytes for %u frames", sz, n);
                igsa_jarr(c, "times");
                for (f = 0; f < n; f++)
                    igsa_jnum(c, NULL, f32(r));
                igsa_jend(c);
            } else if (sid == Z_MORPH_VN || sid == Z_MORPH_VNT) {
                rec = sid == Z_MORPH_VN ? 24 : 48;
                if (need(r, 4, "morph vertices"))
                    return -1;
                nv = u32(r);
                if (sz != 4 + (uint64_t)n * nv * rec)
                    return igsa_fail(c, "morph frames: %u bytes for %u x %u", sz, n, nv);
                igsa_juint(c, "vertices", nv);
                igsa_jarr(c, sid == Z_MORPH_VN ? "frames_position_normal" : "frames_position_normal_tangent");
                for (f = 0; f < n; f++) {
                    igsa_jarr(c, NULL);
                    for (i = 0; i < nv * rec / 4; i++)
                        igsa_jnum(c, NULL, f32(r));
                    igsa_jend(c);
                }
                igsa_jend(c);
            } else
                return igsa_fail(c, "morph track: chunk 0x%x", sid);
            r->o = end;
        }
        return 0;
    case Z_SHADOW_MAP:
        if (need(r, 4, "shadow map"))
            return -1;
        n = u32(r);
        igsa_jarr(c, "vertex_map");
        for (i = 0; i < n; i++) {
            igsa_jobj(c, NULL);
            if (need(r, 4, "shadow map") || (k = u32(r), need(r, (size_t)k * 4, "shadow map indices")))
                return -1;
            igsa_jarr(c, "indices");
            while (k--)
                igsa_jint(c, NULL, (int32_t)u32(r));
            igsa_jend(c);
            if (need(r, 4, "shadow map") || (k = u32(r), need(r, (size_t)k * 16, "shadow map planes")))
                return -1;
            igsa_jarr(c, "planes");
            while (k--)
                jvec(c, NULL, r, 4);
            igsa_jend(c);
            igsa_jend(c);
        }
        igsa_jend(c);
        return 0;
    case Z_OCTREE:
        if (need(r, 32, "octree"))
            return -1;
        igsa_juint(c, "depth", u32(r));
        igsa_juint(c, "nodes", u32(r));
        jvec(c, "min", r, 3);
        jvec(c, "max", r, 3);
        igsa_jarr(c, "node_chunks");    /* the loader skips them by size */
        for (;;) {
            uint32_t sid, sz;

            if (need(r, 4, "octree node"))
                return -1;
            if ((sid = u32(r)) == 0xFFFFFFFF)
                break;
            if (need(r, 4, "octree node size") || (sz = u32(r), need(r, sz, "octree node")))
                return -1;
            igsa_jobj(c, NULL);
            igsa_jhex(c, "id", sid);
            igsa_jbytes(c, "data", r->d + r->o, sz);
            igsa_jend(c);
            r->o += sz;
        }
        igsa_jend(c);
        return 0;
    case Z_STOCK_NAMES:
        if (need(r, 4, "names"))
            return -1;
        n = u32(r);
        igsa_jarr(c, "names");
        for (i = 0; i < n; i++) {
            const char *s = cstr(r, "name");

            if (!s)
                return -1;
            igsa_jgbk(c, NULL, s, strlen(s));
        }
        igsa_jend(c);
        return 0;
    case Z_POLYMESH:
        if (need(r, 28, "poly mesh"))
            return -1;
        n = u32(r);
        jvec(c, "min", r, 3);
        jvec(c, "max", r, 3);
        igsa_jarr(c, "polys");
        for (i = 0; i < n; i++) {
            if (need(r, 20, "poly"))
                return -1;
            igsa_jobj(c, NULL);
            jvec(c, "plane", r, 4);
            k = u32(r);
            if (need(r, (size_t)k * 28 + 24, "poly vertices"))
                return -1;
            igsa_jarr(c, "vertices");
            for (uint32_t j = 0; j < k; j++)
                jvec(c, NULL, r, 3);
            igsa_jend(c);
            igsa_jarr(c, "edge_planes");
            for (uint32_t j = 0; j < k; j++)
                jvec(c, NULL, r, 4);
            igsa_jend(c);
            jvec(c, "min", r, 3);
            jvec(c, "max", r, 3);
            igsa_jend(c);
        }
        igsa_jend(c);
        return 0;
    case Z_GEOMMOV_DEF:
        if (need(r, 4, "geommov defs"))
            return -1;
        n = u32(r);
        if (need(r, (size_t)n * 16, "geommov defs"))
            return -1;
        igsa_jarr(c, "defs");
        for (i = 0; i < n; i++) {
            igsa_jobj(c, NULL);
            igsa_jint(c, "type", (int32_t)u32(r));
            igsa_jint(c, "kind", (int32_t)u32(r));
            igsa_jint(c, "object", (int32_t)u32(r));
            igsa_jint(c, "target", (int32_t)u32(r));
            igsa_jend(c);
        }
        igsa_jend(c);
        return 0;
    case Z_TRIMESH:
        if (need(r, 28, "tri mesh"))
            return -1;
        jvec(c, "min", r, 3);
        jvec(c, "max", r, 3);
        n = u32(r);
        if (need(r, (size_t)n * 12 + 4, "tri mesh vertices"))
            return -1;
        igsa_jarr(c, "vertices");
        for (i = 0; i < n; i++)
            jvec(c, NULL, r, 3);
        igsa_jend(c);
        k = u32(r);
        if (need(r, (size_t)k * 4 + 4, "tri mesh indices"))
            return -1;
        igsa_jarr(c, "indices");
        for (i = 0; i < k; i++) {
            uint32_t x = u32(r);

            if (x >= n)
                return igsa_fail(c, "tri mesh: index %u = %u, %u vertices", i, x, n);
            igsa_juint(c, NULL, x);
        }
        igsa_jend(c);
        n = u32(r);
        if (need(r, (size_t)n * 12, "tri mesh normals"))
            return -1;
        igsa_jarr(c, "normals");
        for (i = 0; i < n; i++)
            jvec(c, NULL, r, 3);
        igsa_jend(c);
        return 0;
    case Z_GEOMMOV:
        if (need(r, 4, "geommovs"))
            return -1;
        n = u32(r);
        igsa_jarr(c, "objects");
        for (i = 0; i < n; i++) {
            if (need(r, 16 + 26 * 4 + 24, "geommov"))
                return -1;
            igsa_jobj(c, NULL);
            igsa_jint(c, "id", (int32_t)u32(r));
            igsa_jint(c, "type", (int32_t)u32(r));
            igsa_jint(c, "object", (int32_t)u32(r));
            igsa_jint(c, "shader", (int32_t)u32(r));
            jvec(c, "position", r, 3);
            jvec(c, "rotation", r, 4);
            jvec(c, "scale", r, 3);
            jvec(c, "matrix", r, 16);
            jvec(c, "min", r, 3);
            jvec(c, "max", r, 3);
            igsa_jend(c);
        }
        igsa_jend(c);
        return 0;
    case Z_CAMERA: {
        const char *nm = cstr(r, "camera name");

        if (!nm || need(r, 4, "camera keys"))
            return -1;
        igsa_jgbk(c, "name", nm, strlen(nm));
        n = u32(r);
        if (need(r, (size_t)n * 52, "camera keys"))
            return -1;
        igsa_jarr(c, "keys");
        for (i = 0; i < n; i++) {
            igsa_jobj(c, NULL);
            igsa_jnum(c, "time", f32(r));
            igsa_jnum(c, "fov", f32(r));
            igsa_jnum(c, "near", f32(r));
            igsa_jnum(c, "far", f32(r));
            jvec(c, "eye", r, 3);
            jvec(c, "target", r, 3);
            jvec(c, "up", r, 3);
            igsa_jend(c);
        }
        igsa_jend(c);
        return 0;
    }
    }
    return 1;   /* unknown */
}

static const char *chunk_name(uint32_t id)
{
    switch (id) {
    case Z_MODEL: return "model";
    case Z_MODEL_HEAD: return "model_head";
    case Z_MESH: return "mesh";
    case Z_MESH_DATA: return "mesh_data";
    case Z_MESH_HEAD: return "mesh_head";
    case Z_MESH_HEAD2: return "mesh_head2";
    case Z_SKELETON: return "skeleton";
    case Z_BONE: return "bone";
    case Z_BONE2: return "bone2";
    case Z_ANIMATION: return "animation";
    case Z_ANIMATION2: return "animation2";
    case Z_TRACK: return "track";
    case Z_SKIN: return "skin_mesh";
    case Z_SKINMESH: return "skin_influences";
    case Z_SKINMESH_BASIS: return "skin_influences_basis";
    case Z_SKINMESH_BONE: return "skin_bones";
    case Z_MORPH: return "morph_mesh";
    case Z_MORPHTRACK: return "morph_track";
    case Z_SHADOW: return "shadow_mesh";
    case Z_SHADOW_MAP: return "shadow_vertex_map";
    case Z_CAMERA: return "camera";
    case Z_SCENE: return "scene";
    case Z_OCTREE: return "octree";
    case Z_STOCK: return "stock";
    case Z_STOCK_NAMES: return "names";
    case Z_POLYMESH: return "poly_mesh";
    case Z_GEOMMOV_DEF: return "geommov_defs";
    case Z_TRIMESH: return "tri_mesh";
    case Z_GEOMMOV: return "geommovs";
    }
    return NULL;
}

/*
 * Children of a container: up to its 0xFFFFFFFF (LIST_END), exactly n chunks
 * (the skeleton's bones, an animation's tracks), or the file's chunks
 * (LIST_FILE: up to the end, zero padding allowed).  Like the loader, a known
 * chunk's successor starts where its content ends: some writers count 4
 * bytes too many in a size (the boundary scenes' tri meshes).
 */
static int chunk_list(rd *r, mlist *ml, int mode, mesh *cur)
{
    igsa_ctx *c = r->c;
    int done = 0;

    igsa_jarr(c, "chunks");
    for (;;) {
        uint32_t id, sz;
        size_t at = r->o;
        rd s;
        int k;

        if (mode >= 0 && done == mode)
            break;
        if (mode == LIST_FILE) {
            size_t z = r->o;

            while (z < r->end && !r->d[z])
                z++;
            if (z == r->end) {
                if (r->o != r->end)
                    igsa_juint(c, "zero_padding", r->end - r->o);
                r->o = r->end;
                break;
            }
        }
        if (need(r, 4, "chunk id"))
            return -1;
        if ((id = u32(r)) == 0xFFFFFFFF) {
            if (mode == LIST_END)
                break;
            return igsa_fail(c, "end mark at 0x%zx: %d of %d chunks", at, done, mode);
        }
        if (need(r, 4, "chunk size"))
            return -1;
        sz = u32(r);
        s = *r;
        igsa_jobj(c, NULL);
        igsa_jstr(c, "chunk", chunk_name(id) ? chunk_name(id) : "unknown");
        igsa_jhex(c, "id", id);
        igsa_juint(c, "size", sz);
        if ((k = chunk(&s, id, ml, cur)) < 0)
            return -1;
        if (k > 0)
            return igsa_fail(c, "chunk 0x%x at 0x%zx (size %u): not a zl chunk", id, at, sz);
        if (s.o - r->o != sz)
            igsa_juint(c, "content", s.o - r->o);
        igsa_jend(c);
        r->o = s.o;
        done++;
    }
    igsa_jend(c);
    return 0;
}
/* ---- the outputs ---------------------------------------------------------------------------------- */

typedef struct sbuf { char *p; size_t n, cap; int err; } sbuf;

static void sb_printf(sbuf *b, const char *fmt, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 2, 3)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    int k;

    if (b->err)
        return;
    if (b->cap - b->n < 256) {
        char *t = realloc(b->p, b->cap = b->cap * 2 + 65536);

        if (!t) {
            b->err = 1;
            return;
        }
        b->p = t;
    }
    va_start(ap, fmt);
    k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k < 0)
        b->err = 1;
    else if ((size_t)k >= b->cap - b->n) {
        char *t = realloc(b->p, b->cap = b->n + (size_t)k + 65536);

        if (!t) {
            b->err = 1;
            return;
        }
        b->p = t;
        va_start(ap, fmt);
        vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        b->n += (size_t)k;
    } else
        b->n += (size_t)k;
}

static const char *base_of(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');

    if (b > a)
        a = b;
    return a ? a + 1 : p;
}

/* the first "Map" of each "Shader <name>" in a shader file (the MTL's textures) */
typedef struct shmap { char name[64], map[128]; } shmap;

static int shader_maps(const uint8_t *d, size_t n, shmap *out, int max)
{
    size_t o = 0;
    int k = -1, cnt = 0, depth = 0;

    while (o < n) {
        char line[512], *t, *v;
        size_t l = 0;

        while (o < n && d[o] != '\n' && l < sizeof(line) - 1)
            line[l++] = (char)d[o++];
        while (o < n && d[o] != '\n')
            o++;
        o++;
        line[l] = 0;
        t = strtok(line, " \t\r");
        if (!t || !strncmp(t, "//", 2))
            continue;
        if (!strcmp(t, "{"))
            depth++;
        else if (!strcmp(t, "}"))
            depth--;
        else if (!strcasecmp(t, "Shader") && depth == 0 && (v = strtok(NULL, " \t\r")) && cnt < max) {
            k = cnt++;
            snprintf(out[k].name, sizeof(out[k].name), "%s", v);
            out[k].map[0] = 0;
        } else if (!strcasecmp(t, "Map") && k >= 0 && !out[k].map[0] && (v = strtok(NULL, " \t\r")))
            snprintf(out[k].map, sizeof(out[k].map), "%s", v);
    }
    return cnt;
}

static uint8_t *read_disk(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *d = NULL;

    if (!f)
        return NULL;
    if (!fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)len))) {
        if (fread(d, 1, (size_t)len, f) != (size_t)len) {
            free(d);
            d = NULL;
        } else
            *n = (size_t)len;
    }
    fclose(f);
    return d;
}

/* the model's shader file: <model>.shader, a scene's <base>.z.shader */
static int load_shaders(igsa_ctx *c, int scene, shmap *sm, int max, int *inzpk)
{
    char nm[1400], *e;
    const uint8_t *d = NULL;
    uint8_t *own = NULL;
    size_t n = 0;
    int k;

    snprintf(nm, sizeof(nm), "%s", c->path);
    if (scene && (e = strstr(nm, ".map.z")) && !e[6])
        strcpy(e, ".z.shader");
    else
        strncat(nm, ".shader", sizeof(nm) - strlen(nm) - 1);
    *inzpk = igsa_zl_sibling != NULL;
    if (igsa_zl_sibling)
        d = igsa_zl_sibling(nm, &n);
    else
        d = own = read_disk(nm, &n);
    if (!d)
        return 0;
    k = shader_maps(d, n, sm, max);
    free(own);
    return k;
}

typedef struct jarg { mlist *ml; } jarg;

static void json_meshes(igsa_ctx *c, void *arg)
{
    mlist *ml = ((jarg *)arg)->ml;
    int i, l;
    uint32_t v, k;

    igsa_jobj(c, NULL);
    igsa_jarr(c, "meshes");
    for (i = 0; i < ml->n; i++) {
        mesh *m = &ml->m[i];

        igsa_jobj(c, NULL);
        igsa_jstr(c, "kind", m->kind);
        if (m->name)
            igsa_jgbk(c, "name", m->name, strlen(m->name));
        if (m->tex)
            igsa_jgbk(c, "shader", m->tex, strlen(m->tex));
        igsa_jarr(c, "lods");
        for (l = 0; l < m->nd; l++) {
            mdata *d = &m->d[l];
            int t;

            igsa_jobj(c, NULL);
            igsa_jhex(c, "fvf", d->fvf);
            igsa_jarr(c, "position");
            for (v = 0; v < d->nv && d->v; v++)
                for (k = 0; k < 3; k++)
                    igsa_jnum(c, NULL, igsa_lef(d->v + v * d->stride + 4 * k));
            igsa_jend(c);
            if (d->off_n >= 0) {
                igsa_jarr(c, "normal");
                for (v = 0; v < d->nv && d->v; v++)
                    for (k = 0; k < 3; k++)
                        igsa_jnum(c, NULL, igsa_lef(d->v + v * d->stride + d->off_n + 4 * k));
                igsa_jend(c);
            }
            if (d->off_psize >= 0) {
                igsa_jarr(c, "point_size");
                for (v = 0; v < d->nv && d->v; v++)
                    igsa_jnum(c, NULL, igsa_lef(d->v + v * d->stride + d->off_psize));
                igsa_jend(c);
            }
            if (d->off_diffuse >= 0) {
                igsa_jarr(c, "diffuse_argb");
                for (v = 0; v < d->nv && d->v; v++)
                    igsa_jhex(c, NULL, igsa_le32(d->v + v * d->stride + d->off_diffuse));
                igsa_jend(c);
            }
            if (d->off_specular >= 0) {
                igsa_jarr(c, "specular_argb");
                for (v = 0; v < d->nv && d->v; v++)
                    igsa_jhex(c, NULL, igsa_le32(d->v + v * d->stride + d->off_specular));
                igsa_jend(c);
            }
            for (t = 0; t < d->ntex; t++) {
                char key[16];

                snprintf(key, sizeof(key), "uv%d", t);
                igsa_jarr(c, key);
                for (v = 0; v < d->nv && d->v; v++)
                    for (k = 0; k < (uint32_t)d->tex_dim[t]; k++)
                        igsa_jnum(c, NULL, igsa_lef(d->v + v * d->stride + d->off_tex[t] + 4 * k));
                igsa_jend(c);
            }
            igsa_jarr(c, "triangles");
            for (v = 0; v < d->ni && d->ix; v++)
                igsa_juint(c, NULL, index_at(d, v));
            igsa_jend(c);
            igsa_jend(c);
        }
        igsa_jend(c);
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
}

static int emit_obj(igsa_ctx *c, mlist *ml, int scene)
{
    sbuf o = { 0 }, t = { 0 };
    shmap sm[512];
    int ns, inzpk, i, l, r;
    uint32_t base = 1, v, k;
    const char *b = base_of(c->path);
    char nm[600];

    ns = load_shaders(c, scene, sm, 512, &inzpk);
    sb_printf(&o, "# %s (zl %s)\nmtllib %s.mtl\n", b, scene ? "scene" : "model", b);
    sb_printf(&t, "# %s\n", b);
    for (i = 0; i < ns; i++) {
        sb_printf(&t, "newmtl %s\nKd 1 1 1\n", sm[i].name);
        if (sm[i].map[0])
            sb_printf(&t, "map_Kd %s%s%s\n", inzpk ? "../" : "", sm[i].map, inzpk ? ".png" : "");
    }
    for (i = 0; i < ml->n; i++) {
        mesh *m = &ml->m[i];

        for (l = 0; l < m->nd; l++) {
            mdata *d = &m->d[l];
            int uv = d->ntex > 0 && d->tex_dim[0] >= 2;

            if (!d->v)
                continue;
            sb_printf(&o, "o %s%s%d\n", m->name ? m->name : m->kind, "_lod", l);
            if (m->tex && *m->tex)
                sb_printf(&o, "usemtl %s\n", m->tex);
            for (v = 0; v < d->nv; v++) {
                const uint8_t *p = d->v + v * d->stride;

                sb_printf(&o, "v %.6g %.6g %.6g\n", igsa_lef(p), igsa_lef(p + 4), igsa_lef(p + 8));
            }
            if (uv)
                for (v = 0; v < d->nv; v++) {
                    const uint8_t *p = d->v + v * d->stride + d->off_tex[0];

                    sb_printf(&o, "vt %.6g %.6g\n", igsa_lef(p), 1.0 - igsa_lef(p + 4));
                }
            if (d->off_n >= 0)
                for (v = 0; v < d->nv; v++) {
                    const uint8_t *p = d->v + v * d->stride + d->off_n;

                    sb_printf(&o, "vn %.6g %.6g %.6g\n", igsa_lef(p), igsa_lef(p + 4), igsa_lef(p + 8));
                }
            for (v = 0; v + 2 < d->ni; v += 3) {
                sb_printf(&o, "f");
                for (k = 0; k < 3; k++) {
                    uint32_t x = index_at(d, v + k) + base;

                    if (uv && d->off_n >= 0)
                        sb_printf(&o, " %u/%u/%u", x, x, x);
                    else if (uv)
                        sb_printf(&o, " %u/%u", x, x);
                    else if (d->off_n >= 0)
                        sb_printf(&o, " %u//%u", x, x);
                    else
                        sb_printf(&o, " %u", x);
                }
                sb_printf(&o, "\n");
            }
            base += d->nv;
        }
    }
    if (o.err || t.err) {
        free(o.p);
        free(t.p);
        return igsa_fail(c, "out of memory");
    }
    snprintf(nm, sizeof(nm), "%s.obj", b);
    r = igsa_emit_raw(c, nm, o.p, o.n);
    snprintf(nm, sizeof(nm), "%s.mtl", b);
    if (!r)
        r = igsa_emit_raw(c, nm, t.p, t.n);
    free(o.p);
    free(t.p);
    return r;
}

/* ---- the formats ---------------------------------------------------------------------------------- */

/* the first chunk is the given container and its size covers the file */
static int probe_top(const uint8_t *d, size_t n, uint32_t id)
{
    uint32_t sz;

    if (n < 12 || igsa_le32(d) != id)
        return 0;
    sz = igsa_le32(d + 4);
    return (size_t)sz + 8 <= n && sz >= 8 ? 80 : 0;
}

static int probe_model(const uint8_t *d, size_t n, const char *path) { (void)path; return probe_top(d, n, Z_MODEL); }
static int probe_scene(const uint8_t *d, size_t n, const char *path) { (void)path; return probe_top(d, n, Z_SCENE); }

static int probe_cam(const uint8_t *d, size_t n, const char *path)
{
    uint32_t sz;

    (void)path;
    if (n < 16 || igsa_le32(d) != Z_CAMERA)
        return 0;
    sz = igsa_le32(d + 4);
    return (size_t)sz + 12 == n && igsa_le32(d + n - 4) == 0xFFFFFFFF ? 90 : 0;
}

static int parse_z(igsa_ctx *c, int scene)
{
    rd r = { c, c->d, 0, c->n };
    mlist ml = { 0 };
    jarg ja;
    int i, ret = 0;
    uint32_t tv = 0, ti = 0;
    char nm[600];

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", scene ? "zl scene" : "zl model");
    if (chunk_list(&r, &ml, LIST_FILE, NULL)) {
        free(ml.m);
        return -1;
    }
    for (i = 0; i < ml.n; i++)
        for (int l = 0; l < ml.m[i].nd; l++) {
            tv += ml.m[i].d[l].nv;
            ti += ml.m[i].d[l].ni;
        }
    igsa_jint(c, "meshes", ml.n);
    igsa_juint(c, "vertices", tv);
    igsa_juint(c, "triangles", ti / 3);
    igsa_jend(c);
    ja.ml = &ml;
    snprintf(nm, sizeof(nm), "%s.json", base_of(c->path));
    if (igsa_emit_json(c, nm, json_meshes, &ja) || emit_obj(c, &ml, scene))
        ret = -1;
    free(ml.m);
    return ret;
}

static int parse_model(igsa_ctx *c) { return parse_z(c, 0); }
static int parse_scene(igsa_ctx *c) { return parse_z(c, 1); }

static int parse_cam(igsa_ctx *c)
{
    rd r = { c, c->d, 0, c->n };
    mlist ml = { 0 };
    int k;

    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "zl camera");
    k = chunk_list(&r, &ml, LIST_END, NULL);   /* LoadCam: chunks up to 0xFFFFFFFF */
    free(ml.m);
    if (k)
        return -1;
    if (r.o != c->n)
        return igsa_fail(c, "%zu bytes after the end mark", c->n - r.o);
    igsa_jend(c);
    c->entries++;
    return 0;
}

const igsa_format igsa_fmt_zlmodel = { "zlmodel", "zl engine model (.z): meshes, skeleton, animations -> OBJ/MTL, JSON",
                                       probe_model, parse_model };
const igsa_format igsa_fmt_zlscene = { "zlscene", "zl engine scene (.map.z): meshes, collision, objects -> OBJ/MTL, JSON",
                                       probe_scene, parse_scene };
const igsa_format igsa_fmt_zlcam = { "zlcam", "zl engine camera path (.cam): keys of fov, eye, target, up", probe_cam,
                                     parse_cam };
