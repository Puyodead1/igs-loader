/*
 * E3000 plaintext root filesystem: Speed Driver 3's p2, a "Game Image v2"
 * ROFS without encryption (wiki Disk Formats, "Variant: Speed Driver 3").
 *
 * The superblock holds the ROFS magic at 0x7C and the root list's size at
 * 0x50 (xor the size key); the root list's entries start at 0xC4 and end at
 * 0xA0 + size.  An entry: [check, w1, -, w3, mode u16, name length u16], the
 * name in 4 * (length + 1) bytes.  w3 ^ size key is the size; w1 ^ offset key
 * is the offset in words: one past it for a directory's entries, at it (low
 * two bits varying) for a file's cramfs-style block table, each 4 KiB block
 * zlib or, zero length, a hole.
 *
 * Speed Rider 2 ("Variant: Speed Rider 2"): keyed inodes.  Each entry is a
 * fixed header then its name padded to 16; the header's words are xored with
 * the kernel's constants: major and minor checks, the size (low 28 bits),
 * the offset in words (low 26 bits; the directory's entries or the file's
 * block table, both at it) and the name length (one byte).  The root inode
 * is at 0x40.  Speed Rider 2 FA's headers are longer and their first 0x60
 * bytes AES-ECB under the ROFS key transform of "riderspeed2-v1.0".
 */
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

#include "Aes.h"
#include "zlib.h"

#define GIMG_MAGIC 0xD4AA2684u

struct gimg_profile { const char *name; uint32_t off_key, size_key; int keyed; };

static const struct gimg_profile gprofiles[] = {
    { "speeddriver3", 0x016F88CC, 0x0777613F, 0 },
    { "speedrider2", 0, 0, 1 },
};

/* Speed Rider 2's kernel constants (init_igs_rofs_fs) */
#define K_SIZE 0x505D3EC3u
#define K_MAJ  0x1F416F72u
#define K_MIN  0xBAE4CAD7u
#define K_ROFS 0x10B1219Fu
#define K_ROOT 0x04E20F34u
#define K_D    0x2A1Fu
#define SR2_MAJOR 0xA5A5A5A5u
#define SR2_MINOR 0xAA55EB48u
#define SR2_ROFS  0x14532EABu

/* header size, encrypted bytes, field offsets, the root list's offset */
struct klayout { const char *name; uint32_t hdr, enc, maj, nlen, off, size, minor, mode, root; };

static const struct klayout klayouts[] = {
    { "Speed Rider 2", 0x58, 0, 0x04, 0x1C, 0x28, 0x30, 0x4C, 0x54, 0x130 },
    { "Speed Rider 2 FA", 0x68, 0x60, 0x04, 0x2C, 0x38, 0x40, 0x5C, 0x64, 0x140 },
};

struct gimg {
    struct mkr_job *j;
    const uint8_t *d;
    size_t n;
    const struct gimg_profile *p;
    const struct klayout *k;
    UInt32 aesbuf[AES_NUM_IVMRK_WORDS + 4], *aes;
    uint32_t check;
    int dirs, files, links, other;
};

struct gent { uint32_t off, w1, w3, next; unsigned mode; char name[260]; };

static int entry(struct gimg *g, uint32_t o, struct gent *e)
{
    uint32_t nl, len, i;

    if ((uint64_t)o + 20 > g->n)
        return mkr_fail(g->j, "gimg: entry at 0x%X past the end", o);
    if (rd32(g->d + o) != g->check)
        return mkr_fail(g->j, "gimg: no entry at 0x%X", o);
    nl = rd16(g->d + o + 18);
    len = 4 * (nl + 1);
    if (len > 256 || (uint64_t)o + 20 + len > g->n)
        return mkr_fail(g->j, "gimg: bad name at 0x%X", o);
    e->off = o;
    e->w1 = rd32(g->d + o + 4);
    e->w3 = rd32(g->d + o + 12);
    e->mode = rd16(g->d + o + 16);
    for (i = 0; i < len && g->d[o + 20 + i]; i++)
        e->name[i] = g->d[o + 20 + i] >= 0x80 ? '?' : (char)g->d[o + 20 + i];
    e->name[i] = 0;
    e->next = o + 20 + len;
    return 0;
}

