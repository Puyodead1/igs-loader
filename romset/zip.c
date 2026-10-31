/*
 * .zip rom sets: stored and deflated entries (zip64 too), with a small
 * streaming inflate (RFC 1951) so that no zlib is needed.  Info-ZIP unix
 * entries keep their modes; their symlinks (S_IFLNK, the data is the
 * target) become links.  Encrypted entries and other methods are refused.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "romset.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

static int zfail(const char *a, const char *what)
{
    snprintf(rs_error, sizeof(rs_error), "%s: %s", a, what);
    return -1;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

/* ---- inflate ---------------------------------------------------------------------------- */

#define WSIZE 65536             /* output ring; distances are at most 32768 */
#define MAXBITS 15

struct huff {
    uint16_t *table;            /* (1 << bits) entries: symbol << 4 | length; 0 = invalid */
    int bits;
};

struct inf {
    FILE *f;
    uint64_t left;              /* compressed bytes not yet read */
    uint8_t in[65536];
    size_t ip, il;
    uint32_t bitbuf;
    int bitcnt, eof_bits;
    uint8_t win[WSIZE];
    size_t wpos;
    FILE *out;
    uint64_t total;
    uint32_t crc;
    int err;
    uint16_t lt[1 << MAXBITS], dt[1 << MAXBITS];
};

static int getbyte(struct inf *s)
{
    if (s->ip == s->il) {
        size_t want = s->left < sizeof(s->in) ? (size_t)s->left : sizeof(s->in);

        if (!want)
            return -1;
        s->il = fread(s->in, 1, want, s->f);
        s->ip = 0;
        if (!s->il)
            return -1;
        s->left -= s->il;
    }
    return s->in[s->ip++];
}

static int need(struct inf *s, int n)
{
    while (s->bitcnt < n) {
        int c = getbyte(s);

        if (c < 0) {
            /* pad with zeros so that a short code at the very end still decodes */
            c = 0;
            if (++s->eof_bits > 4)
                return -1;
        }
        s->bitbuf |= (uint32_t)c << s->bitcnt;
        s->bitcnt += 8;
    }
    return 0;
}

