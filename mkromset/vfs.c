/*
 * The set tree: directories, files (contents in blobs), symlinks and modes,
 * with path lookup that follows the set's own symlinks the way the host
 * would inside the set.  mkromset.py built the same tree on disk; the
 * operations here keep its semantics (os.makedirs, open "wb", os.remove,
 * finish()).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MAX_HOPS 40

/* ---- nodes ------------------------------------------------------------------------------ */

static size_t hkey(const struct mkr_node *dir, const char *name)
{
    size_t h = (size_t)(uintptr_t)dir * 0x9E3779B1u;

    while (*name)
        h = (h ^ (unsigned char)*name++) * 0x01000193u;
    return h;
}

static void hgrow(struct mkr_job *j)
{
    size_t n = j->hsize ? j->hsize * 2 : 4096, i;
    struct mkr_node **t = calloc(n, sizeof(*t));

    for (i = 0; i < j->hsize; i++) {
        struct mkr_node *e = j->hash[i], *nx;

        for (; e; e = nx) {
            size_t k = hkey(e->parent, e->name) & (n - 1);

            nx = e->hnext;
            e->hnext = t[k];
            t[k] = e;
        }
    }
    free(j->hash);
    j->hash = t;
    j->hsize = n;
}

int vfs_init(struct mkr_job *j)
{
    j->root = calloc(1, sizeof(*j->root));
    j->root->name = strdup("");
    j->root->type = N_DIR;
    j->root->mode = 0755;
    hgrow(j);
    return 0;
}

static void free_node(struct mkr_node *n)
{
    int i;

    for (i = 0; i < n->nkids; i++)
        free_node(n->kids[i]);
    free(n->kids);
    free(n->name);
    free(n->target);
    free(n);
}

void vfs_free(struct mkr_job *j)
{
    if (j->root)
        free_node(j->root);
    j->root = NULL;
    free(j->hash);
    j->hash = NULL;
    j->hsize = j->hcount = 0;
}

struct mkr_node *vfs_child(struct mkr_job *j, struct mkr_node *dir, const char *name)
{
    struct mkr_node *e;

    if (!dir || dir->type != N_DIR)
        return NULL;
    for (e = j->hash[hkey(dir, name) & (j->hsize - 1)]; e; e = e->hnext)
        if (e->parent == dir && !strcmp(e->name, name))
            return e;
    return NULL;
}

struct mkr_node *vfs_new(struct mkr_job *j, struct mkr_node *dir, const char *name, int type, unsigned mode)
{
    struct mkr_node *n = calloc(1, sizeof(*n));
    size_t k;

    n->name = strdup(name);
    n->type = type;
    n->mode = mode & 07777;
    n->parent = dir;
    if (dir->nkids == dir->capkids) {
        dir->capkids = dir->capkids ? dir->capkids * 2 : 8;
        dir->kids = realloc(dir->kids, dir->capkids * sizeof(*dir->kids));
    }
    dir->kids[dir->nkids++] = n;
    if (++j->hcount > j->hsize)
        hgrow(j);
    k = hkey(dir, name) & (j->hsize - 1);
    n->hnext = j->hash[k];
    j->hash[k] = n;
    return n;
}

static void blob_path(const struct mkr_job *j, uint32_t blob, char *out, size_t n)
{
#ifdef _WIN32
    snprintf(out, n, "%s\\%x", j->blobdir, (unsigned)blob);
#else
    snprintf(out, n, "%s/%x", j->blobdir, (unsigned)blob);
#endif
}

/* a node's path under t/ */
static void tree_path(const struct mkr_job *j, const struct mkr_node *n, char *out, size_t size)
{
    char p[MKR_PATH];

    vfs_path(n, p, sizeof(p));
    snprintf(out, size, "%s%s", j->tree, n->parent ? p : "");
}

void vfs_file_path(const struct mkr_job *j, const struct mkr_node *f, char *out, size_t n)
{
    if (f->inplace)
        tree_path(j, f, out, n);
    else
        blob_path(j, f->blob, out, n);
}

static void drop_blob(struct mkr_job *j, struct mkr_node *f)
{
    char p[MKR_PATH];

    if (f->blob) {
        vfs_file_path(j, f, p, sizeof(p));
        remove(p);
    }
    f->blob = 0;
    f->size = 0;
    f->inplace = 0;
}

