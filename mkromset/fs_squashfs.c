/*
 * SquashFS: version 4 (E2100 p3, E2200's ROMS/<name>.rom loop images), read the
 * way unsquashfs -n -q [-f] extracted it for mkromset.py, and IGS_SHFS (E2000
 * p3), SquashFS 2.x behind an IGS superblock, a port of romtool/igs_shfs.py.
 * Both share the metadata blocks: a 16-bit header (0x8000 = stored, low 15
 * bits the size), at most 8 KiB each.
 *
 * unsquashfs, run as a user with umask 022, decided the modes mkromset.py's
 * sets have: directories get their mode without setuid/setgid; files too
 * with -f (mount), without it (loop) the mode they were created with, mode
 * & 0755, unless they have the sticky bit.  Device nodes, fifos and sockets
 * are left out (counted).  Hard links become copies.
 */
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

#include "7zCrc.h"
#include "LzmaDec.h"
#include "Xz.h"
#include "XzCrc64.h"
#include "zlib.h"
#include "zstd.h"
#include "lzokay-c.h"

#define META 8192

enum { C_GZIP = 1, C_LZMA, C_LZO, C_XZ, C_LZ4, C_ZSTD };

struct mblock { uint64_t off, next; uint8_t *data; size_t n; struct mblock *hnext; };

struct sq {
    struct mkr_job *j;
    struct mkr_img *m;
    uint64_t base;
    int comp;                   /* C_GZIP...; SHFS: gzip */
    uint32_t block_size;
    struct mblock **cache;      /* metadata blocks by offset */
    size_t csize;
    uint8_t *ibuf, *obuf;       /* a data block, compressed and not */
    int dirs, files, links, other;
    uint64_t bytes;
};

/* ---- decompression ---------------------------------------------------------------------- */

static void *sz_alloc(ISzAllocPtr p, size_t n) { (void)p; return malloc(n); }
static void sz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc alloc = { sz_alloc, sz_free };

static const char *comp_name(int c)
{
    static const char *const n[] = { "?", "gzip", "lzma", "lzo", "xz", "lz4", "zstd" };

    return c >= 1 && c <= 6 ? n[c] : "unknown";
}

/* the whole of src into dst (at most cap); the size out or -1 */
static long long unpack(struct sq *s, const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    switch (s->comp) {
    case C_GZIP: {
        z_stream z;
        int e;
        long long out;

        memset(&z, 0, sizeof(z));
        inflateInit(&z);
        z.next_in = (Bytef *)src;
        z.avail_in = (uInt)n;
        z.next_out = dst;
        z.avail_out = (uInt)cap;
        e = inflate(&z, Z_FINISH);
        out = (long long)(cap - z.avail_out);
        inflateEnd(&z);
        return e == Z_STREAM_END ? out : -1;
    }
    case C_LZMA: {
        /* lzma-alone: 5 property bytes, the 64-bit size, the data */
        SizeT dl = cap, sl;
        ELzmaStatus st;

        if (n < 13)
            return -1;
        sl = n - 13;
        if (LzmaDecode(dst, &dl, src + 13, &sl, src, 5, LZMA_FINISH_ANY, &st, &alloc) != SZ_OK)
            return -1;
        return (long long)dl;
    }
    case C_XZ: {
        CXzUnpacker x;
        SizeT dl = cap, sl = n;
        ECoderStatus st;
        SRes r;

        XzUnpacker_Construct(&x, &alloc);
        XzUnpacker_Init(&x);
        r = XzUnpacker_Code(&x, dst, &dl, src, &sl, 1, CODER_FINISH_END, &st);
        if (r == SZ_OK && !XzUnpacker_IsStreamWasFinished(&x))
            r = SZ_ERROR_DATA;
        XzUnpacker_Free(&x);
        return r == SZ_OK ? (long long)dl : -1;
    }
    case C_ZSTD: {
        size_t r = ZSTD_decompress(dst, cap, src, n);

        return ZSTD_isError(r) ? -1 : (long long)r;
    }
    case C_LZO: {
        /* LZO1X, the kernel's lzo1x_decompress_safe (third_party/lzokay) */
        size_t out = cap;

        return lzokay_decompress(src, n, dst, &out) == EResult_Success ? (long long)out : -1;
    }
    }
    return -1;
}

