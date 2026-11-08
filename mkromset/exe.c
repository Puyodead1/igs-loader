/*
 * The game executables: the FPGA asset cipher (romtool/igs_fpga_decrypt.py),
 * UPX unpacking (romtool/igs_upx.py) and the E2000 split-executable assembly
 * (romtool/igs_e2000_extract.py).  Keys and constants: wiki Keys.
 */
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

/* ---- the FPGA cipher -------------------------------------------------------------------- */

/*
 * Not AES: each 16-byte block is XORed with the mask, its two 64-bit LE
 * halves rotated right (low by 1, high by 3), bit 7 inverted in eight slots,
 * then shuffled: plaintext byte i comes from byte PERM[i].
 */
static const uint8_t perm[16] = { 14, 10, 8, 1, 0, 12, 9, 4, 3, 6, 11, 7, 2, 13, 5, 15 };
static const uint8_t flip[16] = { 0, 0x80, 0, 0, 0x80, 0, 0x80, 0x80, 0, 0, 0x80, 0, 0x80, 0x80, 0, 0x80 };

void fpga_decrypt(const uint8_t mask[16], uint8_t *data, size_t n)
{
    size_t o;
    int i;

    for (o = 0; o + 16 <= n; o += 16) {
        uint8_t *c = data + o, u[16];
        uint64_t lo = 0, hi = 0;

        for (i = 7; i >= 0; i--) {
            lo = lo << 8 | (uint8_t)(c[i] ^ mask[i]);
            hi = hi << 8 | (uint8_t)(c[8 + i] ^ mask[8 + i]);
        }
        lo = lo >> 1 | lo << 63;
        hi = hi >> 3 | hi << 61;
        for (i = 0; i < 8; i++) {
            u[i] = (uint8_t)(lo >> (8 * i)) ^ flip[i];
            u[8 + i] = (uint8_t)(hi >> (8 * i)) ^ flip[8 + i];
        }
        for (i = 0; i < 16; i++)
            c[i] = u[perm[i]];
    }
}

/* ---- UPX -------------------------------------------------------------------------------- */

/*
 * Stock UPX, linux/elf386 (format 12): after the stub's program headers come
 * l_info, p_info (the original file size) and the blocks, each a b_info
 * {sz_unc, sz_cpr, method, filter id, filter cto, 0} and its data; sz_unc 0
 * ends them.  NRV2B (method 2), NRV2D (5) or NRV2E (others), le32 bit
 * buffer, no filters.  wiki Disk Formats (E2000).
 */
struct bits {
    const uint8_t *src;
    size_t n, ilen;
    uint32_t bb;
    int bc, err;
};

static int bit(struct bits *s)
{
    if (!s->bc) {
        if (s->ilen + 4 > s->n) {
            s->err = 1;
            return 0;
        }
        s->bb = rd32(s->src + s->ilen);
        s->ilen += 4;
        s->bc = 32;
    }
    s->bc--;
    return (s->bb >> s->bc) & 1;
}

static int byte_(struct bits *s)
{
    if (s->ilen >= s->n) {
        s->err = 1;
        return 0;
    }
    return s->src[s->ilen++];
}

