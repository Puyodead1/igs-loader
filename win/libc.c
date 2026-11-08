/*
 * IGS loader, Windows core: glibc stand-in, part 1.
 * Memory, strings, stdio (FILE is an LFILE wrapping the CRT's FILE),
 * formatted I/O (mingw's C99 printf: %lld, %zu, 80-bit %Lf), conversions,
 * the glibc ctype tables and the C locale libstdc++'s "gnu" locale model
 * reads, wide characters (32-bit wchar_t), math and process odds and ends.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lx.h"

/*
 * ---- memory (one allocator for everything ELF code may free) -------------------
 *
 * A heap of its own in which the bytes around a block are always mapped,
 * as in glibc's brk heap: games read past their blocks (Speed Driver 2's
 * replay camera starts at index -1 and reads the entry before its camera
 * array).  The CRT heap does not promise that: its Low Fragmentation Heap
 * puts a no-access guard page after each subsegment, so a block at the start
 * of one has a guard page just below it, and the backend decommits large
 * free runs.  This heap is RtlCreateHeap'd with HEAP_NO_SERIALIZE, which
 * keeps the LFH off (our lock serializes it instead), and with the decommit
 * thresholds out of reach.
 * Blocks are 16-aligned (malloc's on i386 glibc: 8; SSE code wants 16);
 * the raw block and the size sit just below the returned pointer.
 */
typedef struct {
    ULONG Length;
    SIZE_T SegmentReserve, SegmentCommit, DeCommitFreeBlockThreshold, DeCommitTotalFreeThreshold,
        MaximumAllocationSize, VirtualMemoryThreshold, InitialCommit, InitialReserve;
    void *CommitRoutine;
    SIZE_T Reserved[2];
} heap_params;

static HANDLE gheap;
static INIT_ONCE gheap_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK glock = SRWLOCK_INIT;

static BOOL CALLBACK gheap_init(PINIT_ONCE o, void *a, void **c)
{
    void *(NTAPI *create)(ULONG, void *, SIZE_T, SIZE_T, void *, heap_params *);
    heap_params hp;

    (void)o; (void)a; (void)c;
    *(FARPROC *)&create = GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCreateHeap");
    memset(&hp, 0, sizeof(hp));
    hp.Length = sizeof(hp);
    hp.DeCommitFreeBlockThreshold = hp.DeCommitTotalFreeThreshold = 0x7FFFFFFF;
    hp.InitialReserve = 64 << 20;
    hp.SegmentReserve = 64 << 20;
    if (create)
        gheap = create(HEAP_GROWABLE | HEAP_NO_SERIALIZE, NULL, 0, 0, NULL, &hp);
    if (!gheap)
        gheap = GetProcessHeap();
    return TRUE;
}

/* tag: ties the header to its pointer, so a block from another allocator is told apart */
struct blk_hdr { void *raw; size_t n; uintptr_t tag; };
#define BLK_TAG(p, raw) ((uintptr_t)(p) ^ (uintptr_t)(raw) ^ (uintptr_t)0x9E3779B9u)

static struct blk_hdr *blk(void *p)
{
    struct blk_hdr *h = &((struct blk_hdr *)p)[-1];

    if (h->tag == BLK_TAG(p, h->raw) && (char *)h->raw <= (char *)h)
        return h;
    return NULL;
}

/* a block not from this heap: the CRT's if it is one of its (native code's malloc), else kept */
static void foreign_free(void *p, void *caller)
{
    static int said;
    int crt = HeapValidate(GetProcessHeap(), 0, p) != 0;

    if (said++ < 8)
        LOG("free(%p) from %p: not a malloc block%s", p, caller, crt ? " (CRT's, freed there)" : " (left alone)");
    if (crt)
        free(p);
}

static void *heap_get(size_t n, size_t align)
{
    char *raw = NULL, *p;

    if (!gheap)
        InitOnceExecuteOnce(&gheap_once, gheap_init, NULL, NULL);
    if (n <= (size_t)-1 - align - sizeof(struct blk_hdr)) {
        AcquireSRWLockExclusive(&glock);
        raw = HeapAlloc(gheap, 0, n + align + sizeof(struct blk_hdr));
        ReleaseSRWLockExclusive(&glock);
    }
    if (!raw) {
        SET_ERRNO(ENOMEM);
        return NULL;
    }
    p = (char *)(((uintptr_t)raw + sizeof(struct blk_hdr) + align - 1) & ~(uintptr_t)(align - 1));
    ((struct blk_hdr *)p)[-1].raw = raw;
    ((struct blk_hdr *)p)[-1].n = n;
    ((struct blk_hdr *)p)[-1].tag = BLK_TAG(p, raw);
    return p;
}

void *l_malloc(size_t n) { return heap_get(n, 16); }

void l_free(void *p)
{
    struct blk_hdr *h;

    if (!p)
        return;
    if (!(h = blk(p))) {
        foreign_free(p, __builtin_return_address(0));
        return;
    }
    h->tag = 0;
    AcquireSRWLockExclusive(&glock);
    HeapFree(gheap, 0, h->raw);
    ReleaseSRWLockExclusive(&glock);
}

void *l_calloc(size_t n, size_t m)
{
    void *p;

    if (m && n > (size_t)-1 / m) {
        SET_ERRNO(ENOMEM);
        return NULL;
    }
    p = l_malloc(n * m);
    if (p)
        memset(p, 0, n * m);
    return p;
}

void *l_realloc(void *p, size_t n)
{
    if (!p)
        return l_malloc(n);
    if (!n) {
        l_free(p);
        return NULL;
    }
    {
        struct blk_hdr *h = blk(p);
        size_t head;
        void *q = NULL;

        if (!h) {
            /* another allocator's block: its size is unknown, so move it as the CRT's */
            size_t old = HeapValidate(GetProcessHeap(), 0, p) ? _msize(p) : 0;

            LOG("realloc(%p) from %p: not a malloc block", p, __builtin_return_address(0));
            if (!old || !(q = heap_get(n, 16)))
                return NULL;
            memcpy(q, p, old < n ? old : n);
            free(p);
            return q;
        }
        head = (size_t)((char *)p - (char *)h->raw);

        /* in place keeps the alignment (the raw block does not move) */
        if (n <= (size_t)-1 - head) {
            AcquireSRWLockExclusive(&glock);
            q = HeapReAlloc(gheap, HEAP_REALLOC_IN_PLACE_ONLY, h->raw, head + n);
            ReleaseSRWLockExclusive(&glock);
        }
        if (q) {
            h->n = n;
            return p;
        }
        if (!(q = heap_get(n, 16)))
            return NULL;
        memcpy(q, p, h->n < n ? h->n : n);
        l_free(p);
        return q;
    }
}

static void *l_memalign(size_t a, size_t n)
{
    if (a < 16)
        a = 16;
    if (a & (a - 1)) {
        SET_ERRNO(EINVAL);
        return NULL;
    }
    return heap_get(n, a);
}

static int l_posix_memalign(void **out, size_t a, size_t n)
{
    *out = l_memalign(a, n);
    return *out ? 0 : ENOMEM;
}

static void *l_valloc(size_t n) { return l_memalign(4096, n); }

/* the block's size as asked (glibc may report more; the bytes asked are what is safe) */
static size_t l_malloc_usable_size(void *p)
{
    struct blk_hdr *h = p ? blk(p) : NULL;

    return h ? h->n : 0;
}

char *l_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = l_malloc(n);

    if (p)
        memcpy(p, s, n);
    return p;
}

static char *l_strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = l_malloc(len + 1);

    if (p) {
        memcpy(p, s, len);
        p[len] = 0;
    }
    return p;
}

/* ---- errno ------------------------------------------------------------------------- */

static __thread int errno_v;

int *l___errno_location(void)
{
    return &errno_v;
}

int lx_errno_from_crt(int e)
{
    switch (e) {
    case EDEADLK: return L_EDEADLK;
    case ENAMETOOLONG: return L_ENAMETOOLONG;
    case ENOSYS: return L_ENOSYS;
    case ENOTEMPTY: return L_ENOTEMPTY;
    default: return e;
    }
}

static char *l_strerror(int e)
{
    switch (e) {
    case 0: return "Success";
    case L_EDEADLK: return "Resource deadlock avoided";
    case L_ENAMETOOLONG: return "File name too long";
    case L_ENOSYS: return "Function not implemented";
    case L_ENOTEMPTY: return "Directory not empty";
    case L_EOVERFLOW: return "Value too large for defined data type";
    case L_ENOTSUP: return "Operation not supported";
    case L_EAFNOSUPPORT: return "Address family not supported by protocol";
    case L_ETIMEDOUT: return "Connection timed out";
    }
    if (e > 0 && e <= 34)
        return strerror(e);
    return "Unknown error";
}

static int l___xpg_strerror_r(int e, char *buf, size_t n)
{
    snprintf(buf, n, "%s", l_strerror(e));
    return 0;
}

static char *l_strerror_r(int e, char *buf, size_t n)
{
    (void)buf;
    (void)n;
    return l_strerror(e);
}