/* ---- metadata --------------------------------------------------------------------------- */

static int meta(struct sq *s, uint64_t off, struct mblock **out)
{
    struct mkr_job *j = s->j;
    size_t k = (size_t)(off * 0x9E3779B97F4A7C15ull >> 40) & (s->csize - 1);
    struct mblock *b;
    uint8_t h[2], raw[META + 16];
    unsigned l;

    for (b = s->cache[k]; b; b = b->hnext)
        if (b->off == off) {
            *out = b;
            return 0;
        }
    if (img_read(j, s->m, s->base + off, h, 2))
        return -1;
    l = rd16(h) & 0x7FFF;
    if (l > META + 16)
        return mkr_fail(j, "SquashFS: bad metadata block at 0x%llX", (unsigned long long)off);
    if (img_read(j, s->m, s->base + off + 2, raw, l))
        return -1;
    b = calloc(1, sizeof(*b));
    b->off = off;
    b->next = off + 2 + l;
    b->data = malloc(META);
    if (rd16(h) & 0x8000) {
        memcpy(b->data, raw, l);
        b->n = l;
    } else {
        long long got = unpack(s, raw, l, b->data, META);

        if (got < 0) {
            free(b->data);
            free(b);
            return mkr_fail(j, "SquashFS: metadata block at 0x%llX does not decompress (%s)", (unsigned long long)off,
                            comp_name(s->comp));
        }
        b->n = (size_t)got;
    }
    b->hnext = s->cache[k];
    s->cache[k] = b;
    *out = b;
    return 0;
}

/* n bytes of a metadata table from (block, offset) */
static int mread(struct sq *s, uint64_t table, uint64_t block, uint32_t offset, uint8_t *out, size_t n)
{
    uint64_t off = table + block;
    size_t skip = offset, got = 0;

    while (got < n) {
        struct mblock *b;
        size_t k;

        if (meta(s, off, &b))
            return -1;
        if (!b->n && b->next == off + 2)
            return mkr_fail(s->j, "SquashFS: empty metadata block");
        if (skip >= b->n) {
            skip -= b->n;
        } else {
            k = b->n - skip < n - got ? b->n - skip : n - got;
            memcpy(out + got, b->data + skip, k);
            got += k;
            skip = 0;
        }
        off = b->next;
    }
    return 0;
}

static void sq_free(struct sq *s)
{
    size_t i;

    for (i = 0; i < s->csize; i++) {
        struct mblock *b = s->cache[i], *nx;

        for (; b; b = nx) {
            nx = b->hnext;
            free(b->data);
            free(b);
        }
    }
    free(s->cache);
    free(s->ibuf);
    free(s->obuf);
}

static void sq_init(struct sq *s, struct mkr_job *j, struct mkr_img *m, uint64_t base)
{
    memset(s, 0, sizeof(*s));
    s->j = j;
    s->m = m;
    s->base = base;
    s->csize = 4096;
    s->cache = calloc(s->csize, sizeof(*s->cache));
}

/* a data block (or fragment block): size word -> s->obuf; the bytes out, or -1 */
static long long data_block(struct sq *s, uint64_t off, uint32_t word, uint32_t want)
{
    uint32_t l = word & 0xFFFFFF;

    if (l > s->block_size + 4096)
        return mkr_fail(s->j, "SquashFS: bad block size word 0x%X", word);
    if (img_read(s->j, s->m, s->base + off, s->ibuf, l))
        return -1;
    if (word & (1u << 24)) {
        memcpy(s->obuf, s->ibuf, l);
        return l;
    }
    (void)want;
    {
        long long got = unpack(s, s->ibuf, l, s->obuf, s->block_size);

        if (got < 0)
            return mkr_fail(s->j, "SquashFS: data block at 0x%llX does not decompress (%s)", (unsigned long long)off,
                            comp_name(s->comp));
        return got;
    }
}