static int nrv(struct mkr_job *j, const uint8_t *src, size_t n, size_t size, int method, uint8_t *dst)
{
    struct bits s;
    size_t olen = 0;
    uint64_t last = 1;

    memset(&s, 0, sizeof(s));
    s.src = src;
    s.n = n;
    for (;;) {
        uint64_t m_off = 1, m_len, p, k;

        while (bit(&s)) {
            int b = byte_(&s);

            if (s.err || olen >= size)
                return mkr_fail(j, "UPX: corrupt block");
            dst[olen++] = (uint8_t)b;
        }
        if (method == 2) {
            do
                m_off = m_off * 2 + bit(&s);
            while (!bit(&s) && !s.err);
        } else {
            for (;;) {
                m_off = m_off * 2 + bit(&s);
                if (bit(&s) || s.err)
                    break;
                m_off = (m_off - 1) * 2 + bit(&s);
            }
        }
        if (method == 2) {
            if (m_off == 2) {
                m_off = last;
            } else {
                m_off = (m_off - 3) * 256 + byte_(&s);
                if (m_off == 0xffffffffu)
                    break;
                last = m_off = m_off + 1;
            }
            m_len = bit(&s) * 2;
            m_len += bit(&s);
            if (!m_len) {
                m_len = 1;
                do
                    m_len = m_len * 2 + bit(&s);
                while (!bit(&s) && !s.err);
                m_len += 2;
            }
            m_len += m_off > 0xd00;
            m_len += 1;
        } else {
            if (m_off == 2) {
                m_off = last;
                m_len = bit(&s);
            } else {
                m_off = (m_off - 3) * 256 + byte_(&s);
                if (m_off == 0xffffffffu)
                    break;
                m_len = (m_off ^ 0xffffffffu) & 1;
                m_off >>= 1;
                last = m_off = m_off + 1;
            }
            if (method == 5) {
                m_len = m_len * 2 + bit(&s);
                if (!m_len) {
                    m_len = 1;
                    do
                        m_len = m_len * 2 + bit(&s);
                    while (!bit(&s) && !s.err);
                    m_len += 2;
                }
                m_len += m_off > 0x500;
                m_len += 1;
            } else {
                if (m_len)
                    m_len = 1 + bit(&s);
                else if (bit(&s))
                    m_len = 3 + bit(&s);
                else {
                    m_len = 1;
                    do
                        m_len = m_len * 2 + bit(&s);
                    while (!bit(&s) && !s.err);
                    m_len += 3;
                }
                m_len += m_off > 0x500;
                m_len += 1;
            }
        }
        if (s.err)
            return mkr_fail(j, "UPX: corrupt block");
        if (m_off > olen)
            return mkr_fail(j, "UPX: match before the start of the block");
        if (m_len > size - olen)
            return mkr_fail(j, "UPX: block unpacked to more than %zu bytes", size);
        p = olen - m_off;
        for (k = 0; k < m_len; k++)
            dst[olen + k] = dst[p + k];
        olen += m_len;
    }
    if (olen != size)
        return mkr_fail(j, "UPX: block unpacked to %zu bytes, expected %zu", olen, size);
    return 0;
}

struct ext { uint64_t off, n; };

static int ext_cmp(const void *a, const void *b)
{
    const struct ext *x = a, *y = b;

    return x->off < y->off ? -1 : x->off > y->off ? 1 : x->n < y->n ? -1 : x->n > y->n;
}

/* a b_info header at p whose block fits: what UPX writes, not its stub's code */
static int upx_plausible(const uint8_t *data, size_t n, size_t p, uint64_t left)
{
    uint32_t u, c;
    uint8_t m;

    if (p + 12 > n)
        return 0;
    u = rd32(data + p);
    c = rd32(data + p + 4);
    m = data[p + 8];
    return c && c <= u && u <= left && (m == 2 || m == 5 || m == 8) && !data[p + 11] && p + 12 + c <= n;
}

/* plausible headers from p on that end in the sz_unc 0 terminator */
static int upx_chain(const uint8_t *data, size_t n, size_t p, uint64_t left)
{
    while (upx_plausible(data, n, p, left)) {
        left -= rd32(data + p);
        p += 12 + rd32(data + p + 4);
        if (p + 4 <= n && !rd32(data + p))
            return 1;
    }
    return 0;
}