/* the content at word offset unit: the block table, then the blocks; 0, or -1 when they do not decode */
static int blocks(struct gimg *g, uint32_t unit, uint32_t size, struct mkr_w *w, uint8_t *mem)
{
    uint32_t nb = (size + 0xFFF) >> 12, b, start, off = unit * 4;
    uint8_t page[0x1000];
    z_stream z;
    int rc = 0;

    if ((uint64_t)off + 4ull * nb > g->n)
        return -1;
    start = off + 4 * nb;
    memset(&z, 0, sizeof(z));
    inflateInit(&z);
    for (b = 0; b < nb && !rc; b++) {
        uint32_t end = rd32(g->d + off + 4 * b), want = size - b * 0x1000 < 0x1000 ? size - b * 0x1000 : 0x1000;

        if (end < start || end > g->n || end - start > 0x2000) {
            rc = -1;
            break;
        }
        if (end == start) {
            memset(page, 0, want);
        } else {
            int e;

            inflateReset(&z);
            z.next_in = (Bytef *)(g->d + start);
            z.avail_in = end - start;
            z.next_out = page;
            z.avail_out = want;
            do
                e = inflate(&z, Z_NO_FLUSH);
            while (e == Z_OK && z.avail_out && z.avail_in);
            if ((e != Z_OK && e != Z_STREAM_END && e != Z_BUF_ERROR) || z.avail_out) {
                rc = -1;
                break;
            }
        }
        if (mem)
            memcpy(mem + b * 0x1000, page, want);
        else if (w && vfs_write(g->j, w, page, want))
            rc = -2;
        start = end;
    }
    inflateEnd(&z);
    return rc;
}

/* the low two bits of a file's offset vary: the one whose blocks decode */
static int content(struct gimg *g, const struct gent *e, uint32_t size, uint8_t **out)
{
    uint32_t base = e->w1 ^ g->p->off_key, x;
    uint8_t *mem = malloc(size ? size : 1);

    for (x = 0; x < 4; x++)
        if (!blocks(g, base ^ x, size, NULL, mem)) {
            *out = mem;
            return 0;
        }
    free(mem);
    return mkr_fail(g->j, "gimg: no decodable blocks for %s", e->name);
}

static void sanitize(char *s)
{
    for (; *s; s++)
        if (strchr("<>:\"/\\|?*", *s) || (unsigned char)*s < 32)
            *s = '_';
}

static int list(struct gimg *g, struct mkr_node *dir, uint32_t start, uint32_t end, int depth)
{
    uint32_t o = start;

    if (depth > 200)
        return mkr_fail(g->j, "gimg: directories nested too deep");
    while (o < end) {
        struct gent e;
        uint32_t size;
        unsigned kind;

        if (entry(g, o, &e))
            return -1;
        sanitize(e.name);
        size = e.w3 ^ g->p->size_key;
        kind = e.mode & 0xF000;
        if (kind == 0x4000) {
            struct mkr_node *sub = vfs_dir(g->j, dir, e.name, 0755);
            uint32_t at = ((e.w1 ^ g->p->off_key) + 1) * 4;

            if (!sub)
                return -1;
            g->dirs++;
            if (size && list(g, sub, at, at + size, depth + 1))
                return -1;
        } else if (kind == 0x8000 || kind == 0xA000) {
            uint8_t *data = NULL;

            if (size && content(g, &e, size, &data))
                return -1;
            if (kind == 0x8000) {
                struct mkr_w w;

                if (vfs_wopen(g->j, &w, dir, e.name))
                    return free(data), -1;
                if (size && vfs_write(g->j, &w, data, size)) {
                    vfs_wabort(g->j, &w);
                    return free(data), -1;
                }
                if (!vfs_put(g->j, dir, e.name, &w, e.mode & 07777))
                    return free(data), -1;
                g->files++;
            } else {
                char *t = mkr_latin1(data ? data : (const uint8_t *)"", size);
                struct mkr_node *l = vfs_link(g->j, dir, e.name, t);

                free(t);
                if (!l)
                    return free(data), -1;
                g->links++;
            }
            free(data);
        } else {
            g->other++;
        }
        if (mkr_check(g->j))
            return -1;
        o = e.next;
    }
    return 0;
}