/* ======================================================================================== */
/* ---- SquashFS 4 ------------------------------------------------------------------------- */

struct sq4 {
    struct sq s;
    uint64_t inode_table, dir_table, frag_table;
    uint32_t frag_count;
    int force;
};

struct ino4 {
    unsigned type, mode;
    /* dir */
    uint32_t dstart, dsize;
    uint16_t doff;
    /* file */
    uint64_t bstart, fsize;
    uint32_t frag, foff, nblocks;
    uint32_t *blocks;
    /* symlink */
    char *target;
};

static int ino4(struct sq4 *q, uint64_t ref, struct ino4 *in)
{
    struct mkr_job *j = q->s.j;
    uint64_t block = ref >> 16;
    uint32_t off = (uint32_t)(ref & 0xFFFF), hdr;
    uint8_t b[64];

    memset(in, 0, sizeof(*in));
    if (mread(&q->s, q->inode_table, block, off, b, 16))
        return -1;
    in->type = rd16(b);
    in->mode = rd16(b + 2) & 07777;
    switch (in->type) {
    case 1:                                     /* dir */
        if (mread(&q->s, q->inode_table, block, off, b, 32))
            return -1;
        in->dstart = rd32(b + 16);
        in->dsize = rd16(b + 24);
        in->doff = rd16(b + 26);
        break;
    case 8:                                     /* extended dir */
        if (mread(&q->s, q->inode_table, block, off, b, 40))
            return -1;
        in->dsize = rd32(b + 20);
        in->dstart = rd32(b + 24);
        in->doff = rd16(b + 34);
        break;
    case 2:                                     /* file */
    case 9:                                     /* extended file */
        if (in->type == 2) {
            if (mread(&q->s, q->inode_table, block, off, b, 32))
                return -1;
            in->bstart = rd32(b + 16);
            in->frag = rd32(b + 20);
            in->foff = rd32(b + 24);
            in->fsize = rd32(b + 28);
            hdr = 32;
        } else {
            if (mread(&q->s, q->inode_table, block, off, b, 56))
                return -1;
            in->bstart = rd64(b + 16);
            in->fsize = rd64(b + 24);
            in->frag = rd32(b + 44);
            in->foff = rd32(b + 48);
            hdr = 56;
        }
        in->nblocks = (uint32_t)(in->frag == 0xFFFFFFFFu ? (in->fsize + q->s.block_size - 1) / q->s.block_size
                                                          : in->fsize / q->s.block_size);
        in->blocks = malloc(4ull * in->nblocks + 4);
        {
            uint8_t *raw = malloc(4ull * in->nblocks + 4);
            uint32_t i;

            if (mread(&q->s, q->inode_table, block, off + hdr, raw, 4ull * in->nblocks)) {
                free(raw);
                free(in->blocks);
                in->blocks = NULL;
                return -1;
            }
            for (i = 0; i < in->nblocks; i++)
                in->blocks[i] = rd32(raw + 4 * i);
            free(raw);
        }
        break;
    case 3:                                     /* symlink */
    case 10: {
        uint32_t l;
        uint8_t *t;

        if (mread(&q->s, q->inode_table, block, off, b, 24))
            return -1;
        l = rd32(b + 20);
        if (l > 65536)
            return mkr_fail(j, "SquashFS: bad symlink");
        t = malloc(l + 1);
        if (mread(&q->s, q->inode_table, block, off + 24, t, l)) {
            free(t);
            return -1;
        }
        t[l] = 0;
        in->target = (char *)t;
        break;
    }
    default:
        break;
    }
    return 0;
}