/* ---- strings --------------------------------------------------------------------------- */

static char *l_strtok_r(char *s, const char *delim, char **save)
{
    char *end;

    if (!s)
        s = *save;
    s += strspn(s, delim);
    if (!*s) {
        *save = s;
        return NULL;
    }
    end = s + strcspn(s, delim);
    if (*end)
        *end++ = 0;
    *save = end;
    return s;
}

static char *l_stpcpy(char *d, const char *s)
{
    size_t n = strlen(s);

    memcpy(d, s, n + 1);
    return d + n;
}

static int l_strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
static int l_strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
static char *l_strcasestr(const char *h, const char *n)
{
    size_t len = strlen(n);

    for (; *h; h++)
        if (!_strnicmp(h, n, len))
            return (char *)h;
    return len ? NULL : (char *)h;
}
static void l_bzero(void *p, size_t n) { memset(p, 0, n); }
static void *l_mempcpy(void *d, const void *s, size_t n) { return (char *)memcpy(d, s, n) + n; }
static void *l_memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    while (n--)
        if (p[n] == (unsigned char)c)
            return (void *)(p + n);
    return NULL;
}

static double l___strtod_internal(const char *s, char **end, int group) { (void)group; return strtod(s, end); }
static float l___strtof_internal(const char *s, char **end, int group) { (void)group; return strtof(s, end); }
static long l___strtol_internal(const char *s, char **end, int base, int group) { (void)group; return strtol(s, end, base); }
static unsigned long l___strtoul_internal(const char *s, char **end, int base, int group) { (void)group; return strtoul(s, end, base); }
static long long l___strtoll_internal(const char *s, char **end, int base, int group) { (void)group; return strtoll(s, end, base); }
static unsigned long long l___strtoull_internal(const char *s, char **end, int base, int group) { (void)group; return strtoull(s, end, base); }
static long double l_strtold(const char *s, char **end) { return strtold(s, end); }

/* ---- stdio ----------------------------------------------------------------------------- */

static LFILE lf_std[3] = {
    { LFILE_MAGIC, NULL, 0 }, { LFILE_MAGIC, NULL, 1 }, { LFILE_MAGIC, NULL, 2 },
};
LFILE *l_stdin_p = &lf_std[0], *l_stdout_p = &lf_std[1], *l_stderr_p = &lf_std[2];

static void std_init(void)
{
    if (!lf_std[0].f) {
        lf_std[0].f = stdin;
        lf_std[1].f = stdout;
        lf_std[2].f = stderr;
    }
}

LFILE *lfile_wrap(FILE *f)
{
    LFILE *lf;

    if (!f)
        return NULL;
    lf = calloc(1, sizeof(*lf));
    lf->magic = LFILE_MAGIC;
    lf->f = f;
    lf->fd = -1;
    return lf;
}

FILE *lfile_crt(LFILE *lf)
{
    std_init();
    if (!lf || lf->magic != LFILE_MAGIC)
        return NULL;
    return lf->f;
}

#define F(lf) lfile_crt(lf)

/* console streams: keep the game's lines in order with the loader's */
static void after_write(LFILE *lf)
{
    if (lf == &lf_std[1] || lf == &lf_std[2])
        fflush(lf->f);
}

/* binary, as on Linux: a "t" (We Dance's "rt") would make the CRT drop the \r of CRLF lines */
static void mode_b(const char *mode, char *out)
{
    int i, n = 0;

    for (i = 0; mode[i] && n < 5; i++)
        if (mode[i] != 't' && mode[i] != 'b')
            out[n++] = mode[i];
    strcpy(out + n, "b");
}

/* a file the machine provides (igs_machine.vfile): a temporary copy, deleted on close */
void *lx_vfile_open(const char *name)
{
    const char *txt = core_vfile(name);
    char dir[MAX_PATH], tmp[MAX_PATH];
    FILE *f;

    if (!txt || !GetTempPathA(sizeof(dir), dir) || !GetTempFileNameA(dir, "igs", 0, tmp))
        return NULL;
    if (!(f = fopen(tmp, "w+bD"))) {
        DeleteFileA(tmp);
        return NULL;
    }
    fputs(txt, f);
    rewind(f);
    if (lx_verbose)
        LOG("%s: provided by the machine", name);
    return lfile_wrap(f);
}

static int l_fclose(LFILE *lf);
int lx_vfile_close(void *lf) { return l_fclose(lf); }

static LFILE *l_fopen(const char *path, const char *mode)
{
    char win[MAX_PATH], m[8];
    FILE *f;

    if (!path) {
        SET_ERRNO(EINVAL);
        return NULL;
    }
    if (!strpbrk(mode, "wa+")) {
        LFILE *vf = lx_vfile_open(path);

        if (vf)
            return vf;
    }
    if (!strcmp(path, "/dev/null")) {
        snprintf(win, sizeof(win), "NUL");
    } else if (lx_path(path, win, sizeof(win))) {
        if (lx_verbose)
            LOG("fopen %s: no such file", path);
        SET_ERRNO(ENOENT);
        return NULL;
    }
    mode_b(mode, m);
    f = fopen(win, m);
    if (!f) {
        SET_ERRNO(lx_errno_from_crt(errno));
        if (lx_verbose)
            LOG("fopen %s (%s) %s: %s", path, win, mode, strerror(errno));
        return NULL;
    }
    core_note_fopen(path);
    return lfile_wrap(f);
}

static LFILE *l_freopen(const char *path, const char *mode, LFILE *lf)
{
    char win[MAX_PATH], m[8];

    if (!F(lf) || !path || lx_path(path, win, sizeof(win)))
        return NULL;
    mode_b(mode, m);
    lf->f = freopen(win, m, lf->f);
    return lf->f ? lf : NULL;
}

static LFILE *l_fdopen(int fd, const char *mode);
static int l_fclose(LFILE *lf)
{
    FILE *f = F(lf);
    int r;

    if (!f)
        return -1;
    r = fclose(f);
    if (lf >= lf_std && lf < lf_std + 3)
        return r;
    lf->magic = 0;
    free(lf);
    return r ? -1 : 0;
}

static size_t l_fread(void *p, size_t s, size_t n, LFILE *lf) { FILE *f = F(lf); return f ? fread(p, s, n, f) : 0; }
static size_t l_fwrite(const void *p, size_t s, size_t n, LFILE *lf)
{
    FILE *f = F(lf);
    size_t r;

    if (!f)
        return 0;
    r = fwrite(p, s, n, f);
    after_write(lf);
    return r;
}
static char *l_fgets(char *s, int n, LFILE *lf) { FILE *f = F(lf); return f ? fgets(s, n, f) : NULL; }
static int l_fputs(const char *s, LFILE *lf)
{
    FILE *f = F(lf);
    int r;

    if (!f)
        return -1;
    r = fputs(s, f);
    after_write(lf);
    return r;
}
static int l_fputc(int c, LFILE *lf)
{
    FILE *f = F(lf);
    int r;

    if (!f)
        return -1;
    r = fputc(c, f);
    if (c == '\n')
        after_write(lf);
    return r;
}
static int l_fgetc(LFILE *lf) { FILE *f = F(lf); return f ? fgetc(f) : -1; }
static int l_ungetc(int c, LFILE *lf) { FILE *f = F(lf); return f ? ungetc(c, f) : -1; }
static int l_feof(LFILE *lf) { FILE *f = F(lf); return f ? feof(f) : 1; }
static int l_ferror(LFILE *lf) { FILE *f = F(lf); return f ? ferror(f) : 1; }
static void l_clearerr(LFILE *lf) { FILE *f = F(lf); if (f) clearerr(f); }
static int l_fflush(LFILE *lf)
{
    if (!lf)
        return fflush(NULL);
    return F(lf) ? fflush(F(lf)) : -1;
}
static int l_fseek(LFILE *lf, long off, int wh) { FILE *f = F(lf); return f ? fseek(f, off, wh) : -1; }
static long l_ftell(LFILE *lf) { FILE *f = F(lf); return f ? ftell(f) : -1; }
static int l_fseeko64(LFILE *lf, long long off, int wh) { FILE *f = F(lf); return f ? _fseeki64(f, off, wh) : -1; }
static long long l_ftello64(LFILE *lf) { FILE *f = F(lf); return f ? _ftelli64(f) : -1; }
static void l_rewind(LFILE *lf) { FILE *f = F(lf); if (f) rewind(f); }
static int l_fgetpos(LFILE *lf, long long *pos) { *pos = l_ftello64(lf); return *pos < 0 ? -1 : 0; }
static int l_fsetpos(LFILE *lf, const long long *pos) { return l_fseeko64(lf, *pos, SEEK_SET); }
static int l_setvbuf(LFILE *lf, char *buf, int mode, size_t n)
{
    FILE *f = F(lf);

    (void)buf;
    if (!f || lf < lf_std || lf >= lf_std + 3)
        return f ? setvbuf(f, NULL, mode == 2 ? _IONBF : _IOFBF, n ? n : 4096) : -1;
    return 0;
}
static void l_setbuf(LFILE *lf, char *buf) { (void)lf; (void)buf; }
static int l_fileno(LFILE *lf)
{
    FILE *f = F(lf);

    if (!f)
        return -1;
    if (lf->fd < 0)
        lf->fd = lx_fd_from_crt(_fileno(f));
    return lf->fd;
}
static int l_putchar(int c) { return l_fputc(c, l_stdout_p); }
static int l_puts(const char *s) { l_fputs(s, l_stdout_p); return l_fputc('\n', l_stdout_p); }
static int l_getchar(void) { return l_fgetc(l_stdin_p); }