int upx_unpack(struct mkr_job *j, const uint8_t *data, size_t n, uint8_t **out, size_t *outn)
{
    uint64_t total = 0;
    uint8_t **blk = NULL, *o = NULL;
    size_t *blen = NULL, nb = 0, cap = 0, i, p, bi = 0, olen, ocap;
    struct ext *ex = NULL, *sorted;
    size_t nex = 0, k;
    uint64_t pos, filesize;
    const uint8_t *hdr;
    size_t hlen;
    long long at = -1;
    uint32_t phoff, phentsize, phnum;
    int rc = -1;

    for (i = 0; i + 4 <= n && i + 4 <= 0x400; i++)
        if (!memcmp(data + i, "UPX!", 4)) {
            at = (long long)i - 4;
            break;
        }
    if (at < 0)
        return mkr_fail(j, "not UPX-packed");
    i = (size_t)at;
    if (i + 24 > n)
        return mkr_fail(j, "UPX: truncated header");
    if (data[i + 11] != 12)
        return mkr_fail(j, "UPX format %d, only linux/elf386 (12) is handled", data[i + 11]);
    filesize = rd32(data + i + 16);
    for (p = i + 24;; ) {
        uint32_t u, c;

        if (p + 12 > n) {
            mkr_fail(j, "UPX: truncated block list");
            goto out;
        }
        u = rd32(data + p);
        c = rd32(data + p + 4);
        if (!u)
            break;
        if (!upx_plausible(data, n, p, filesize - total)) {
            /* UPX 3.08 puts its decompressor between the PT_LOADs' blocks and
               the gaps' (Speed Driver 4): the next block header after it */
            size_t q, lim = p + 0x10000 < n ? p + 0x10000 : n;

            for (q = p + 1; q < lim && !upx_chain(data, n, q, filesize - total); q++)
                ;
            if (q >= lim) {
                mkr_fail(j, "UPX: no block header at 0x%zx", p);
                goto out;
            }
            p = q;
            u = rd32(data + p);
            c = rd32(data + p + 4);
        }
        total += u;
        if (data[p + 9]) {
            mkr_fail(j, "UPX filter 0x%x not handled", data[p + 9]);
            goto out;
        }
        if (p + 12 + c > n) {
            mkr_fail(j, "UPX: truncated block");
            goto out;
        }
        if (nb == cap) {
            cap = cap ? cap * 2 : 64;
            blk = realloc(blk, cap * sizeof(*blk));
            blen = realloc(blen, cap * sizeof(*blen));
        }
        blk[nb] = malloc(u ? u : 1);
        blen[nb] = u;
        if (c == u)
            memcpy(blk[nb], data + p + 12, c);
        else if (nrv(j, data + p + 12, c, u, data[p + 8], blk[nb])) {
            free(blk[nb]);
            goto out;
        }
        nb++;
        p += 12 + c;
        if (mkr_check(j))
            goto out;
    }
    if (!nb) {
        mkr_fail(j, "UPX: no blocks");
        goto out;
    }
    /* the extents in the order UPX stored them: the headers, each PT_LOAD's
       file bytes (the first after the headers), then the gaps in file order */
    hdr = blk[0];
    hlen = blen[0];
    if (hlen < 52) {
        mkr_fail(j, "UPX: the first block is not an ELF header");
        goto out;
    }
    phoff = rd32(hdr + 28);
    phentsize = rd16(hdr + 42);
    phnum = rd16(hdr + 44);
    ex = malloc((2 * (phnum + 2) + 2) * sizeof(*ex));
    ex[nex].off = 0;
    ex[nex++].n = hlen;
    for (k = 0; k < phnum; k++) {
        const uint8_t *ph = hdr + phoff + k * phentsize;
        uint64_t off, filesz;

        if (phoff + k * phentsize + 20 > hlen) {
            mkr_fail(j, "UPX: program headers outside the first block");
            goto out;
        }
        off = rd32(ph + 4);
        filesz = rd32(ph + 16);
        if (rd32(ph) == 1 && filesz) {
            if (off < hlen) {
                filesz -= hlen - off;
                off = hlen;
            }
            ex[nex].off = off;
            ex[nex++].n = filesz;
        }
    }
    sorted = malloc(nex * sizeof(*sorted));
    memcpy(sorted, ex, nex * sizeof(*sorted));
    qsort(sorted, nex, sizeof(*sorted), ext_cmp);
    pos = 0;
    for (k = 0, i = nex; k < i; k++) {
        if (sorted[k].off > pos) {
            ex[nex].off = pos;
            ex[nex++].n = sorted[k].off - pos;
        }
        if (sorted[k].off + sorted[k].n > pos)
            pos = sorted[k].off + sorted[k].n;
    }
    free(sorted);
    if (pos < filesize) {
        ex[nex].off = pos;
        ex[nex++].n = filesize - pos;
    }
    /* bytearray(filesize); a slice past the end grows it */
    olen = (size_t)filesize;
    ocap = olen + 16;
    o = calloc(1, ocap);
    for (k = 0; k < nex; k++) {
        uint8_t *got = NULL;
        size_t gn = 0;

        while (gn < ex[k].n) {
            if (bi == nb) {
                free(got);
                mkr_fail(j, "UPX: block sizes do not follow the extents");
                goto out;
            }
            got = realloc(got, gn + blen[bi]);
            memcpy(got + gn, blk[bi], blen[bi]);
            gn += blen[bi++];
        }
        if (gn != ex[k].n) {
            free(got);
            mkr_fail(j, "UPX: block sizes do not follow the extents");
            goto out;
        }
        if (ex[k].off + gn > olen) {
            /* out[off:off+n] = got with off+n past the end: the array becomes off+n long */
            size_t nl = ex[k].off > olen ? olen + gn : (size_t)ex[k].off + gn;

            if (nl + 16 > ocap) {
                ocap = nl + 16;
                o = realloc(o, ocap);
            }
            if (ex[k].off > olen) {
                memcpy(o + olen, got, gn);
                olen += gn;
                free(got);
                continue;
            }
            olen = nl;
        }
        memcpy(o + ex[k].off, got, gn);
        free(got);
    }
    *out = o;
    *outn = olen;
    o = NULL;
    rc = 0;
out:
    free(o);
    for (i = 0; i < nb; i++)
        free(blk[i]);
    free(blk);
    free(blen);
    free(ex);
    return rc;
}