static int frag4(struct sq4 *q, uint32_t idx, uint64_t *start, uint32_t *size)
{
    uint8_t p[8], e[16];
    uint64_t at = q->frag_table + 8ull * (idx / 512);

    if (idx >= q->frag_count)
        return mkr_fail(q->s.j, "SquashFS: bad fragment %u", idx);
    if (img_read(q->s.j, q->s.m, q->s.base + at, p, 8))
        return -1;
    if (mread(&q->s, 0, rd64(p), (idx % 512) * 16, e, 16))
        return -1;
    *start = rd64(e);
    *size = rd32(e + 8);
    return 0;
}

static int file4(struct sq4 *q, struct ino4 *in, struct mkr_w *w)
{
    struct sq *s = &q->s;
    uint64_t off = in->bstart, left = in->fsize;
    uint32_t i;

    for (i = 0; i < in->nblocks && left; i++) {
        uint32_t want = left < s->block_size ? (uint32_t)left : s->block_size;
        long long got;

        if (!(in->blocks[i] & 0xFFFFFF)) {
            /* a sparse block */
            if (vfs_wzero(s->j, w, want))
                return -1;
            left -= want;
            continue;
        }
        if ((got = data_block(s, off, in->blocks[i], want)) < 0)
            return -1;
        if ((uint64_t)got < want)
            return mkr_fail(s->j, "SquashFS: short data block");
        if (vfs_write(s->j, w, s->obuf, want))
            return -1;
        off += in->blocks[i] & 0xFFFFFF;
        left -= want;
    }
    if (in->frag != 0xFFFFFFFFu && left) {
        uint64_t fs = 0;
        uint32_t fz = 0;
        long long got;

        if (frag4(q, in->frag, &fs, &fz))
            return -1;
        if ((got = data_block(s, fs, fz, 0)) < 0)
            return -1;
        if ((uint64_t)in->foff + left > (uint64_t)got)
            return mkr_fail(s->j, "SquashFS: fragment too short");
        if (vfs_write(s->j, w, s->obuf + in->foff, (size_t)left))
            return -1;
    }
    return 0;
}

static unsigned file_mode(const struct sq4 *q, unsigned mode)
{
    mode &= 01777;
    if (q->force || (mode & 01000))
        return mode;
    return mode & 0755;
}

static int dir4(struct sq4 *q, const struct ino4 *d, struct mkr_node *node, int depth)
{
    struct mkr_job *j = q->s.j;
    uint8_t *data;
    uint32_t p = 0, size;
    int rc = 0;

    node->mode = d->mode & 01777;
    if (depth > 200)
        return mkr_fail(j, "SquashFS: directories nested too deep");
    if (d->dsize <= 3)
        return 0;
    size = d->dsize - 3;
    data = malloc(size);
    if (mread(&q->s, q->dir_table, d->dstart, d->doff, data, size)) {
        free(data);
        return -1;
    }
    while (!rc && p + 12 <= size) {
        uint32_t count = rd32(data + p) + 1, start = rd32(data + p + 4), k;

        p += 12;
        for (k = 0; k < count && !rc && p + 8 <= size; k++) {
            uint32_t eoff = rd16(data + p), nl = rd16(data + p + 6) + 1u;
            char name[260];
            struct ino4 in;

            if (p + 8 + nl > size || nl > 256) {
                rc = mkr_fail(j, "SquashFS: bad directory entry");
                break;
            }
            memcpy(name, data + p + 8, nl);
            name[nl] = 0;
            p += 8 + nl;
            if (ino4(q, (uint64_t)start << 16 | eoff, &in)) {
                rc = -1;
                break;
            }
            switch (in.type) {
            case 1:
            case 8: {
                struct mkr_node *sub = vfs_dir(j, node, name, 0700);

                q->s.dirs++;
                rc = sub ? dir4(q, &in, sub, depth + 1) : -1;
                break;
            }
            case 2:
            case 9: {
                struct mkr_w w;

                if (vfs_wopen(j, &w, node, name))
                    rc = -1;
                else if (file4(q, &in, &w)) {
                    vfs_wabort(j, &w);
                    rc = -1;
                } else if (!vfs_put(j, node, name, &w, file_mode(q, in.mode)))
                    rc = -1;
                q->s.files++;
                q->s.bytes += in.fsize;
                break;
            }
            case 3:
            case 10:
                if (!vfs_link(j, node, name, in.target))
                    rc = -1;
                q->s.links++;
                break;
            default:
                q->s.other++;
                break;
            }
            free(in.blocks);
            free(in.target);
            if (!rc)
                rc = mkr_check(j);
        }
    }
    free(data);
    return rc;
}