static int l_vfprintf(LFILE *lf, const char *fmt, va_list ap)
{
    FILE *f = F(lf);
    int r;

    if (!f)
        return -1;
    r = __mingw_vfprintf(f, fmt, ap);
    after_write(lf);
    return r;
}

static int l_fprintf(LFILE *lf, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = l_vfprintf(lf, fmt, ap);
    va_end(ap);
    return r;
}

static int l_vprintf(const char *fmt, va_list ap) { return l_vfprintf(l_stdout_p, fmt, ap); }

static int l_printf(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = l_vfprintf(l_stdout_p, fmt, ap);
    va_end(ap);
    return r;
}

static int l_vsnprintf(char *s, size_t n, const char *fmt, va_list ap) { return __mingw_vsnprintf(s, n, fmt, ap); }
static int l_vsprintf(char *s, const char *fmt, va_list ap) { return __mingw_vsprintf(s, fmt, ap); }

static int l_snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = __mingw_vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

static int l_sprintf(char *s, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = __mingw_vsprintf(s, fmt, ap);
    va_end(ap);
    return r;
}

static int l_vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;

    va_copy(ap2, ap);
    n = __mingw_vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    *out = l_malloc(n + 1);
    return *out ? __mingw_vsnprintf(*out, n + 1, fmt, ap) : -1;
}

static int l_asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = l_vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}

/* _FORTIFY_SOURCE variants */
static int l___printf_chk(int fl, const char *fmt, ...)
{
    va_list ap;
    int r;

    (void)fl;
    va_start(ap, fmt);
    r = l_vfprintf(l_stdout_p, fmt, ap);
    va_end(ap);
    return r;
}

static int l___fprintf_chk(LFILE *lf, int fl, const char *fmt, ...)
{
    va_list ap;
    int r;

    (void)fl;
    va_start(ap, fmt);
    r = l_vfprintf(lf, fmt, ap);
    va_end(ap);
    return r;
}

static int l___sprintf_chk(char *s, int fl, size_t slen, const char *fmt, ...)
{
    va_list ap;
    int r;

    (void)fl;
    va_start(ap, fmt);
    r = __mingw_vsnprintf(s, slen, fmt, ap);
    va_end(ap);
    return r;
}