/* ---- E2000 executable assembly ---------------------------------------------------------- */

/*
 * The executable is the first KiB from HEAD_PART sector HEAD_SECTOR plus a
 * piece of each root-fs file PIECE_FILE % d, d in PIECES.  PIECE_MODE says
 * how much of each piece is kept; n starts at TRIM_KB and changes by
 * TRIM_STEP after each piece:
 *   trim  the first size/1K - n KiB; the last piece keeps size - n*1K bytes
 *   skip  everything after the first n KiB
 *   cut   skip the first n KiB and drop the last t KiB of the whole KiB (last
 *         piece: of the bytes); t from CUT_KB by CUT_STEP; empty or missing
 *         pieces are passed over and do not step
 * Python slice rules throughout (negative bounds count from the end).
 */
static void pyslice(int64_t len, int64_t a, int64_t b, int64_t *from, int64_t *n)
{
    if (a < 0)
        a = a + len < 0 ? 0 : a + len;
    else if (a > len)
        a = len;
    if (b < 0)
        b = b + len < 0 ? 0 : b + len;
    else if (b > len)
        b = len;
    *from = a;
    *n = b > a ? b - a : 0;
}

static int int_feature(struct mkr_job *j, const char *name, long long *v)
{
    const char *s = mkr_feature(j, name, NULL);
    char *e;

    if (!s)
        return mkr_fail(j, "the entry has no %s", name);
    *v = strtoll(s, &e, 10);
    while (*e == ' ' || *e == '\t')
        e++;
    if (e == s || *e)
        return mkr_fail(j, "%s: not a number: %s", name, s);
    return 0;
}

static int part_feature(struct mkr_job *j, const char *name, const char *def, int *idx)
{
    const char *p = mkr_feature(j, name, def);

    if (strlen(p) != 2 || p[0] != 'p' || p[1] < '1' || p[1] > '4')
        return mkr_fail(j, "bad %s '%s'", name, p);
    *idx = p[1] - '0';
    return 0;
}

static int piece_path(struct mkr_job *j, char d, char *out, size_t n)
{
    const char *f = mkr_feature(j, "piece_file", "/lib/libdb%s.a"), *c;
    size_t o = 0;
    int used = 0;

    for (c = f; *c && o + 2 < n; c++) {
        if (*c != '%') {
            out[o++] = *c;
        } else if (c[1] == '%') {
            out[o++] = '%';
            c++;
        } else if (c[1] == 's' && !used) {
            out[o++] = d;
            used = 1;
            c++;
        } else {
            return mkr_fail(j, "bad piece_file '%s'", f);
        }
    }
    out[o] = 0;
    if (!used)
        return mkr_fail(j, "piece_file '%s' has no %%s", f);
    return 0;
}

static int append(uint8_t **out, size_t *len, size_t *cap, const uint8_t *p, size_t n)
{
    if (*len + n > *cap) {
        *cap = (*len + n) * 2;
        *out = realloc(*out, *cap);
    }
    memcpy(*out + *len, p, n);
    *len += n;
    return 0;
}

