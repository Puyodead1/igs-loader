/*
 * E2000 (Linux 2.4.31) root filesystems: the plaintext IGS ROFS, cramfs with
 * IGS changes; a port of romtool/igs_rofs24.py.  See
 * docs/IGS_ROFS_REVERSE_ENGINEERING.md, "E2000 plaintext ROFS":
 *
 *   superblock: magic 0xD4AA2681 at +0x4C, root inode at +0xA8
 *   16-byte inodes: version word, namelen:6|offset:26 (4-byte units),
 *     size:24, mode:16|uid:8|gid:8, then the name
 *   files and symlinks: ceil(size/4096) block end offsets, then one zlib
 *     stream per 4 KiB block (an empty extent is a block of zeros)
 */
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

#include "zlib.h"

#define ROFS24_MAGIC 0xD4AA2681u
#define ROFS24_ROOT 0xA8

struct inode {
    uint32_t off, size, next;
    unsigned mode;
    char *name;                 /* UTF-8 (latin-1 decoded) */
};

struct r24 {
    struct mkr_job *j;
    const uint8_t *d;
    size_t n;
    int dirs, files, links, other;
};

static int inode_at(struct r24 *f, uint32_t o, struct inode *in)
{
    uint32_t a, namelen, l;

    if ((uint64_t)o + 16 > f->n)
        return mkr_fail(f->j, "ROFS: inode 0x%X past the end", o);
    a = rd32(f->d + o + 4);
    namelen = (a & 63) * 4;
    in->off = (a >> 6) * 4;
    in->size = rd32(f->d + o + 8) & 0xFFFFFF;
    in->mode = rd32(f->d + o + 12) & 0xFFFF;
    in->next = o + 16 + namelen;
    l = (uint64_t)o + 16 + namelen <= f->n ? namelen : (uint32_t)(f->n - o - 16);
    while (l && !f->d[o + 16 + l - 1])
        l--;
    in->name = mkr_latin1(f->d + o + 16, l);
    return 0;
}

/* one zlib stream, any length (zlib.decompress) */
static int inflate_all(struct mkr_job *j, const uint8_t *src, size_t n, uint8_t **out, size_t *outn)
{
    z_stream z;
    size_t cap = 8192;
    int e;

    *out = malloc(cap);
    *outn = 0;
    memset(&z, 0, sizeof(z));
    inflateInit(&z);
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)n;
    for (;;) {
        if (*outn == cap) {
            cap *= 2;
            *out = realloc(*out, cap);
        }
        z.next_out = *out + *outn;
        z.avail_out = (uInt)(cap - *outn);
        e = inflate(&z, Z_NO_FLUSH);
        *outn = cap - z.avail_out;
        if (e == Z_STREAM_END)
            break;
        if (e != Z_OK && !(e == Z_BUF_ERROR && !z.avail_out)) {
            inflateEnd(&z);
            free(*out);
            return mkr_fail(j, "ROFS: zlib error: %s", e == Z_BUF_ERROR ? "incomplete or truncated stream"
                                                                      : z.msg ? z.msg : "?");
        }
    }
    inflateEnd(&z);
    return 0;
}

/* the data of a file or link: into w, or (w NULL) into *mem */
static int data(struct r24 *f, const struct inode *in, struct mkr_w *w, uint8_t **mem, size_t *memn)
{
    uint32_t nb = (in->size + 4095) / 4096, i, start;
    uint64_t left = in->size;
    static const uint8_t zero[4096];

    if ((uint64_t)in->off + 4ull * nb > f->n)
        return mkr_fail(f->j, "ROFS: block table past the end");
    start = in->off + 4 * nb;
    if (mem) {
        *mem = malloc(in->size + 1);
        *memn = 0;
    }
    for (i = 0; i < nb && left; i++) {
        uint32_t end = rd32(f->d + in->off + 4 * i);
        const uint8_t *b = zero;
        uint8_t *buf = NULL;
        size_t bn = 4096, k;

        if (end > start) {
            if (end > f->n)
                return mkr_fail(f->j, "ROFS: block past the end");
            if (inflate_all(f->j, f->d + start, end - start, &buf, &bn))
                return -1;
            b = buf;
        }
        k = bn < left ? bn : (size_t)left;
        if (w ? vfs_write(f->j, w, b, k) : (memcpy(*mem + *memn, b, k), *memn += k, 0)) {
            free(buf);
            return -1;
        }
        left -= k;
        free(buf);
        start = end;
    }
    return 0;
}

static int walk(struct r24 *f, uint32_t o, struct mkr_node *parent, struct mkr_node *self, int depth)
{
    struct mkr_job *j = f->j;
    struct inode in;
    unsigned kind;
    int rc = 0;

    if (depth > 200)
        return mkr_fail(j, "ROFS: directories nested too deep");
    if (inode_at(f, o, &in))
        return -1;
    kind = in.mode & 0xF000;
    if (self && kind != 0x4000) {
        free(in.name);
        return mkr_fail(j, "ROFS: the root is not a directory");
    }
    if (kind == 0x4000) {
        struct mkr_node *d = self ? self : vfs_dir(j, parent, in.name, 0755);
        uint32_t p = in.off, end = in.off + in.size;

        f->dirs++;
        while (d && !rc && p < end) {
            struct inode c;

            if (inode_at(f, p, &c))
                rc = -1;
            else {
                free(c.name);
                rc = walk(f, p, d, NULL, depth + 1);
                p = c.next;
            }
        }
        if (!d)
            rc = -1;
    } else if (kind == 0x8000) {
        struct mkr_w w;

        if (vfs_wopen(j, &w, parent, in.name))
            rc = -1;
        else if (data(f, &in, &w, NULL, NULL)) {
            vfs_wabort(j, &w);
            rc = -1;
        } else if (!vfs_put(j, parent, in.name, &w, in.mode & 0777))
            rc = -1;
        f->files++;
    } else if (kind == 0xA000) {
        uint8_t *t = NULL;
        size_t tn = 0;
        char *u;

        if (data(f, &in, NULL, &t, &tn))
            rc = -1;
        else {
            u = mkr_latin1(t, tn);
            if (!vfs_link(j, parent, in.name, u))
                rc = -1;
            free(u);
        }
        free(t);
        f->links++;
    } else {
        f->other++;                     /* device nodes, fifos: nothing to extract */
    }
    free(in.name);
    return rc ? rc : mkr_check(j);
}

int fs_rofs24(struct mkr_job *j, uint64_t off, uint64_t size, struct mkr_node *dir)
{
    struct r24 f;
    uint64_t k = off < j->img.size ? j->img.size - off : 0;
    uint8_t *d;
    int rc;

    if (size && size < k)
        k = size;
    if (!(d = malloc(k ? (size_t)k : 1)))
        return mkr_fail(j, "out of memory for the root filesystem");
    if (k && img_read(j, &j->img, off, d, (size_t)k)) {
        free(d);
        return -1;
    }
    if (k < 0x50 || rd32(d + 0x4C) != ROFS24_MAGIC) {
        free(d);
        return mkr_fail(j, "not an E2000 plaintext ROFS (magic at +0x4C)");
    }
    memset(&f, 0, sizeof(f));
    f.j = j;
    f.d = d;
    f.n = (size_t)k;
    rc = walk(&f, ROFS24_ROOT, NULL, dir, 0);
    if (!rc)
        mkr_log(j, MKR_TOOL, "%d dirs, %d files, %d symlinks, %d other", f.dirs, f.files, f.links, f.other);
    free(d);
    return rc;
}
