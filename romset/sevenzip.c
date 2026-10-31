/*
 * .7z rom sets, decoded as a stream.
 *
 * The LZMA SDK (third_party/lzma) parses the archive (SzArEx_Open; its
 * compressed header is small and decoded in memory).  The file data is not
 * decoded with SzArEx_Extract, which unpacks a whole solid block into RAM
 * (gigabytes for a "7z a -mx9" set, too much for the 32-bit Windows loader).
 * Instead each folder's coder graph becomes a chain of pull streams:
 *
 *   pack stream (a byte range of the archive, its own FILE)
 *   -> LZMA / LZMA2 (LzmaDec / Lzma2Dec, memory = the dictionary)
 *   -> BCJ (x86) / Delta filter, or BCJ2 with its four inputs
 *
 * and the files of the folder are read off the end of the chain in order.
 * Memory use is the dictionary sizes (64 MiB for -mx9) plus small buffers.
 * Supported methods: Copy, LZMA, LZMA2, BCJ, BCJ2, Delta (what 7-Zip uses
 * for x86 data); others (PPMd, BZip2, ARM filters, AES) are refused.
 *
 * Symlinks: p7zip / 7-Zip for Linux store the unix mode in the high 16 bits
 * of the attributes (with 0x8000 set); a link's data is its target.
 */
#include <stdlib.h>
#include <string.h>

#include "romset.h"

#include "../third_party/lzma/7z.h"
#include "../third_party/lzma/7zCrc.h"
#include "../third_party/lzma/Bcj2.h"
#include "../third_party/lzma/Bra.h"
#include "../third_party/lzma/Delta.h"
#include "../third_party/lzma/Lzma2Dec.h"
#include "../third_party/lzma/LzmaDec.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

#define K_COPY  0x00
#define K_DELTA 0x03
#define K_LZMA2 0x21
#define K_LZMA  0x030101
#define K_BCJ   0x03030103
#define K_BCJ2  0x0303011B

#define BUF (1 << 16)

static const char *arcname;

static int fail(const char *what)
{
    snprintf(rs_error, sizeof(rs_error), "%s: %s", arcname, what);
    return -1;
}

static void *sz_alloc(ISzAllocPtr p, size_t n) { (void)p; return n ? malloc(n) : NULL; }
static void sz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc alloc = { sz_alloc, sz_free };

/* ---- the archive as an ISeekInStream ---------------------------------------------------- */

typedef struct {
    ISeekInStream vt;
    FILE *f;
} CFileSeek;

static SRes fs_read(ISeekInStreamPtr pp, void *buf, size_t *size)
{
    CFileSeek *p = (CFileSeek *)(void *)pp;

    *size = fread(buf, 1, *size, p->f);
    return ferror(p->f) ? SZ_ERROR_READ : SZ_OK;
}

static SRes fs_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
    CFileSeek *p = (CFileSeek *)(void *)pp;
    int w = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;

    if (fseek64(p->f, *pos, w))
        return SZ_ERROR_READ;
    *pos = ftell64(p->f);
    return SZ_OK;
}

/* ---- pull streams ----------------------------------------------------------------------- */

struct src {
    /* read up to n bytes; *got = 0 at the end; 0 or an SRes error */
    int (*read)(struct src *s, Byte *buf, size_t n, size_t *got);
    void (*free)(struct src *s);
};

/* a byte range of the archive */
struct pack_src {
    struct src s;
    FILE *f;
    UInt64 left;
};

static int pack_read(struct src *s, Byte *buf, size_t n, size_t *got)
{
    struct pack_src *p = (struct pack_src *)s;

    if (n > p->left)
        n = (size_t)p->left;
    *got = n ? fread(buf, 1, n, p->f) : 0;
    if (n && !*got)
        return SZ_ERROR_INPUT_EOF;
    p->left -= *got;
    return 0;
}

static void pack_free(struct src *s)
{
    fclose(((struct pack_src *)s)->f);
    free(s);
}

static struct src *pack_new(const char *path, UInt64 off, UInt64 size)
{
    struct pack_src *p = calloc(1, sizeof(*p));