/* ---- Speed Rider 2: keyed inodes ------------------------------------------------------------ */

static void k_setkey(struct gimg *g, const char *literal)
{
    uint8_t k[16];
    uint32_t w[4];
    int i;

    for (i = 0; i < 4; i++)                       /* the ROFS key transform (fs_rofs.c) */
        w[i] = (uint32_t)(uint8_t)literal[4 * i] << 24 | (uint32_t)(uint8_t)literal[4 * i + 1] << 16 |
               (uint32_t)(uint8_t)literal[4 * i + 2] << 8 | (uint8_t)literal[4 * i + 3];
    w[0] >>= 2;
    w[1] >>= 1;
    w[3] <<= 1;
    for (i = 0; i < 16; i++)
        k[i] = (uint8_t)(w[i / 4] >> (24 - 8 * (i % 4)));
    g->aes = g->aesbuf + ((0u - (unsigned)(uintptr_t)g->aesbuf) & 15) / 4;
    Aes_SetKey_Dec(g->aes + 4, k, 16);
}

static void k_ecb(struct gimg *g, uint8_t *b, size_t n)
{
    static const uint8_t zero[16];
    size_t i;

    for (i = 0; i < n; i += 16) {
        AesCbc_Init(g->aes, zero);
        g_AesCbc_Decode(g->aes, b + i, 1);
    }
}

struct kent { uint32_t size, off, next; unsigned mode; char name[260]; };

/* the inode at o (the root's checks differ); 0, or -1 quietly when it is not one */
static int k_inode(struct gimg *g, uint32_t o, int root, struct kent *e)
{
    const struct klayout *L = g->k;
    uint8_t h[0x80];
    uint32_t okey, nl, i;

    if ((uint64_t)o + L->hdr > g->n)
        return -1;
    memcpy(h, g->d + o, L->hdr);
    if (L->enc)
        k_ecb(g, h, L->enc);
    if (root) {
        if ((rd32(h + L->maj) ^ K_ROFS) != SR2_ROFS)
            return -1;
        okey = K_ROOT + K_D;
    } else {
        if ((rd32(h + L->maj) ^ K_MAJ) != SR2_MAJOR)
            return -1;
        okey = K_MAJ - K_D;
    }
    if ((rd32(h + L->minor) ^ K_MIN) != SR2_MINOR)
        return -1;
    e->size = (rd32(h + L->size) ^ (K_SIZE - K_D)) & 0xFFFFFFF;
    e->off = ((rd32(h + L->off) ^ okey) & 0x3FFFFFF) * 4;
    e->mode = rd16(h + L->mode);
    e->name[0] = 0;
    e->next = o + L->hdr;
    if (!root) {
        nl = h[L->nlen] ^ ((K_MIN - K_D) & 0xFF);
        if (!nl || (uint64_t)o + L->hdr + nl > g->n)
            return -1;
        for (i = 0; i < nl && g->d[o + L->hdr + i]; i++)
            e->name[i] = g->d[o + L->hdr + i] >= 0x80 ? '?' : (char)g->d[o + L->hdr + i];
        e->name[i] = 0;
        e->next = o + L->hdr + ((nl + 15) & ~15u);
    }
    return 0;
}

