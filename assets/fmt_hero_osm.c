/*
 * Hero of Robots media/osmbin/<name>.osmenc: the game's OgreMax scenes (.osm XML) compiled to a
 * boost::serialization text archive (OsmMgr::SaveToOSMBinIndividual) and FPGA-cipher
 * encrypted with the set's asset mask; the last 4 plaintext bytes are the text length,
 * big-endian (Encrypt::AES_iCryptoForFile).  Loaded by OsmMgr::LoadOSMFile ->
 * LoadFromCharArray (text_iarchive of OsmParser).  The parser rebuilds the OSM XML from
 * the archive.  wiki Format-OSMENC.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

static const char sig[] = "22 serialization::archive ";

static int probe(const uint8_t *d, size_t n, const char *path)
{
    uint32_t l;

    (void)path;
    if (n < 32 || memcmp(d, sig, sizeof(sig) - 1))
        return 0;
    l = igsa_be32(d + n - 4);
    return l > 30 && l <= n - 4 ? 100 : 0;
}

/* ---- the archive's objects ----------------------------------------------------------------- */

struct trans { float p[3], q[4], s[3]; };
struct kf { double time, fov; struct trans t; };
struct anim { char *name; int loop; double length; unsigned keys; int nkf; struct kf *kf; };
struct amap { int n; char **key; struct anim **a; };
struct sd { int n; char **key; double *v; };
struct ss { int n; char **key, **v; };
struct entity { char *name, *file, *parent, *s128; int hidden, cast, recv; struct trans t; struct amap an; struct sd sd; struct ss ss; };
struct light { char *name, *type; int on, cast; double intensity; struct trans t; float color[3], spec[3]; struct amap an; struct sd sd; };
struct camera { char *name, *target; double fov; struct trans t, tt; struct amap an, tan; struct sd sd; };
struct header { float light[3], bkg[3]; char *s; };
struct emap { int n; char **key; struct entity **e; };
struct lmap { int n; char **key; struct light **l; };
struct cmap { int n; char **key; struct camera **c; };
struct scene { struct header h; int mgr, fog, shadow, texsize, texcount; double density, lstart, lend; float fogc[3], shc[3];
               struct lmap lights; struct emap ents; };
struct actor { struct header h; struct emap ents; struct sd sd; };
struct camfile { struct header h; struct cmap cams; };
struct parser { char *name; unsigned type; struct scene *scene; struct actor *actor; struct camfile *cam; struct header *head; };

enum { T_PARSER, T_SCENE, T_ACTOR, T_CAMFILE, T_HEADER, T_TRANS, T_ENTITY, T_LIGHT, T_CAMERA, T_ANIM, T_KF,
       T_MAPCAM, T_PAIRCAM, T_MAPENT, T_PAIRENT, T_MAPLIGHT, T_PAIRLIGHT, T_MAPANIM, T_PAIRANIM, T_MAPSD, T_PAIRSD,
       T_MAPSS, T_PAIRSS, T_VECKF, T_N };

typedef struct ar {
    igsa_ctx *c;
    const char *t;
    size_t n, o;
    int bad, nclass;
    int id[T_N], init[T_N], track[T_N], ver[T_N];
    void **obj;                 /* tracked objects by object id */
    int nobj, capobj;
    char **blocks;              /* the arena */
    int nblk, capblk;
} ar;

static void *alloc(ar *a, size_t n)
{
    void *p;

    if (a->bad)
        return NULL;
    if (a->nblk == a->capblk) {
        char **b = realloc(a->blocks, (a->capblk = a->capblk ? a->capblk * 2 : 64) * sizeof(*b));

        if (!b) {
            a->bad = 1;
            igsa_fail(a->c, "out of memory");
            return NULL;
        }
        a->blocks = b;
    }
    if (!(p = calloc(1, n ? n : 1))) {
        a->bad = 1;
        igsa_fail(a->c, "out of memory");
        return NULL;
    }
    return a->blocks[a->nblk++] = p;
}

static int fail(ar *a, const char *what)
{
    if (!a->bad)
        igsa_fail(a->c, "archive at %zu: %s", a->o, what);
    a->bad = 1;
    return -1;
}

static const char *token(ar *a, size_t *len)
{
    size_t s;

    while (a->o < a->n && (a->t[a->o] == ' ' || a->t[a->o] == '\n' || a->t[a->o] == '\r' || a->t[a->o] == '\t'))
        a->o++;
    if (a->o >= a->n) {
        fail(a, "unexpected end");
        return NULL;
    }
    s = a->o;
    while (a->o < a->n && a->t[a->o] != ' ' && a->t[a->o] != '\n' && a->t[a->o] != '\r' && a->t[a->o] != '\t')
        a->o++;
    *len = a->o - s;
    return a->t + s;
}