    p->f = fopen(path, "rb");
    if (!p->f || fseek64(p->f, (long long)off, SEEK_SET)) {
        if (p->f)
            fclose(p->f);
        free(p);
        return NULL;
    }
    setvbuf(p->f, NULL, _IOFBF, BUF);
    p->s.read = pack_read;
    p->s.free = pack_free;
    p->left = size;
    return &p->s;
}

/* LZMA and LZMA2 */
struct lz_src {
    struct src s;
    struct src *in;
    int two;
    CLzmaDec lz;
    CLzma2Dec lz2;
    UInt64 out_left;
    Byte buf[BUF];
    size_t pos, lim;
    int in_end;
};

static int lz_read(struct src *s, Byte *dst, size_t n, size_t *got)
{
    struct lz_src *p = (struct lz_src *)s;

    *got = 0;
    if (n > p->out_left)
        n = (size_t)p->out_left;
    while (*got < n) {
        SizeT dl = n - *got, sl;
        ELzmaStatus st;
        SRes r;
        ELzmaFinishMode fm;

        if (p->pos == p->lim && !p->in_end) {
            size_t g;
            int e = p->in->read(p->in, p->buf, sizeof(p->buf), &g);

            if (e)
                return e;
            p->pos = 0;
            p->lim = g;
            p->in_end = !g;
        }
        sl = p->lim - p->pos;
        fm = (UInt64)dl == p->out_left - *got ? LZMA_FINISH_END : LZMA_FINISH_ANY;
        if (p->two)
            r = Lzma2Dec_DecodeToBuf(&p->lz2, dst + *got, &dl, p->buf + p->pos, &sl, fm, &st);
        else
            r = LzmaDec_DecodeToBuf(&p->lz, dst + *got, &dl, p->buf + p->pos, &sl, fm, &st);
        if (r)
            return r;
        p->pos += sl;
        *got += dl;
        if (!dl && !sl) {
            if (p->in_end || st == LZMA_STATUS_FINISHED_WITH_MARK)
                break;      /* no more output */
        }
    }
    p->out_left -= *got;
    if (!*got && n)
        return SZ_ERROR_DATA;   /* the stream ended before its unpack size */
    return 0;
}

static void lz_free(struct src *s)
{
    struct lz_src *p = (struct lz_src *)s;

    if (p->two)
        Lzma2Dec_Free(&p->lz2, &alloc);
    else
        LzmaDec_Free(&p->lz, &alloc);
    p->in->free(p->in);
    free(p);
}

static struct src *lz_new(struct src *in, int two, const Byte *props, unsigned nprops, UInt64 out_size)
{
    struct lz_src *p = calloc(1, sizeof(*p));
    SRes r;

    p->two = two;
    if (two) {
        LzmaDec_CONSTRUCT(&p->lz2.decoder)
        r = nprops == 1 ? Lzma2Dec_Allocate(&p->lz2, props[0], &alloc) : SZ_ERROR_UNSUPPORTED;
        if (!r)
            Lzma2Dec_Init(&p->lz2);
    } else {
        LzmaDec_CONSTRUCT(&p->lz)
        r = LzmaDec_Allocate(&p->lz, props, nprops, &alloc);
        if (!r)
            LzmaDec_Init(&p->lz);
    }
    if (r) {
        fail(r == SZ_ERROR_MEM ? "out of memory for the LZMA dictionary (repack with a smaller -md)"
                               : "bad LZMA properties");
        free(p);
        in->free(in);
        return NULL;
    }
    p->in = in;
    p->out_left = out_size;
    p->s.read = lz_read;
    p->s.free = lz_free;
    return &p->s;
}

/* BCJ (x86) and Delta: in-place filters over a buffer */
struct filt_src {
    struct src s;
    struct src *in;
    UInt32 method, ip, x86_state;
    unsigned delta;
    Byte delta_state[DELTA_STATE_SIZE];
    Byte buf[BUF];
    size_t pos, conv, lim;      /* [pos, conv) ready, [conv, lim) not converted yet */
    int in_end;
};