static int l___snprintf_chk(char *s, size_t n, int fl, size_t slen, const char *fmt, ...)
{
    va_list ap;
    int r;

    (void)fl;
    (void)slen;
    va_start(ap, fmt);
    r = __mingw_vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

static int l___vsnprintf_chk(char *s, size_t n, int fl, size_t slen, const char *fmt, va_list ap)
{
    (void)fl;
    (void)slen;
    return __mingw_vsnprintf(s, n, fmt, ap);
}

static int l___vsprintf_chk(char *s, int fl, size_t slen, const char *fmt, va_list ap)
{
    (void)fl;
    return __mingw_vsnprintf(s, slen, fmt, ap);
}

static int l___vfprintf_chk(LFILE *lf, int fl, const char *fmt, va_list ap)
{
    (void)fl;
    return l_vfprintf(lf, fmt, ap);
}

static int l_vsscanf(const char *s, const char *fmt, va_list ap) { return __mingw_vsscanf(s, fmt, ap); }

static int l_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = __mingw_vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

static int l_vfscanf(LFILE *lf, const char *fmt, va_list ap)
{
    FILE *f = F(lf);

    return f ? __mingw_vfscanf(f, fmt, ap) : -1;
}

static int l_fscanf(LFILE *lf, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = l_vfscanf(lf, fmt, ap);
    va_end(ap);
    return r;
}

static int l_scanf(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = l_vfscanf(l_stdin_p, fmt, ap);
    va_end(ap);
    return r;
}

static void l_perror(const char *s)
{
    int e = errno_v;

    l_fprintf(l_stderr_p, "%s%s%s\n", s && *s ? s : "", s && *s ? ": " : "", l_strerror(e));
}

static int l_remove(const char *path)
{
    char win[MAX_PATH];

    if (lx_path(path, win, sizeof(win)))
        return -1;
    return remove(win) ? -1 : 0;
}

static int l_rename(const char *a, const char *b)
{
    char wa[MAX_PATH], wb[MAX_PATH];

    if (lx_path(a, wa, sizeof(wa)) || lx_path(b, wb, sizeof(wb)))
        return -1;
    if (MoveFileExA(wa, wb, MOVEFILE_REPLACE_EXISTING))
        return 0;
    /* a file the game still has open cannot move (no FILE_SHARE_DELETE): copy it */
    if (!CopyFileA(wa, wb, FALSE))
        return -1;
    DeleteFileA(wa);
    return 0;
}

static LFILE *l_tmpfile(void) { return lfile_wrap(tmpfile()); }

static LFILE *l_fdopen(int fd, const char *mode);

/* ---- ctype (glibc's tables, C locale) ------------------------------------------- */

/* glibc bit layout, little-endian: _ISbit(b) = b < 8 ? (1 << b) << 8 : (1 << b) >> 8 */
#define C_UPPER  0x0100
#define C_LOWER  0x0200
#define C_ALPHA  0x0400
#define C_DIGIT  0x0800
#define C_XDIGIT 0x1000
#define C_SPACE  0x2000
#define C_PRINT  0x4000
#define C_GRAPH  0x8000
#define C_BLANK  0x0001
#define C_CNTRL  0x0002
#define C_PUNCT  0x0004
#define C_ALNUM  0x0008

static uint16_t ctype_b[384];
static int32_t ctype_lower[384], ctype_upper[384];
static const uint16_t *ctype_b_p = ctype_b + 128;
static const int32_t *ctype_lower_p = ctype_lower + 128, *ctype_upper_p = ctype_upper + 128;

static void ctype_init(void)
{
    int c;

    for (c = -128; c < 256; c++) {
        uint16_t m = 0;
        int u = c & 0xff;

        if (c >= 0 && c < 128) {
            if (u >= 'A' && u <= 'Z') m |= C_UPPER | C_ALPHA | C_ALNUM | C_GRAPH | C_PRINT;
            if (u >= 'a' && u <= 'z') m |= C_LOWER | C_ALPHA | C_ALNUM | C_GRAPH | C_PRINT;
            if (u >= '0' && u <= '9') m |= C_DIGIT | C_XDIGIT | C_ALNUM | C_GRAPH | C_PRINT;
            if ((u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F')) m |= C_XDIGIT;
            if (u == ' ' || (u >= 9 && u <= 13)) m |= C_SPACE;
            if (u == ' ' || u == '\t') m |= C_BLANK;
            if (u < 32 || u == 127) m |= C_CNTRL;
            if (u >= 32 && u < 127) m |= C_PRINT;
            if (u > 32 && u < 127) m |= C_GRAPH;
            if (u > 32 && u < 127 && !((u >= '0' && u <= '9') || ((u | 32) >= 'a' && (u | 32) <= 'z')))
                m |= C_PUNCT;
        }
        ctype_b[c + 128] = m;
        ctype_lower[c + 128] = c >= 'A' && c <= 'Z' ? c + 32 : c;
        ctype_upper[c + 128] = c >= 'a' && c <= 'z' ? c - 32 : c;
    }
}

/* uClibc 0.9.26 (E1000): __ctype_b as glibc's, __ctype_toupper with 16-bit entries */
static int16_t uc_toupper[384];
static const int16_t *uc_toupper_p = uc_toupper + 128;

static void uc_ctype_init(void)
{
    int c;

    for (c = -128; c < 256; c++)
        uc_toupper[c + 128] = (int16_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
}

static const uint16_t **l___ctype_b_loc(void) { return &ctype_b_p; }
static const int32_t **l___ctype_tolower_loc(void) { return &ctype_lower_p; }
static const int32_t **l___ctype_toupper_loc(void) { return &ctype_upper_p; }

static int cls(int c, int m) { return c >= -128 && c < 256 ? ctype_b[c + 128] & m : 0; }
static int l_isalnum(int c) { return cls(c, C_ALNUM); }
static int l_isalpha(int c) { return cls(c, C_ALPHA); }
static int l_isdigit(int c) { return cls(c, C_DIGIT); }
static int l_isxdigit(int c) { return cls(c, C_XDIGIT); }
static int l_isspace(int c) { return cls(c, C_SPACE); }
static int l_isupper(int c) { return cls(c, C_UPPER); }
static int l_islower(int c) { return cls(c, C_LOWER); }
static int l_isprint(int c) { return cls(c, C_PRINT); }
static int l_isgraph(int c) { return cls(c, C_GRAPH); }
static int l_ispunct(int c) { return cls(c, C_PUNCT); }
static int l_iscntrl(int c) { return cls(c, C_CNTRL); }
static int l_isblank(int c) { return cls(c, C_BLANK); }
static int l_tolower(int c) { return c >= -128 && c < 256 ? ctype_lower[c + 128] : c; }
static int l_toupper(int c) { return c >= -128 && c < 256 ? ctype_upper[c + 128] : c; }

/* ---- the C locale, as libstdc++'s gnu model sees it ---------------------------------- */

struct l_locale {
    void *locales[13];
    const uint16_t *ctype_b;
    const int32_t *ctype_tolower, *ctype_toupper;
    const char *names[13];
};
static struct l_locale c_locale;
static __thread void *cur_locale = (void *)-1;

static void *l___newlocale(int mask, const char *name, void *base)
{
    (void)mask;
    (void)base;
    if (name && *name && strcmp(name, "C") && strcmp(name, "POSIX") && lx_verbose)
        LOG("newlocale(%s): using C", name);
    return &c_locale;
}
static void *l___duplocale(void *l) { return l == (void *)-1 ? &c_locale : l; }
static void l___freelocale(void *l) { (void)l; }
static void *l___uselocale(void *l)
{
    void *old = cur_locale;

    if (l)
        cur_locale = l;
    return old;
}
static char *l_setlocale(int cat, const char *name) { (void)cat; (void)name; return "C"; }

static char *l_nl_langinfo(int item)
{
    switch (item) {
    case 14: return "ANSI_X3.4-1968";      /* CODESET */
    case 0x10000: return ".";              /* RADIXCHAR */
    default: return "";
    }
}
static char *l___nl_langinfo_l(int item, void *l) { (void)l; return l_nl_langinfo(item); }
static int l___strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
static size_t l___strxfrm_l(char *d, const char *s, size_t n, void *l)
{
    size_t len = strlen(s);

    (void)l;
    if (n > len)
        memcpy(d, s, len + 1);
    return len;
}

static void tm_to_crt(const struct l_tm *in, struct tm *out)
{
    memset(out, 0, sizeof(*out));
    out->tm_sec = in->tm_sec;
    out->tm_min = in->tm_min;
    out->tm_hour = in->tm_hour;
    out->tm_mday = in->tm_mday;
    out->tm_mon = in->tm_mon;
    out->tm_year = in->tm_year;
    out->tm_wday = in->tm_wday;
    out->tm_yday = in->tm_yday;
    out->tm_isdst = in->tm_isdst;
}

static size_t l_strftime(char *s, size_t n, const char *fmt, const struct l_tm *t)
{
    struct tm c;

    tm_to_crt(t, &c);
    return strftime(s, n, fmt, &c);
}
static size_t l___strftime_l(char *s, size_t n, const char *fmt, const struct l_tm *t, void *l)
{
    (void)l;
    return l_strftime(s, n, fmt, t);
}

/* strptime: the numeric fields, month and weekday names, %%, white space and literals */
static const char *const tm_mon_names[12] = {
    "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec",
};
static const char *const tm_day_names[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

static const char *tm_num(const char *s, int maxdig, int lo, int hi, int32_t *out)
{
    int v = 0, n = 0, neg = 0;

    while (*s == ' ')
        s++;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    while (n < maxdig && *s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        n++;
    }
    if (!n || (neg ? -v : v) < lo || (neg ? -v : v) > hi)
        return NULL;
    *out = neg ? -v : v;
    return s;
}

static const char *tm_name(const char *s, const char *const *names, int count, int32_t *out)
{
    int i;
    size_t k;

    for (i = 0; i < count; i++) {
        if (_strnicmp(s, names[i], 3))
            continue;
        *out = i;
        for (k = 3; isalpha((unsigned char)s[k]); k++)
            ;
        return s + k;             /* the abbreviation or the full name */
    }
    return NULL;
}

static char *l_strptime(const char *s, const char *fmt, struct l_tm *t)
{
    int32_t v;

    for (; s && *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s))
                s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s++ != *fmt)
                return NULL;
            continue;
        }
        switch (*++fmt) {
        case 'Y': if ((s = tm_num(s, 4, 0, 9999, &v))) t->tm_year = v - 1900; break;
        case 'y': if ((s = tm_num(s, 2, 0, 99, &v))) t->tm_year = v < 69 ? v + 100 : v; break;
        case 'm': if ((s = tm_num(s, 2, 1, 12, &v))) t->tm_mon = v - 1; break;
        case 'd': case 'e': s = tm_num(s, 2, 1, 31, &t->tm_mday); break;
        case 'H': s = tm_num(s, 2, 0, 23, &t->tm_hour); break;
        case 'M': s = tm_num(s, 2, 0, 59, &t->tm_min); break;
        case 'S': s = tm_num(s, 2, 0, 61, &t->tm_sec); break;
        case 'j': if ((s = tm_num(s, 3, 1, 366, &v))) t->tm_yday = v - 1; break;
        case 'b': case 'B': case 'h': s = tm_name(s, tm_mon_names, 12, &t->tm_mon); break;
        case 'a': case 'A': s = tm_name(s, tm_day_names, 7, &t->tm_wday); break;
        case 'n': case 't': while (isspace((unsigned char)*s)) s++; break;
        case 'D': s = l_strptime(s, "%m/%d/%y", t); break;
        case 'T': s = l_strptime(s, "%H:%M:%S", t); break;
        case 'R': s = l_strptime(s, "%H:%M", t); break;
        case '%': if (*s++ != '%') return NULL; break;
        default: return NULL;
        }
    }
    return (char *)s;
}
static double l___strtod_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }
static float l___strtof_l(const char *s, char **e, void *l) { (void)l; return strtof(s, e); }
static long double l___strtold_l(const char *s, char **e, void *l) { (void)l; return strtold(s, e); }
static long l___strtol_l(const char *s, char **e, int b, void *l) { (void)l; return strtol(s, e, b); }
static unsigned long l___strtoul_l(const char *s, char **e, int b, void *l) { (void)l; return strtoul(s, e, b); }
static long long l___strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
static unsigned long long l___strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
static void *l___rawmemchr(const void *s, int c)
{
    const unsigned char *p = s;

    while (*p != (unsigned char)c)
        p++;
    return (void *)p;
}
static size_t l___ctype_get_mb_cur_max(void) { return 1; }

/* ---- wide characters: glibc's wchar_t is 32-bit -------------------------------------- */

typedef uint32_t lwchar;
#define L_WEOF 0xffffffffu

static uint32_t l___towlower_l(uint32_t c, void *l) { (void)l; return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static uint32_t l___towupper_l(uint32_t c, void *l) { (void)l; return c >= 'a' && c <= 'z' ? c - 32 : c; }
static uint32_t l_towlower(uint32_t c) { return l___towlower_l(c, NULL); }
static uint32_t l_towupper(uint32_t c) { return l___towupper_l(c, NULL); }

static uint32_t l___wctype_l(const char *name, void *l)
{
    static const struct { const char *n; uint16_t m; } t[] = {
        { "alnum", C_ALNUM }, { "alpha", C_ALPHA }, { "blank", C_BLANK }, { "cntrl", C_CNTRL },
        { "digit", C_DIGIT }, { "graph", C_GRAPH }, { "lower", C_LOWER }, { "print", C_PRINT },
        { "punct", C_PUNCT }, { "space", C_SPACE }, { "upper", C_UPPER }, { "xdigit", C_XDIGIT },
    };
    unsigned i;

    (void)l;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(t[i].n, name))
            return t[i].m;
    return 0;
}
static uint32_t l_wctype(const char *name) { return l___wctype_l(name, NULL); }
static int l___iswctype_l(uint32_t c, uint32_t desc, void *l) { (void)l; return c < 256 ? (ctype_b[c + 128] & desc) != 0 : 0; }
static int l_iswctype(uint32_t c, uint32_t desc) { return l___iswctype_l(c, desc, NULL); }

static int l_wctob(uint32_t c) { return c < 128 ? (int)c : -1; }
static uint32_t l_btowc(int c) { return c >= 0 && c < 128 ? (uint32_t)c : L_WEOF; }

static size_t l_mbrtowc(lwchar *pwc, const char *s, size_t n, void *ps)
{
    unsigned char c;

    (void)ps;
    if (!s)
        return 0;
    if (!n)
        return (size_t)-2;
    c = *s;
    if (c >= 0x80) {
        SET_ERRNO(EILSEQ);
        return (size_t)-1;
    }
    if (pwc)
        *pwc = c;
    return c ? 1 : 0;
}

static size_t l_wcrtomb(char *s, lwchar wc, void *ps)
{
    (void)ps;
    if (!s)
        return 1;
    if (wc >= 0x80) {
        SET_ERRNO(EILSEQ);
        return (size_t)-1;
    }
    *s = (char)wc;
    return 1;
}

