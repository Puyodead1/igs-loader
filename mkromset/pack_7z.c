/*
 * .7z sets: one solid LZMA2 folder (the LZMA SDK's multithreaded Lzma2Enc;
 * -mx0 stores), then a plain (uncompressed) header.  The C SDK has no
 * archive writer, so the container is written here (DOC/7zFormat.txt).
 *
 * Entries are what "7z a -snl" stores on Linux: every directory, file and
 * symlink, attributes = Windows bits | 0x8000 | unix mode << 16 (a link's
 * data is its target).  romset/sevenzip.c, 7-Zip and p7zip read it.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mkr.h"

#include "7zCrc.h"
#include "Lzma2Enc.h"

#define K_END 0x00
#define K_HEADER 0x01
#define K_MAIN_STREAMS 0x04
#define K_FILES 0x05
#define K_PACK_INFO 0x06
#define K_UNPACK_INFO 0x07
#define K_SUBSTREAMS 0x08
#define K_SIZE 0x09
#define K_CRC 0x0A
#define K_FOLDER 0x0B
#define K_UNPACK_SIZE 0x0C
#define K_NUM_UNPACK 0x0D
#define K_EMPTY_STREAM 0x0E
#define K_EMPTY_FILE 0x0F
#define K_NAME 0x11
#define K_MTIME 0x14
#define K_ATTRIB 0x15

struct item {
    const struct mkr_node *n;
    char *path;
    uint32_t attrib, crc;
    uint64_t size;
    int stream;                 /* has data */
};

struct buf { uint8_t *p; size_t n, cap; };