static int filt_read(struct src *s, Byte *dst, size_t n, size_t *got)
{
    struct filt_src *p = (struct filt_src *)s;

    *got = 0;
    while (p->pos == p->conv) {
        size_t keep = p->lim - p->conv, g;
        int e;

        if (p->in_end && !keep)
            return 0;
        memmove(p->buf, p->buf + p->conv, keep);
        p->pos = p->conv = 0;
        p->lim = keep;
        if (!p->in_end) {
            if ((e = p->in->read(p->in, p->buf + keep, sizeof(p->buf) - keep, &g)))
                return e;
            p->lim += g;
            p->in_end = !g;
        }
        if (p->method == K_DELTA) {
            Delta_Decode(p->delta_state, p->delta, p->buf, p->lim);
            p->conv = p->lim;
        } else if (p->in_end) {
            /* the last bytes stay as they are (the encoder left them too) */
            Byte *e2 = z7_BranchConvSt_X86_Dec(p->buf, p->lim, p->ip, &p->x86_state);

            (void)e2;
            p->conv = p->lim;
        } else {
            Byte *e2 = z7_BranchConvSt_X86_Dec(p->buf, p->lim, p->ip, &p->x86_state);

            p->conv = (size_t)(e2 - p->buf);
            p->ip += (UInt32)p->conv;
        }
    }
    *got = p->conv - p->pos < n ? p->conv - p->pos : n;
    memcpy(dst, p->buf + p->pos, *got);
    p->pos += *got;
    return 0;
}

static void filt_free(struct src *s)
{
    struct filt_src *p = (struct filt_src *)s;

    p->in->free(p->in);
    free(p);
}

static struct src *filt_new(struct src *in, UInt32 method, const Byte *props, unsigned nprops)
{
    struct filt_src *p = calloc(1, sizeof(*p));

    p->in = in;
    p->method = method;
    p->x86_state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
    if (method == K_DELTA) {
        p->delta = nprops == 1 ? (unsigned)props[0] + 1 : 0;
        Delta_Init(p->delta_state);
        if (!p->delta) {
            free(p);
            in->free(in);
            fail("bad Delta properties");
            return NULL;
        }
    }
    p->s.read = filt_read;
    p->s.free = filt_free;
    return &p->s;
}

/* BCJ2: four inputs (main, call, jump, range coder) */
struct bcj2_src {
    struct src s;
    struct src *in[4];
    Byte buf[4][BUF];
    int end[4];
    CBcj2Dec dec;
    UInt64 out_left;
};

static int bcj2_read(struct src *s, Byte *dst, size_t n, size_t *got)
{
    struct bcj2_src *p = (struct bcj2_src *)s;

    if (n > p->out_left)
        n = (size_t)p->out_left;
    p->dec.dest = dst;
    p->dec.destLim = dst + n;
    while (p->dec.dest != p->dec.destLim) {
        unsigned st;

        if (Bcj2Dec_Decode(&p->dec))
            return SZ_ERROR_DATA;
        st = p->dec.state;
        if (p->dec.dest == p->dec.destLim)
            break;              /* done, whatever it would need next */
        if (st < BCJ2_NUM_STREAMS) {
            /* that input is used up: refill it (call/jump in whole words) */
            size_t have = 0, g;
            int e;

            if (p->end[st])
                return SZ_ERROR_DATA;
            do {
                if ((e = p->in[st]->read(p->in[st], p->buf[st] + have, BUF - have, &g)))
                    return e;
                have += g;
            } while (g && BCJ2_IS_32BIT_STREAM(st) && (have & 3));
            if (!have) {
                p->end[st] = 1;
                if (getenv("IGS_ROMSET_DEBUG"))
                    fprintf(stderr, "bcj2: stream %u ended, %lu bytes of output still wanted, e=%d\n", st,
                            (unsigned long)(p->dec.destLim - p->dec.dest), e);
            }
            p->dec.bufs[st] = p->buf[st];
            p->dec.lims[st] = p->buf[st] + have;
            if (!have)
                return SZ_ERROR_DATA;
        } else if (st == BCJ2_DEC_STATE_ERROR) {
            return SZ_ERROR_DATA;
        }
    }
    *got = n;
    p->out_left -= n;
    return 0;
}

