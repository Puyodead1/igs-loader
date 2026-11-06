/*
 * E2100/E2200 root filesystems: IGS ROFS v2 ("igs_rofs"), a port of
 * romtool/igs_rofs.py (itself a port of romtool/IgsRofsExtractor).  Format:
 * wiki Disk Formats; keys and constants: wiki Keys.
 *
 * Every per-kernel profile shares the AES-128 key transform, the record
 * stream and the per-page zlib layout; they differ in constants, field
 * positions, name encryption and the page-table encoding.  AES is the LZMA
 * SDK's (AES-NI when the CPU has it).
 */
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

#include "Aes.h"
#include "zlib.h"

#define ROFS_MAGIC 0xD4AA2684u

struct field { uint32_t off; int word; uint32_t xor; };

struct profile {
    const char *name, *key;
    uint32_t mblock;
    int mword;
    uint32_t first, hsize, modeoff;
    struct field namelen, size, table;
    int enc_names, enc_table, blanked, pt_word;
    /* plaintext profiles (key NULL): name length = (field & mask) * unit,
       names padded to 4 */
    uint32_t nl_mask, nl_unit;
    /* compressed pages longer than this have their first enc_head bytes
       AES-CBC encrypted (zero IV, the same key); 0: none */
    uint32_t enc_head;
};