static long long inum(ar *a)
{
    char b[32];
    size_t l;
    const char *t = a->bad ? NULL : token(a, &l);
    char *e;
    long long v;

    if (!t)
        return 0;
    if (l >= sizeof(b)) {
        fail(a, "number too long");
        return 0;
    }
    memcpy(b, t, l);
    b[l] = 0;
    v = strtoll(b, &e, 10);
    if (*e) {
        fail(a, "not an integer");
        return 0;
    }
    return v;
}

static double fnum(ar *a)
{
    char b[64];
    size_t l;
    const char *t = a->bad ? NULL : token(a, &l);
    char *e;
    double v;

    if (!t)
        return 0;
    if (l >= sizeof(b)) {
        fail(a, "number too long");
        return 0;
    }
    memcpy(b, t, l);
    b[l] = 0;
    v = strtod(b, &e);
    if (*e)
        fail(a, "not a number");
    return v;
}

/* a string: its length, one space, the bytes */
static char *str(ar *a)
{
    long long l = inum(a);
    char *s;

    if (a->bad)
        return NULL;
    if (l < 0 || (size_t)l > a->n - a->o || (l && a->t[a->o] != ' ') || (size_t)l + (l ? 1 : 0) > a->n - a->o) {
        fail(a, "string length");
        return NULL;
    }
    if (l)
        a->o++;
    if (!(s = alloc(a, (size_t)l + 1)))
        return NULL;
    memcpy(s, a->t + a->o, (size_t)l);
    a->o += (size_t)l;
    return s;
}

/* the class preamble on first use (tracking, version), then the object id of a tracked object */
static int preamble(ar *a, int ty, int ptr)
{
    if (a->id[ty] < 0)
        a->id[ty] = a->nclass++;
    if (!ptr && !a->init[ty]) {
        a->track[ty] = (int)inum(a);
        a->ver[ty] = (int)inum(a);
        a->init[ty] = 1;
    }
    return a->bad ? -1 : 0;
}

/* a new tracked object: its id must be the next one */
static int newobj(ar *a, void *o)
{
    long long oid = inum(a);

    if (a->bad)
        return -1;
    if (oid != a->nobj)
        return fail(a, "object id out of sequence");
    if (a->nobj == a->capobj) {
        void **v = realloc(a->obj, (a->capobj = a->capobj ? a->capobj * 2 : 64) * sizeof(*v));

        if (!v)
            return fail(a, "out of memory");
        a->obj = v;
    }
    a->obj[a->nobj++] = o;
    return 0;
}

/* a by-value object of class ty: preamble, object id when tracked */
static int byval(ar *a, int ty, void *o)
{
    if (preamble(a, ty, 0))
        return -1;
    return a->track[ty] ? newobj(a, o) : 0;
}

typedef int (*loadfn)(ar *a, void *o);

/* a pointer member: class id (-1 null), preamble, object id (or a reference), the data */
static void *pointer(ar *a, int ty, size_t sz, loadfn fn)
{
    long long cid;
    void *o;

    if (a->id[ty] < 0)
        a->id[ty] = a->nclass++;        /* register_basic_serializer before load_pointer */
    cid = inum(a);
    if (a->bad || cid == -1)
        return NULL;
    if (cid != a->id[ty]) {
        fail(a, "pointer of another class");
        return NULL;
    }
    if (!a->init[ty]) {
        a->track[ty] = (int)inum(a);
        a->ver[ty] = (int)inum(a);
        a->init[ty] = 1;
    }
    if (a->track[ty]) {
        size_t save = a->o;
        long long oid = inum(a);

        if (a->bad)
            return NULL;
        if (oid < a->nobj)
            return a->obj[oid];         /* a reference to an object already read */
        a->o = save;
    }
    if (!(o = alloc(a, sz)))
        return NULL;
    if (a->track[ty] && newobj(a, o))
        return NULL;
    if (fn(a, o))
        return NULL;
    return o;
}

static int l_trans(ar *a, struct trans *t)
{
    int i;

    if (byval(a, T_TRANS, t))
        return -1;
    for (i = 0; i < 3; i++)
        t->p[i] = (float)fnum(a);
    for (i = 0; i < 4; i++)
        t->q[i] = (float)fnum(a);
    for (i = 0; i < 3; i++)
        t->s[i] = (float)fnum(a);
    return a->bad ? -1 : 0;
}