static void bcj2_free(struct src *s)
{
    struct bcj2_src *p = (struct bcj2_src *)s;
    int i;

    for (i = 0; i < 4; i++)
        if (p->in[i])
            p->in[i]->free(p->in[i]);
    free(p);
}

/* ---- a folder's coder graph ------------------------------------------------------------- */

struct folder_ctx {
    const CSzArEx *db;
    UInt32 fi;
    CSzFolder f;
    const Byte *props;
    const char *path;
};

static struct src *coder_src(struct folder_ctx *c, UInt32 coder);

/* the source feeding in-stream 'si' (a packed stream, or another coder's output) */
static struct src *instream_src(struct folder_ctx *c, UInt32 si)
{
    const CSzAr *ar = &c->db->db;
    UInt32 k;

    for (k = 0; k < c->f.NumPackStreams; k++)
        if (c->f.PackStreams[k] == si) {
            UInt32 pi = ar->FoStartPackStreamIndex[c->fi] + k;
            struct src *s = pack_new(c->path, c->db->dataPos + ar->PackPositions[pi],
                                     ar->PackPositions[pi + 1] - ar->PackPositions[pi]);

            if (!s)
                fail("cannot reopen the archive");
            return s;
        }
    for (k = 0; k < c->f.NumBonds; k++)
        if (c->f.Bonds[k].InIndex == si)
            return coder_src(c, c->f.Bonds[k].OutIndex);
    fail("broken coder graph");
    return NULL;
}

static struct src *coder_src(struct folder_ctx *c, UInt32 ci)
{
    const CSzAr *ar = &c->db->db;
    const CSzCoderInfo *co = &c->f.Coders[ci];
    UInt64 out_size = ar->CoderUnpackSizes[ar->FoToCoderUnpackSizes[c->fi] + ci];
    const Byte *props = c->props + co->PropsOffset;
    UInt32 first = 0, i;
    struct src *in;

    for (i = 0; i < ci; i++)
        first += c->f.Coders[i].NumStreams;
    if (co->MethodID == K_BCJ2) {
        struct bcj2_src *p;

        if (co->NumStreams != 4) {
            fail("bad BCJ2 coder");
            return NULL;
        }
        p = calloc(1, sizeof(*p));
        for (i = 0; i < 4; i++)
            if (!(p->in[i] = instream_src(c, first + i))) {
                bcj2_free(&p->s);
                return NULL;
            }
        Bcj2Dec_Init(&p->dec);
        for (i = 0; i < 4; i++)
            p->dec.bufs[i] = p->dec.lims[i] = p->buf[i];
        p->out_left = out_size;
        p->s.read = bcj2_read;
        p->s.free = bcj2_free;
        return &p->s;
    }
    if (co->NumStreams != 1) {
        fail("unsupported coder");
        return NULL;
    }
    switch (co->MethodID) {
    case K_COPY:
        return instream_src(c, first);
    case K_LZMA:
    case K_LZMA2:
        if (!(in = instream_src(c, first)))
            return NULL;
        return lz_new(in, co->MethodID == K_LZMA2, props, co->PropsSize, out_size);
    case K_BCJ:
    case K_DELTA:
        if (!(in = instream_src(c, first)))
            return NULL;
        return filt_new(in, co->MethodID, props, co->PropsSize);
    default: {
        char m[96];

        snprintf(m, sizeof(m), "compression method 0x%X is not supported (use LZMA/LZMA2)", (unsigned)co->MethodID);
        fail(m);
        return NULL;
    }
    }
}

static struct src *folder_open(const CSzArEx *db, UInt32 fi, const char *path)
{
    struct folder_ctx c;
    CSzData sd;
    const Byte *data = db->db.CodersData + db->db.FoCodersOffsets[fi];