#ifndef _WIN32
/* a directory of the tree made under t/ (its parents first) */
static int ondisk(struct mkr_job *j, struct mkr_node *d)
{
    char p[MKR_PATH];

    if (d->ondisk || !d->parent)
        return 0;
    if (ondisk(j, d->parent))
        return -1;
    tree_path(j, d, p, sizeof(p));
    if (mkdir(p, 0755) && errno != EEXIST)
        return -1;
    d->ondisk = 1;
    return 0;
}
#endif

static void unhash(struct mkr_job *j, struct mkr_node *n)
{
    struct mkr_node **pp = &j->hash[hkey(n->parent, n->name) & (j->hsize - 1)];

    for (; *pp; pp = &(*pp)->hnext)
        if (*pp == n) {
            *pp = n->hnext;
            j->hcount--;
            return;
        }
}

static void remove_rec(struct mkr_job *j, struct mkr_node *n)
{
    int i;

    for (i = 0; i < n->nkids; i++) {
        remove_rec(j, n->kids[i]);
        unhash(j, n->kids[i]);
        n->kids[i]->nkids = 0;          /* already freed below */
        free_node(n->kids[i]);
    }
    n->nkids = 0;
    if (n->type == N_FILE)
        drop_blob(j, n);
#ifndef _WIN32
    if (n->type == N_DIR && n->ondisk) {
        char p[MKR_PATH];

        tree_path(j, n, p, sizeof(p));
        rmdir(p);
    }
#endif
}

void vfs_remove(struct mkr_job *j, struct mkr_node *n)
{
    struct mkr_node *d = n->parent;
    int i;

    if (!d)
        return;
    remove_rec(j, n);
    unhash(j, n);
    for (i = 0; i < d->nkids; i++)
        if (d->kids[i] == n) {
            d->kids[i] = d->kids[--d->nkids];
            break;
        }
    free_node(n);
}

void vfs_clear(struct mkr_job *j, struct mkr_node *dir)
{
    while (dir->nkids)
        vfs_remove(j, dir->kids[dir->nkids - 1]);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp((*(struct mkr_node *const *)a)->name, (*(struct mkr_node *const *)b)->name);
}

struct mkr_node **vfs_sorted(struct mkr_node *dir)
{
    struct mkr_node **v = malloc((dir->nkids + 1) * sizeof(*v));

    memcpy(v, dir->kids, dir->nkids * sizeof(*v));
    qsort(v, dir->nkids, sizeof(*v), name_cmp);
    return v;
}

void vfs_path(const struct mkr_node *n, char *out, size_t size)
{
    const struct mkr_node *c;
    size_t len = 0, pos;

    if (!n->parent) {
        snprintf(out, size, "/");
        return;
    }
    for (c = n; c->parent; c = c->parent)
        len += 1 + strlen(c->name);
    if (len + 1 > size) {
        snprintf(out, size, "/...");
        return;
    }
    out[len] = 0;
    pos = len;
    for (c = n; c->parent; c = c->parent) {
        size_t l = strlen(c->name);

        pos -= l;
        memcpy(out + pos, c->name, l);
        out[--pos] = '/';
    }
}

/* ---- lookup ----------------------------------------------------------------------------- */

static struct mkr_node *walk(struct mkr_job *j, struct mkr_node *start, const char *path, int follow, int *hops)
{
    struct mkr_node *cur = path[0] == '/' ? j->root : start;
    const char *p = path;

    while (*p) {
        const char *e;
        char c[1024];
        size_t l;
        struct mkr_node *n;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        e = strchr(p, '/');
        l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof(c))
            return NULL;
        memcpy(c, p, l);
        c[l] = 0;
        p += l;
        if (cur->type != N_DIR)
            return NULL;
        if (!strcmp(c, "."))
            continue;
        if (!strcmp(c, "..")) {
            if (cur->parent)
                cur = cur->parent;
            continue;
        }
        if (!(n = vfs_child(j, cur, c)))
            return NULL;
        while (*p == '/')
            p++;
        if (n->type == N_LINK && (*p || follow)) {
            if (++*hops > MAX_HOPS)
                return NULL;
            if (!(n = walk(j, cur, n->target, 1, hops)))
                return NULL;
        }
        cur = n;
    }
    return cur;
}

struct mkr_node *vfs_lookup(struct mkr_job *j, const char *path, int follow)
{
    int hops = 0;

    return walk(j, j->root, path, follow, &hops);
}