static int l_kf(ar *a, struct kf *k)
{
    if (byval(a, T_KF, k))
        return -1;
    k->time = fnum(a);
    k->fov = fnum(a);
    return l_trans(a, &k->t);
}

static int l_anim(ar *a, void *o)
{
    struct anim *m = o;
    long long n, i;

    m->name = str(a);
    m->loop = (int)inum(a);
    m->length = fnum(a);
    m->keys = (unsigned)inum(a);
    if (byval(a, T_VECKF, NULL))
        return -1;
    n = inum(a);
    inum(a);                            /* item_version */
    if (a->bad || n < 0 || n > 1000000)
        return fail(a, "key frame count");
    if (!(m->kf = alloc(a, sizeof(*m->kf) * (size_t)(n ? n : 1))))
        return -1;
    m->nkf = (int)n;
    for (i = 0; i < n; i++)
        if (l_kf(a, &m->kf[i]))
            return -1;
    return 0;
}

/* map<string, T *> and friends: count, item_version, pairs (class preamble once) */
static long long map_head(ar *a, int mty, int pty)
{
    long long n;

    if (byval(a, mty, NULL))
        return -1;
    n = inum(a);
    inum(a);
    if (a->bad || n < 0 || n > 1000000) {
        fail(a, "map count");
        return -1;
    }
    (void)pty;
    return n;
}

static int l_amap(ar *a, struct amap *m)
{
    long long n = map_head(a, T_MAPANIM, T_PAIRANIM), i;

    if (n < 0)
        return -1;
    m->n = (int)n;
    if (!(m->key = alloc(a, sizeof(char *) * (size_t)(n + 1))) || !(m->a = alloc(a, sizeof(void *) * (size_t)(n + 1))))
        return -1;
    for (i = 0; i < n; i++) {
        if (byval(a, T_PAIRANIM, NULL))
            return -1;
        m->key[i] = str(a);
        m->a[i] = pointer(a, T_ANIM, sizeof(struct anim), l_anim);
        if (a->bad)
            return -1;
    }
    return 0;
}

static int l_sd(ar *a, struct sd *m)
{
    long long n = map_head(a, T_MAPSD, T_PAIRSD), i;

    if (n < 0)
        return -1;
    m->n = (int)n;
    if (!(m->key = alloc(a, sizeof(char *) * (size_t)(n + 1))) || !(m->v = alloc(a, sizeof(double) * (size_t)(n + 1))))
        return -1;
    for (i = 0; i < n; i++) {
        if (byval(a, T_PAIRSD, NULL))
            return -1;
        m->key[i] = str(a);
        m->v[i] = fnum(a);
    }
    return a->bad ? -1 : 0;
}

static int l_ss(ar *a, struct ss *m)
{
    long long n = map_head(a, T_MAPSS, T_PAIRSS), i;

    if (n < 0)
        return -1;
    m->n = (int)n;
    if (!(m->key = alloc(a, sizeof(char *) * (size_t)(n + 1))) || !(m->v = alloc(a, sizeof(char *) * (size_t)(n + 1))))
        return -1;
    for (i = 0; i < n; i++) {
        if (byval(a, T_PAIRSS, NULL))
            return -1;
        m->key[i] = str(a);
        m->v[i] = str(a);
    }
    return a->bad ? -1 : 0;
}

static int l_entity(ar *a, void *o)
{
    struct entity *e = o;

    e->name = str(a);
    e->file = str(a);
    e->parent = str(a);
    e->hidden = (int)inum(a);
    e->cast = (int)inum(a);
    e->recv = (int)inum(a);
    if (a->bad || l_trans(a, &e->t) || l_amap(a, &e->an) || l_sd(a, &e->sd) || l_ss(a, &e->ss))
        return -1;
    e->s128 = str(a);
    return a->bad ? -1 : 0;
}

static int l_light(ar *a, void *o)
{
    struct light *l = o;
    int i;

    l->name = str(a);
    l->type = str(a);
    l->on = (int)inum(a);
    l->cast = (int)inum(a);
    l->intensity = fnum(a);
    if (a->bad || l_trans(a, &l->t))
        return -1;
    for (i = 0; i < 3; i++)
        l->color[i] = (float)fnum(a);
    for (i = 0; i < 3; i++)
        l->spec[i] = (float)fnum(a);
    return a->bad || l_amap(a, &l->an) || l_sd(a, &l->sd) ? -1 : 0;
}

