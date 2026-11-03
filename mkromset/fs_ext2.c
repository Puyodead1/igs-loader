/*
 * ext2/ext3 (and ext4 extents): every top-level entry of the filesystem but
 * lost+found, read the way "debugfs -R 'rdump /<name> <dir>'" extracted it
 * for mkromset.py: files, directories and symlinks, with the rwx bits of
 * their modes (debugfs's mode_xlate() drops setuid, setgid and sticky),
 * device nodes, fifos and sockets left out, hard links as copies.  The journal is not replayed (debugfs
 * does not either).
 */
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

#define EXT_MAGIC 0xEF53
#define F_EXTENTS 0x80000u
#define F_INLINE 0x10000000u
#define INCOMPAT_FILETYPE 0x2
#define INCOMPAT_64BIT 0x80

struct ext {
    struct mkr_job *j;
    struct mkr_img *m;
    uint64_t base;
    uint32_t bs, ipg, isize, first, ngroups, desc;
    int filetype, b64;
    uint8_t *gd;
    uint8_t *buf;               /* a run of blocks */
    size_t bufn;
};

struct einode {
    unsigned mode;
    uint64_t size;
    uint32_t flags, blocks512, acl;
    uint8_t blk[60];
};

/* a run of physical blocks for logical [lb, lb + n) (phys 0 = a hole) */
struct run { uint64_t lb, pb, n; };
struct runs { struct run *v; size_t n, cap; };

static void add_run(struct runs *r, uint64_t lb, uint64_t pb, uint64_t n)
{
    if (r->n && r->v[r->n - 1].lb + r->v[r->n - 1].n == lb && pb && r->v[r->n - 1].pb &&
        r->v[r->n - 1].pb + r->v[r->n - 1].n == pb) {
        r->v[r->n - 1].n += n;
        return;
    }
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->v = realloc(r->v, r->cap * sizeof(*r->v));
    }
    r->v[r->n].lb = lb;
    r->v[r->n].pb = pb;
    r->v[r->n].n = n;
    r->n++;
}

static int rblock(struct ext *e, uint64_t b, uint8_t *out)
{
    return img_read(e->j, e->m, e->base + b * e->bs, out, e->bs);
}

static int read_inode(struct ext *e, uint32_t ino, struct einode *in)
{
    uint32_t g = (ino - 1) / e->ipg, i = (ino - 1) % e->ipg;
    const uint8_t *d;
    uint64_t table;
    uint8_t b[256];

    if (!ino || g >= e->ngroups)
        return mkr_fail(e->j, "ext2: bad inode %u", ino);
    d = e->gd + (size_t)g * e->desc;
    table = rd32(d + 8) | (e->b64 && e->desc >= 64 ? (uint64_t)rd32(d + 0x28) << 32 : 0);
    if (img_read(e->j, e->m, e->base + table * e->bs + (uint64_t)i * e->isize, b, e->isize < 256 ? e->isize : 256))
        return -1;
    in->mode = rd16(b);
    in->size = rd32(b + 4) | (uint64_t)rd32(b + 108) << 32;
    in->blocks512 = rd32(b + 28);
    in->flags = rd32(b + 32);
    in->acl = rd32(b + 104);
    memcpy(in->blk, b + 40, 60);
    return 0;
}

/* the block map: 12 direct, then single, double and triple indirect */
static int map_ind(struct ext *e, uint32_t blk, int level, uint64_t *lb, uint64_t nblocks, struct runs *r)
{
    uint32_t per = e->bs / 4, i;
    uint64_t span = 1;
    uint8_t *b;
    int k;

    for (k = 0; k < level; k++)
        span *= per;
    if (!blk) {
        uint64_t n = span < nblocks - *lb ? span : nblocks - *lb;

        add_run(r, *lb, 0, n);
        *lb += n;
        return 0;
    }
    b = malloc(e->bs);
    if (rblock(e, blk, b)) {
        free(b);
        return -1;
    }
    for (i = 0; i < per && *lb < nblocks; i++) {
        uint32_t c = rd32(b + 4 * i);

        if (level == 1) {
            add_run(r, *lb, c, 1);
            (*lb)++;
        } else if (map_ind(e, c, level - 1, lb, nblocks, r)) {
            free(b);
            return -1;
        }
    }
    free(b);
    return 0;
}