    memset(&c, 0, sizeof(c));
    c.db = db;
    c.fi = fi;
    c.path = path;
    c.props = data;
    sd.Data = data;
    sd.Size = db->db.FoCodersOffsets[fi + 1] - db->db.FoCodersOffsets[fi];
    if (SzGetNextFolderItem(&c.f, &sd) != SZ_OK) {
        fail("unsupported folder (too many coders?)");
        return NULL;
    }
    if (getenv("IGS_ROMSET_DEBUG")) {
        UInt32 k;

        fprintf(stderr, "folder %u: %u coders, unpack stream %u, packs", (unsigned)fi, (unsigned)c.f.NumCoders,
                (unsigned)c.f.UnpackStream);
        for (k = 0; k < c.f.NumPackStreams; k++)
            fprintf(stderr, " %u", (unsigned)c.f.PackStreams[k]);
        for (k = 0; k < c.f.NumCoders; k++)
            fprintf(stderr, "; coder %u method %X streams %u size %llu", (unsigned)k, (unsigned)c.f.Coders[k].MethodID,
                    (unsigned)c.f.Coders[k].NumStreams,
                    (unsigned long long)db->db.CoderUnpackSizes[db->db.FoToCoderUnpackSizes[fi] + k]);
        for (k = 0; k < c.f.NumBonds; k++)
            fprintf(stderr, "; bond in %u <- coder %u", (unsigned)c.f.Bonds[k].InIndex, (unsigned)c.f.Bonds[k].OutIndex);
        fputc('\n', stderr);
    }
    return coder_src(&c, c.f.UnpackStream);
}

/* ---- extraction ------------------------------------------------------------------------- */

static char *utf8_name(const CSzArEx *db, size_t i)
{
    size_t n = SzArEx_GetFileNameUtf16(db, i, NULL), j;
    UInt16 *w = malloc(n * sizeof(UInt16));
    char *s = malloc(n * 3 + 1), *o = s;

    SzArEx_GetFileNameUtf16(db, i, w);
    for (j = 0; j < n && w[j]; j++) {
        unsigned c = w[j];

        if (c >= 0xD800 && c < 0xDC00 && j + 1 < n && w[j + 1] >= 0xDC00 && w[j + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[++j] - 0xDC00);
            *o++ = (char)(0xF0 | c >> 18);
            *o++ = (char)(0x80 | ((c >> 12) & 63));
            *o++ = (char)(0x80 | ((c >> 6) & 63));
            *o++ = (char)(0x80 | (c & 63));
            continue;
        }
        if (c < 0x80) {
            *o++ = (char)c;
        } else if (c < 0x800) {
            *o++ = (char)(0xC0 | c >> 6);
            *o++ = (char)(0x80 | (c & 63));
        } else {
            *o++ = (char)(0xE0 | c >> 12);
            *o++ = (char)(0x80 | ((c >> 6) & 63));
            *o++ = (char)(0x80 | (c & 63));
        }
    }
    *o = 0;
    free(w);
    for (o = s; *o; o++)
        if (*o == '\\')
            *o = '/';
    return s;
}

static unsigned unix_mode(const CSzArEx *db, UInt32 i)
{
    UInt32 a;

    if (!SzBitWithVals_Check(&db->Attribs, i))
        return 0;
    a = db->Attribs.Vals[i];
    return (a & 0x8000) ? a >> 16 : 0;
}