static int l_camera(ar *a, void *o)
{
    struct camera *m = o;

    m->name = str(a);
    m->fov = fnum(a);
    if (a->bad || l_trans(a, &m->t) || l_amap(a, &m->an))
        return -1;
    m->target = str(a);
    return a->bad || l_trans(a, &m->tt) || l_amap(a, &m->tan) || l_sd(a, &m->sd) ? -1 : 0;
}

static int l_header(ar *a, void *o)
{
    struct header *h = o;
    int i;

    for (i = 0; i < 3; i++)
        h->light[i] = (float)fnum(a);
    for (i = 0; i < 3; i++)
        h->bkg[i] = (float)fnum(a);
    h->s = str(a);
    return a->bad ? -1 : 0;
}

static int l_header_val(ar *a, struct header *h)
{
    return byval(a, T_HEADER, h) || l_header(a, h) ? -1 : 0;
}

static int l_emap(ar *a, struct emap *m)
{
    long long n = map_head(a, T_MAPENT, T_PAIRENT), i;

    if (n < 0)
        return -1;
    m->n = (int)n;
    if (!(m->key = alloc(a, sizeof(char *) * (size_t)(n + 1))) || !(m->e = alloc(a, sizeof(void *) * (size_t)(n + 1))))
        return -1;
    for (i = 0; i < n; i++) {
        if (byval(a, T_PAIRENT, NULL))
            return -1;
        m->key[i] = str(a);
        m->e[i] = pointer(a, T_ENTITY, sizeof(struct entity), l_entity);
        if (a->bad)
            return -1;
    }
    return 0;
}

static int l_lmap(ar *a, struct lmap *m)
{
    long long n = map_head(a, T_MAPLIGHT, T_PAIRLIGHT), i;

    if (n < 0)
        return -1;
    m->n = (int)n;
    if (!(m->key = alloc(a, sizeof(char *) * (size_t)(n + 1))) || !(m->l = alloc(a, sizeof(void *) * (size_t)(n + 1))))
        return -1;
    for (i = 0; i < n; i++) {
        if (byval(a, T_PAIRLIGHT, NULL))
            return -1;
        m->key[i] = str(a);
        m->l[i] = pointer(a, T_LIGHT, sizeof(struct light), l_light);
        if (a->bad)
            return -1;
    }
    return 0;
}

static int l_cmap(ar *a, struct cmap *m)
{
    long long n = map_head(a, T_MAPCAM, T_PAIRCAM), i;

    if (n < 0)
        return -1;
    m->n = (int)n;
    if (!(m->key = alloc(a, sizeof(char *) * (size_t)(n + 1))) || !(m->c = alloc(a, sizeof(void *) * (size_t)(n + 1))))
        return -1;
    for (i = 0; i < n; i++) {
        if (byval(a, T_PAIRCAM, NULL))
            return -1;
        m->key[i] = str(a);
        m->c[i] = pointer(a, T_CAMERA, sizeof(struct camera), l_camera);
        if (a->bad)
            return -1;
    }
    return 0;
}

static int l_scene(ar *a, void *o)
{
    struct scene *s = o;
    int i;

    if (l_header_val(a, &s->h))
        return -1;
    s->mgr = (int)inum(a);
    s->fog = (int)inum(a);
    s->density = fnum(a);
    s->lstart = fnum(a);
    s->lend = fnum(a);
    for (i = 0; i < 3; i++)
        s->fogc[i] = (float)fnum(a);
    s->shadow = (int)inum(a);
    s->texsize = (int)inum(a);
    s->texcount = (int)inum(a);
    for (i = 0; i < 3; i++)
        s->shc[i] = (float)fnum(a);
    return a->bad || l_lmap(a, &s->lights) || l_emap(a, &s->ents) ? -1 : 0;
}

static int l_actor(ar *a, void *o)
{
    struct actor *s = o;

    return l_header_val(a, &s->h) || l_emap(a, &s->ents) || l_sd(a, &s->sd) ? -1 : 0;
}

static int l_camfile(ar *a, void *o)
{
    struct camfile *s = o;

    return l_header_val(a, &s->h) || l_cmap(a, &s->cams) ? -1 : 0;
}