struct mkr_node *vfs_follow(struct mkr_job *j, struct mkr_node *n)
{
    int hops = 0;

    if (!n || n->type != N_LINK)
        return n;
    return walk(j, n->parent, n->target, 1, &hops);
}

struct mkr_node *vfs_mkdirs(struct mkr_job *j, const char *path)
{
    struct mkr_node *cur = j->root;
    const char *p = path;

    while (*p) {
        const char *e;
        char c[1024];
        size_t l;
        struct mkr_node *n;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        e = strchr(p, '/');
        l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof(c)) {
            mkr_fail(j, "path too long: %s", path);
            return NULL;
        }
        memcpy(c, p, l);
        c[l] = 0;
        p += l;
        if (!strcmp(c, "."))
            continue;
        if (!strcmp(c, "..")) {
            if (cur->parent)
                cur = cur->parent;
            continue;
        }
        n = vfs_child(j, cur, c);
        if (!n)
            n = vfs_new(j, cur, c, N_DIR, 0755);
        else if (n->type == N_LINK)
            n = vfs_follow(j, n);
        if (!n || n->type != N_DIR) {
            mkr_fail(j, "%s: not a directory", path);
            return NULL;
        }
        cur = n;
    }
    return cur;
}

struct mkr_node *vfs_parent(struct mkr_job *j, const char *path, char *name, size_t n, int make)
{
    char dir[MKR_PATH], *s;
    size_t l;
    struct mkr_node *d;

    snprintf(dir, sizeof(dir), "%s", path);
    for (l = strlen(dir); l > 1 && dir[l - 1] == '/'; l--)
        dir[l - 1] = 0;
    s = strrchr(dir, '/');
    snprintf(name, n, "%s", s ? s + 1 : dir);
    if (s)
        *s = 0;
    else
        dir[0] = 0;
    if (!name[0]) {
        mkr_fail(j, "%s: no file name", path);
        return NULL;
    }
    d = make ? vfs_mkdirs(j, dir) : vfs_lookup(j, dir, 1);
    if (!d || d->type != N_DIR) {
        if (!make)
            mkr_fail(j, "%s: no such directory", path);
        return NULL;
    }
    return d;
}

/* ---- blobs ------------------------------------------------------------------------------ */

int vfs_wopen(struct mkr_job *j, struct mkr_w *w, struct mkr_node *dir, const char *name)
{
    w->blob = ++j->nblob;
    w->size = 0;
    w->inplace = 0;
    w->f = NULL;
#ifndef _WIN32
    /* a new name: the file goes where it belongs (a name already there, a link to
       follow or a file being replaced, takes a blob) */
    if (dir && dir->type == N_DIR && !vfs_child(j, dir, name) && !ondisk(j, dir)) {
        char d[MKR_PATH];

        tree_path(j, dir, d, sizeof(d));
        snprintf(w->path, sizeof(w->path), "%s/%s", d, name);
        if ((w->f = fopen(w->path, "wb")))
            w->inplace = 1;
    }
#else
    (void)dir;
    (void)name;
#endif
    if (!w->f) {
        blob_path(j, w->blob, w->path, sizeof(w->path));
        if (!(w->f = fopen(w->path, "wb")))
            return mkr_fail(j, "cannot create %s: %s", w->path, strerror(errno));
    }
    setvbuf(w->f, NULL, _IOFBF, 1 << 20);
    return 0;
}

int vfs_write(struct mkr_job *j, struct mkr_w *w, const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, w->f) != n)
        return mkr_fail(j, "write error in %s (disk full?)", j->blobdir);
    w->size += n;
    return mkr_progress(j, n);
}

int vfs_wzero(struct mkr_job *j, struct mkr_w *w, uint64_t n)
{
    static const uint8_t z[65536];

    while (n) {
        size_t k = n < sizeof(z) ? (size_t)n : sizeof(z);

        if (vfs_write(j, w, z, k))
            return -1;
        n -= k;
    }
    return 0;
}

void vfs_wabort(struct mkr_job *j, struct mkr_w *w)
{
    (void)j;
    if (w->f)
        fclose(w->f);
    w->f = NULL;
    remove(w->path);
}

static int wclose(struct mkr_job *j, struct mkr_w *w)
{
    int bad = fclose(w->f) != 0;

    w->f = NULL;
    if (bad) {
        vfs_wabort(j, w);
        return mkr_fail(j, "write error in %s (disk full?)", w->path);
    }
    if (!w->size) {
        /* empty files have no blob */
        remove(w->path);
        w->blob = 0;
        w->inplace = 0;
    }
    return 0;
}