static const struct profile profiles[] = {
    /* Harpoon Lagoon (Fish4P-OS).  XORs are the init constants - 0x3CE8. */
    { "fish4p", "Hunter4PFishV1.0", 0xC8, 1, 0x140, 0x68, 0x64,
      { 0x20, 1, 0x3A9A44A4 }, { 0x40, 1, 0x3CED596B }, { 0x30, 2, 0x9F3EE741 }, 0, 0, 0, 0 },
    /* Ocean Party (OceanParty-OS).  Names are AES-CBC (zero IV, same key);
       page-table entries are one encrypted 16-byte block each; each page's
       zlib header is zeroed. */
    { "oceanparty", "PartyOcean6PV1.0", 0x58, 0, 0x150, 0x78, 0x74,
      { 0x50, 3, 0x02C4290D }, { 0x30, 1, 0x02C529C9 }, { 0x00, 3, 0x03CB8124 }, 1, 1, 1, 0 },
    /* Hero of Robots (HeroOfRobots-OS).  Harpoon's layout, other positions. */
    { "heroofrobots", "RobotsHeroOfV1.0", 0xF0, 3, 0x140, 0x68, 0x64,
      { 0x40, 2, 0x51223E7A }, { 0x30, 0, 0xEF250457 }, { 0x00, 2, 0xF487975F }, 0, 0, 0, 0 },
    /* Scratch (Scratch-OS).  Ocean Party's design; magic block unaligned at
       0x74, modes at +0x72, page ends are word 1 of each table block. */
    { "scratch", "SwinV1.00Scratch", 0x74, 2, 0x150, 0x78, 0x72,
      { 0x00, 3, 0x2C858D5A }, { 0x30, 1, 0x2C858D5A }, { 0x50, 1, 0xE1ECA651 }, 1, 1, 1, 1 },
    /* Percussion Master 3 (PM3-OS, E2200).  Ocean Party's design; XORs are
       the init constants - 0xED2E; page ends are word 1. */
    { "pmaster3", "E2200-PMaster-V3", 0xD8, 1, 0x150, 0x78, 0x74,
      { 0x20, 1, 0x498C027F }, { 0x50, 3, 0xCD83B702 }, { 0x40, 0, 0xEC285CDA }, 1, 1, 1, 1 },
    /* Fish Hunter 4 M400CN (Fish4P-OS build #38).  Harpoon's key and
       constants, an earlier record layout: header 0x58, mode at +0x54. */
    { "fish4p_m400", "Hunter4PFishV1.0", 0xB8, 1, 0x130, 0x58, 0x54,
      { 0x10, 1, 0x3A9A44A4 }, { 0x30, 1, 0x3CED596B }, { 0x20, 2, 0x9F3EE741 }, 0, 0, 0, 0 },
    /* Fish Hunter 4 / 6 (Fish-OS).  No AES: a cramfs-like plaintext stream,
       24-byte headers, names padded to 4.  XORs are the init constants -
       0x2E9B. */
    { "fish", NULL, 0x78, 0, 0xC0, 0x18, 0x10,
      { 0x00, 0, 0 }, { 0x0C, 0, 0xBE5BE76E }, { 0x14, 0, 0xED8D65C8 }, 0, 0, 0, 0, 0x3F, 4 },
    /* Fish Hunter 6 M607CN (Fish6Pv2-OS).  Ocean Party's design; XORs are
       the init constants +/- 0xC7; page ends are word 3. */
    { "fish6pv2", "Fish6PHunterV2.0", 0xD0, 1, 0x150, 0x78, 0x72,
      { 0x00, 0, 0x683CD4AC }, { 0x30, 0, 0x67CC9C41 }, { 0x50, 3, 0xC2693974 }, 1, 1, 1, 3 },
    /* Fish Hunter SKL (FishSKL-OS).  Ocean Party's design; XORs are the
       init constants - 0x85D0; page ends are word 1. */
    { "fishskl", "SKLHunterFish-V1", 0xD8, 0, 0x150, 0x78, 0x74,
      { 0x20, 0, 0x01655DAF }, { 0x50, 0, 0xACA3EBBD }, { 0x40, 1, 0xA4BFC10A }, 1, 1, 1, 1 },
    /* Fish Hunter SKL2 (FishSKL2-OS).  Ocean Party's design; XORs are the
       init constants +/- 0xAADE; page ends are word 0; the first 0xA0
       bytes of a longer page are encrypted. */
    { "fishskl2", "OSFSFishSKLv2-RF", 0xD0, 2, 0x150, 0x78, 0x72,
      { 0x00, 0, 0x25B70D1E }, { 0x30, 1, 0x2A4581CF }, { 0x50, 3, 0x8FE433E6 }, 1, 1, 1, 0, 0, 0, 0xA0 },
    /* Demon Hunter (DemonHunter-OS).  Ocean Party's superblock and record
       size, Harpoon's plain names and page table; XORs are the init
       constants - 0xA37E. */
    { "demonhunter", "Hunter_DemonV1.0", 0xD8, 3, 0x150, 0x78, 0x74,
      { 0x20, 1, 0xD02D3979 }, { 0x50, 1, 0xBEEF2E50 }, { 0x40, 2, 0x7587D5D4 }, 0, 0, 0, 0 },
    /* Crazy Shoot (CrazyShoot-OS).  SKL2's design with its own positions:
       encrypted names and page table (page ends word 2), blanked zlib
       header, the first 0xA0 bytes of a longer page encrypted. */
    { "crazyshoot", "LsT9eNJaHQb2zmMw", 0x100, 3, 0x150, 0x78, 0x74,
      { 0x50, 1, 0x1F011239 }, { 0x30, 3, 0xA1075718 }, { 0x00, 1, 0xBAA513A0 }, 1, 1, 1, 2, 0, 0, 0xA0 },
    /* Ocean King (E2200; also Poseidon, Full Attack).  Crazy Shoot's design;
       page ends are word 3. */
    { "oceanking", "OceanKingRFOSysK", 0xD8, 1, 0x150, 0x78, 0x74,
      { 0x20, 0, 0x1069E4F0 }, { 0x50, 0, 0xDBEC369E }, { 0x40, 1, 0xB5CF8A8D }, 1, 1, 1, 3, 0, 0, 0xA0 },
    /* Soul of Gun (E2200).  Crazy Shoot's design; page ends are word 1. */
    { "soulofgun", "FSV2SoulRO-OfGun", 0x100, 0, 0x150, 0x78, 0x74,
      { 0x50, 2, 0xA2963B4A }, { 0x30, 1, 0x1C937DAD }, { 0x00, 3, 0x03301B0B }, 1, 1, 1, 1, 0, 0, 0xA0 },
    /* Westtrip (E2200).  Crazy Shoot's design, magic block unaligned at
       0x58; page ends are word 1. */
    { "westtrip", "MakeWestRFOSTrip", 0x58, 2, 0x150, 0x78, 0x74,
      { 0x50, 3, 0x27EB5867 }, { 0x30, 0, 0x27EC5B59 }, { 0x00, 0, 0x00F2465A }, 1, 1, 1, 1, 0, 0, 0xA0 },
    /* Fish Hunter SKL2 8P (E2200).  Crazy Shoot's design moved by 8: the
       mode leads the record and its AES blocks start at +8; magic block at
       0x58; page ends are word 1. */
    { "fishskl2_8p", "8PFishSKLv2InOST", 0x58, 2, 0x150, 0x78, 0x00,
      { 0x28, 1, 0xD3A4465C }, { 0x08, 3, 0x531CABC7 }, { 0x48, 2, 0x034FC930 }, 1, 1, 1, 1, 0, 0, 0xA0 },};