static size_t l_mbsnrtowcs(lwchar *d, const char **src, size_t nms, size_t len, void *ps)
{
    const char *s = *src;
    size_t i = 0;

    (void)ps;
    while (nms-- && (!d || i < len)) {
        unsigned char c = *s;

        if (c >= 0x80) {
            SET_ERRNO(EILSEQ);
            return (size_t)-1;
        }
        if (d)
            d[i] = c;
        if (!c) {
            if (d)
                *src = NULL;
            return i;
        }
        s++;
        i++;
    }
    if (d)
        *src = s;
    return i;
}

static size_t l_mbsrtowcs(lwchar *d, const char **src, size_t len, void *ps)
{
    return l_mbsnrtowcs(d, src, (size_t)-1, len, ps);
}

static size_t l_wcsnrtombs(char *d, const lwchar **src, size_t nwc, size_t len, void *ps)
{
    const lwchar *s = *src;
    size_t i = 0;

    (void)ps;
    while (nwc-- && (!d || i < len)) {
        lwchar c = *s;

        if (c >= 0x80) {
            SET_ERRNO(EILSEQ);
            return (size_t)-1;
        }
        if (d)
            d[i] = (char)c;
        if (!c) {
            if (d)
                *src = NULL;
            return i;
        }
        s++;
        i++;
    }
    if (d)
        *src = s;
    return i;
}

static size_t l_wcsrtombs(char *d, const lwchar **src, size_t len, void *ps)
{
    return l_wcsnrtombs(d, src, (size_t)-1, len, ps);
}

static size_t l_wcslen(const lwchar *s) { size_t n = 0; while (s[n]) n++; return n; }
static int l_wcscmp(const lwchar *a, const lwchar *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a < *b ? -1 : *a > *b;
}
static lwchar *l_wmemchr(const lwchar *s, lwchar c, size_t n)
{
    for (; n--; s++)
        if (*s == c)
            return (lwchar *)s;
    return NULL;
}
static int l_wmemcmp(const lwchar *a, const lwchar *b, size_t n)
{
    for (; n--; a++, b++)
        if (*a != *b)
            return *a < *b ? -1 : 1;
    return 0;
}
static lwchar *l_wmemcpy(lwchar *d, const lwchar *s, size_t n) { return memcpy(d, s, n * 4); }
static lwchar *l_wmemmove(lwchar *d, const lwchar *s, size_t n) { return memmove(d, s, n * 4); }
static lwchar *l_wmemset(lwchar *d, lwchar c, size_t n) { size_t i; for (i = 0; i < n; i++) d[i] = c; return d; }
static int l___wcscoll_l(const lwchar *a, const lwchar *b, void *l) { (void)l; return l_wcscmp(a, b); }
static int l_wcscoll(const lwchar *a, const lwchar *b) { return l_wcscmp(a, b); }   /* the C locale */
static size_t l___wcsxfrm_l(lwchar *d, const lwchar *s, size_t n, void *l)
{
    size_t len = l_wcslen(s);

    (void)l;
    if (n > len)
        memcpy(d, s, (len + 1) * 4);
    return len;
}
static size_t l___wcsftime_l(lwchar *s, size_t n, const lwchar *fmt, const struct l_tm *t, void *l)
{
    char nf[256], out[512];
    size_t i, len;

    (void)l;
    for (i = 0; fmt[i] && i < sizeof(nf) - 1; i++)
        nf[i] = (char)fmt[i];
    nf[i] = 0;
    len = l_strftime(out, sizeof(out), nf, t);
    if (len >= n)
        return 0;
    for (i = 0; i <= len; i++)
        s[i] = (unsigned char)out[i];
    return len;
}
static uint32_t l_getwc(LFILE *lf) { int c = l_fgetc(lf); return c < 0 ? L_WEOF : (uint32_t)c; }
static uint32_t l_putwc(uint32_t c, LFILE *lf) { return l_fputc((int)c, lf) < 0 ? L_WEOF : c; }
static uint32_t l_ungetwc(uint32_t c, LFILE *lf) { return l_ungetc((int)c, lf) < 0 ? L_WEOF : c; }

static char *l_gettext(const char *m) { return (char *)m; }
static char *l_dgettext(const char *d, const char *m) { (void)d; return (char *)m; }
static char *l_dcgettext(const char *d, const char *m, int c) { (void)d; (void)c; return (char *)m; }
static char *l_textdomain(const char *d) { return (char *)(d ? d : "messages"); }
static char *l_bindtextdomain(const char *d, const char *dir) { (void)d; return (char *)dir; }
static char *l_bind_textdomain_codeset(const char *d, const char *c) { (void)d; return (char *)c; }

/* ---- math ------------------------------------------------------------------------------ */

static int l___isnan(double x) { return isnan(x); }
static int l___isnanf(float x) { return isnan(x); }
static int l___isinf(double x) { return isinf(x) ? (x > 0 ? 1 : -1) : 0; }
static int l___finite(double x) { return isfinite(x); }
static void l_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
static void l_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
static long double l_frexpl(long double x, int *e) { return frexpl(x, e); }
static long double l_powl(long double x, long double y) { return powl(x, y); }
static long double l_ldexpl(long double x, int e) { return ldexpl(x, e); }
static long double l_modfl(long double x, long double *i) { return modfl(x, i); }
static long l_lrintf(float x) { return lrintf(x); }
static long l_lrint(double x) { return lrint(x); }
static double l_round(double x) { return round(x); }
static float l_roundf(float x) { return roundf(x); }
static double l_trunc(double x) { return trunc(x); }
static float l_truncf(float x) { return truncf(x); }
static double l_rint(double x) { return rint(x); }
static float l_rintf(float x) { return rintf(x); }
static long long l_llrint(double x) { return llrint(x); }
static float l_ldexpf(float x, int e) { return ldexpf(x, e); }
static float l_frexpf(float x, int *e) { return frexpf(x, e); }
static float l_cbrtf(float x) { return cbrtf(x); }
static float l_fabsf(float x) { return fabsf(x); }
static float l_sqrtf(float x) { return sqrtf(x); }
static float l_sinf(float x) { return sinf(x); }
static float l_cosf(float x) { return cosf(x); }
static float l_tanf(float x) { return tanf(x); }
static float l_atan2f(float y, float x) { return atan2f(y, x); }
static float l_atanf(float x) { return atanf(x); }
static float l_acosf(float x) { return acosf(x); }
static float l_asinf(float x) { return asinf(x); }
static float l_ceilf(float x) { return ceilf(x); }
static float l_floorf(float x) { return floorf(x); }
static float l_fmodf(float x, float y) { return fmodf(x, y); }
static float l_powf(float x, float y) { return powf(x, y); }
static float l_expf(float x) { return expf(x); }
static float l_logf(float x) { return logf(x); }
static float l_log10f(float x) { return log10f(x); }
static float l_log2f(float x) { return log2f(x); }
static double l_log2(double x) { return log2(x); }
static float l_modff(float x, float *i) { return modff(x, i); }
static float l_exp2f(float x) { return exp2f(x); }

/* ---- stdlib ----------------------------------------------------------------------------- */

/* glibc's random(): the TYPE_3 additive feedback generator, RAND_MAX 2^31-1 */
static int32_t rnd_tbl[34];
static int rnd_f, rnd_r;
static int rnd_seeded;
static CRITICAL_SECTION rnd_lock;

static void l_srandom(unsigned seed)
{
    int32_t word;
    int i;

    if (seed == 0)
        seed = 1;
    rnd_tbl[0] = seed;
    word = seed;
    for (i = 1; i < 31; i++) {
        int32_t hi = word / 127773, lo = word % 127773;

        word = 16807 * lo - 2836 * hi;
        if (word < 0)
            word += 2147483647;
        rnd_tbl[i] = word;
    }
    rnd_f = 3;
    rnd_r = 0;
    rnd_seeded = 1;
    for (i = 0; i < 310; i++) {
        uint32_t v = (uint32_t)rnd_tbl[rnd_f] + (uint32_t)rnd_tbl[rnd_r];

        rnd_tbl[rnd_f] = v;
        rnd_f = (rnd_f + 1) % 31;
        rnd_r = (rnd_r + 1) % 31;
    }
}

static long l_random(void)
{
    uint32_t v;

    EnterCriticalSection(&rnd_lock);
    if (!rnd_seeded)
        l_srandom(1);
    v = (uint32_t)rnd_tbl[rnd_f] + (uint32_t)rnd_tbl[rnd_r];
    rnd_tbl[rnd_f] = v;
    rnd_f = (rnd_f + 1) % 31;
    rnd_r = (rnd_r + 1) % 31;
    LeaveCriticalSection(&rnd_lock);
    return v >> 1;
}