struct mkr_node *vfs_put(struct mkr_job *j, struct mkr_node *dir, const char *name, struct mkr_w *w, unsigned mode)
{
    struct mkr_node *e = vfs_child(j, dir, name);

    if (e && e->type == N_LINK) {
        struct mkr_node *t = vfs_follow(j, e);

        if (t && t->type == N_FILE)
            e = t;
        else if (!t) {
            vfs_remove(j, e);
            e = NULL;
        }
    }
    if (e && e->type == N_DIR) {
        vfs_wabort(j, w);
        mkr_fail(j, "%s: is a directory", name);
        return NULL;
    }
    if (wclose(j, w))
        return NULL;
    if (e)
        drop_blob(j, e);
    else
        e = vfs_new(j, dir, name, N_FILE, mode);
    e->blob = w->blob;
    e->size = w->size;
    e->inplace = w->inplace;
    e->mode = mode & 07777;
    return e;
}

struct mkr_node *vfs_link(struct mkr_job *j, struct mkr_node *dir, const char *name, const char *target)
{
    struct mkr_node *e = vfs_child(j, dir, name);

    if (e && e->type == N_DIR) {
        mkr_fail(j, "%s: is a directory", name);
        return NULL;
    }
    if (e)
        vfs_remove(j, e);
    e = vfs_new(j, dir, name, N_LINK, 0777);
    e->target = strdup(target);
    return e;
}

struct mkr_node *vfs_dir(struct mkr_job *j, struct mkr_node *dir, const char *name, unsigned mode)
{
    struct mkr_node *e = vfs_child(j, dir, name);

    if (!e)
        return vfs_new(j, dir, name, N_DIR, mode);
    if (e->type == N_LINK)
        e = vfs_follow(j, e);
    if (!e || e->type != N_DIR) {
        mkr_fail(j, "%s: exists and is not a directory", name);
        return NULL;
    }
    return e;
}

FILE *vfs_ropen(struct mkr_job *j, const struct mkr_node *f)
{
    char p[MKR_PATH];
    FILE *fp;

    if (!f->blob)
        return NULL;
    vfs_file_path(j, f, p, sizeof(p));
    if (!(fp = fopen(p, "rb")))
        mkr_fail(j, "cannot open %s", p);
    else
        setvbuf(fp, NULL, _IOFBF, 1 << 20);
    return fp;
}

int vfs_load(struct mkr_job *j, const struct mkr_node *f, uint8_t **data, size_t *n)
{
    FILE *fp;

    *n = (size_t)f->size;
    *data = malloc(*n + 16);
    if (!*data)
        return mkr_fail(j, "out of memory");
    if (!f->blob)
        return 0;
    if (!(fp = vfs_ropen(j, f))) {
        free(*data);
        return -1;
    }
    if (fread(*data, 1, *n, fp) != *n) {
        fclose(fp);
        free(*data);
        return mkr_fail(j, "read error in %s", j->blobdir);
    }
    fclose(fp);
    return 0;
}

int vfs_save(struct mkr_job *j, const char *path, const uint8_t *data, size_t n)
{
    char name[1024];
    struct mkr_node *d = vfs_parent(j, path, name, sizeof(name), 1), *e;
    struct mkr_w w;

    if (!d)
        return -1;
    if ((e = vfs_child(j, d, name))) {
        if (e->type == N_DIR)
            return mkr_fail(j, "%s: is a directory", path);
        vfs_remove(j, e);
    }
    if (vfs_wopen(j, &w, d, name))
        return -1;
    if (vfs_write(j, &w, data, n)) {
        vfs_wabort(j, &w);
        return -1;
    }
    return vfs_put(j, d, name, &w, 0755) ? 0 : -1;
}

/* ---- finish ----------------------------------------------------------------------------- */

/*
 * os.path.relpath(normpath(<stage>/<target>), <stage><dir>): the components
 * of the stage's absolute path matter only for links that climb out of it.
 */
struct comps { char **v; int n, cap; };

static void cpush(struct comps *c, const char *s, size_t l)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->v = realloc(c->v, c->cap * sizeof(*c->v));
    }
    c->v[c->n] = malloc(l + 1);
    memcpy(c->v[c->n], s, l);
    c->v[c->n++][l] = 0;
}