#define NPROF ((int)(sizeof(profiles) / sizeof(profiles[0])))

struct rec {
    uint32_t off, size, table;
    unsigned mode, namelen;
    char *name;
};

struct rofs {
    struct mkr_job *j;
    const uint8_t *d;
    size_t n;
    const struct profile *p;
    UInt32 aesbuf[AES_NUM_IVMRK_WORDS + 4], *aes;
    struct rec *r;
    int nr, cap;
    uint32_t meta_end;
    char *used;
    int nused;
    int dirs, files, links, other;
};

/* ---- AES -------------------------------------------------------------------------------- */

static void set_key(struct rofs *f, const char *literal)
{
    uint8_t k[16];
    uint32_t w[4];
    int i;

    /* the driver's transform of the key literal before the key expansion */
    for (i = 0; i < 4; i++)
        w[i] = (uint32_t)(uint8_t)literal[4 * i] << 24 | (uint32_t)(uint8_t)literal[4 * i + 1] << 16 |
               (uint32_t)(uint8_t)literal[4 * i + 2] << 8 | (uint8_t)literal[4 * i + 3];
    w[0] >>= 2;
    w[1] >>= 1;
    w[3] <<= 1;
    for (i = 0; i < 16; i++)
        k[i] = (uint8_t)(w[i / 4] >> (24 - 8 * (i % 4)));
    f->aes = f->aesbuf + ((0u - (unsigned)(uintptr_t)f->aesbuf) & 15) / 4;
    Aes_SetKey_Dec(f->aes + 4, k, 16);
}

/* CBC with a zero IV over n bytes (n a multiple of 16) */
static void cbc0(struct rofs *f, uint8_t *b, size_t n)
{
    static const uint8_t zero[16];

    AesCbc_Init(f->aes, zero);
    g_AesCbc_Decode(f->aes, b, n / 16);
}

/* ECB: one block at a time */
static void ecb(struct rofs *f, uint8_t *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i += 16)
        cbc0(f, b + i, 16);
}

/* 0 and the word, or -1 past the end */
static int word_at(struct rofs *f, uint32_t off, int word, uint32_t *out)
{
    uint8_t b[16];

    if (!f->p->key) {
        if ((uint64_t)off + 4 * word + 4 > f->n)
            return -1;
        *out = rd32(f->d + off + 4 * word);
        return 0;
    }
    if ((uint64_t)off + 16 > f->n)
        return -1;
    memcpy(b, f->d + off, 16);
    ecb(f, b, 16);
    *out = rd32(b + 4 * word);
    return 0;
}

static int field_at(struct rofs *f, const struct field *fl, uint32_t off, uint32_t *out)
{
    if (word_at(f, off + fl->off, fl->word, out))
        return -1;
    *out ^= fl->xor;
    return 0;
}

/* ---- records ---------------------------------------------------------------------------- */