int rs_extract_7z(const char *path, struct rs_sink *sk)
{
    CFileSeek in;
    CLookToRead2 look;
    CSzArEx db;
    Byte *buf = NULL;
    UInt32 i, fi;
    int rc = 0;
    SRes r;
    static int crc_ready;

    arcname = path;
    if (!crc_ready) {
        CrcGenerateTable();
        crc_ready = 1;
    }
    in.vt.Read = fs_read;
    in.vt.Seek = fs_seek;
    if (!(in.f = fopen(path, "rb")))
        return fail("cannot open");
    LookToRead2_CreateVTable(&look, 0);
    look.buf = malloc(BUF);
    look.bufSize = BUF;
    look.realStream = &in.vt;
    LookToRead2_INIT(&look)
    SzArEx_Init(&db);
    r = SzArEx_Open(&db, &look.vt, &alloc, &alloc);
    if (r != SZ_OK) {
        fclose(in.f);
        free(look.buf);
        return fail(r == SZ_ERROR_NO_ARCHIVE ? "not a 7z archive" :
                    r == SZ_ERROR_UNSUPPORTED ? "unsupported 7z archive (encrypted header?)" : "corrupt 7z archive");
    }
    for (i = 0; i < db.NumFiles; i++) {
        char *n = utf8_name(&db, i);

        rs_sink_scan(sk, n);
        free(n);
    }
    /* directories and empty files (no folder) */
    for (i = 0; i < db.NumFiles && !rc; i++) {
        char *n;
        const char *rel;
        unsigned mode = unix_mode(&db, i);

        if (db.FileToFolder[i] != (UInt32)-1)
            continue;
        n = utf8_name(&db, i);
        if ((rel = rs_sink_name(sk, n))) {
            if (SzArEx_IsDir(&db, i)) {
                rc = rs_sink_dir(sk, rel, mode & 07777);
            } else if ((mode & 0170000) == 0120000) {
                rc = fail("empty symlink");
            } else {
                FILE *f = rs_sink_file(sk, rel);

                if (!f)
                    rc = -1;
                else
                    rs_sink_close(sk, f, rel, mode & 07777);
            }
        }
        free(n);
    }
    buf = malloc(BUF);
    /* the folders, each streamed once from start to end */
    for (fi = 0; fi < db.db.NumFolders && !rc; fi++) {
        struct src *s = folder_open(&db, fi, path);

        if (!s) {
            rc = -1;
            break;
        }
        for (i = db.FolderToFile[fi]; i < db.FolderToFile[fi + 1] && !rc; i++) {
            UInt64 left;
            UInt32 crc = CRC_INIT_VAL;
            unsigned mode;
            char *n, target[4097];
            const char *rel;
            FILE *f = NULL;
            int link;
            size_t tl = 0;

            if (db.FileToFolder[i] != fi)
                continue;
            left = SzArEx_GetFileSize(&db, i);
            mode = unix_mode(&db, i);
            link = (mode & 0170000) == 0120000;
            n = utf8_name(&db, i);
            rel = rs_sink_name(sk, n);
            if (rel && link && left >= sizeof(target))
                rc = fail("symlink target too long");
            else if (rel && !link && !(f = rs_sink_file(sk, rel)))
                rc = -1;
            while (left && !rc) {
                size_t want = left < BUF ? (size_t)left : BUF, got;
                int e = s->read(s, buf, want, &got);

                if (e || !got) {
                    if (getenv("IGS_ROMSET_DEBUG"))
                        fprintf(stderr, "folder %u file %u %s: read error %d, %llu bytes left\n",
                                (unsigned)fi, (unsigned)i, n, e, (unsigned long long)left);
                    fail(e == SZ_ERROR_MEM ? "out of memory" : e == SZ_ERROR_READ ? "read error" : "data error");
                    rc = -1;
                    break;
                }
                crc = CrcUpdate(crc, buf, got);
                if (f && fwrite(buf, 1, got, f) != got) {
                    snprintf(rs_error, sizeof(rs_error), "%s: write error (disk full?)", rel);
                    rc = -1;
                }
                if (rel && link) {
                    memcpy(target + tl, buf, got);
                    tl += got;
                }
                left -= got;
            }
            if (f)
                rs_sink_close(sk, f, rel, mode & 07777);
            if (!rc && SzBitWithVals_Check(&db.CRCs, i) && CRC_GET_DIGEST(crc) != db.CRCs.Vals[i]) {
                snprintf(rs_error, sizeof(rs_error), "%s: CRC error in %s", path, n);
                rc = -1;
            }
            if (!rc && rel && link) {
                target[tl] = 0;
                rc = rs_sink_link(sk, rel, target);
            }
            free(n);
        }
        s->free(s);
    }
    free(buf);
    SzArEx_Free(&db, &alloc);
    free(look.buf);
    fclose(in.f);
    return rc;
}