static void cfree(struct comps *c)
{
    while (c->n)
        free(c->v[--c->n]);
    free(c->v);
    c->v = NULL;
    c->cap = 0;
}

/* append a path's components with normpath rules (absolute: ".." stops at the root) */
static void cadd(struct comps *c, const char *p, int seps2)
{
    while (*p) {
        const char *e = p;

        while (*e && *e != '/' && !(seps2 && *e == '\\'))
            e++;
        if (e == p || (e - p == 1 && *p == '.')) {
        } else if (e - p == 2 && p[0] == '.' && p[1] == '.') {
            if (c->n) {
                free(c->v[--c->n]);
            }
        } else {
            cpush(c, p, e - p);
        }
        p = *e ? e + 1 : e;
    }
}

static void stage_comps(const struct mkr_job *j, struct comps *c)
{
    char cwd[MKR_PATH] = "";
    const char *s = j->stage;

#ifdef _WIN32
    if (!(s[0] && s[1] == ':') && s[0] != '\\' && s[0] != '/' && _getcwd(cwd, sizeof(cwd)))
        cadd(c, cwd, 1);
#else
    if (s[0] != '/' && getcwd(cwd, sizeof(cwd)))
        cadd(c, cwd, 0);
#endif
#ifdef _WIN32
    cadd(c, s, 1);
#else
    cadd(c, s, 0);
#endif
}

static char *relpath_for(const struct comps *stage, const struct mkr_node *link)
{
    struct comps a = { 0 }, d = { 0 };
    char dirp[MKR_PATH], *out;
    size_t len = 0;
    int i, k, common = 0;

    for (i = 0; i < stage->n; i++) {
        cpush(&a, stage->v[i], strlen(stage->v[i]));
        cpush(&d, stage->v[i], strlen(stage->v[i]));
    }
    cadd(&a, link->target, 0);
    vfs_path(link->parent, dirp, sizeof(dirp));
    cadd(&d, dirp, 0);
    while (common < a.n && common < d.n && !strcmp(a.v[common], d.v[common]))
        common++;
    for (i = common; i < d.n; i++)
        len += 3;
    for (k = common; k < a.n; k++)
        len += strlen(a.v[k]) + 1;
    out = malloc(len + 2);
    out[0] = 0;
    for (i = common; i < d.n; i++)
        strcat(out, i > common ? "/.." : "..");
    for (k = common; k < a.n; k++) {
        if (out[0])
            strcat(out, "/");
        strcat(out, a.v[k]);
    }
    if (!out[0])
        strcpy(out, ".");
    cfree(&a);
    cfree(&d);
    return out;
}

static int finish_dir(struct mkr_job *j, const struct comps *stage, struct mkr_node *dir)
{
    struct mkr_node **v;
    int i, n, fixed = 0;

    /* lost+found: os.walk lists it with the directories (a link to one too); rmtree
       removes a real one, then it is neither visited nor fixed */
    v = malloc((dir->nkids + 1) * sizeof(*v));
    memcpy(v, dir->kids, dir->nkids * sizeof(*v));
    n = dir->nkids;
    for (i = 0; i < n; i++) {
        struct mkr_node *t;

        if (strcmp(v[i]->name, "lost+found"))
            continue;
        t = vfs_follow(j, v[i]);
        if (!t || t->type != N_DIR)
            continue;
        if (v[i]->type == N_DIR)
            vfs_remove(j, v[i]);
        v[i] = NULL;
    }
    for (i = 0; i < n; i++)
        if (v[i] && v[i]->type == N_LINK && v[i]->target[0] == '/') {
            char *r = relpath_for(stage, v[i]);

            free(v[i]->target);
            v[i]->target = r;
            fixed++;
        }
    for (i = 0; i < n; i++)
        if (v[i] && v[i]->type == N_DIR)
            fixed += finish_dir(j, stage, v[i]);
    free(v);
    return fixed;
}

int vfs_finish(struct mkr_job *j)
{
    struct comps s = { 0 };
    int n;

    stage_comps(j, &s);
    n = finish_dir(j, &s, j->root);
    cfree(&s);
    return n;
}

/* ---- the folder set --------------------------------------------------------------------- */