/* a SquashFS 4 image at off whose (standard) superblock is sb */
static int sq4_run(struct mkr_job *j, struct mkr_img *m, uint64_t off, const uint8_t *sb, struct mkr_node *dir, int force)
{
    struct sq4 q;
    struct ino4 root;
    int rc;

    memset(&q, 0, sizeof(q));
    sq_init(&q.s, j, m, off);
    q.force = force;
    q.s.block_size = rd32(sb + 12);
    q.frag_count = rd32(sb + 16);
    q.s.comp = rd16(sb + 20);
    q.inode_table = rd64(sb + 64);
    q.dir_table = rd64(sb + 72);
    q.frag_table = rd64(sb + 80);
    if (q.s.comp != C_GZIP && q.s.comp != C_LZMA && q.s.comp != C_LZO && q.s.comp != C_XZ && q.s.comp != C_ZSTD) {
        sq_free(&q.s);
        return mkr_fail(j, "SquashFS compression %s is not supported (gzip, lzma, lzo, xz, zstd are)",
                        comp_name(q.s.comp));
    }
    if (q.s.block_size < 4096 || q.s.block_size > (1u << 20)) {
        sq_free(&q.s);
        return mkr_fail(j, "SquashFS: bad block size %u", q.s.block_size);
    }
    q.s.ibuf = malloc(q.s.block_size + 8192);
    q.s.obuf = malloc(q.s.block_size + 8192);
    rc = ino4(&q, rd64(sb + 32), &root);
    if (!rc && root.type != 1 && root.type != 8)
        rc = mkr_fail(j, "SquashFS: the root is not a directory");
    if (!rc)
        rc = dir4(&q, &root, dir, 0);
    free(root.blocks);
    free(root.target);
    if (!rc && q.s.other)
        mkr_log(j, MKR_INFO, "SquashFS: %d device nodes, fifos or sockets left out", q.s.other);
    sq_free(&q.s);
    return rc;
}

int fs_squashfs(struct mkr_job *j, struct mkr_img *m, uint64_t off, struct mkr_node *dir, int force)
{
    uint8_t sb[96];

    if (img_read(j, m, off, sb, 96))
        return -1;
    if (rd32(sb) != SQFS_MAGIC)
        return mkr_fail(j, "not a SquashFS filesystem");
    if (rd16(sb + 28) != 4)
        return mkr_fail(j, "SquashFS %u.%u is not supported (only 4.x)", rd16(sb + 28), rd16(sb + 30));
    return sq4_run(j, m, off, sb, dir, force);
}

/*
 * IGS_SHFS on SquashFS 4 (Crazy Shoot's p3): the IGS magic and text, then
 * SquashFS 4's fields reordered: 0x44 root inode, 0x4C bytes used, 0x54..0x63
 * four obfuscated words (the kernel's version checks), 0x64 inodes, mkfs
 * time, block size, fragments, compression, block log, flags, ids, 0x7C the
 * id table, xattr, inode, directory, fragment and lookup tables (u64 each).
 * Recognised by block size = 1 << block log.  Rebuilt into a standard
 * superblock for the SquashFS 4 reader.
 */
static int shfs4(const uint8_t *h, uint8_t *sb)
{
    uint32_t bs = rd32(h + 0x6C);
    unsigned lg = rd16(h + 0x76);

    if (lg < 12 || lg > 20 || bs != 1u << lg)
        return 0;
    memset(sb, 0, 96);
    memcpy(sb, "hsqs", 4);
    memcpy(sb + 4, h + 0x64, 24);           /* inodes, mkfs time, block size, fragments, comp, log, flags, ids */
    sb[28] = 4;                              /* version 4.0 */
    memcpy(sb + 32, h + 0x44, 16);          /* root inode, bytes used */
    memcpy(sb + 48, h + 0x7C, 48);          /* id, xattr, inode, directory, fragment, lookup tables */
    return 1;
}