static int l_parser(ar *a, struct parser *p)
{
    if (byval(a, T_PARSER, p))
        return -1;
    p->name = str(a);
    p->type = (unsigned)inum(a);
    if (a->bad)
        return -1;
    p->scene = pointer(a, T_SCENE, sizeof(struct scene), l_scene);
    if (a->bad)
        return -1;
    p->actor = pointer(a, T_ACTOR, sizeof(struct actor), l_actor);
    if (a->bad)
        return -1;
    p->cam = pointer(a, T_CAMFILE, sizeof(struct camfile), l_camfile);
    if (a->bad)
        return -1;
    p->head = pointer(a, T_HEADER, sizeof(struct header), l_header);
    return a->bad ? -1 : 0;
}

/* ---- JSON ---------------------------------------------------------------------------------- */

static void j_trans(igsa_ctx *c, const char *k, const struct trans *t)
{
    int i;

    igsa_jobj(c, k);
    igsa_jarr(c, "position");
    for (i = 0; i < 3; i++)
        igsa_jnum(c, NULL, t->p[i]);
    igsa_jend(c);
    igsa_jarr(c, "rotation_xyzw");
    for (i = 0; i < 4; i++)
        igsa_jnum(c, NULL, t->q[i]);
    igsa_jend(c);
    igsa_jarr(c, "scale");
    for (i = 0; i < 3; i++)
        igsa_jnum(c, NULL, t->s[i]);
    igsa_jend(c);
    igsa_jend(c);
}

static void j_rgb(igsa_ctx *c, const char *k, const float *v)
{
    int i;

    igsa_jarr(c, k);
    for (i = 0; i < 3; i++)
        igsa_jnum(c, NULL, v[i]);
    igsa_jend(c);
}

static void j_anims(igsa_ctx *c, const char *k, const struct amap *m)
{
    int i, j;

    igsa_jarr(c, k);
    for (i = 0; i < m->n; i++) {
        const struct anim *a = m->a[i];

        igsa_jobj(c, NULL);
        igsa_jstr(c, "key", m->key[i]);
        if (a) {
            igsa_jstr(c, "name", a->name);
            igsa_jbool(c, "loop", a->loop);
            igsa_jnum(c, "length", a->length);
            igsa_juint(c, "keys", a->keys);
            igsa_jarr(c, "keyframes");
            for (j = 0; j < a->nkf; j++) {
                igsa_jobj(c, NULL);
                igsa_jnum(c, "time", a->kf[j].time);
                if (a->kf[j].fov)
                    igsa_jnum(c, "fov_rad", a->kf[j].fov);
                j_trans(c, "transform", &a->kf[j].t);
                igsa_jend(c);
            }
            igsa_jend(c);
        }
        igsa_jend(c);
    }
    igsa_jend(c);
}

static void j_sd(igsa_ctx *c, const char *k, const struct sd *m)
{
    int i;

    igsa_jobj(c, k);
    for (i = 0; i < m->n; i++)
        igsa_jnum(c, m->key[i], m->v[i]);
    igsa_jend(c);
}

static void j_header(igsa_ctx *c, const char *k, const struct header *h)
{
    igsa_jobj(c, k);
    j_rgb(c, "lightColor", h->light);
    j_rgb(c, "bkgcolor", h->bkg);
    igsa_jstr(c, "string", h->s);
    igsa_jend(c);
}

static void j_ents(igsa_ctx *c, const struct emap *m)
{
    int i, k;

    igsa_jarr(c, "entities");
    for (i = 0; i < m->n; i++) {
        const struct entity *e = m->e[i];

        igsa_jobj(c, NULL);
        if (e) {
            igsa_jstr(c, "name", e->name);
            igsa_jstr(c, "filename", e->file);
            igsa_jstr(c, "parent", e->parent);
            igsa_jbool(c, "hidden", e->hidden);
            igsa_jbool(c, "CastShadows", e->cast);
            igsa_jbool(c, "ReceiveShadows", e->recv);
            j_trans(c, "transform", &e->t);
            j_anims(c, "animations", &e->an);
            j_sd(c, "anim_lengths", &e->sd);
            igsa_jobj(c, "strings");
            for (k = 0; k < e->ss.n; k++)
                igsa_jstr(c, e->ss.key[k], e->ss.v[k]);
            igsa_jend(c);
            igsa_jstr(c, "string", e->s128);
        }
        igsa_jend(c);
    }
    igsa_jend(c);
}

