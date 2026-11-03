/*
 * Disk images: a raw .img, a MAME .chd (read in place through libchdr, no
 * temporary image), or a file of the set itself (the SquashFS images of a
 * "loop" step).  Reads are positioned, so a reader keeps no file offset.
 * Also the MBR/EBR partition table and the image SHA-1.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "mkr.h"

#include "Sha1.h"
#include "Threads.h"
#include "libchdr/chd.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct mkr_chd {
    chd_file *chd;
    uint32_t hunkbytes;
    uint8_t *hunk;
    uint32_t cur;               /* the hunk in hunk[], ~0 = none */
};

/* ---- raw files -------------------------------------------------------------------------- */

static int raw_open(const char *path, void **fh, uint64_t *size)
{
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz;

    if (h == INVALID_HANDLE_VALUE)
        return -1;
    GetFileSizeEx(h, &sz);
    *size = (uint64_t)sz.QuadPart;
    *fh = h;
#else
    int fd = open(path, O_RDONLY);
    struct stat st;

    if (fd < 0)
        return -1;
    fstat(fd, &st);
    *size = (uint64_t)st.st_size;
    *fh = (void *)(intptr_t)(fd + 1);
#endif
    return 0;
}

static void raw_close(void *fh)
{
#ifdef _WIN32
    CloseHandle((HANDLE)fh);
#else
    close((int)(intptr_t)fh - 1);
#endif
}

/* bytes read at off (short at the end of the file); -1 on error */
static long long raw_pread(void *fh, uint64_t off, void *buf, size_t n)
{
    size_t got = 0;

    while (got < n) {
#ifdef _WIN32
        OVERLAPPED ov;
        DWORD k = 0, want = n - got > (1u << 30) ? (1u << 30) : (DWORD)(n - got);

        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(off + got);
        ov.OffsetHigh = (DWORD)((off + got) >> 32);
        if (!ReadFile((HANDLE)fh, (uint8_t *)buf + got, want, &k, &ov))
            return GetLastError() == ERROR_HANDLE_EOF ? (long long)got : -1;
#else
        ssize_t k = pread((int)(intptr_t)fh - 1, (uint8_t *)buf + got, n - got, (off_t)(off + got));

        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
#endif
        if (!k)
            break;
        got += (size_t)k;
    }
    return (long long)got;
}

/* ---- the image -------------------------------------------------------------------------- */

static int ends_with_chd(const char *p)
{
    size_t l = strlen(p);

    return l >= 4 && p[l - 4] == '.' && (p[l - 3] | 32) == 'c' && (p[l - 2] | 32) == 'h' && (p[l - 1] | 32) == 'd';
}

int img_open(struct mkr_job *j, struct mkr_img *m, const char *path)
{
    memset(m, 0, sizeof(*m));
    if (ends_with_chd(path)) {
        struct mkr_chd *c = calloc(1, sizeof(*c));
        const chd_header *h;
        chd_error e = chd_open(path, CHD_OPEN_READ, NULL, &c->chd);

        if (e != CHDERR_NONE) {
            free(c);
            return mkr_fail(j, "%s: %s", path, chd_error_string(e));
        }
        h = chd_get_header(c->chd);
        c->hunkbytes = h->hunkbytes;
        c->hunk = malloc(c->hunkbytes);
        c->cur = ~0u;
        m->chd = c;
        m->size = h->logicalbytes;
        return 0;
    }
    if (raw_open(path, &m->fh, &m->size))
        return mkr_fail(j, "cannot open %s: %s", path, strerror(errno));
    return 0;
}

int img_open_node(struct mkr_job *j, struct mkr_img *m, const struct mkr_node *f)
{
    char p[MKR_PATH];

    memset(m, 0, sizeof(*m));
    if (!f->blob)
        return mkr_fail(j, "empty image file");
    vfs_file_path(j, f, p, sizeof(p));
    if (raw_open(p, &m->fh, &m->size))
        return mkr_fail(j, "cannot open %s", p);
    return 0;
}

void img_close(struct mkr_img *m)
{
    if (m->chd) {
        chd_close(m->chd->chd);
        free(m->chd->hunk);
        free(m->chd);
    }
    if (m->fh)
        raw_close(m->fh);
    memset(m, 0, sizeof(*m));
}

static long long chd_pread(struct mkr_chd *c, uint64_t size, uint64_t off, void *buf, size_t n)
{
    size_t got = 0;

    if (off >= size)
        return 0;
    if (n > size - off)
        n = (size_t)(size - off);
    while (got < n) {
        uint64_t pos = off + got;
        uint32_t hunk = (uint32_t)(pos / c->hunkbytes), in = (uint32_t)(pos % c->hunkbytes);
        size_t k = c->hunkbytes - in < n - got ? c->hunkbytes - in : n - got;

        if (!in && k == c->hunkbytes) {
            /* a whole hunk: straight into the caller's buffer */
            if (chd_read(c->chd, hunk, (uint8_t *)buf + got) != CHDERR_NONE)
                return -1;
        } else {
            if (c->cur != hunk) {
                c->cur = ~0u;
                if (chd_read(c->chd, hunk, c->hunk) != CHDERR_NONE)
                    return -1;
                c->cur = hunk;
            }
            memcpy((uint8_t *)buf + got, c->hunk + in, k);
        }
        got += k;
    }
    return (long long)got;
}

static long long img_pread(struct mkr_img *m, uint64_t off, void *buf, size_t n)
{
    return m->chd ? chd_pread(m->chd, m->size, off, buf, n) : raw_pread(m->fh, off, buf, n);
}