static int bits(struct inf *s, int n)
{
    int v;

    if (!n)
        return 0;
    if (need(s, n)) {
        s->err = 1;
        return 0;
    }
    v = (int)(s->bitbuf & ((1u << n) - 1));
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void flush(struct inf *s, size_t upto)
{
    if (upto) {
        if (s->out && fwrite(s->win, 1, upto, s->out) != upto)
            s->err = 2;
        s->crc = rs_crc32(s->crc, s->win, upto);
    }
}

static void put(struct inf *s, uint8_t c)
{
    s->win[s->wpos++] = c;
    s->total++;
    if (s->wpos == WSIZE) {
        flush(s, WSIZE);
        s->wpos = 0;
    }
}

/* canonical Huffman table from code lengths; 0 or -1 for an over-subscribed set */
static int build(struct huff *h, uint16_t *table, const uint8_t *len, int n)
{
    int count[MAXBITS + 1] = { 0 }, next[MAXBITS + 1], i, max = 0, code = 0, left = 1;

    for (i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    for (i = 1; i <= MAXBITS; i++) {
        left = (left << 1) - count[i];
        if (left < 0)
            return -1;
        if (count[i])
            max = i;
    }
    if (!max)
        max = 1;
    for (i = 1; i <= MAXBITS; i++) {
        code = (code + count[i - 1]) << 1;
        next[i] = code;
    }
    memset(table, 0, sizeof(uint16_t) << max);
    for (i = 0; i < n; i++) {
        int l = len[i], c, r = 0, b, k;

        if (!l)
            continue;
        c = next[l]++;
        for (b = 0; b < l; b++)
            r |= ((c >> b) & 1) << (l - 1 - b);
        for (k = r; k < (1 << max); k += 1 << l)
            table[k] = (uint16_t)(i << 4 | l);
    }
    h->table = table;
    h->bits = max;
    return 0;
}

static int decode(struct inf *s, const struct huff *h)
{
    uint16_t e;

    if (need(s, h->bits)) {
        /* near the end: try with what is there (padded with zeros) */
        s->err = 1;
        return -1;
    }
    e = h->table[s->bitbuf & ((1u << h->bits) - 1)];
    if (!e) {
        s->err = 1;
        return -1;
    }
    s->bitbuf >>= e & 15;
    s->bitcnt -= e & 15;
    return e >> 4;
}

static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                    67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
                                  12, 12, 13, 13 };

static int codes(struct inf *s, const struct huff *lh, const struct huff *dh)
{
    for (;;) {
        int sym = decode(s, lh);

        if (sym < 0 || s->err)
            return -1;
        if (sym < 256) {
            put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            int len, d, dist;

            sym -= 257;
            if (sym >= 29)
                return -1;
            len = lbase[sym] + bits(s, lext[sym]);
            d = decode(s, dh);
            if (d < 0 || d >= 30)
                return -1;
            dist = dbase[d] + bits(s, dext[d]);
            if (s->err || (uint64_t)dist > s->total)
                return -1;
            while (len--)
                put(s, s->win[(s->wpos - dist) & (WSIZE - 1)]);
        }
    }
}

static int inflate_all(struct inf *s)
{
    int last;

    do {
        int type;
        struct huff lh, dh;
        uint8_t len[320];

        last = bits(s, 1);
        type = bits(s, 2);
        if (s->err)
            return -1;
        if (type == 0) {
            int n, i;

            bits(s, s->bitcnt & 7);     /* to a byte boundary */
            n = bits(s, 16);
            if ((bits(s, 16) ^ 0xFFFF) != n || s->err)
                return -1;
            for (i = 0; i < n; i++) {
                int c;

                if (s->bitcnt >= 8) {   /* whole bytes still in the bit buffer */
                    c = bits(s, 8);
                } else if ((c = getbyte(s)) < 0) {
                    return -1;
                }

                put(s, (uint8_t)c);
            }
        } else if (type == 1) {
            int i;

            for (i = 0; i < 144; i++) len[i] = 8;
            for (; i < 256; i++) len[i] = 9;
            for (; i < 280; i++) len[i] = 7;
            for (; i < 288; i++) len[i] = 8;
            build(&lh, s->lt, len, 288);
            for (i = 0; i < 30; i++)
                len[i] = 5;
            build(&dh, s->dt, len, 30);
            if (codes(s, &lh, &dh))
                return -1;
        } else if (type == 2) {
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4, i;
            uint8_t cl[19] = { 0 };
            struct huff ch;

            if (s->err || nlen > 286 || ndist > 30)
                return -1;
            for (i = 0; i < ncode; i++)
                cl[order[i]] = (uint8_t)bits(s, 3);
            if (build(&ch, s->lt, cl, 19))
                return -1;
            for (i = 0; i < nlen + ndist;) {
                int sym = decode(s, &ch), rep, val = 0;

                if (sym < 0)
                    return -1;
                if (sym < 16) {
                    len[i++] = (uint8_t)sym;
                    continue;
                }
                if (sym == 16) {
                    if (!i)
                        return -1;
                    val = len[i - 1];
                    rep = 3 + bits(s, 2);
                } else if (sym == 17) {
                    rep = 3 + bits(s, 3);
                } else {
                    rep = 11 + bits(s, 7);
                }
                if (i + rep > nlen + ndist)
                    return -1;
                while (rep--)
                    len[i++] = (uint8_t)val;
            }
            if (!len[256] || build(&lh, s->lt, len, nlen) || build(&dh, s->dt, len + nlen, ndist))
                return -1;
            if (codes(s, &lh, &dh))
                return -1;
        } else {
            return -1;
        }
        if (s->err)
            return -1;
    } while (!last);
    flush(s, s->wpos);
    return s->err ? -1 : 0;
}

/* ---- the archive ------------------------------------------------------------------------ */

struct zent {
    char *name;
    uint64_t csize, usize, off;
    uint32_t crc, attr;
    uint16_t method, flags, host;
};

static int read_dir(const char *a, FILE *f, struct zent **out, int *count)
{
    uint8_t tail[65557], *e = NULL, rec[56];
    uint64_t size, cd_off, cd_size, n_ent, i;
    long long back;
    size_t got;
    uint8_t *cd, *p;
    struct zent *ents;

    fseek64(f, 0, SEEK_END);
    size = (uint64_t)ftell64(f);
    back = size < sizeof(tail) ? (long long)size : (long long)sizeof(tail);
    fseek64(f, (long long)size - back, SEEK_SET);
    got = fread(tail, 1, (size_t)back, f);
    for (p = tail + got - 22; got >= 22 && p >= tail; p--)
        if (u32(p) == 0x06054b50) {
            e = p;
            break;
        }
    if (!e)
        return zfail(a, "not a zip archive (no end of central directory)");
    n_ent = u16(e + 10);
    cd_size = u32(e + 12);
    cd_off = u32(e + 16);
    if (n_ent == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF) {
        /* zip64: the locator sits just before the end record */
        uint64_t loc = (uint64_t)((long long)size - back + (e - tail)) - 20;

        fseek64(f, (long long)loc, SEEK_SET);
        if (fread(rec, 1, 20, f) != 20 || u32(rec) != 0x07064b50)
            return zfail(a, "zip64 locator missing");
        fseek64(f, (long long)u64(rec + 8), SEEK_SET);
        if (fread(rec, 1, 56, f) != 56 || u32(rec) != 0x06064b50)
            return zfail(a, "zip64 end record missing");
        n_ent = u64(rec + 32);
        cd_size = u64(rec + 40);
        cd_off = u64(rec + 48);
    }
    if (cd_size > (64u << 20) || cd_off + cd_size > size)
        return zfail(a, "bad central directory");
    cd = malloc((size_t)cd_size + 1);
    fseek64(f, (long long)cd_off, SEEK_SET);
    if (fread(cd, 1, (size_t)cd_size, f) != cd_size) {
        free(cd);
        return zfail(a, "short read");
    }
    ents = calloc((size_t)n_ent + 1, sizeof(*ents));
    for (i = 0, p = cd; i < n_ent; i++) {
        struct zent *z = &ents[i];
        uint16_t nl, xl, cl;
        uint8_t *x, *xe;

        if (p + 46 > cd + cd_size || u32(p) != 0x02014b50) {
            free(cd);
            *out = ents;
            *count = (int)i;
            return zfail(a, "bad central directory entry");
        }
        z->host = p[5];
        z->flags = u16(p + 8);
        z->method = u16(p + 10);
        z->crc = u32(p + 16);
        z->csize = u32(p + 20);
        z->usize = u32(p + 24);
        nl = u16(p + 28);
        xl = u16(p + 30);
        cl = u16(p + 32);
        z->attr = u32(p + 38);
        z->off = u32(p + 42);
        z->name = malloc(nl + 1);
        memcpy(z->name, p + 46, nl);
        z->name[nl] = 0;
        for (x = p + 46 + nl, xe = x + xl; x + 4 <= xe; x += 4 + u16(x + 2)) {
            if (u16(x) == 1) {
                uint8_t *v = x + 4;

                if (z->usize == 0xFFFFFFFF) { z->usize = u64(v); v += 8; }
                if (z->csize == 0xFFFFFFFF) { z->csize = u64(v); v += 8; }
                if (z->off == 0xFFFFFFFF) { z->off = u64(v); }
            }
        }
        p += 46 + nl + xl + cl;
    }
    free(cd);
    *out = ents;
    *count = (int)n_ent;
    return 0;
}

int rs_extract_zip(const char *a, struct rs_sink *sk)
{
    FILE *f = fopen(a, "rb");
    struct zent *ents = NULL;
    struct inf *s = NULL;
    int n = 0, i, rc = 0;

    if (!f)
        return zfail(a, "cannot open");
    if (read_dir(a, f, &ents, &n)) {
        rc = -1;
        goto out;
    }
    for (i = 0; i < n; i++)
        rs_sink_scan(sk, ents[i].name);
    s = malloc(sizeof(*s));
    for (i = 0; i < n && !rc; i++) {
        struct zent *z = &ents[i];
        unsigned mode = z->host == 3 ? z->attr >> 16 : 0;
        const char *rel = rs_sink_name(sk, z->name);
        size_t nl = strlen(z->name);
        uint8_t lh[30];
        int isdir = (nl && z->name[nl - 1] == '/') || (z->attr & 0x10) || (mode & 0170000) == 0040000;
        int islink = (mode & 0170000) == 0120000;
        FILE *o = NULL;

        if (!rel)
            continue;
        if (isdir) {
            rs_sink_dir(sk, rel, mode & 07777);
            continue;
        }
        if (z->flags & 1) {
            rc = zfail(z->name, "encrypted zip entries are not supported");
            break;
        }
        if (z->method != 0 && z->method != 8) {
            rc = zfail(z->name, "only stored and deflated zip entries are supported");
            break;
        }
        fseek64(f, (long long)z->off, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || u32(lh) != 0x04034b50) {
            rc = zfail(z->name, "bad local header");
            break;
        }
        fseek64(f, (long long)(z->off + 30 + u16(lh + 26) + u16(lh + 28)), SEEK_SET);
        if (islink && z->usize > 4096) {
            rc = zfail(z->name, "symlink target too long");
            break;
        }
        if (!islink && !(o = rs_sink_file(sk, rel))) {
            rc = -1;
            break;
        }
        memset(s, 0, offsetof(struct inf, lt));
        s->f = f;
        s->left = z->csize;
        s->out = o;
        if (z->method == 0) {
            uint64_t left = z->csize;
            int c;

            while (left--) {
                if ((c = getbyte(s)) < 0) {
                    s->err = 1;
                    break;
                }
                put(s, (uint8_t)c);
            }
            if (!s->err)
                flush(s, s->wpos);
        } else if (inflate_all(s)) {
            s->err = 1;
        }
        if (islink) {
            if (!s->err && s->total == z->usize && s->total < WSIZE) {
                char target[4097];

                memcpy(target, s->win, (size_t)s->total);
                target[s->total] = 0;
                s->crc = rs_crc32(0, s->win, (size_t)s->total);
                if (s->crc == z->crc)
                    rs_sink_link(sk, rel, target);
                else
                    s->err = 1;
            }
        } else {
            rs_sink_close(sk, o, rel, mode & 07777);
        }
        if (s->err == 2)
            rc = zfail(rel, "write error (disk full?)");
        else if (s->err || s->total != z->usize || s->crc != z->crc)
            rc = zfail(z->name, "data error (CRC or size mismatch)");
    }
out:
    for (i = 0; i < n; i++)
        free(ents[i].name);
    free(ents);
    free(s);
    fclose(f);
    return rc;
}