int e2000_assemble(struct mkr_job *j, uint8_t **out, size_t *outn)
{
    long long sector, n, step, t = 0, tstep = 0;
    const char *pieces, *mode;
    uint8_t *o;
    size_t len = 0, cap = 1 << 20, i, np;
    int hp = 2;
    uint32_t shoff, shnum;
    /* head_cipher fpga (Fish Hunter SKL2): the pieces are the FPGA-encrypted
       executable; the build's decrypt step checks the result */
    const char *cipher = mkr_feature(j, "head_cipher", "");
    int plain = strcmp(cipher, "fpga") != 0;

    if (*cipher && plain)
        return mkr_fail(j, "bad head_cipher '%s'", cipher);
    if (part_feature(j, "head_part", "p2", &hp) || int_feature(j, "head_sector", &sector) ||
        int_feature(j, "trim_kb", &n) || int_feature(j, "trim_step", &step))
        return -1;
    if (!(pieces = mkr_feature(j, "pieces", NULL)))
        return mkr_fail(j, "the entry has no pieces");
    mode = mkr_feature(j, "piece_mode", "trim");
    if (strcmp(mode, "trim") && strcmp(mode, "skip") && strcmp(mode, "cut"))
        return mkr_fail(j, "bad piece_mode '%s'", mode);
    if (!strcmp(mode, "cut") && (int_feature(j, "cut_kb", &t) || int_feature(j, "cut_step", &tstep)))
        return -1;
    o = malloc(cap);
    {
        uint8_t head[1024];
        long long got = 0;
        uint64_t at = (j->pstart[hp] + (uint64_t)sector) * 512;

        if (at + 1024 <= j->img.size && img_read(j, &j->img, at, head, 1024)) {
            free(o);
            return -1;
        }
        got = at + 1024 <= j->img.size ? 1024 : 0;
        if (got < 4 || (plain && memcmp(head, "\x7f" "ELF", 4))) {
            free(o);
            return mkr_fail(j, "HEAD_SECTOR does not point at an ELF header");
        }
        append(&o, &len, &cap, head, 1024);
    }
    np = strlen(pieces);
    for (i = 0; i < np; i++) {
        char path[1024];
        struct mkr_node *f;
        uint8_t *piece = NULL;
        size_t pn = 0;
        int64_t from, k, plen;

        if (piece_path(j, pieces[i], path, sizeof(path)))
            goto bad;
        f = vfs_lookup(j, path, 1);
        if (!strcmp(mode, "cut")) {
            /* missing or empty pieces are passed over without stepping */
            if (!f || f->type != N_FILE || !f->size)
                continue;
        } else if (!f || f->type != N_FILE) {
            mkr_fail(j, "%s: no such file in the root filesystem", path);
            goto bad;
        }
        if (vfs_load(j, f, &piece, &pn))
            goto bad;
        plen = (int64_t)pn;
        if (!strcmp(mode, "cut")) {
            if (i == np - 1)
                pyslice(plen, n * 1024, plen - t * 1024, &from, &k);
            else
                pyslice(plen, n * 1024, (plen / 1024 - t) * 1024, &from, &k);
            t += tstep;
        } else if (!strcmp(mode, "skip")) {
            pyslice(plen, n * 1024, plen, &from, &k);
        } else if (i == np - 1) {
            pyslice(plen, 0, plen - n * 1024, &from, &k);
        } else {
            pyslice(plen, 0, (plen / 1024 - n) * 1024, &from, &k);
        }
        append(&o, &len, &cap, piece + from, (size_t)k);
        free(piece);
        n += step;
    }
    /* the section header table ends the file: a cheap integrity check */
    shoff = rd32(o + 0x20);
    shnum = rd16(o + 0x30);
    if (plain && (uint64_t)shoff + 40ull * shnum != len) {
        mkr_fail(j, "assembled executable is %zu bytes, ELF header expects %llu", len,
                 (unsigned long long)shoff + 40ull * shnum);
        goto bad;
    }
    *out = o;
    *outn = len;
    return 0;
bad:
    free(o);
    return -1;
}