static uint32_t align16(uint32_t v) { return (v + 15) & ~15u; }

/* a name's padded length: 16, or 4 in the plaintext profiles */
static uint32_t name_align(const struct rofs *f, uint32_t v)
{
    return f->p->key ? align16(v) : (v + 3) & ~3u;
}

/* 0, or -1 where igs_rofs.py's _scan stops */
static int record(struct rofs *f, uint32_t off, struct rec *r)
{
    uint32_t v, start, al, i;
    uint8_t raw[256 + 16];
    char *name;

    if (field_at(f, &f->p->namelen, off, &v))
        return -1;
    r->namelen = f->p->key ? v & 0xFF : (v & f->p->nl_mask) * f->p->nl_unit;
    start = off + f->p->hsize;
    al = name_align(f, r->namelen);
    if (!r->namelen || r->namelen > 255 || (uint64_t)start + al > f->n)
        return -1;
    memcpy(raw, f->d + start, al);
    if (f->p->enc_names)
        cbc0(f, raw, al);
    /* bytes >= 0x80 read as '?', trailing NULs stripped; a NUL inside is a
       control character (scan() stops there), kept here as 0x01 */
    for (al = r->namelen; al && !raw[al - 1]; al--)
        ;
    name = malloc(al + 1);
    for (i = 0; i < al; i++)
        name[i] = raw[i] >= 0x80 ? '?' : raw[i] ? (char)raw[i] : 1;
    name[al] = 0;
    if (field_at(f, &f->p->size, off, &v)) {
        free(name);
        return -1;
    }
    r->size = v & 0x0FFFFFFF;
    if (field_at(f, &f->p->table, off, &v)) {
        free(name);
        return -1;
    }
    r->table = (v & 0x03FFFFFF) * 4;
    if ((uint64_t)off + f->p->modeoff + 2 > f->n) {
        free(name);
        return -1;
    }
    r->mode = rd16(f->d + off + f->p->modeoff);
    r->off = off;
    r->name = name;
    return 0;
}

static void scan(struct rofs *f)
{
    uint32_t off = f->p->first;

    while ((uint64_t)off + f->p->hsize <= f->n) {
        struct rec r;
        const char *c;
        int bad = 0;

        if (record(f, off, &r))
            break;
        if (!r.name[0])
            bad = 1;
        for (c = r.name; *c && !bad; c++)
            if ((unsigned char)*c < 32 || (unsigned char)*c == 0x7F)
                bad = 1;
        if (bad) {
            free(r.name);
            break;
        }
        if (f->nr == f->cap) {
            f->cap = f->cap ? f->cap * 2 : 1024;
            f->r = realloc(f->r, f->cap * sizeof(*f->r));
        }
        f->r[f->nr++] = r;
        off += f->p->hsize + name_align(f, r.namelen);
    }
    f->meta_end = off;
}

