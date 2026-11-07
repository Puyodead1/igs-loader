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
 */
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

#include "zlib.h"

#define GIMG_MAGIC 0xD4AA2684u

struct gimg_profile { const char *name; uint32_t off_key, size_key; };

static const struct gimg_profile gprofiles[] = {
    { "speeddriver3", 0x016F88CC, 0x0777613F },
};

struct gimg {
    struct mkr_job *j;
    const uint8_t *d;
    size_t n;
    const struct gimg_profile *p;
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