static void host_join(char *out, size_t n, const char *base, const struct mkr_node *node)
{
    char p[MKR_PATH], *c;

    vfs_path(node, p, sizeof(p));
    snprintf(out, n, "%s%s", base, node->parent ? p : "");
#ifdef _WIN32
    for (c = out; *c; c++)
        if (*c == '/')
            *c = '\\';
#else
    (void)c;
#endif
}

static int place(struct mkr_job *j, const char *base, struct mkr_node *n)
{
    char p[MKR_PATH];
    int i;

    if (mkr_check(j))
        return -1;
    host_join(p, sizeof(p), base, n);
    if (n->type == N_DIR) {
        if (n->parent && !n->ondisk && mkr_mkdirs(p))
            return mkr_fail(j, "cannot create %s", p);
        for (i = 0; i < n->nkids; i++)
            if (place(j, base, n->kids[i]))
                return -1;
#ifndef _WIN32
        /* after the children: the mode may not allow writing */
        chmod(p, n->mode);
#endif
        return 0;
    }
    if (n->type == N_FILE) {
        if (n->inplace) {
            /* already there */
        } else if (n->blob) {
            char b[MKR_PATH];

            blob_path(j, n->blob, b, sizeof(b));
            if (mkr_rename(b, p))
                return mkr_fail(j, "cannot move %s to %s", b, p);
            n->blob = 0;        /* moved: not the stage's to delete any more */
        } else {
            FILE *f = fopen(p, "wb");

            if (!f)
                return mkr_fail(j, "cannot create %s", p);
            fclose(f);
        }
#ifndef _WIN32
        chmod(p, n->mode);
#endif
        return 0;
    }
#ifndef _WIN32
    if (symlink(n->target, p))
        return mkr_fail(j, "cannot create symlink %s: %s", p, strerror(errno));
#endif
    return 0;
}

#ifdef _WIN32
/*
 * Windows: links become copies of their targets (resolved in the set; links
 * to directories copied as trees, links into themselves and dangling links
 * skipped), as romset/ does when it extracts an archive set.
 */
struct copies { long long copied, skipped; };

static void copy_node(struct mkr_job *j, const char *base, const struct mkr_node *src, const char *dst, int depth,
                      struct copies *n)
{
    char s[MKR_PATH], d[MKR_PATH];
    int i;

    host_join(s, sizeof(s), base, src);
    if (src->type == N_FILE) {
        if (CopyFileA(s, dst, FALSE))
            n->copied++;
        return;
    }
    if (src->type != N_DIR || depth > 8)
        return;
    mkr_mkdirs(dst);
    for (i = 0; i < src->nkids; i++) {
        const struct mkr_node *k = src->kids[i];

        snprintf(d, sizeof(d), "%s\\%s", dst, k->name);
        if (k->type == N_LINK) {
            struct mkr_node *t = vfs_follow(j, (struct mkr_node *)k);
            const struct mkr_node *a;

            /* not into a directory this copy is inside of */
            for (a = src; a && a != t; a = a->parent)
                ;
            if (t && !a)
                copy_node(j, base, t, d, depth + 1, n);
            else
                n->skipped++;
        } else {
            copy_node(j, base, k, d, depth, n);
        }
    }
}

static void copy_links(struct mkr_job *j, const char *base, struct mkr_node *n, struct copies *c)
{
    int i;

    for (i = 0; i < n->nkids; i++) {
        struct mkr_node *k = n->kids[i];

        if (k->type == N_DIR) {
            copy_links(j, base, k, c);
        } else if (k->type == N_LINK) {
            struct mkr_node *t = vfs_follow(j, k);
            const struct mkr_node *a;
            char d[MKR_PATH];

            /* a link to a directory above itself would copy forever */
            for (a = n; a && a != t; a = a->parent)
                ;
            host_join(d, sizeof(d), base, k);
            if (!t || a)
                c->skipped++;
            else
                copy_node(j, base, t, d, 0, c);
        }
    }
}
#endif

int vfs_materialise(struct mkr_job *j, const char *dir)
{
    /* dir is the stage's t/, where the files written in place already are */
    if (mkr_mkdirs(dir))
        return mkr_fail(j, "cannot create %s", dir);
    if (place(j, dir, j->root))
        return -1;
#ifdef _WIN32
    struct copies c = { 0, 0 };

    copy_links(j, dir, j->root, &c);
    mkr_log(j, MKR_INFO, "symlinks materialised as copies: %lld files copied, %lld links dangling or looping",
            c.copied, c.skipped);
#endif
    return 0;
}