/* ======================================================================================== */
/* ---- IGS_SHFS (SquashFS 2.x) ------------------------------------------------------------ */

struct shfs {
    struct sq s;
    uint64_t inode_start, dir_start;
    uint32_t fragments;
    uint32_t *fstart, *fsize;
};

struct ino2 {
    unsigned type, mode;
    uint32_t size, offset, start;       /* dir */
    uint32_t fstart, frag, foff, fsz, nblocks, *blocks;  /* file */
    char *target;
};

static int ino2(struct shfs *f, uint64_t ref, struct ino2 *in)
{
    uint64_t block = ref >> 16;
    uint32_t off = (uint32_t)(ref & 0xFFFF), hdr;
    uint8_t b[64];

    memset(in, 0, sizeof(*in));
    if (mread(&f->s, f->inode_start, block, off, b, 64))
        return -1;
    hdr = rd32(b);
    in->type = hdr & 15;
    in->mode = (hdr >> 4) & 0xFFF;
    if (in->type == 1) {
        uint32_t w = rd32(b + 4);

        in->size = w & 0x7FFFF;
        in->offset = w >> 19;
        in->start = b[12] | b[13] << 8 | (uint32_t)b[14] << 16;
    } else if (in->type == 8) {
        uint64_t w = rd32(b + 4) | (uint64_t)b[8] << 32;
        uint64_t s = rd32(b + 13) | (uint64_t)b[17] << 32;

        in->type = 1;
        in->size = (uint32_t)(w & 0x7FFFFFF);
        in->offset = (uint32_t)(w >> 27);
        in->start = (uint32_t)(s & 0xFFFFFF);
    } else if (in->type == 2) {
        uint8_t *raw;
        uint32_t i;

        in->fstart = rd32(b + 8);
        in->frag = rd32(b + 12);
        in->foff = rd32(b + 16);
        in->fsz = rd32(b + 20);
        in->nblocks = in->frag == 0xFFFFFFFFu ? (uint32_t)(((uint64_t)in->fsz + f->s.block_size - 1) / f->s.block_size)
                                                : in->fsz / f->s.block_size;
        raw = malloc(4ull * in->nblocks + 4);
        in->blocks = malloc(4ull * in->nblocks + 4);
        if (mread(&f->s, f->inode_start, block, off + 24, raw, 4ull * in->nblocks)) {
            free(raw);
            return -1;
        }
        for (i = 0; i < in->nblocks; i++)
            in->blocks[i] = rd32(raw + 4 * i);
        free(raw);
    } else if (in->type == 3) {
        uint32_t l = rd16(b + 4);
        uint8_t *t = malloc(l + 1);

        if (mread(&f->s, f->inode_start, block, off + 6, t, l)) {
            free(t);
            return -1;
        }
        in->target = mkr_latin1(t, l);
        free(t);
    }
    return 0;
}

/* zlib.decompress of a whole block, any length, into s->obuf (block_size + 8 KiB) */
static long long shfs_block(struct shfs *f, uint64_t off, uint32_t word, int frag)
{
    struct sq *s = &f->s;
    uint32_t l = word & 0xFFFFFF;
    z_stream z;
    int e;
    long long out;

    (void)frag;
    if (l > s->block_size + 8192)
        return mkr_fail(s->j, "IGS_SHFS: bad block size word 0x%X", word);
    if (img_read(s->j, s->m, s->base + off, s->ibuf, l))
        return -1;
    if (word & (1u << 24)) {
        memcpy(s->obuf, s->ibuf, l);
        return l;
    }
    memset(&z, 0, sizeof(z));
    inflateInit(&z);
    z.next_in = s->ibuf;
    z.avail_in = l;
    z.next_out = s->obuf;
    z.avail_out = s->block_size + 8192;
    e = inflate(&z, Z_FINISH);
    out = (long long)(s->block_size + 8192 - z.avail_out);
    inflateEnd(&z);
    if (e != Z_STREAM_END)
        return mkr_fail(s->j, "IGS_SHFS: zlib error at 0x%llX", (unsigned long long)off);
    return out;
}