static int l_rand(void) { return (int)l_random(); }
static void l_srand(unsigned s)
{
    EnterCriticalSection(&rnd_lock);
    l_srandom(s);
    LeaveCriticalSection(&rnd_lock);
}
static int l_rand_r(unsigned *seed)
{
    /* glibc's rand_r */
    unsigned next = *seed;
    int result;

    next *= 1103515245; next += 12345; result = (unsigned)(next / 65536) % 2048;
    next *= 1103515245; next += 12345; result <<= 10; result ^= (unsigned)(next / 65536) % 1024;
    next *= 1103515245; next += 12345; result <<= 10; result ^= (unsigned)(next / 65536) % 1024;
    *seed = next;
    return result;
}

static __thread void *qsort_cmp;
static int qsort_tramp(const void *a, const void *b) { return (int)elf_call2(qsort_cmp, U32(a), U32(b)); }

static void l_qsort(void *base, size_t n, size_t sz, void *cmp)
{
    void *saved = qsort_cmp;

    qsort_cmp = cmp;
    qsort(base, n, sz, qsort_tramp);
    qsort_cmp = saved;
}

static void *l_bsearch(const void *key, const void *base, size_t n, size_t sz, void *cmp)
{
    void *saved = qsort_cmp, *r;

    qsort_cmp = cmp;
    r = bsearch(key, base, n, sz, qsort_tramp);
    qsort_cmp = saved;
    return r;
}

static char *l_getenv(const char *n) { return getenv(n); }
static int l_setenv(const char *n, const char *v, int over)
{
    if (!over && getenv(n))
        return 0;
    return _putenv_s(n, v) ? -1 : 0;
}
static int l_unsetenv(const char *n) { return _putenv_s(n, "") ? -1 : 0; }
static int l_putenv(char *s) { return _putenv(s) ? -1 : 0; }

static int l_atexit(void *fn) { (void)fn; return 0; }
static int l___cxa_atexit(void *fn, void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
static void l___cxa_finalize(void *dso) { (void)dso; }

void __attribute__((noreturn)) l_exit(int code)
{
    LOG("exit(%d)", code);
    fflush(NULL);
    ExitProcess(code);
}

static void __attribute__((noreturn)) l__exit(int code)
{
    LOG("_exit(%d)", code);
    fflush(NULL);
    ExitProcess(code);
}

static void __attribute__((noreturn)) l_abort(void)
{
    char where[128];

    elf_describe(U32(__builtin_return_address(0)), where, sizeof(where));
    LOG("abort() called from %s", where);
    fflush(NULL);
    ExitProcess(134);
}

static void __attribute__((noreturn)) l___assert_fail(const char *a, const char *file, unsigned line, const char *fn)
{
    LOG("assertion failed: %s (%s:%u, %s)", a, file, line, fn ? fn : "?");
    l_abort();
}

static void l___stack_chk_fail(void)
{
    static int once;
    char where[128];

    if (once++)
        return;
    elf_describe(U32(__builtin_return_address(0)), where, sizeof(where));
    LOG("stack protector check failed in %s (continuing; logged once)", where);
}

static int l_abs(int x) { return x < 0 ? -x : x; }
static long l_labs(long x) { return x < 0 ? -x : x; }
static long long l_llabs(long long x) { return x < 0 ? -x : x; }
static int l_atoi(const char *s) { return atoi(s); }
static long l_atol(const char *s) { return atol(s); }
static double l_atof(const char *s) { return strtod(s, NULL); }

/* div / ldiv: i386 SysV returns the struct through a hidden pointer it pops */
__asm__(
    ".globl _l_div\n"
    "_l_div:\n"
    "    movl 4(%esp), %ecx\n"
    "    movl 8(%esp), %eax\n"
    "    cltd\n"
    "    idivl 12(%esp)\n"
    "    movl %eax, (%ecx)\n"
    "    movl %edx, 4(%ecx)\n"
    "    movl %ecx, %eax\n"
    "    ret $4\n");
void l_div(void);

/* compiler runtime the executables import from libc (glibc exports them) */
extern long long __divdi3(long long, long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);

/* called by the executable's _start: run its initialisers, then main */
static void __attribute__((noreturn)) l___libc_start_main(void *main_fn, int argc, char **argv, void *init,
                                                          void *fini, void *rtld_fini, void *stack_end)
{
    char **envp = argv + argc + 1;

    (void)fini;
    (void)rtld_fini;
    (void)stack_end;
    if (init)
        elf_call3(init, argc, U32(argv), U32(envp));
    if (lx_lddebug)
        LOG("main(%d, %s)", argc, argv[0]);
    l_exit((int)elf_call3(main_fn, argc, U32(argv), U32(envp)));
}

/* uClibc's _start (E1000): no main argument, the library finds the executable's */
static void __attribute__((noreturn)) l___uClibc_start_main(int argc, char **argv, char **envp,
                                                            void *init, void *fini)
{
    void *main_fn = elf_dlsym(NULL, "main");

    (void)fini;
    if (!main_fn) {
        LOG("the executable exports no main");
        l_exit(127);
    }
    if (init)
        elf_call3(init, argc, U32(argv), U32(envp));
    l_exit((int)elf_call3(main_fn, argc, U32(argv), U32(envp)));
}

void libc_init(void)
{
    InitializeCriticalSection(&rnd_lock);
    ctype_init();
    uc_ctype_init();
    c_locale.ctype_b = ctype_b_p;
    c_locale.ctype_tolower = ctype_lower_p;
    c_locale.ctype_toupper = ctype_upper_p;
    {
        int i;

        for (i = 0; i < 13; i++)
            c_locale.names[i] = "C";
    }
    std_init();
}

/* ---- odds and ends (Hero of Robots' Ogre and Cg) ----------------------------------------------------------- */

/* _FORTIFY_SOURCE entry points: the checks are dropped */
static void *l___memcpy_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memcpy(d, s, n); }
static void *l___memmove_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memmove(d, s, n); }
static void *l___memset_chk(void *d, int c, size_t n, size_t dn) { (void)dn; return memset(d, c, n); }
static char *l___strcpy_chk(char *d, const char *s, size_t dn) { (void)dn; return strcpy(d, s); }
static char *l___strcat_chk(char *d, const char *s, size_t dn) { (void)dn; return strcat(d, s); }
static char *l___strncpy_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strncpy(d, s, n); }
static char *l___strncat_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strncat(d, s, n); }
static size_t l___fread_chk(void *p, size_t dn, size_t s, size_t n, LFILE *lf) { (void)dn; return l_fread(p, s, n, lf); }

static int l___isinff(float f) { return isinf(f) ? (f > 0 ? 1 : -1) : 0; }
static int l_finite(double d) { return isfinite(d); }
static int l_finitef(float f) { return isfinite(f); }

static void *l_memmem(const void *h, size_t hn, const void *n, size_t nn)
{
    const uint8_t *p = h;
    size_t i;

    if (!nn)
        return (void *)h;
    for (i = 0; i + nn <= hn; i++)
        if (p[i] == *(const uint8_t *)n && !memcmp(p + i, n, nn))
            return (void *)(p + i);
    return NULL;
}

static void l_swab(const void *from, void *to, ssize_t n)
{
    const uint8_t *s = from;
    uint8_t *d = to;
    ssize_t i;

    for (i = 0; i + 1 < n; i += 2) {
        uint8_t t = s[i];

        d[i] = s[i + 1];
        d[i + 1] = t;
    }
}

static void *l_lfind(const void *key, const void *base, size_t *n, size_t size, void *cmp)
{
    size_t i;

    for (i = 0; i < *n; i++)
        if (!elf_call2(cmp, U32(key), U32((const uint8_t *)base + i * size)))
            return (void *)((const uint8_t *)base + i * size);
    return NULL;
}

/* fnmatch: *, ?, [...] (FNM_PATHNAME 1, FNM_NOESCAPE 2, FNM_PERIOD 4) */
static int l_fnmatch(const char *pat, const char *s, int flags)
{
    for (; *pat; pat++, s++) {
        switch (*pat) {
        case '?':
            if (!*s || (*s == '/' && (flags & 1)))
                return 1;
            break;
        case '*':
            while (pat[1] == '*')
                pat++;
            if (!pat[1])
                return (flags & 1) && strchr(s, '/') ? 1 : 0;
            for (; *s; s++) {
                if (!l_fnmatch(pat + 1, s, flags))
                    return 0;
                if (*s == '/' && (flags & 1))
                    return 1;
            }
            return 1;
        case '[': {
            const char *p = pat + 1;
            int neg = *p == '!' || *p == '^', ok = 0;

            if (neg)
                p++;
            for (; *p && (*p != ']' || p == pat + 1 + neg); p++) {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    ok |= *s >= p[0] && *s <= p[2];
                    p += 2;
                } else {
                    ok |= *s == *p;
                }
            }
            if (!*p || !*s || ok == neg)
                return 1;
            pat = p;
            break;
        }
        case '\\':
            if (!(flags & 2) && pat[1])
                pat++;
            /* fall through */
        default:
            if (*s != *pat)
                return 1;
        }
    }
    return *s ? 1 : 0;
}