static void j_parser(igsa_ctx *c, const struct parser *p)
{
    int i;

    igsa_jstr(c, "name", p->name);
    igsa_juint(c, "type", p->type);
    if (p->scene) {
        const struct scene *s = p->scene;

        igsa_jobj(c, "scene");
        j_header(c, "header", &s->h);
        igsa_jint(c, "sceneManager", s->mgr);
        igsa_jobj(c, "fogMode");
        igsa_jint(c, "type", s->fog);
        igsa_jnum(c, "density", s->density);
        igsa_jnum(c, "linearStart", s->lstart);
        igsa_jnum(c, "linearEnd", s->lend);
        j_rgb(c, "color", s->fogc);
        igsa_jend(c);
        igsa_jobj(c, "shadowTechnique");
        igsa_jint(c, "type", s->shadow);
        igsa_jint(c, "tex_size", s->texsize);
        igsa_jint(c, "tex_count", s->texcount);
        j_rgb(c, "color", s->shc);
        igsa_jend(c);
        igsa_jarr(c, "lights");
        for (i = 0; i < s->lights.n; i++) {
            const struct light *l = s->lights.l[i];

            igsa_jobj(c, NULL);
            if (l) {
                igsa_jstr(c, "name", l->name);
                igsa_jstr(c, "type", l->type);
                igsa_jbool(c, "on", l->on);
                igsa_jbool(c, "CastShadows", l->cast);
                igsa_jnum(c, "intensity", l->intensity);
                j_trans(c, "transform", &l->t);
                j_rgb(c, "color", l->color);
                j_rgb(c, "specular", l->spec);
                j_anims(c, "animations", &l->an);
                j_sd(c, "anim_lengths", &l->sd);
            }
            igsa_jend(c);
        }
        igsa_jend(c);
        j_ents(c, &s->ents);
        igsa_jend(c);
    }
    if (p->actor) {
        igsa_jobj(c, "actor");
        j_header(c, "header", &p->actor->h);
        j_ents(c, &p->actor->ents);
        j_sd(c, "anim_lengths", &p->actor->sd);
        igsa_jend(c);
    }
    if (p->cam) {
        igsa_jobj(c, "cameras");
        j_header(c, "header", &p->cam->h);
        igsa_jarr(c, "cameras");
        for (i = 0; i < p->cam->cams.n; i++) {
            const struct camera *m = p->cam->cams.c[i];

            igsa_jobj(c, NULL);
            if (m) {
                igsa_jstr(c, "name", m->name);
                igsa_jnum(c, "FOV", m->fov);
                j_trans(c, "transform", &m->t);
                j_anims(c, "animations", &m->an);
                igsa_jstr(c, "target", m->target);
                j_trans(c, "target_transform", &m->tt);
                j_anims(c, "target_animations", &m->tan);
                j_sd(c, "anim_lengths", &m->sd);
            }
            igsa_jend(c);
        }
        igsa_jend(c);
        igsa_jend(c);
    }
    if (p->head)
        j_header(c, "header", p->head);
}

/* ---- XML ----------------------------------------------------------------------------------- */

typedef struct xb { char *p; size_t n, cap; int bad; } xb;

static void xp(xb *b, const char *f, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 2, 3)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void xp(xb *b, const char *f, ...)
{
    va_list v;
    int l;

    if (b->bad)
        return;
    for (;;) {
        va_start(v, f);
        l = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, f, v);
        va_end(v);
        if (l < 0) {
            b->bad = 1;
            return;
        }
        if (b->p && b->n + (size_t)l < b->cap) {
            b->n += (size_t)l;
            return;
        }
        {
            size_t nc = (b->cap + (size_t)l + 1) * 2;
            char *q = realloc(b->p, nc);

            if (!q) {
                b->bad = 1;
                return;
            }
            b->p = q;
            b->cap = nc;
        }
    }
}

/* an attribute value, XML-escaped */
static void xs(xb *b, const char *s)
{
    for (; s && *s; s++)
        switch (*s) {
        case '&': xp(b, "&amp;"); break;
        case '<': xp(b, "&lt;"); break;
        case '>': xp(b, "&gt;"); break;
        case '"': xp(b, "&quot;"); break;
        default: xp(b, "%c", *s);
        }
}

static void ind(xb *b, int d)
{
    xp(b, "%*s", 4 * d, "");
}

static void x_rgb(xb *b, int d, const char *tag, const float *v)
{
    ind(b, d);
    xp(b, "<%s r=\"%.9g\" g=\"%.9g\" b=\"%.9g\" />\n", tag, v[0], v[1], v[2]);
}