static int shfs_file(struct shfs *f, const struct ino2 *in, struct mkr_w *w)
{
    struct sq *s = &f->s;
    uint64_t off = in->fstart, left = in->fsz;
    uint32_t i;

    /* b''.join(parts)[:size] */
    for (i = 0; i < in->nblocks; i++) {
        long long got = shfs_block(f, off, in->blocks[i], 0);
        size_t k;

        if (got < 0)
            return -1;
        k = (uint64_t)got < left ? (size_t)got : (size_t)left;
        if (vfs_write(s->j, w, s->obuf, k))
            return -1;
        left -= k;
        off += in->blocks[i] & 0xFFFFFF;
    }
    if (in->frag != 0xFFFFFFFFu) {
        long long got;
        uint32_t want = in->fsz % s->block_size, a, b, k;

        if (in->frag >= f->fragments)
            return mkr_fail(s->j, "IGS_SHFS: bad fragment %u", in->frag);
        if ((got = shfs_block(f, f->fstart[in->frag], f->fsize[in->frag], 1)) < 0)
            return -1;
        /* frag[foff:foff + size % block] */
        a = in->foff < (uint64_t)got ? in->foff : (uint32_t)got;
        b = (uint64_t)in->foff + want < (uint64_t)got ? in->foff + want : (uint32_t)got;
        k = b > a ? b - a : 0;
        if (k > left)
            k = (uint32_t)left;
        if (vfs_write(s->j, w, s->obuf + a, k))
            return -1;
    }
    return 0;
}

static int shfs_walk(struct shfs *f, uint64_t ref, struct mkr_node *parent, const char *name, struct mkr_node *self,
                     int depth)
{
    struct mkr_job *j = f->s.j;
    struct ino2 in;
    int rc = 0;

    if (depth > 200)
        return mkr_fail(j, "IGS_SHFS: directories nested too deep");
    if (ino2(f, ref, &in))
        return -1;
    if (in.type == 1) {
        struct mkr_node *d = self ? self : vfs_dir(j, parent, name, 0755);
        uint8_t *data;
        uint32_t p = 0;

        f->s.dirs++;
        if (!d)
            return -1;
        /* i_size counts 3 bytes more than the listing (2.x "." / ".." convention) */
        data = malloc(in.size + 1);
        if (mread(&f->s, f->dir_start, in.start, in.offset, data, in.size)) {
            free(data);
            return -1;
        }
        while (!rc && p + 3 < in.size) {
            uint32_t count = data[p] + 1u, start = data[p + 1] | data[p + 2] << 8 | (uint32_t)data[p + 3] << 16, k;

            p += 4;
            for (k = 0; k < count && !rc; k++) {
                uint32_t w, sz;
                char *nm;

                if (p + 4 > in.size) {
                    rc = mkr_fail(j, "IGS_SHFS: bad directory");
                    break;
                }
                w = rd16(data + p);
                sz = data[p + 2];
                if (p + 4 + sz > in.size) {
                    rc = mkr_fail(j, "IGS_SHFS: bad directory entry");
                    break;
                }
                nm = mkr_latin1(data + p + 3, sz + 1);
                rc = shfs_walk(f, (uint64_t)start << 16 | (w & 0x1FFF), d, nm, NULL, depth + 1);
                free(nm);
                p += 4 + sz;
            }
        }
        free(data);
    } else if (in.type == 2) {
        struct mkr_w w;

        f->s.files++;
        f->s.bytes += in.fsz;
        if (vfs_wopen(j, &w, parent, name))
            rc = -1;
        else if (shfs_file(f, &in, &w)) {
            vfs_wabort(j, &w);
            rc = -1;
        } else if (!vfs_put(j, parent, name, &w, in.mode & 0777))
            rc = -1;
    } else if (in.type == 3) {
        f->s.links++;
        if (!vfs_link(j, parent, name, in.target))
            rc = -1;
    } else {
        f->s.other++;
    }
    free(in.blocks);
    free(in.target);
    return rc ? rc : mkr_check(j);
}

