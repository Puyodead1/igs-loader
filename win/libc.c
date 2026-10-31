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

/* ---- memory (one allocator for everything ELF code may free) ------------------- */

void *l_malloc(size_t n)
{
    void *p = _aligned_malloc(n ? n : 1, 16);

    if (!p)
        SET_ERRNO(ENOMEM);
    return p;
}

void l_free(void *p)
{
    if (p)
        _aligned_free(p);
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
    return _aligned_realloc(p, n, 16);
}

static void *l_memalign(size_t a, size_t n)
{
    if (a < 16)
        a = 16;
    return _aligned_malloc(n ? n : 1, a);
}

static int l_posix_memalign(void **out, size_t a, size_t n)
{
    *out = l_memalign(a, n);
    return *out ? 0 : ENOMEM;
}

static void *l_valloc(size_t n) { return l_memalign(4096, n); }

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

static void mode_b(const char *mode, char *out)
{
    snprintf(out, 8, "%.5s", mode);
    if (!strchr(out, 'b'))
        strcat(out, "b");
}

static LFILE *l_fopen(const char *path, const char *mode)
{
    char win[MAX_PATH], m[8];
    FILE *f;

    if (!path) {
        SET_ERRNO(EINVAL);
        return NULL;
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
    return MoveFileExA(wa, wb, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
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
static double l___strtod_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }
static float l___strtof_l(const char *s, char **e, void *l) { (void)l; return strtof(s, e); }
static long double l___strtold_l(const char *s, char **e, void *l) { (void)l; return strtold(s, e); }
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

void libc_init(void)
{
    InitializeCriticalSection(&rnd_lock);
    ctype_init();
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
    NA(__libc_memalign, l_memalign),
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
    N(clearerr), N(fflush), N(fseek), N(ftell), NA(fseeko, l_fseek), NA(ftello, l_ftell),
    N(fseeko64), N(ftello64), N(rewind), N(fgetpos), N(fsetpos), NA(fgetpos64, l_fgetpos),
    NA(fsetpos64, l_fsetpos), N(setvbuf), N(setbuf), N(fileno), N(putchar), N(puts), N(getchar),
    N(fprintf), N(vfprintf), N(printf), N(vprintf), N(snprintf), N(vsnprintf), N(sprintf),
    N(vsprintf), N(asprintf), N(vasprintf), N(__printf_chk), N(__fprintf_chk), N(__sprintf_chk),
    N(__snprintf_chk), N(__vsnprintf_chk), N(__vsprintf_chk), N(__vfprintf_chk),
    N(sscanf), N(vsscanf), N(fscanf), N(vfscanf), N(scanf), NA(__isoc99_sscanf, l_sscanf),
    NA(__isoc99_fscanf, l_fscanf), NA(__isoc99_scanf, l_scanf), N(perror), N(remove), N(rename),
    N(tmpfile),
    /* ctype, locale, wide characters */
    N(__ctype_b_loc), N(__ctype_tolower_loc), N(__ctype_toupper_loc),
    N(isalnum), N(isalpha), N(isdigit), N(isxdigit), N(isspace), N(isupper), N(islower),
    N(isprint), N(isgraph), N(ispunct), N(iscntrl), N(isblank), N(tolower), N(toupper),
    N(__newlocale), N(__duplocale), N(__freelocale), N(__uselocale), NA(newlocale, l___newlocale),
    NA(duplocale, l___duplocale), NA(freelocale, l___freelocale), NA(uselocale, l___uselocale),
    N(setlocale), N(nl_langinfo), N(__nl_langinfo_l), N(__strcoll_l), N(__strxfrm_l),
    N(strftime), N(__strftime_l), N(__strtod_l), N(__strtof_l), N(__strtold_l),
    NA(strtold_l, l___strtold_l), N(__ctype_get_mb_cur_max),
    N(__towlower_l), N(__towupper_l), N(towlower), N(towupper), N(__wctype_l), N(wctype),
    N(__iswctype_l), N(iswctype), N(wctob), N(btowc), N(mbrtowc), N(wcrtomb), N(mbsrtowcs),
    N(mbsnrtowcs), N(wcsrtombs), N(wcsnrtombs), N(wcslen), N(wcscmp), N(wmemchr), N(wmemcmp),
    N(wmemcpy), N(wmemmove), N(wmemset), N(__wcscoll_l), N(__wcsxfrm_l), N(__wcsftime_l),
    N(getwc), N(putwc), N(ungetwc), N(gettext), N(dgettext), N(dcgettext), N(textdomain),
    N(bindtextdomain), N(bind_textdomain_codeset),
    /* math */
    NF(acos), NF(asin), NF(atan), NF(atan2), NF(ceil), NF(cos), NF(cosh), NF(exp), NF(fabs),
    NF(floor), NF(fmod), NF(frexp), NF(ldexp), NF(log), NF(log10), NF(modf), NF(pow), NF(sin),
    NF(sinh), NF(sqrt), NF(tan), NF(tanh), NF(hypot),
    N(sqrtf), N(sinf), N(cosf), N(tanf), N(atan2f), N(atanf), N(acosf), N(asinf), N(ceilf),
    N(floorf), N(fmodf), N(powf), N(expf), N(logf), N(log10f), N(fabsf),
    N(__isnan), N(__isnanf), N(__isinf), N(__finite), NA(isnan, l___isnan), NA(isinf, l___isinf),
    N(sincosf), N(sincos), N(frexpl), N(powl), N(ldexpl), N(modfl), N(lrintf), N(lrint), N(round),
    N(roundf), N(trunc),
    NF(__divdi3), NF(__moddi3), NF(__udivdi3), NF(__umoddi3),
    /* stdlib */
    N(__libc_start_main), N(__errno_location),
    N(rand), N(srand), N(random), N(srandom), N(rand_r), N(qsort), N(bsearch),
    N(getenv), NA(secure_getenv, l_getenv), N(setenv), N(unsetenv), N(putenv),
    N(atexit), N(__cxa_atexit), N(__cxa_finalize), N(exit), N(_exit), NA(_Exit, l__exit),
    N(abort), N(__assert_fail), N(__stack_chk_fail), NA(__stack_chk_fail_local, l___stack_chk_fail),
    { NULL, NULL },
};