static int by_off(const struct rofs *f, uint32_t off)
{
    int lo = 0, hi = f->nr - 1;

    while (lo <= hi) {
        int m = (lo + hi) / 2;

        if (f->r[m].off == off)
            return m;
        if (f->r[m].off < off)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return -1;
}

/* ---- contents --------------------------------------------------------------------------- */

static int content(struct rofs *f, const struct rec *r, struct mkr_w *w, uint8_t **mem, size_t *memn)
{
    struct mkr_job *j = f->j;
    uint32_t pages = (r->size + 0xFFF) >> 12, p, start;
    uint32_t *ends;
    uint8_t page[0x1000], comp[0x2000];
    z_stream z;
    int rc = 0;

    if (!r->size)
        return 0;
    ends = malloc(pages * sizeof(*ends));
    if (f->p->enc_table) {
        if ((uint64_t)r->table + 16ull * pages > f->n) {
            free(ends);
            return mkr_fail(j, "igs_rofs: page table of %s past the end", r->name);
        }
        for (p = 0; p < pages; p++) {
            uint8_t b[16];

            memcpy(b, f->d + r->table + 16 * p, 16);
            ecb(f, b, 16);
            ends[p] = rd32(b + 4 * f->p->pt_word);
        }
        start = r->table + 16 * pages;
    } else {
        if ((uint64_t)r->table + 4ull * pages > f->n) {
            free(ends);
            return mkr_fail(j, "igs_rofs: page table of %s past the end", r->name);
        }
        for (p = 0; p < pages; p++)
            ends[p] = rd32(f->d + r->table + 4 * p);
        start = r->table + 4 * pages;
    }
    memset(&z, 0, sizeof(z));
    inflateInit(&z);
    for (p = 0; p < pages && !rc; p++) {
        uint32_t end = ends[p], want = r->size - p * 0x1000 < 0x1000 ? r->size - p * 0x1000 : 0x1000;
        int e;

        if (end <= start || end - start > 0x2000 || end > f->n) {
            rc = mkr_fail(j, "bad compressed extent for %s, page %u: 0x%X-0x%X", r->name, p, start, end);
            break;
        }
        memcpy(comp, f->d + start, end - start);
        if (f->p->enc_head && end - start > f->p->enc_head)
            cbc0(f, comp, f->p->enc_head);
        if (f->p->blanked) {
            comp[0] = 0x78;
            comp[1] = 0x9C;
        }
        /* zlib.decompressobj().decompress(comp, want): stops at want bytes */
        inflateReset(&z);
        z.next_in = comp;
        z.avail_in = end - start;
        z.next_out = page;
        z.avail_out = want;
        do
            e = inflate(&z, Z_NO_FLUSH);
        while (e == Z_OK && z.avail_out && z.avail_in);
        if (e != Z_OK && e != Z_STREAM_END && e != Z_BUF_ERROR) {
            rc = mkr_fail(j, "zlib error in %s, page %u: %s", r->name, p, z.msg ? z.msg : "?");
            break;
        }
        if (z.avail_out) {
            rc = mkr_fail(j, "short zlib block for %s, page %u", r->name, p);
            break;
        }
        if (w)
            rc = vfs_write(j, w, page, want);
        else {
            memcpy(*mem + *memn, page, want);
            *memn += want;
        }
        start = end;
    }
    inflateEnd(&z);
    free(ends);
    return rc;
}

/* ---- the tree --------------------------------------------------------------------------- */

static void sanitize(char *s)
{
    for (; *s; s++)
        if (strchr("<>:\"/\\|?*", *s) || (unsigned char)*s < 32)
            *s = '_';
}

static int extract_rec(struct rofs *f, struct mkr_node *dir, const struct rec *r, struct mkr_node **sub)
{
    struct mkr_job *j = f->j;
    char name[300];
    unsigned kind = r->mode & 0xF000;

    snprintf(name, sizeof(name), "%s", r->name);
    sanitize(name);
    *sub = NULL;
    if (kind == 0x4000) {
        if (!(*sub = vfs_dir(j, dir, name, 0755)))
            return -1;
        f->dirs++;
    } else if (kind == 0x8000) {
        struct mkr_w w;

        if (vfs_wopen(j, &w, dir, name))
            return -1;
        if (content(f, r, &w, NULL, NULL)) {
            vfs_wabort(j, &w);
            return -1;
        }
        if (!vfs_put(j, dir, name, &w, r->mode & 07777))
            return -1;
        f->files++;
    } else if (kind == 0xA000) {
        uint8_t *t = malloc(r->size + 1);
        size_t tn = 0;
        char *u;
        struct mkr_node *e;

        if (content(f, r, NULL, &t, &tn)) {
            free(t);
            return -1;
        }
        u = mkr_latin1(t, tn);
        free(t);
        e = vfs_link(j, dir, name, u);
        free(u);
        if (!e)
            return -1;
        f->links++;
    } else {
        f->other++;
    }
    return mkr_check(j);
}

static int range(struct rofs *f, struct mkr_node *dir, uint32_t start, uint32_t length, int depth)
{
    struct mkr_job *j = f->j;
    uint32_t off = start;

    if (start < f->p->first || (uint64_t)start + length > f->meta_end || (start & 3))
        return mkr_fail(j, "bad directory range 0x%X+0x%X", start, length);
    if (depth > 200)
        return mkr_fail(j, "igs_rofs: directories nested too deep");
    while (off < start + length) {
        int i = by_off(f, off);
        const struct rec *r;
        struct mkr_node *sub;

        if (i < 0)
            return mkr_fail(j, "no metadata record at 0x%X", off);
        r = &f->r[i];
        if (f->used[i])
            return mkr_fail(j, "directory graph reuses record 0x%X (%s)", off, r->name);
        f->used[i] = 1;
        f->nused++;
        if (extract_rec(f, dir, r, &sub))
            return -1;
        if ((r->mode & 0xF000) == 0x4000 && r->size && range(f, sub, r->table, r->size, depth + 1))
            return -1;
        off += f->p->hsize + name_align(f, r->namelen);
    }
    return 0;
}

int fs_rofs(struct mkr_job *j, uint64_t off, uint64_t size, struct mkr_node *dir, const char *profile)
{
    struct rofs f;
    uint8_t *data = malloc(size ? (size_t)size : 1);
    uint32_t magic, root_end = 0xFFFFFFFFu;
    long long got;
    int i, rc = -1, have_dir = 0;

    memset(&f, 0, sizeof(f));
    f.j = j;
    if (!data)
        return mkr_fail(j, "out of memory for the root filesystem (%llu bytes)", (unsigned long long)size);
    /* f.read(size): short at the end of the image */
    got = 0;
    if (off < j->img.size) {
        uint64_t k = j->img.size - off < size ? j->img.size - off : size;

        if (img_read(j, &j->img, off, data, (size_t)k)) {
            free(data);
            return -1;
        }
        got = (long long)k;
    }
    f.d = data;
    f.n = (size_t)got;
    for (i = 0; i < NPROF; i++) {
        if (profile && strcmp(profile, profiles[i].name))
            continue;
        f.p = &profiles[i];
        if (f.p->key)
            set_key(&f, f.p->key);
        if (!word_at(&f, f.p->mblock, f.p->mword, &magic) && magic == ROFS_MAGIC)
            break;
        f.p = NULL;
    }
    if (!f.p) {
        if (profile) {
            for (i = 0; i < NPROF && strcmp(profile, profiles[i].name); i++)
                ;
            if (i == NPROF)
                mkr_fail(j, "unknown ROFS profile '%s' (known: fish4p, oceanparty, heroofrobots, scratch, pmaster3, fish4p_m400, fish, fish6pv2, fishskl, fishskl2, demonhunter, crazyshoot, oceanking, soulofgun, westtrip, fishskl2_8p)",
                         profile);
        }
        mkr_fail(j, "not an IGS ROFS v2 image for any known profile (no key decodes the superblock magic)");
        free(data);
        return -1;
    }
    scan(&f);
    f.used = calloc(f.nr + 1, 1);
    for (i = 0; i < f.nr; i++)
        if ((f.r[i].mode & 0xF000) == 0x4000 && f.r[i].size) {
            have_dir = 1;
            if (f.r[i].table < root_end)
                root_end = f.r[i].table;
        }
    if (!have_dir)
        mkr_fail(j, "igs_rofs: no directories");
    else if (!range(&f, dir, f.p->first, root_end - f.p->first, 0)) {
        if (f.nused != f.nr)
            mkr_fail(j, "tree reconstruction missed %d metadata records", f.nr - f.nused);
        else
            rc = 0;
    }
    if (!rc)
        mkr_log(j, MKR_TOOL, "igs_rofs profile %s: %d dirs, %d files, %d symlinks, %d special nodes skipped",
                f.p->name, f.dirs, f.files, f.links, f.other);
    for (i = 0; i < f.nr; i++)
        free(f.r[i].name);
    free(f.r);
    free(f.used);
    free(data);
    return rc;
}