/* glibc's struct lconv, C locale */
static void *l_localeconv(void)
{
    static char dot[] = ".", empty[] = "";
    static struct { char *s[10]; char c[14]; } lc = {
        { dot, empty, empty, empty, empty, empty, empty, empty, empty, empty },
        { 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127 },
    };

    return &lc;
}

/* glibc's struct mallinfo (10 ints), returned through the hidden pointer the callee pops */
struct l_mallinfo { int v[10]; };
static struct l_mallinfo __attribute__((callee_pop_aggregate_return(1))) l_mallinfo(void)
{
    struct l_mallinfo m;

    memset(&m, 0, sizeof(m));
    return m;
}

static size_t l_mbstowcs(lwchar *d, const char *s, size_t n)
{
    size_t i = 0, len = strlen(s);

    if (!d)
        return len;
    for (; i < n && s[i]; i++)
        d[i] = (uint8_t)s[i];
    if (i < n)
        d[i] = 0;
    return i;
}

static int l_wcscasecmp(const lwchar *a, const lwchar *b)
{
    for (;; a++, b++) {
        lwchar x = *a < 128 ? (lwchar)tolower((int)*a) : *a, y = *b < 128 ? (lwchar)tolower((int)*b) : *b;

        if (x != y || !x)
            return (int)x - (int)y;
    }
}

/* vswprintf on 32-bit wchar_t: the format narrowed, the result widened (no %ls) */
static int l_vswprintf(lwchar *d, size_t n, const lwchar *fmt, va_list ap)
{
    char f[512], out[2048];
    size_t i;
    int r;

    for (i = 0; fmt[i] && i < sizeof(f) - 1; i++)
        f[i] = fmt[i] < 128 ? (char)fmt[i] : '?';
    f[i] = 0;
    if (strstr(f, "%ls") || strstr(f, "%S"))
        LOG("vswprintf(\"%s\"): wide string arguments are not converted", f);
    r = __mingw_vsnprintf(out, sizeof(out), f, ap);
    if (r < 0 || (size_t)r >= n) {
        SET_ERRNO(E2BIG);
        return -1;
    }
    for (i = 0; i <= (size_t)r; i++)
        d[i] = (uint8_t)out[i];
    return r;
}

/*
 * iconv: UTF-8, WCHAR_T / UCS-4LE / UTF-32LE, UTF-16LE / UCS-2LE, Latin-1,
 * and Windows code pages for GBK / GB2312 / Big5 / Shift_JIS / EUC-KR.
 */
enum { IC_UTF8 = 1, IC_UTF32, IC_UTF16, IC_LATIN1 };
struct l_iconv { int from, to; };

static int ic_code(const char *name)
{
    static const struct { const char *n; int c; } codes[] = {
        { "UTF-8", IC_UTF8 }, { "UTF8", IC_UTF8 }, { "WCHAR_T", IC_UTF32 }, { "UCS-4LE", IC_UTF32 },
        { "UTF-32LE", IC_UTF32 }, { "UCS4LE", IC_UTF32 }, { "UTF-16LE", IC_UTF16 },
        { "UCS-2LE", IC_UTF16 }, { "UNICODELITTLE", IC_UTF16 }, { "ISO-8859-1", IC_LATIN1 },
        { "ISO8859-1", IC_LATIN1 }, { "LATIN1", IC_LATIN1 }, { "ASCII", IC_LATIN1 },
        { "US-ASCII", IC_LATIN1 }, { "ANSI_X3.4-1968", IC_LATIN1 },
        { "GBK", 936 }, { "GB2312", 936 }, { "CP936", 936 }, { "EUC-CN", 936 }, { "GB18030", 54936 },
        { "BIG5", 950 }, { "BIG-5", 950 }, { "CP950", 950 }, { "SHIFT_JIS", 932 }, { "SJIS", 932 },
        { "CP932", 932 }, { "EUC-KR", 949 }, { "CP949", 949 },
    };
    char n[32];
    size_t i;

    snprintf(n, sizeof(n), "%s", name);
    if (strstr(n, "//"))
        *strstr(n, "//") = 0;
    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
        if (!_stricmp(codes[i].n, n))
            return codes[i].c;
    return 0;
}

static void *l_iconv_open(const char *to, const char *from)
{
    struct l_iconv *cd;
    int t = ic_code(to), f = ic_code(from);

    if (!t || !f) {
        LOG("iconv_open(\"%s\", \"%s\"): not supported", to, from);
        SET_ERRNO(EINVAL);
        return (void *)-1;
    }
    cd = l_malloc(sizeof(*cd));
    cd->from = f;
    cd->to = t;
    return cd;
}

static int l_iconv_close(void *cd) { l_free(cd); return 0; }

/* one character from s (n bytes): its code point and length; 0 incomplete, -1 invalid */
static int ic_decode(int code, const uint8_t *s, size_t n, uint32_t *cp)
{
    int len, i;

    switch (code) {
    case IC_LATIN1:
        *cp = s[0];
        return 1;
    case IC_UTF32:
        if (n < 4)
            return 0;
        memcpy(cp, s, 4);
        return 4;
    case IC_UTF16:
        if (n < 2)
            return 0;
        *cp = s[0] | s[1] << 8;
        if (*cp >= 0xd800 && *cp < 0xdc00) {
            if (n < 4)
                return 0;
            *cp = 0x10000 + ((*cp - 0xd800) << 10) + ((s[2] | s[3] << 8) - 0xdc00);
            return 4;
        }
        return 2;
    case IC_UTF8:
        if (s[0] < 0x80) {
            *cp = s[0];
            return 1;
        }
        len = s[0] >= 0xf0 ? 4 : s[0] >= 0xe0 ? 3 : s[0] >= 0xc0 ? 2 : -1;
        if (len < 0)
            return -1;
        if (n < (size_t)len)
            return 0;
        *cp = s[0] & (0x7f >> len);
        for (i = 1; i < len; i++) {
            if ((s[i] & 0xc0) != 0x80)
                return -1;
            *cp = *cp << 6 | (s[i] & 0x3f);
        }
        return len;
    default: {                               /* a Windows code page */
        WCHAR w[2];

        if (s[0] < 0x80) {
            *cp = s[0];
            return 1;
        }
        len = IsDBCSLeadByteEx(code, s[0]) ? 2 : 1;
        if (n < (size_t)len)
            return 0;
        if (MultiByteToWideChar(code, MB_ERR_INVALID_CHARS, (const char *)s, len, w, 2) != 1)
            return -1;
        *cp = w[0];
        return len;
    }
    }
}

/* cp into out (n bytes): length written, 0 if it does not fit */
static int ic_encode(int code, uint32_t cp, uint8_t *out, size_t n)
{
    uint8_t b[8];
    int len;

    switch (code) {
    case IC_LATIN1:
        b[0] = cp < 256 ? (uint8_t)cp : '?';
        len = 1;
        break;
    case IC_UTF32:
        memcpy(b, &cp, 4);
        len = 4;
        break;
    case IC_UTF16:
        if (cp >= 0x10000) {
            uint32_t v = cp - 0x10000, hi = 0xd800 + (v >> 10), lo = 0xdc00 + (v & 0x3ff);

            b[0] = (uint8_t)hi; b[1] = (uint8_t)(hi >> 8); b[2] = (uint8_t)lo; b[3] = (uint8_t)(lo >> 8);
            len = 4;
        } else {
            b[0] = (uint8_t)cp;
            b[1] = (uint8_t)(cp >> 8);
            len = 2;
        }
        break;
    case IC_UTF8:
        if (cp < 0x80) {
            b[0] = (uint8_t)cp;
            len = 1;
        } else if (cp < 0x800) {
            b[0] = 0xc0 | cp >> 6; b[1] = 0x80 | (cp & 0x3f);
            len = 2;
        } else if (cp < 0x10000) {
            b[0] = 0xe0 | cp >> 12; b[1] = 0x80 | (cp >> 6 & 0x3f); b[2] = 0x80 | (cp & 0x3f);
            len = 3;
        } else {
            b[0] = 0xf0 | cp >> 18; b[1] = 0x80 | (cp >> 12 & 0x3f); b[2] = 0x80 | (cp >> 6 & 0x3f);
            b[3] = 0x80 | (cp & 0x3f);
            len = 4;
        }
        break;
    default: {
        WCHAR w = (WCHAR)cp;

        if (cp < 0x80) {
            b[0] = (uint8_t)cp;
            len = 1;
        } else {
            len = WideCharToMultiByte(code, 0, &w, 1, (char *)b, sizeof(b), NULL, NULL);
            if (len <= 0) {
                b[0] = '?';
                len = 1;
            }
        }
    }
    }
    if ((size_t)len > n)
        return 0;
    memcpy(out, b, len);
    return len;
}