static int k_list(struct gimg *g, struct mkr_node *dir, uint32_t start, uint32_t end, int depth)
{
    uint32_t o = start;

    if (depth > 64)
        return mkr_fail(g->j, "gimg: directories nested too deep");
    while (o < end) {
        struct kent e;
        unsigned kind;

        if (k_inode(g, o, 0, &e))
            return mkr_fail(g->j, "gimg: no inode at 0x%X", o);
        sanitize(e.name);
        kind = e.mode & 0xF000;
        if (kind == 0x4000) {
            struct mkr_node *sub = vfs_dir(g->j, dir, e.name, 0755);

            if (!sub)
                return -1;
            g->dirs++;
            if (e.size && k_list(g, sub, e.off, e.off + e.size, depth + 1))
                return -1;
        } else if (kind == 0x8000 || kind == 0xA000) {
            uint8_t *data = malloc(e.size ? e.size : 1);

            if (!data)
                return mkr_fail(g->j, "out of memory");
            if (e.size && blocks(g, e.off / 4, e.size, NULL, data)) {
                free(data);
                return mkr_fail(g->j, "gimg: %s does not decode", e.name);
            }
            if (kind == 0x8000) {
                struct mkr_w w;

                if (vfs_wopen(g->j, &w, dir, e.name))
                    return free(data), -1;
                if (e.size && vfs_write(g->j, &w, data, e.size)) {
                    vfs_wabort(g->j, &w);
                    return free(data), -1;
                }
                if (!vfs_put(g->j, dir, e.name, &w, e.mode & 07777))
                    return free(data), -1;
                g->files++;
            } else {
                char *t = mkr_latin1(data, e.size);
                struct mkr_node *l = vfs_link(g->j, dir, e.name, t);

                free(t);
                if (!l)
                    return free(data), -1;
                g->links++;
            }
            free(data);
        } else {
            g->other++;
        }
        if (mkr_check(g->j))
            return -1;
        o = e.next;
    }
    if (o != end)
        return mkr_fail(g->j, "gimg: a directory's entries end at 0x%X, its size says 0x%X", o, end);
    return 0;
}

/* the layout whose root inode checks out */
static int k_root(struct gimg *g, struct mkr_node *dir)
{
    unsigned i;

    k_setkey(g, "riderspeed2-v1.0");
    for (i = 0; i < sizeof(klayouts) / sizeof(klayouts[0]); i++) {
        struct kent r;

        g->k = &klayouts[i];
        if (!k_inode(g, 0x40, 1, &r) && (r.mode & 0xF000) == 0x4000 && r.off == g->k->root) {
            mkr_log(g->j, MKR_TOOL, "Game Image: %s layout", g->k->name);
            return k_list(g, dir, r.off, r.off + r.size, 0);
        }
    }
    return mkr_fail(g->j, "not a Speed Rider 2 Game Image v2 root (no root inode at 0x40)");
}

int fs_gimg_known(const char *profile)
{
    unsigned i;

    for (i = 0; profile && i < sizeof(gprofiles) / sizeof(gprofiles[0]); i++)
        if (!strcmp(profile, gprofiles[i].name))
            return 1;
    return 0;
}

int fs_gimg(struct mkr_job *j, uint64_t off, uint64_t size, struct mkr_node *dir, const char *profile)
{
    struct gimg g;
    uint8_t *data = malloc(size ? (size_t)size : 1);
    uint64_t k;
    unsigned i;
    int rc;

    memset(&g, 0, sizeof(g));
    g.j = j;
    for (i = 0; i < sizeof(gprofiles) / sizeof(gprofiles[0]); i++)
        if (!strcmp(profile, gprofiles[i].name))
            g.p = &gprofiles[i];
    if (!data)
        return mkr_fail(j, "out of memory for the root filesystem (%llu bytes)", (unsigned long long)size);
    if (!g.p) {
        free(data);
        return mkr_fail(j, "unknown Game Image profile '%s'", profile);
    }
    k = off < j->img.size ? (j->img.size - off < size ? j->img.size - off : size) : 0;
    if (k && img_read(j, &j->img, off, data, (size_t)k)) {
        free(data);
        return -1;
    }
    g.d = data;
    g.n = (size_t)k;
    if (g.p->keyed) {
        rc = k_root(&g, dir);
        if (!rc)
            mkr_log(j, MKR_TOOL, "Game Image %s: %d dirs, %d files, %d symlinks, %d special nodes skipped",
                    g.p->name, g.dirs, g.files, g.links, g.other);
        free(data);
        return rc;
    }
    if (g.n < 0x100 || rd32(data + 0x7C) != GIMG_MAGIC) {
        free(data);
        return mkr_fail(j, "not a plaintext Game Image v2 root (no magic at 0x7C)");
    }
    g.check = rd32(data + 0xC4);
    rc = list(&g, dir, 0xC4, 0xA0 + (rd32(data + 0x50) ^ g.p->size_key), 0);
    if (!rc)
        mkr_log(j, MKR_TOOL, "Game Image %s: %d dirs, %d files, %d symlinks, %d special nodes skipped",
                g.p->name, g.dirs, g.files, g.links, g.other);
    free(data);
    return rc;
}