int img_read(struct mkr_job *j, struct mkr_img *m, uint64_t off, void *buf, size_t n)
{
    long long got = img_pread(m, off, buf, n);

    if (got < 0)
        return mkr_fail(j, "read error at offset %llu%s", (unsigned long long)off, m->chd ? " (CHD)" : "");
    if ((size_t)got != n)
        return mkr_fail(j, "read past the end of the image (offset %llu)", (unsigned long long)(off + got));
    return 0;
}

int img_chd_sha1(struct mkr_job *j, const char *path, char hex[41])
{
    uint8_t h[124];
    FILE *f = fopen(path, "rb");
    size_t got;

    hex[0] = 0;
    if (!f)
        return mkr_fail(j, "cannot open %s", path);
    got = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (got < 16 || memcmp(h, "MComprHD", 8))
        return mkr_fail(j, "%s: not a CHD", path);
    /* v5: the raw data SHA-1 at 64 */
    if (got >= 84 && (h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15]) == 5)
        mkr_sha1_hex(h + 64, hex);
    return 0;
}

/*
 * mkromset.py partitions(): p1..p4 from the MBR, then the logical partitions
 * p5.. from each extended partition's EBR chain.
 */
int img_partitions(struct mkr_job *j)
{
    uint8_t mbr[512], b[512];
    int i;

    if (img_read(j, &j->img, 0, mbr, 512) || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        j->err[0] = 0;
        return mkr_fail(j, "%s: no MBR partition table", j->o->image);
    }
    for (i = 0; i < 4; i++) {
        j->pstart[i + 1] = rd32(mbr + 446 + 16 * i + 8);
        j->pcount[i + 1] = rd32(mbr + 446 + 16 * i + 12);
        j->phave[i + 1] = 1;
    }
    for (i = 0; i < 4; i++) {
        uint8_t t = mbr[446 + 16 * i + 4];
        uint64_t ext, ebr;
        int n = 5;

        if (t != 0x05 && t != 0x0f && t != 0x85)
            continue;
        ext = ebr = j->pstart[i + 1];
        while (n < 64) {
            uint32_t start, count, link;

            if (img_pread(&j->img, ebr * 512, b, 512) != 512 || b[510] != 0x55 || b[511] != 0xAA)
                break;
            start = rd32(b + 446 + 8);
            count = rd32(b + 446 + 12);
            if (count) {
                j->pstart[n] = ebr + start;
                j->pcount[n] = count;
                j->phave[n] = 1;
                n++;
            }
            link = rd32(b + 446 + 16 + 8);
            if (!link)
                break;
            ebr = ext + link;
        }
    }
    return 0;
}

/* ---- the image SHA-1: one thread reads, this one hashes ---------------------------------- */

#define HBUF (8u << 20)
#define NHBUF 3

struct hasher {
    void *fh;
    uint64_t size;
    uint8_t *buf[NHBUF];
    size_t len[NHBUF];
    int err, stop;
    CSemaphore filled, empty;
};

static THREAD_FUNC_DECL hash_reader(void *arg)
{
    struct hasher *h = arg;
    uint64_t off = 0;
    int k = 0;

    for (;;) {
        long long got;

        Semaphore_Wait(&h->empty);
        if (h->stop)
            break;
        got = off < h->size ? raw_pread(h->fh, off, h->buf[k], HBUF) : 0;
        if (got < 0) {
            h->err = 1;
            got = 0;
        }
        h->len[k] = (size_t)got;
        off += (uint64_t)got;
        Semaphore_Release1(&h->filled);
        if (!got)
            break;
        k = (k + 1) % NHBUF;
    }
    return THREAD_FUNC_RET_ZERO;
}

int img_hash(struct mkr_job *j, const char *path, char hex[41])
{
    struct hasher h;
    CThread t;
    CSha1 c;
    uint8_t d[20];
    int k = 0, i, rc = 0;

    memset(&h, 0, sizeof(h));
    if (raw_open(path, &h.fh, &h.size))
        return mkr_fail(j, "cannot open %s: %s", path, strerror(errno));
    for (i = 0; i < NHBUF; i++)
        h.buf[i] = malloc(HBUF);
    Semaphore_Construct(&h.filled);
    Semaphore_Construct(&h.empty);
    Semaphore_Create(&h.filled, 0, NHBUF);
    Semaphore_Create(&h.empty, NHBUF, NHBUF);
    Thread_CONSTRUCT(&t)
    Thread_Create(&t, hash_reader, &h);
    mkr_phase(j, "hash", h.size);
    Sha1_Init(&c);
    for (;;) {
        Semaphore_Wait(&h.filled);
        if (!h.len[k])
            break;
        Sha1_Update(&c, h.buf[k], h.len[k]);
        if (mkr_progress(j, h.len[k])) {
            rc = -1;
            h.stop = 1;
            Semaphore_Release1(&h.empty);
            break;
        }
        Semaphore_Release1(&h.empty);
        k = (k + 1) % NHBUF;
    }
    Thread_Wait_Close(&t);
    Semaphore_Close(&h.filled);
    Semaphore_Close(&h.empty);
    for (i = 0; i < NHBUF; i++)
        free(h.buf[i]);
    raw_close(h.fh);
    if (rc)
        return -1;
    if (h.err)
        return mkr_fail(j, "read error in %s", path);
    Sha1_Final(&c, d);
    mkr_sha1_hex(d, hex);
    return 0;
}