static size_t l_iconv(struct l_iconv *cd, char **in, size_t *inleft, char **out, size_t *outleft)
{
    if (!in || !*in)
        return 0;
    while (*inleft) {
        uint32_t cp;
        int r = ic_decode(cd->from, (const uint8_t *)*in, *inleft, &cp), w;

        if (r <= 0) {
            SET_ERRNO(r ? 84 : 22);                        /* EILSEQ / EINVAL */
            return (size_t)-1;
        }
        w = ic_encode(cd->to, cp, (uint8_t *)*out, *outleft);
        if (!w) {
            SET_ERRNO(7);                                  /* E2BIG */
            return (size_t)-1;
        }
        *in += r;
        *inleft -= r;
        *out += w;
        *outleft -= w;
    }
    return 0;
}

static LFILE *l_fdopen(int fd, const char *mode);
extern int lx_crt_fd(int fd);

static LFILE *l_fdopen(int fd, const char *mode)
{
    char m[8];
    int c = lx_crt_fd(fd);
    LFILE *lf;

    if (c < 0) {
        SET_ERRNO(EBADF);
        return NULL;
    }
    mode_b(mode, m);
    lf = lfile_wrap(_fdopen(c, m));
    if (lf)
        lf->fd = fd;
    return lf;
}

const struct nsym libc_syms[] = {
    /* memory */
    N(malloc), N(free), N(calloc), N(realloc), N(memalign), N(posix_memalign), N(valloc),
    NA(__libc_memalign, l_memalign), N(malloc_usable_size),
    /* strings */
    NF(memcpy), NF(memmove), NF(memset), NF(memcmp), NF(memchr), N(memrchr), N(mempcpy),
    NF(strcpy), NF(strncpy), NF(strcat), NF(strncat), NF(strcmp), NF(strncmp), NF(strchr),
    NF(strrchr), NF(strstr), NF(strlen), NF(strnlen), NF(strspn), NF(strcspn), NF(strpbrk),
    NF(strtok), N(strtok_r), NA(__strtok_r, l_strtok_r), N(strdup), NA(__strdup, l_strdup),
    N(strndup), N(stpcpy), N(strcasecmp), N(strncasecmp), N(strcasestr), N(bzero),
    NA(index, strchr), NA(rindex, strrchr), NF(strcoll), NF(strxfrm),
    N(strerror), N(strerror_r), N(__xpg_strerror_r),
    NF(strtol), NF(strtoul), NF(strtoll), NF(strtoull), NA(strtoq, strtoll), NA(strtouq, strtoull),
    NF(strtod), NF(strtof), N(strtold), N(__strtod_internal), N(__strtof_internal),
    N(__strtol_internal), N(__strtoul_internal), N(__strtoll_internal), N(__strtoull_internal),
    N(atoi), N(atol), N(atof), N(abs), N(labs), N(llabs), N(div), NA(ldiv, l_div),
    /* stdio */
    ND(stdin, &l_stdin_p), ND(stdout, &l_stdout_p), ND(stderr, &l_stderr_p),
    N(fopen), NA(fopen64, l_fopen), N(freopen), NA(freopen64, l_freopen), N(fdopen), N(fclose),
    N(fread), N(fwrite), N(fgets), N(fputs), N(fputc), NA(putc, l_fputc), NA(_IO_putc, l_fputc),
    N(fgetc), NA(getc, l_fgetc), NA(_IO_getc, l_fgetc), N(ungetc), N(feof), N(ferror),
    NA(__fgetc_unlocked, l_fgetc), NA(__fputc_unlocked, l_fputc),
    N(clearerr), N(fflush), N(fseek), N(ftell), NA(fseeko, l_fseek), NA(ftello, l_ftell),
    N(fseeko64), N(ftello64), N(rewind), N(fgetpos), N(fsetpos), NA(fgetpos64, l_fgetpos),
    NA(fsetpos64, l_fsetpos), N(setvbuf), N(setbuf), N(fileno), N(putchar), N(puts), N(getchar),
    N(fprintf), N(vfprintf), N(printf), N(vprintf), N(snprintf), N(vsnprintf), N(sprintf),
    N(vsprintf), N(asprintf), N(vasprintf), N(__printf_chk), N(__fprintf_chk), N(__sprintf_chk),
    N(__snprintf_chk), N(__vsnprintf_chk), N(__vsprintf_chk), N(__vfprintf_chk),
    N(sscanf), N(vsscanf), N(fscanf), N(vfscanf), N(scanf), NA(__isoc99_sscanf, l_sscanf),
    NA(__isoc99_fscanf, l_fscanf), NA(__isoc99_scanf, l_scanf), N(perror), N(remove), N(rename),
    N(tmpfile), NA(tmpfile64, l_tmpfile),
    /* ctype, locale, wide characters */
    N(__ctype_b_loc), N(__ctype_tolower_loc), N(__ctype_toupper_loc),
    ND(__ctype_b, &ctype_b_p), ND(__ctype_toupper, &uc_toupper_p),
    N(isalnum), N(isalpha), N(isdigit), N(isxdigit), N(isspace), N(isupper), N(islower),
    N(isprint), N(isgraph), N(ispunct), N(iscntrl), N(isblank), N(tolower), N(toupper),
    N(__newlocale), N(__duplocale), N(__freelocale), N(__uselocale), NA(newlocale, l___newlocale),
    NA(duplocale, l___duplocale), NA(freelocale, l___freelocale), NA(uselocale, l___uselocale),
    N(setlocale), N(nl_langinfo), N(__nl_langinfo_l), N(__strcoll_l), N(__strxfrm_l),
    N(strftime), N(__strftime_l), N(strptime), N(__strtod_l), N(__strtof_l), N(__strtold_l),
    NA(strtold_l, l___strtold_l), N(__ctype_get_mb_cur_max), N(__strtol_l), N(__strtoul_l),
    N(__strtoll_l), N(__strtoull_l), N(__rawmemchr),
    N(__towlower_l), N(__towupper_l), N(towlower), N(towupper), N(__wctype_l), N(wctype),
    N(__iswctype_l), N(iswctype), N(wctob), N(btowc), N(mbrtowc), N(wcrtomb), N(mbsrtowcs),
    N(mbsnrtowcs), N(wcsrtombs), N(wcsnrtombs), N(wcslen), N(wcscmp), N(wcscoll), N(wmemchr), N(wmemcmp),
    N(wmemcpy), N(wmemmove), N(wmemset), N(__wcscoll_l), N(__wcsxfrm_l), N(__wcsftime_l),
    N(getwc), N(putwc), N(ungetwc), N(gettext), N(dgettext), N(dcgettext), N(textdomain),
    N(bindtextdomain), N(bind_textdomain_codeset),
    /* math */
    NF(acos), NF(asin), NF(atan), NF(atan2), NF(ceil), NF(cos), NF(cosh), NF(exp), NF(fabs),
    NF(floor), NF(fmod), NF(frexp), NF(ldexp), NF(log), NF(log10), NF(modf), NF(pow), NF(sin),
    NF(sinh), NF(sqrt), NF(tan), NF(tanh), NF(hypot),
    N(sqrtf), N(sinf), N(cosf), N(tanf), N(atan2f), N(atanf), N(acosf), N(asinf), N(ceilf),
    N(floorf), N(fmodf), N(powf), N(expf), N(logf), N(log10f), N(fabsf), N(log2f), N(log2),
    N(modff), N(exp2f),
    N(__isnan), N(__isnanf), N(__isinf), N(__finite), NA(isnan, l___isnan), NA(isinf, l___isinf),
    N(sincosf), N(sincos), N(frexpl), N(powl), N(ldexpl), N(modfl), N(lrintf), N(lrint), N(round),
    N(roundf), N(trunc), N(truncf), N(rint), N(rintf), N(llrint), N(ldexpf), N(frexpf), N(cbrtf),
    NF(__divdi3), NF(__moddi3), NF(__udivdi3), NF(__umoddi3),
    /* stdlib */
    N(__libc_start_main), N(__uClibc_start_main), N(__errno_location),
    N(rand), N(srand), N(random), N(srandom), N(rand_r), N(qsort), N(bsearch),
    N(getenv), NA(secure_getenv, l_getenv), N(setenv), N(unsetenv), N(putenv),
    N(atexit), N(__cxa_atexit), N(__cxa_finalize), N(exit), N(_exit), NA(_Exit, l__exit),
    N(abort), N(__assert_fail), N(__stack_chk_fail), NA(__stack_chk_fail_local, l___stack_chk_fail),
    /* odds and ends */
    ND(_IO_2_1_stdin_, &lf_std[0]), ND(_IO_2_1_stdout_, &lf_std[1]), ND(_IO_2_1_stderr_, &lf_std[2]),
    N(__isinff), N(finite), N(finitef), NA(__finitef, l_finitef), N(memmem), N(swab), N(lfind),
    N(fnmatch), N(localeconv), N(mallinfo), N(mbstowcs), N(wcscasecmp), N(vswprintf),
    N(iconv_open), N(iconv), N(iconv_close),
    N(__memcpy_chk), N(__memmove_chk), N(__memset_chk), N(__strcpy_chk), N(__strcat_chk),
    N(__strncpy_chk), N(__strncat_chk), N(__fread_chk),
    { NULL, NULL },
};