static int map_ext(struct ext *e, const uint8_t *node, size_t len, struct runs *r, int depth)
{
    unsigned n, d, i;

    if (len < 12 || rd16(node) != 0xF30A || depth > 8)
        return mkr_fail(e->j, "ext4: bad extent tree");
    n = rd16(node + 2);
    d = rd16(node + 6);
    if (12 + 12ull * n > len)
        return mkr_fail(e->j, "ext4: bad extent node");
    for (i = 0; i < n; i++) {
        const uint8_t *x = node + 12 + 12 * i;

        if (!d) {
            uint32_t lb = rd32(x), l = rd16(x + 4);
            uint64_t pb = rd32(x + 8) | (uint64_t)rd16(x + 6) << 32;

            /* an uninitialised extent reads as zeros */
            if (l > 32768)
                add_run(r, lb, 0, l - 32768);
            else
                add_run(r, lb, pb, l);
        } else {
            uint64_t pb = rd32(x + 4) | (uint64_t)rd16(x + 8) << 32;
            uint8_t *b = malloc(e->bs);

            if (rblock(e, pb, b) || map_ext(e, b, e->bs, r, depth + 1)) {
                free(b);
                return -1;
            }
            free(b);
        }
    }
    return 0;
}

static int map(struct ext *e, const struct einode *in, struct runs *r)
{
    uint64_t nblocks = (in->size + e->bs - 1) / e->bs, lb = 0;
    int i;

    memset(r, 0, sizeof(*r));
    if (in->flags & F_INLINE)
        return mkr_fail(e->j, "ext4 inline data is not supported");
    if (in->flags & F_EXTENTS)
        return map_ext(e, in->blk, 60, r, 0);
    for (i = 0; i < 12 && lb < nblocks; i++, lb++)
        add_run(r, lb, rd32(in->blk + 4 * i), 1);
    for (i = 1; i <= 3 && lb < nblocks; i++)
        if (map_ind(e, rd32(in->blk + 4 * (11 + i)), i, &lb, nblocks, r))
            return -1;
    return 0;
}

/* the first size bytes of an inode's data: into w, or into mem */
static int data(struct ext *e, const struct einode *in, struct mkr_w *w, uint8_t *mem)
{
    struct runs r;
    uint64_t pos = 0;
    size_t i;
    int rc = 0;

    if (map(e, in, &r))
        return -1;
    for (i = 0; i < r.n && pos < in->size && !rc; i++) {
        uint64_t at = r.v[i].lb * e->bs, end = (r.v[i].lb + r.v[i].n) * e->bs;

        if (end > in->size)
            end = in->size;
        if (at >= end)
            continue;
        if (at > pos) {                 /* a hole before the run */
            if (w)
                rc = vfs_wzero(e->j, w, at - pos);
            else
                memset(mem + pos, 0, (size_t)(at - pos));
            pos = at;
        }
        if (at < pos)
            continue;
        while (pos < end && !rc) {
            size_t k = end - pos < e->bufn ? (size_t)(end - pos) : e->bufn;

            if (!r.v[i].pb)
                memset(e->buf, 0, k);
            else
                rc = img_read(e->j, e->m, e->base + r.v[i].pb * e->bs + (pos - at), e->buf, k);
            if (!rc) {
                if (w)
                    rc = vfs_write(e->j, w, e->buf, k);
                else
                    memcpy(mem + pos, e->buf, k);
            }
            pos += k;
        }
    }
    if (!rc && pos < in->size) {
        if (w)
            rc = vfs_wzero(e->j, w, in->size - pos);
        else
            memset(mem + pos, 0, (size_t)(in->size - pos));
    }
    free(r.v);
    return rc;
}

/* ---- rdump ------------------------------------------------------------------------------ */

static int rdump(struct ext *e, uint32_t ino, struct mkr_node *dir, const char *name, int depth);

static int rdump_dir(struct ext *e, const struct einode *in, struct mkr_node *node, int depth)
{
    uint8_t *d;
    uint64_t p = 0;
    int rc = 0;

    if (in->size > (256u << 20))
        return mkr_fail(e->j, "ext2: directory too large");
    d = malloc((size_t)in->size + 1);
    if (data(e, in, NULL, d)) {
        free(d);
        return -1;
    }
    while (!rc && p + 8 <= in->size) {
        uint32_t ino = rd32(d + p), rec = rd16(d + p + 4), nl = e->filetype ? d[p + 6] : rd16(d + p + 6);
        char name[256];

        if (rec < 8 || p + rec > in->size)
            break;
        if (ino && nl && 8 + nl <= rec) {
            memcpy(name, d + p + 8, nl);
            name[nl] = 0;
            if (strcmp(name, ".") && strcmp(name, ".."))
                rc = rdump(e, ino, node, name, depth + 1);
        }
        p += rec;
    }
    free(d);
    return rc;
}