static void x_trans(xb *b, int d, const struct trans *t, int wfirst)
{
    ind(b, d);
    xp(b, "<position x=\"%.9g\" y=\"%.9g\" z=\"%.9g\" />\n", t->p[0], t->p[1], t->p[2]);
    ind(b, d);
    if (wfirst)
        xp(b, "<rotation w=\"%.9g\" x=\"%.9g\" y=\"%.9g\" z=\"%.9g\" />\n", t->q[3], t->q[0], t->q[1], t->q[2]);
    else
        xp(b, "<rotation x=\"%.9g\" y=\"%.9g\" z=\"%.9g\" w=\"%.9g\" />\n", t->q[0], t->q[1], t->q[2], t->q[3]);
    ind(b, d);
    xp(b, "<scale x=\"%.9g\" y=\"%.9g\" z=\"%.9g\" />\n", t->s[0], t->s[1], t->s[2]);
}

static void x_anims(xb *b, int d, const struct amap *m)
{
    int i, j;

    if (!m->n)
        return;
    ind(b, d);
    xp(b, "<animations>\n");
    for (i = 0; i < m->n; i++) {
        const struct anim *a = m->a[i];

        if (!a)
            continue;
        ind(b, d + 1);
        xp(b, "<animation name=\"");
        xs(b, a->name);
        xp(b, "\" loop=\"%s\" length=\"%.9g\">\n", a->loop ? "true" : "false", a->length);
        for (j = 0; j < a->nkf; j++) {
            ind(b, d + 2);
            xp(b, "<keyframe time=\"%.9g\">\n", a->kf[j].time);
            if (a->kf[j].fov) {
                ind(b, d + 3);
                xp(b, "<fov value=\"%.9g\" />\n", a->kf[j].fov * 180.0 / 3.14159);
            }
            x_trans(b, d + 3, &a->kf[j].t, 1);
            ind(b, d + 2);
            xp(b, "</keyframe>\n");
        }
        ind(b, d + 1);
        xp(b, "</animation>\n");
    }
    ind(b, d);
    xp(b, "</animations>\n");
}

static void x_head(xb *b, const struct header *h)
{
    x_rgb(b, 1, "bkgcolor", h->bkg);
    x_rgb(b, 1, "lightColor", h->light);
}

static void x_ents(xb *b, const struct emap *m)
{
    int i;

    if (!m->n)
        return;
    xp(b, "    <entities>\n");
    for (i = 0; i < m->n; i++) {
        const struct entity *e = m->e[i];

        if (!e)
            continue;
        xp(b, "        <entity name=\"");
        xs(b, e->name);
        xp(b, "\" hidden=\"%s\" filename=\"", e->hidden ? "true" : "false");
        xs(b, e->file);
        xp(b, "\"");
        if (e->parent && *e->parent) {
            xp(b, " parent=\"");
            xs(b, e->parent);
            xp(b, "\"");
        }
        xp(b, " CastShadows=\"%s\" ReceiveShadows=\"%s\">\n", e->cast ? "true" : "false", e->recv ? "true" : "false");
        x_trans(b, 3, &e->t, 0);
        x_anims(b, 3, &e->an);
        xp(b, "        </entity>\n");
    }
    xp(b, "    </entities>\n");
}

static void x_parser(xb *b, const struct parser *p)
{
    int i;

    xp(b, "<oe_scene>\n");
    if (p->scene) {
        const struct scene *s = p->scene;

        xp(b, "    <sceneManager type=\"%d\" />\n", s->mgr);
        x_head(b, &s->h);
        if (s->fog) {
            xp(b, "    <fogMode type=\"%d\" density=\"%.9g\" linearStart=\"%.9g\" linearEnd=\"%.9g\">\n", s->fog,
               s->density, s->lstart, s->lend);
            x_rgb(b, 2, "color", s->fogc);
            xp(b, "    </fogMode>\n");
        }
        xp(b, "    <shadowTechnique type=\"%d\" tex_size=\"%d\" tex_count=\"%d\">\n", s->shadow, s->texsize, s->texcount);
        x_rgb(b, 2, "color", s->shc);
        xp(b, "    </shadowTechnique>\n");
        x_ents(b, &s->ents);
        if (s->lights.n) {
            xp(b, "    <lights>\n");
            for (i = 0; i < s->lights.n; i++) {
                const struct light *l = s->lights.l[i];

                if (!l)
                    continue;
                xp(b, "        <light name=\"");
                xs(b, l->name);
                xp(b, "\" type=\"");
                xs(b, l->type);
                xp(b, "\" on=\"%s\" CastShadows=\"%s\" intensity=\"%.9g\">\n", l->on ? "true" : "false",
                   l->cast ? "true" : "false", l->intensity);
                x_trans(b, 3, &l->t, 0);
                x_rgb(b, 3, "color", l->color);
                x_rgb(b, 3, "specular", l->spec);
                x_anims(b, 3, &l->an);
                xp(b, "        </light>\n");
            }
            xp(b, "    </lights>\n");
        }
    } else if (p->actor) {
        x_head(b, &p->actor->h);
        x_ents(b, &p->actor->ents);
    } else if (p->cam) {
        x_head(b, &p->cam->h);
        xp(b, "    <cameras>\n");
        for (i = 0; i < p->cam->cams.n; i++) {
            const struct camera *m = p->cam->cams.c[i];

            if (!m)
                continue;
            xp(b, "        <camera name=\"");
            xs(b, m->name);
            xp(b, "\" FOV=\"%.9g\">\n", m->fov);
            x_trans(b, 3, &m->t, 0);
            xp(b, "            <target name=\"");
            xs(b, m->target);
            xp(b, "\">\n");
            x_trans(b, 4, &m->tt, 0);
            x_anims(b, 4, &m->tan);
            xp(b, "            </target>\n");
            x_anims(b, 3, &m->an);
            xp(b, "        </camera>\n");
        }
        xp(b, "    </cameras>\n");
    } else if (p->head)
        x_head(b, p->head);
    xp(b, "</oe_scene>\n");
}