int fs_shfs(struct mkr_job *j, struct mkr_img *m, uint64_t off, struct mkr_node *dir)
{
    struct shfs f;
    uint8_t h[0x8B], *raw;
    uint8_t h4[0xAC], sb4[96];
    uint32_t mo, nidx, i, major, minor;
    int rc, game;
    char head[96] = "";

    if (img_read(j, m, off, h, sizeof(h)))
        return -1;
    if (rd32(h) != SHFS_MAGIC)
        return mkr_fail(j, "not an IGS_SHFS partition (magic 0x%08X)", rd32(h));
    if (!memcmp(h + 4, " <Intern", 8) && !img_read(j, m, off, h4, sizeof(h4)) && shfs4(h4, sb4)) {
        mkr_log(j, MKR_TOOL, "IGS_SHFS on SquashFS 4 (%s), checks %08X %08X %08X %08X", comp_name(rd16(sb4 + 20)),
                rd32(h4 + 0x54), rd32(h4 + 0x58), rd32(h4 + 0x5C), rd32(h4 + 0x60));
        return sq4_run(j, m, off, sb4, dir, 1);
    }
    memset(&f, 0, sizeof(f));
    sq_init(&f.s, j, m, off);
    f.s.comp = C_GZIP;
    game = !memcmp(h + 4, " <Intern", 8);
    mo = game ? 0x50 : 0x08;
    if (game)
        snprintf(head, sizeof(head), "game type %08X version %08X, ", rd32(h + 0x48), rd32(h + 0x4C));
    major = rd32(h + mo);
    minor = rd32(h + mo + 4);
    f.inode_start = rd32(h + mo + 0x18);
    f.dir_start = rd32(h + mo + 0x1C);
    f.s.block_size = rd32(h + mo + 0x2F);
    f.fragments = rd32(h + mo + 0x33);
    if (f.s.block_size < 512 || f.s.block_size > (1u << 20) || f.fragments > (1u << 24)) {
        sq_free(&f.s);
        return mkr_fail(j, "IGS_SHFS: bad superblock");
    }
    f.s.ibuf = malloc(f.s.block_size + 8192);
    f.s.obuf = malloc(f.s.block_size + 8192);
    /* the fragment table: an index of metadata blocks of {start, size} entries */
    nidx = (f.fragments * 8 + META - 1) / META;
    raw = malloc(4ull * nidx + 4);
    f.fstart = malloc(4ull * f.fragments + 4);
    f.fsize = malloc(4ull * f.fragments + 4);
    rc = img_read(j, m, off + rd32(h + mo + 0x37), raw, 4ull * nidx);
    for (i = 0; !rc && i < f.fragments; i++) {
        uint8_t e[8];

        rc = mread(&f.s, 0, rd32(raw + 4 * (i * 8 / META)), (i * 8) % META, e, 8);
        f.fstart[i] = rd32(e);
        f.fsize[i] = rd32(e + 4);
    }
    free(raw);
    if (!rc) {
        mkr_log(j, MKR_TOOL, "%smajor %08X minor %08X, block %u, %u fragments", head, major, minor, f.s.block_size,
                f.fragments);
        rc = shfs_walk(&f, rd64(h + mo + 0x27), NULL, NULL, dir, 0);
    }
    if (!rc)
        mkr_log(j, MKR_TOOL, "%d dirs, %d files (%llu bytes), %d symlinks, %d other", f.s.dirs, f.s.files,
                (unsigned long long)f.s.bytes, f.s.links, f.s.other);
    free(f.fstart);
    free(f.fsize);
    sq_free(&f.s);
    return rc;
}