static void put(struct buf *b, const void *p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void put8(struct buf *b, unsigned v) { uint8_t c = (uint8_t)v; put(b, &c, 1); }

static void put32(struct buf *b, uint32_t v)
{
    uint8_t c[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };

    put(b, c, 4);
}

static void put64(struct buf *b, uint64_t v)
{
    put32(b, (uint32_t)v);
    put32(b, (uint32_t)(v >> 32));
}

/* 7z's variable-length number */
static void putn(struct buf *b, uint64_t v)
{
    uint8_t first = 0, mask = 0x80;
    int i;

    for (i = 0; i < 8; i++) {
        if (v < (1ull << (7 * (i + 1)))) {
            first |= (uint8_t)(v >> (8 * i));
            break;
        }
        first |= mask;
        mask >>= 1;
    }
    put8(b, first);
    for (; i > 0; i--) {
        put8(b, (uint8_t)v);
        v >>= 8;
    }
}

static void putbits(struct buf *b, const uint8_t *v, size_t n)
{
    size_t i;
    uint8_t c = 0;

    for (i = 0; i < n; i++) {
        if (v[i])
            c |= 0x80 >> (i & 7);
        if ((i & 7) == 7) {
            put8(b, c);
            c = 0;
        }
    }
    if (n & 7)
        put8(b, c);
}

/* UTF-8 -> UTF-16LE, a byte that is not UTF-8 taken as latin-1 */
static void put_name(struct buf *b, const char *s)
{
    const uint8_t *p = (const uint8_t *)s;

    while (*p) {
        uint32_t c = *p;
        int k = 0;

        if (c >= 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            c = (c & 7) << 18 | (p[1] & 63) << 12 | (p[2] & 63) << 6 | (p[3] & 63);
            k = 4;
        } else if (c >= 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            c = (c & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63);
            k = 3;
        } else if (c >= 0xC0 && (p[1] & 0xC0) == 0x80) {
            c = (c & 31) << 6 | (p[1] & 63);
            k = 2;
        } else {
            k = 1;
        }
        p += k;
        if (c >= 0x10000) {
            c -= 0x10000;
            put8(b, (0xD800 + (c >> 10)) & 0xFF);
            put8(b, (0xD800 + (c >> 10)) >> 8);
            put8(b, (0xDC00 + (c & 0x3FF)) & 0xFF);
            put8(b, (0xDC00 + (c & 0x3FF)) >> 8);
        } else {
            put8(b, c & 0xFF);
            put8(b, c >> 8);
        }
    }
    put8(b, 0);
    put8(b, 0);
}

/* 7-Zip's Get_WinAttribPosix_From_PosixMode */
static uint32_t attrib_of(const struct mkr_node *n)
{
    uint32_t m = n->mode | (n->type == N_DIR ? 0040000 : n->type == N_LINK ? 0120000 : 0100000);
    uint32_t a = n->type == N_DIR ? 0x10 : 0x20;

    if (!(m & 0222))
        a |= 1;
    return a | 0x8000 | m << 16;
}

/* ---- the item list ---------------------------------------------------------------------- */

struct list { struct item *v; size_t n, cap; };

static void collect(struct list *l, const struct mkr_node *d, const char *prefix)
{
    struct mkr_node **k = vfs_sorted((struct mkr_node *)d);
    int i;

    for (i = 0; i < d->nkids; i++) {
        const struct mkr_node *c = k[i];
        struct item *it;
        size_t pl = strlen(prefix);

        if (l->n == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 1024;
            l->v = realloc(l->v, l->cap * sizeof(*l->v));
        }
        it = &l->v[l->n++];
        memset(it, 0, sizeof(*it));
        it->n = c;
        it->path = malloc(pl + strlen(c->name) + 2);
        sprintf(it->path, "%s%s%s", prefix, pl ? "/" : "", c->name);
        it->attrib = attrib_of(c);
        it->size = c->type == N_FILE ? c->size : c->type == N_LINK ? strlen(c->target) : 0;
        it->stream = it->size > 0;
        if (c->type == N_DIR)
            collect(l, c, it->path);
    }
    free(k);
}

/* ---- the encoder's streams -------------------------------------------------------------- */

struct in {
    ISeqInStream vt;
    struct mkr_job *j;
    struct item **s;            /* the items with data, in order */
    size_t ns, cur;
    FILE *f;
    uint64_t left;
    UInt32 crc;
    int err;
};

static SRes in_read(ISeqInStreamPtr pp, void *buf, size_t *size)
{
    struct in *p = Z7_CONTAINER_FROM_VTBL(pp, struct in, vt);
    size_t want = *size, got = 0;

    while (got < want && p->cur < p->ns) {
        struct item *it = p->s[p->cur];
        size_t k;

        if (!p->left && !p->f) {
            /* start the item */
            p->left = it->size;
            p->crc = CRC_INIT_VAL;
            if (it->n->type == N_FILE && !(p->f = vfs_ropen(p->j, it->n))) {
                p->err = 1;
                return SZ_ERROR_READ;
            }
        }
        k = want - got < p->left ? want - got : (size_t)p->left;
        if (it->n->type == N_FILE) {
            if (fread((uint8_t *)buf + got, 1, k, p->f) != k) {
                mkr_fail(p->j, "read error in %s", it->path);
                p->err = 1;
                return SZ_ERROR_READ;
            }
        } else {
            memcpy((uint8_t *)buf + got, it->n->target + (it->size - p->left), k);
        }
        p->crc = CrcUpdate(p->crc, (uint8_t *)buf + got, k);
        got += k;
        p->left -= k;
        if (!p->left) {
            it->crc = CRC_GET_DIGEST(p->crc);
            if (p->f)
                fclose(p->f);
            p->f = NULL;
            p->cur++;
        }
    }
    *size = got;
    return SZ_OK;
}

struct out {
    ISeqOutStream vt;
    FILE *f;
    uint64_t n;
    int err;
};

static size_t out_write(ISeqOutStreamPtr pp, const void *buf, size_t size)
{
    struct out *p = Z7_CONTAINER_FROM_VTBL(pp, struct out, vt);

    if (fwrite(buf, 1, size, p->f) != size) {
        p->err = 1;
        return 0;
    }
    p->n += size;
    return size;
}

struct prog {
    ICompressProgress vt;
    struct mkr_job *j;
    uint64_t last;
};

static SRes prog_cb(ICompressProgressPtr pp, UInt64 in, UInt64 out)
{
    struct prog *p = Z7_CONTAINER_FROM_VTBL(pp, struct prog, vt);

    (void)out;
    if (in != (UInt64)(Int64)-1 && in > p->last) {
        uint64_t d = in - p->last;

        p->last = in;
        if (mkr_progress(p->j, d))
            return SZ_ERROR_PROGRESS;
    }
    return mkr_check(p->j) ? SZ_ERROR_PROGRESS : SZ_OK;
}

static void *sz_alloc(ISzAllocPtr p, size_t n) { (void)p; return malloc(n); }
static void sz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc alloc = { sz_alloc, sz_free };

/* ---- the archive ------------------------------------------------------------------------ */

static int copy_streams(struct in *in, struct out *out)
{
    uint8_t *b = malloc(1 << 20);
    int rc = 0;

    for (;;) {
        size_t n = 1 << 20;

        if (in_read(&in->vt, b, &n) || (n && out_write(&out->vt, b, n) != n)) {
            rc = -1;
            break;
        }
        if (!n)
            break;
        if (mkr_progress(in->j, n)) {
            rc = -1;
            break;
        }
    }
    free(b);
    return rc;
}

int pack_7z(struct mkr_job *j, const char *path)
{
    struct list l = { 0 };
    struct in in;
    struct out out;
    struct prog pr;
    struct buf h = { 0 };
    uint8_t sig[32], *bits;
    uint64_t total = 0, ft;
    size_t i, nstreams = 0, nempty = 0, k;
    Byte prop = 0;
    int rc = -1, copy = j->o->mx <= 0;
    UInt32 crc;

    collect(&l, j->root, "");
    /*
     * the items without data (directories, empty files) first, as 7-Zip
     * writes them: the SDK's reader (romset/sevenzip.c) counts an empty
     * item that sits between two items of a folder as part of that folder
     */
    {
        struct item *v = malloc((l.n + 1) * sizeof(*v));
        size_t o = 0;

        for (i = 0; i < l.n; i++)
            if (!l.v[i].stream)
                v[o++] = l.v[i];
        for (i = 0; i < l.n; i++)
            if (l.v[i].stream)
                v[o++] = l.v[i];
        free(l.v);
        l.v = v;
        l.cap = l.n;
    }
    memset(&in, 0, sizeof(in));
    in.vt.Read = in_read;
    in.j = j;
    in.s = malloc((l.n + 1) * sizeof(*in.s));
    for (i = 0; i < l.n; i++)
        if (l.v[i].stream) {
            in.s[in.ns++] = &l.v[i];
            total += l.v[i].size;
        }
    nstreams = in.ns;
    memset(&out, 0, sizeof(out));
    out.vt.Write = out_write;
    if (!(out.f = fopen(path, "wb"))) {
        mkr_fail(j, "cannot create %s", path);
        goto done;
    }
    setvbuf(out.f, NULL, _IOFBF, 1 << 20);
    memset(sig, 0, sizeof(sig));
    if (fwrite(sig, 1, 32, out.f) != 32) {
        mkr_fail(j, "write error in %s (disk full?)", path);
        goto done;
    }
    mkr_phase(j, "pack", total);
    if (nstreams && copy) {
        if (copy_streams(&in, &out))
            goto encerr;
    } else if (nstreams) {
        CLzma2EncHandle enc = Lzma2Enc_Create(&alloc, &alloc);
        CLzma2EncProps p;
        SRes r;

        Lzma2EncProps_Init(&p);
        p.lzmaProps.level = j->o->mx > 9 ? 9 : j->o->mx;
        p.lzmaProps.reduceSize = total;
        p.numTotalThreads = mkr_threads(j);
        /*
         * blocks are what runs in parallel (two threads each): the SDK's default,
         * four dictionaries, leaves most threads idle on a set of a few hundred MB,
         * so blocks are cut to give each thread pair two, but not below one
         * dictionary
         */
        if (p.numTotalThreads > 2) {
            UInt64 dict = LzmaEncProps_GetDictSize(&p.lzmaProps), b = total / (UInt64)p.numTotalThreads;

            p.blockSize = b < dict ? dict : b > 4 * dict ? 4 * dict : b;
        }
        Lzma2EncProps_Normalize(&p);
        memset(&pr, 0, sizeof(pr));
        pr.vt.Progress = prog_cb;
        pr.j = j;
        r = Lzma2Enc_SetProps(enc, &p);
        if (r == SZ_OK) {
            Lzma2Enc_SetDataSize(enc, total);
            prop = Lzma2Enc_WriteProperties(enc);
            r = Lzma2Enc_Encode2(enc, &out.vt, NULL, NULL, &in.vt, NULL, 0, &pr.vt);
        }
        Lzma2Enc_Destroy(enc);
        if (r != SZ_OK) {
            if (r == SZ_ERROR_MEM)
                mkr_fail(j, "out of memory for LZMA2 (try a lower --mx)");
            else if (out.err)
                mkr_fail(j, "write error in %s (disk full?)", path);
            else if (!j->err[0])
                mkr_fail(j, "LZMA2 encoder error %d", r);
            goto done;
        }
    }
    if (in.cur != in.ns) {
encerr:
        if (!j->err[0])
            mkr_fail(j, out.err ? "write error in %s (disk full?)" : "%s: not all data was packed", path);
        goto done;
    }

    /* the header */
    put8(&h, K_HEADER);
    if (nstreams) {
        put8(&h, K_MAIN_STREAMS);
        put8(&h, K_PACK_INFO);
        putn(&h, 0);
        putn(&h, 1);
        put8(&h, K_SIZE);
        putn(&h, out.n);
        put8(&h, K_END);
        put8(&h, K_UNPACK_INFO);
        put8(&h, K_FOLDER);
        putn(&h, 1);
        put8(&h, 0);
        putn(&h, 1);                    /* one coder */
        if (copy) {
            put8(&h, 0x01);
            put8(&h, 0x00);
        } else {
            put8(&h, 0x21);             /* id size 1, properties */
            put8(&h, 0x21);             /* LZMA2 */
            putn(&h, 1);
            put8(&h, prop);
        }
        put8(&h, K_UNPACK_SIZE);
        putn(&h, total);
        put8(&h, K_END);
        put8(&h, K_SUBSTREAMS);
        put8(&h, K_NUM_UNPACK);
        putn(&h, nstreams);
        put8(&h, K_SIZE);
        for (k = 0; k + 1 < nstreams; k++)
            putn(&h, in.s[k]->size);
        put8(&h, K_CRC);
        put8(&h, 1);
        for (k = 0; k < nstreams; k++)
            put32(&h, in.s[k]->crc);
        put8(&h, K_END);
        put8(&h, K_END);
    }
    put8(&h, K_FILES);
    putn(&h, l.n);
    bits = calloc(l.n + 1, 1);
    if (nstreams < l.n) {
        struct buf t = { 0 };

        for (i = 0; i < l.n; i++)
            bits[i] = !l.v[i].stream;
        putbits(&t, bits, l.n);
        put8(&h, K_EMPTY_STREAM);
        putn(&h, t.n);
        put(&h, t.p, t.n);
        t.n = 0;
        for (i = 0; i < l.n; i++)
            if (!l.v[i].stream)
                bits[nempty++] = l.v[i].n->type != N_DIR;
        putbits(&t, bits, nempty);
        put8(&h, K_EMPTY_FILE);
        putn(&h, t.n);
        put(&h, t.p, t.n);
        free(t.p);
    }
    free(bits);
    {
        struct buf t = { 0 };

        put8(&t, 0);
        for (i = 0; i < l.n; i++)
            put_name(&t, l.v[i].path);
        put8(&h, K_NAME);
        putn(&h, t.n);
        put(&h, t.p, t.n);
        t.n = 0;
        /* the build time as FILETIME for every entry */
        ft = ((uint64_t)time(NULL) + 11644473600ull) * 10000000ull;
        put8(&t, 1);
        put8(&t, 0);
        for (i = 0; i < l.n; i++)
            put64(&t, ft);
        put8(&h, K_MTIME);
        putn(&h, t.n);
        put(&h, t.p, t.n);
        t.n = 0;
        put8(&t, 1);
        put8(&t, 0);
        for (i = 0; i < l.n; i++)
            put32(&t, l.v[i].attrib);
        put8(&h, K_ATTRIB);
        putn(&h, t.n);
        put(&h, t.p, t.n);
        free(t.p);
    }
    put8(&h, K_END);
    put8(&h, K_END);
    if (fwrite(h.p, 1, h.n, out.f) != h.n) {
        mkr_fail(j, "write error in %s (disk full?)", path);
        goto done;
    }
    /* the start header */
    memcpy(sig, "7z\xBC\xAF\x27\x1C", 6);
    sig[6] = 0;
    sig[7] = 4;
    {
        struct buf s = { 0 };

        put64(&s, out.n);
        put64(&s, h.n);
        put32(&s, CrcCalc(h.p, h.n));
        memcpy(sig + 12, s.p, 20);
        free(s.p);
    }
    crc = CrcCalc(sig + 12, 20);
    sig[8] = (uint8_t)crc;
    sig[9] = (uint8_t)(crc >> 8);
    sig[10] = (uint8_t)(crc >> 16);
    sig[11] = (uint8_t)(crc >> 24);
    if (fseek(out.f, 0, SEEK_SET) || fwrite(sig, 1, 32, out.f) != 32) {
        mkr_fail(j, "write error in %s", path);
        goto done;
    }
    rc = 0;
done:
    if (in.f)
        fclose(in.f);
    if (out.f && fclose(out.f) && !rc)
        rc = mkr_fail(j, "write error in %s (disk full?)", path);
    for (i = 0; i < l.n; i++)
        free(l.v[i].path);
    free(l.v);
    free(in.s);
    free(h.p);
    return rc;
}