static void json_fn(igsa_ctx *c, void *arg)
{
    igsa_jobj(c, NULL);
    j_parser(c, arg);
    igsa_jend(c);
}

static int parse(igsa_ctx *c)
{
    ar a;
    struct parser p;
    size_t len;
    long long ver;
    int i, r = -1;
    xb x = { NULL, 0, 0, 0 };
    char nm[256];
    const char *b, *e;

    if (c->n < 8)
        return igsa_fail(c, "too short");
    len = igsa_be32(c->d + c->n - 4);
    if (len > c->n - 4)
        return igsa_fail(c, "text length %zu of %zu", len, c->n);
    memset(&a, 0, sizeof(a));
    memset(&p, 0, sizeof(p));
    a.c = c;
    a.t = (const char *)c->d;
    a.n = len;
    for (i = 0; i < T_N; i++)
        a.id[i] = -1;
    if (len < sizeof(sig) - 1 || memcmp(a.t, sig, sizeof(sig) - 1))
        return igsa_fail(c, "not a boost text archive");
    a.o = sizeof(sig) - 1;
    ver = inum(&a);
    if (!a.bad && !l_parser(&a, &p)) {
        while (a.o < a.n && (a.t[a.o] == ' ' || a.t[a.o] == '\n' || a.t[a.o] == '\r'))
            a.o++;
        if (a.o != a.n)
            fail(&a, "text after the archive");
    }
    if (a.bad)
        goto out;
    for (i = 0; i < T_N; i++)
        if (a.init[i] && a.ver[i])
            igsa_fail(c, "class %d has version %d", i, a.ver[i]), a.bad = 1;
    if (a.bad)
        goto out;
    x_parser(&x, &p);
    if (x.bad) {
        igsa_fail(c, "out of memory");
        goto out;
    }
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "osmenc");
    igsa_jbool(c, "decrypted", c->decrypted);
    igsa_jint(c, "archive_version", ver);
    igsa_juint(c, "text_bytes", len);
    igsa_jint(c, "objects", a.nobj);
    igsa_jint(c, "classes", a.nclass);
    j_parser(c, &p);
    igsa_jend(c);
    /* entries named after the scene */
    b = p.name && *p.name ? p.name : "scene";
    e = strrchr(b, '.');
    snprintf(nm, sizeof(nm), "%.*s.osm", (int)(e ? e - b : (long)strlen(b)), b);
    if (igsa_emit_raw(c, nm, x.p, x.n))
        goto out;
    snprintf(nm + strlen(nm) - 4, 16, ".json");
    if (igsa_emit_json(c, nm, json_fn, &p))
        goto out;
    snprintf(nm + strlen(nm) - 5, 16, ".archive.txt");
    if (igsa_emit_raw(c, nm, c->d, len))
        goto out;
    r = 0;
out:
    for (i = 0; i < a.nblk; i++)
        free(a.blocks[i]);
    free(a.blocks);
    free(a.obj);
    free(x.p);
    return r;
}

const igsa_format igsa_fmt_osmenc = { "osmenc", "Hero of Robots .osmenc: encrypted boost archive of an OgreMax scene, to OSM XML", probe,
                                      parse };