static int rdump(struct ext *e, uint32_t ino, struct mkr_node *dir, const char *name, int depth)
{
    struct mkr_job *j = e->j;
    struct einode in;
    unsigned kind;

    if (depth > 200)
        return mkr_fail(j, "ext2: directories nested too deep");
    if (read_inode(e, ino, &in))
        return -1;
    kind = in.mode & 0xF000;
    if (kind == 0xA000) {
        char t[4097];
        struct mkr_node *n;

        if (in.size > 4096)
            return mkr_fail(j, "ext2: symlink target too long");
        if (in.size && in.size < 60) {
            /* a fast symlink: the target is in i_block */
            memcpy(t, in.blk, (size_t)in.size);
            t[in.size] = 0;
        } else {
            if (data(e, &in, NULL, (uint8_t *)t))
                return -1;
            t[in.size] = 0;
        }
        n = vfs_link(j, dir, name, t);
        return n ? 0 : -1;
    }
    if (kind == 0x8000) {
        struct mkr_w w;

        if (vfs_wopen(j, &w, dir, name))
            return -1;
        if (data(e, &in, &w, NULL)) {
            vfs_wabort(j, &w);
            return -1;
        }
        return vfs_put(j, dir, name, &w, in.mode & 0777) ? mkr_check(j) : -1;
    }
    if (kind == 0x4000) {
        struct mkr_node *d = vfs_dir(j, dir, name, 0700);

        if (!d || rdump_dir(e, &in, d, depth))
            return -1;
        d->mode = in.mode & 0777;
        return 0;
    }
    return 0;                           /* device files, sockets, fifos: not dumped */
}

int fs_ext2(struct mkr_job *j, struct mkr_img *m, uint64_t off, struct mkr_node *dir)
{
    struct ext e;
    uint8_t sb[1024];
    uint32_t blocks, rev, incompat, gdblock;
    struct einode root;
    int rc;

    if (img_read(j, m, off + 1024, sb, 1024))
        return -1;
    if (rd16(sb + 56) != EXT_MAGIC)
        return mkr_fail(j, "not an ext2/3 filesystem");
    memset(&e, 0, sizeof(e));
    e.j = j;
    e.m = m;
    e.base = off;
    if (rd32(sb + 24) > 6)
        return mkr_fail(j, "ext2: bad block size");
    e.bs = 1024u << rd32(sb + 24);
    blocks = rd32(sb + 4);
    e.first = rd32(sb + 20);
    e.ipg = rd32(sb + 40);
    rev = rd32(sb + 76);
    e.isize = rev >= 1 ? rd16(sb + 88) : 128;
    incompat = rev >= 1 ? rd32(sb + 96) : 0;
    e.filetype = !!(incompat & INCOMPAT_FILETYPE);
    e.b64 = !!(incompat & INCOMPAT_64BIT);
    e.desc = e.b64 && rd16(sb + 254) >= 32 ? rd16(sb + 254) : 32;
    if (!e.ipg || e.isize < 128 || !rd32(sb + 32))
        return mkr_fail(j, "ext2: bad superblock");
    e.ngroups = (blocks - e.first + rd32(sb + 32) - 1) / rd32(sb + 32);
    gdblock = e.first + 1;
    e.gd = malloc((size_t)e.ngroups * e.desc + e.bs);
    if (img_read(j, m, off + (uint64_t)gdblock * e.bs, e.gd, (size_t)e.ngroups * e.desc)) {
        free(e.gd);
        return -1;
    }
    e.bufn = 4u << 20;
    e.buf = malloc(e.bufn);
    rc = read_inode(&e, 2, &root);
    if (!rc && (root.mode & 0xF000) != 0x4000)
        rc = mkr_fail(j, "ext2: the root is not a directory");
    if (!rc) {
        /* debugfs "ls -p /": the top-level names, each rdumped but lost+found */
        uint8_t *d = malloc((size_t)root.size + 1);
        uint64_t p = 0;

        rc = data(&e, &root, NULL, d);
        while (!rc && p + 8 <= root.size) {
            uint32_t ino = rd32(d + p), rec = rd16(d + p + 4), nl = e.filetype ? d[p + 6] : rd16(d + p + 6);
            char name[256];

            if (rec < 8 || p + rec > root.size)
                break;
            if (ino && nl && 8 + nl <= rec) {
                memcpy(name, d + p + 8, nl);
                name[nl] = 0;
                if (strcmp(name, ".") && strcmp(name, "..") && strcmp(name, "lost+found"))
                    rc = rdump(&e, ino, dir, name, 0);
            }
            p += rec;
        }
        free(d);
    }
    free(e.buf);
    free(e.gd);
    return rc;
}
