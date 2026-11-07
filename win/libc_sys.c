/*
 * IGS loader, Windows core: glibc stand-in, part 2.
 * Linux paths (the game's DATA dir is the cwd; absolute paths go to the
 * cabinet root fs copy, then DATA; /mnt/<d>/ is drive <d>; WSL symlinks are
 * followed by reading their reparse data), the fd table (CRT fds, device
 * placeholders routed to the machine plugin), stat/dirent in glibc's 32-bit
 * layouts, mmap, termios, time, signals (recorded, never delivered),
 * setjmp/longjmp and process stubs.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "lx.h"
#include "../core/tmpcmd.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
#define IO_REPARSE_TAG_LX_SYMLINK_ 0xA000001Du

char **lx_environ;

/* ---- time base ------------------------------------------------------------------------ */

static LARGE_INTEGER qpf, qp0;

double lx_now(void)
{
    LARGE_INTEGER t;

    if (!qpf.QuadPart) {
        QueryPerformanceFrequency(&qpf);
        QueryPerformanceCounter(&qp0);
    }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - qp0.QuadPart) / qpf.QuadPart;
}

static __thread HANDLE hrtimer;

void lx_usleep(int64_t us)
{
    LARGE_INTEGER due;

    if (us <= 0) {
        Sleep(0);
        return;
    }
    if (!hrtimer)
        hrtimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!hrtimer) {
        lx_sleep_ms((DWORD)((us + 999) / 1000));
        return;
    }
    due.QuadPart = -us * 10;
    SetWaitableTimer(hrtimer, &due, 0, NULL, NULL, FALSE);
    lx_wait(hrtimer);
}

void lx_sleep_until(double t)
{
    double d = t - lx_now();

    if (d > 0)
        lx_usleep((int64_t)(d * 1e6));
}

/* 100 ns since 1601 -> Unix */
static int64_t unix_100ns(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;

    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)u.QuadPart - 116444736000000000LL;
}

static int32_t ft_unix(const FILETIME *ft)
{
    ULARGE_INTEGER u;

    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (int32_t)(((int64_t)u.QuadPart - 116444736000000000LL) / 10000000);
}

/* ---- paths ------------------------------------------------------------------------------ */

static char data_dir[MAX_PATH], rootfs_dir[MAX_PATH];

void lx_set_dirs(const char *data, const char *rootfs)
{
    snprintf(data_dir, sizeof(data_dir), "%s", data);
    snprintf(rootfs_dir, sizeof(rootfs_dir), "%s", rootfs ? rootfs : "");
}

const char *lx_data_dir(void) { return data_dir; }

static void slashes(char *p)
{
    for (; *p; p++)
        if (*p == '/')
            *p = '\\';
}

/* a WSL symlink's target (UTF-8, as stored), or -1 */
static int read_lx_link(const char *path, char *out, size_t n)
{
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    uint8_t buf[4096];
    DWORD got;
    uint32_t tag;
    uint16_t len;

    if (h == INVALID_HANDLE_VALUE)
        return -1;
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof(buf), &got, NULL)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    memcpy(&tag, buf, 4);
    memcpy(&len, buf + 4, 2);
    if (tag != IO_REPARSE_TAG_LX_SYMLINK_ || len < 4 || (size_t)(len - 4) >= n)
        return -1;
    memcpy(out, buf + 12, len - 4);
    out[len - 4] = 0;
    return 0;
}

/*
 * Follow WSL symlinks anywhere in a Windows path, in place.  Cheap when the
 * path exists as a plain file or directory.
 */
int lx_resolve(char *path, size_t n)
{
    int hops;

    for (hops = 0; hops < 16; hops++) {
        DWORD a = GetFileAttributesA(path);
        char *p, prefix[MAX_PATH], target[MAX_PATH], rest[MAX_PATH], next[MAX_PATH];
        int found = 0;

        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT))
            return 0;
        /* walk the components to the first link */
        p = path;
        if (p[0] && p[1] == ':')
            p += 2;
        while (*p == '\\')
            p++;
        for (;;) {
            char *end = strchr(p, '\\');
            size_t plen = end ? (size_t)(end - path) : strlen(path);

            memcpy(prefix, path, plen);
            prefix[plen] = 0;
            a = GetFileAttributesA(prefix);
            if (a == INVALID_FILE_ATTRIBUTES)
                return 0;              /* does not exist: leave it */
            if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (read_lx_link(prefix, target, sizeof(target)))
                    return 0;          /* some other reparse point: Windows follows it */
                snprintf(rest, sizeof(rest), "%s", end ? end : "");
                found = 1;
                break;
            }
            if (!end)
                return 0;
            p = end + 1;
        }
        if (!found)
            return 0;
        if (target[0] == '/') {
            if (lx_path(target, next, sizeof(next)))
                return -1;
        } else {
            char *slash = strrchr(prefix, '\\');

            if (slash)
                *slash = 0;
            slashes(target);
            snprintf(next, sizeof(next), "%s\\%s", prefix, target);
        }
        strncat(next, rest, sizeof(next) - strlen(next) - 1);
        if (!GetFullPathNameA(next, (DWORD)n, path, NULL))
            snprintf(path, n, "%s", next);
    }
    return -1;
}

static int exists(const char *win)
{
    return GetFileAttributesA(win) != INVALID_FILE_ATTRIBUTES;
}

static void tmp_rm(const char *win);

/*
 * The game's /tmp: %TEMP%\igs-tmp\<pid>, one per running loader (We Dance
 * copies each song's music there and empties it; two instances sharing one
 * folder deleted each other's).  Folders of loaders no longer running go.
 */
static const char *tmp_dir(void)
{
    static char dir[MAX_PATH];
    char tmp[MAX_PATH], pat[MAX_PATH], old[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h, p;

    if (*dir)
        return dir;
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(pat, sizeof(pat), "%sigs-tmp\\*", tmp);
    if ((h = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            DWORD pid = strtoul(fd.cFileName, NULL, 10);

            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !pid || pid == GetCurrentProcessId())
                continue;
            if ((p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) {
                CloseHandle(p);
                continue;
            }
            snprintf(old, sizeof(old), "%sigs-tmp\\%s", tmp, fd.cFileName);
            tmp_rm(old);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    snprintf(dir, sizeof(dir), "%sigs-tmp", tmp);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, sizeof(dir), "%sigs-tmp\\%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
    return dir;
}

int lx_path(const char *in, char *out, size_t n)
{
    char a[MAX_PATH], b[MAX_PATH];

    if (!in || !*in)
        return -1;
    if (((in[0] | 32) >= 'a' && (in[0] | 32) <= 'z') && in[1] == ':') {
        snprintf(out, n, "%s", in);
    } else if (in[0] != '/') {
        snprintf(out, n, "%s", in);        /* relative: the cwd is DATA */
    } else if (!strncmp(in, "/mnt/", 5) && in[5] && (in[6] == '/' || !in[6])) {
        snprintf(out, n, "%c:%s", in[5] & ~32, in[6] ? in + 6 : "/");
    } else if (!strncmp(in, "/tmp", 4) && (in[4] == '/' || !in[4])) {
        snprintf(out, n, "%s", tmp_dir());
        strncat(out, in + 4, n - strlen(out) - 1);
    } else if (!strncmp(in, "/dev/", 5) || !strncmp(in, "/proc/", 6) || !strncmp(in, "/sys/", 5)) {
        return -1;
    } else {
        /* the cabinet's root fs copy, then the game dir */
        snprintf(a, sizeof(a), "%s%s", rootfs_dir, in);
        slashes(a);
        lx_resolve(a, sizeof(a));
        snprintf(b, sizeof(b), "%s%s", data_dir, in);
        slashes(b);
        lx_resolve(b, sizeof(b));
        if (*rootfs_dir && exists(a))
            snprintf(out, n, "%s", a);
        else
            snprintf(out, n, "%s", b);
        return 0;
    }
    slashes(out);
    lx_resolve(out, n);
    return 0;
}

/* a Windows path as the Linux side sees it (/mnt/<d>/...) */
static void linux_form(const char *win, char *out, size_t n)
{
    char *p;

    if (win[0] && win[1] == ':')
        snprintf(out, n, "/mnt/%c%s", win[0] | 32, win + 2);
    else
        snprintf(out, n, "%s", win);
    for (p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (strlen(out) > 1 && out[strlen(out) - 1] == '/')
        out[strlen(out) - 1] = 0;
}

/* ---- the fd table ------------------------------------------------------------------------ */

enum { FD_FREE, FD_CRT, FD_NULL, FD_DIR, FD_SOCK, FD_ZERO, FD_RANDOM };
#define NFD 1024
static struct { int type, crt; } fds[NFD] = {
    { FD_CRT, 0 }, { FD_CRT, 1 }, { FD_CRT, 2 },
};
static CRITICAL_SECTION fd_lock;

static int fd_alloc(int type, int crt)
{
    int i;

    EnterCriticalSection(&fd_lock);
    for (i = 3; i < NFD; i++)
        if (fds[i].type == FD_FREE) {
            fds[i].type = type;
            fds[i].crt = crt;
            LeaveCriticalSection(&fd_lock);
            return i;
        }
    LeaveCriticalSection(&fd_lock);
    SET_ERRNO(24);                     /* EMFILE */
    return -1;
}

int lx_fd_new_null(void) { return fd_alloc(FD_NULL, -1); }
int lx_fd_new_sock(void) { return fd_alloc(FD_SOCK, -1); }
int lx_fd_is_sock(int fd) { return fd >= 0 && fd < NFD && fds[fd].type == FD_SOCK; }

int lx_fd_from_crt(int crtfd)
{
    int i;

    if (crtfd < 0)
        return -1;
    if (crtfd <= 2)
        return crtfd;
    for (i = 3; i < NFD; i++)
        if (fds[i].type == FD_CRT && fds[i].crt == crtfd)
            return i;
    return fd_alloc(FD_CRT, crtfd);
}

int lx_crt_fd(int fd)
{
    return fd >= 0 && fd < NFD && fds[fd].type == FD_CRT ? fds[fd].crt : -1;
}

static int bad_fd(int fd)
{
    if (fd < 0 || fd >= NFD || fds[fd].type == FD_FREE) {
        SET_ERRNO(EBADF);
        return 1;
    }
    return 0;
}

static int l_open(const char *path, int flags, ...)
{
    char win[MAX_PATH];
    int fd, c, cf = _O_BINARY | _O_NOINHERIT, mode = 0644;
    DWORD a;

    if (flags & L_O_CREAT) {
        va_list ap;

        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!path) {
        SET_ERRNO(EFAULT);
        return -1;
    }
    if (core_route_open(path, &fd))
        return fd;
    if (!strcmp(path, "/dev/null"))
        return lx_fd_new_null();
    if (!strcmp(path, "/dev/zero"))
        return fd_alloc(FD_ZERO, -1);
    /* Mono's RNG (Guid.NewGuid, Random seeds) */
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random"))
        return fd_alloc(FD_RANDOM, -1);
    /* the machine's or the core's text files (/proc/stat, /proc/self/maps): a temporary copy */
    if ((flags & L_O_ACCMODE) == 0) {
        const char *txt = core_vfile(path);
        char dir[MAX_PATH], tmp[MAX_PATH];

        if (txt && GetTempPathA(sizeof(dir), dir) && GetTempFileNameA(dir, "igs", 0, tmp) &&
            (c = _open(tmp, _O_RDWR | _O_BINARY | _O_TEMPORARY | _O_NOINHERIT)) >= 0) {
            _write(c, txt, (unsigned)strlen(txt));
            _lseek(c, 0, SEEK_SET);
            return lx_fd_from_crt(c);
        }
    }
    if (lx_path(path, win, sizeof(win))) {
        LOG("open %s: no such device or file", path);
        SET_ERRNO(ENOENT);
        return -1;
    }
    a = GetFileAttributesA(win);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
        if ((flags & L_O_ACCMODE) != 0) {
            SET_ERRNO(EISDIR);
            return -1;
        }
        return fd_alloc(FD_DIR, -1);
    }
    if (flags & L_O_DIRECTORY) {
        SET_ERRNO(ENOTDIR);
        return -1;
    }
    switch (flags & L_O_ACCMODE) {
    case 0: cf |= _O_RDONLY; break;
    case 1: cf |= _O_WRONLY; break;
    default: cf |= _O_RDWR; break;
    }
    if (flags & L_O_CREAT) cf |= _O_CREAT;
    if (flags & L_O_EXCL) cf |= _O_EXCL;
    if (flags & L_O_TRUNC) cf |= _O_TRUNC;
    if (flags & L_O_APPEND) cf |= _O_APPEND;
    c = _open(win, cf, (mode & 0200) ? _S_IREAD | _S_IWRITE : _S_IREAD);
    if (c < 0) {
        SET_ERRNO(lx_errno_from_crt(errno));
        if (lx_verbose)
            LOG("open %s (%s): %s", path, win, strerror(errno));
        return -1;
    }
    core_note_fopen(path);
    return fd_alloc(FD_CRT, c);
}

static ssize_t l_read(int fd, void *buf, size_t n)
{
    long r;
    int c;

    if (core_read(fd, buf, n, &r))
        return r;
    if (bad_fd(fd))
        return -1;
    switch (fds[fd].type) {
    case FD_CRT:
        c = _read(fds[fd].crt, buf, (unsigned)n);
        if (c < 0)
            SET_ERRNO(lx_errno_from_crt(errno));
        return c;
    case FD_NULL:
        return 0;
    case FD_ZERO:
        memset(buf, 0, n);
        return (ssize_t)n;
    case FD_RANDOM: {
        static BOOLEAN (WINAPI *gen)(void *, ULONG);

        if (!gen)
            gen = (BOOLEAN (WINAPI *)(void *, ULONG))(void *)GetProcAddress(LoadLibraryA("advapi32.dll"), "SystemFunction036");
        if (!gen || !gen(buf, (ULONG)n)) {
            SET_ERRNO(EIO);
            return -1;
        }
        return (ssize_t)n;
    }
    case FD_SOCK:
        return lx_net_read(fd, buf, n);
    default:
        SET_ERRNO(EISDIR);
        return -1;
    }
}

static ssize_t l_write(int fd, const void *buf, size_t n)
{
    long r;
    int c;

    if (core_write(fd, buf, n, &r))
        return r;
    if (bad_fd(fd))
        return -1;
    switch (fds[fd].type) {
    case FD_CRT:
        if (fd <= 2) {
            fflush(fd == 1 ? stdout : stderr);
        }
        c = _write(fds[fd].crt, buf, (unsigned)n);
        if (c < 0)
            SET_ERRNO(lx_errno_from_crt(errno));
        return c;
    case FD_NULL: case FD_ZERO: case FD_RANDOM:
        return n;
    case FD_SOCK:
        return lx_net_write(fd, buf, n);
    default:
        SET_ERRNO(EISDIR);
        return -1;
    }
}

static int l_close(int fd)
{
    if (core_close(fd))
        return 0;
    if (bad_fd(fd))
        return -1;
    if (fds[fd].type == FD_CRT && fd > 2)
        _close(fds[fd].crt);
    if (fds[fd].type == FD_SOCK)
        lx_net_close(fd);
    if (fd > 2)
        fds[fd].type = FD_FREE;
    return 0;
}

static long l_lseek(int fd, long off, int wh)
{
    long r;

    if (bad_fd(fd))
        return -1;
    if (core_seek(fd, off, wh, &r))
        return r;
    if (fds[fd].type != FD_CRT)
        return 0;
    return _lseek(fds[fd].crt, off, wh);
}

static long long l_lseek64(int fd, long long off, int wh)
{
    long r;

    if (bad_fd(fd))
        return -1;
    if (core_seek(fd, (long)off, wh, &r))
        return r;
    if (fds[fd].type != FD_CRT)
        return 0;
    return _lseeki64(fds[fd].crt, off, wh);
}

static ssize_t l_pread(int fd, void *buf, size_t n, long off)
{
    long long cur = l_lseek64(fd, 0, SEEK_CUR);
    ssize_t r;

    l_lseek64(fd, off, SEEK_SET);
    r = l_read(fd, buf, n);
    l_lseek64(fd, cur, SEEK_SET);
    return r;
}

struct l_iovec { void *base; size_t len; };

static ssize_t l_writev(int fd, const struct l_iovec *iov, int n)
{
    ssize_t total = 0, r;
    int i;

    for (i = 0; i < n; i++) {
        r = l_write(fd, iov[i].base, iov[i].len);
        if (r < 0)
            return total ? total : -1;
        total += r;
    }
    return total;
}

static ssize_t l_readv(int fd, const struct l_iovec *iov, int n)
{
    ssize_t total = 0, r;
    int i;

    for (i = 0; i < n; i++) {
        r = l_read(fd, iov[i].base, iov[i].len);
        if (r < 0)
            return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].len)
            break;
    }
    return total;
}

static int l_dup(int fd)
{
    if (bad_fd(fd))
        return -1;
    if (fds[fd].type == FD_CRT)
        return fd_alloc(FD_CRT, _dup(fds[fd].crt));
    return fd_alloc(fds[fd].type, -1);
}

static int l_dup2(int fd, int to)
{
    if (bad_fd(fd) || to < 0 || to >= NFD)
        return -1;
    if (fd == to)
        return to;
    if (fds[to].type == FD_CRT && to > 2)
        _close(fds[to].crt);
    fds[to].type = fds[fd].type;
    fds[to].crt = fds[fd].type == FD_CRT ? _dup(fds[fd].crt) : -1;
    return to;
}

static int l_pipe(int p[2])
{
    int c[2];

    if (_pipe(c, 65536, _O_BINARY | _O_NOINHERIT))
        return -1;
    p[0] = fd_alloc(FD_CRT, c[0]);
    p[1] = fd_alloc(FD_CRT, c[1]);
    return 0;
}

static int l_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg;

    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    if (bad_fd(fd))
        return -1;
    if (fds[fd].type == FD_SOCK) {    /* F_GETFL / F_SETFL: O_NONBLOCK */
        if (cmd == 3)
            return 2 | (lx_net_nonblocking(fd) ? L_O_NONBLOCK : 0);
        if (cmd == 4)
            lx_net_nonblock(fd, (arg & L_O_NONBLOCK) != 0);
        return cmd == 0 ? (SET_ERRNO(EINVAL), -1) : 0;
    }
    switch (cmd) {
    case 0: {                         /* F_DUPFD */
        int n = l_dup(fd);

        (void)arg;
        return n;
    }
    case 3: return 2;                 /* F_GETFL: O_RDWR */
    default: return 0;                /* F_SETFL, F_GETFD, F_SETFD, locks */
    }
}

static int l_isatty(int fd) { return fd >= 0 && fd <= 2 ? _isatty(fd) != 0 : 0; }
/* poll: sockets only (net.c); nothing else is ever ready */
static int l_poll(void *fds_, unsigned n, int timeout)
{
    const int *p = fds_;
    unsigned i;

    for (i = 0; i < n; i++)
        if (lx_fd_is_sock(p[2 * i]))
            return lx_net_poll(fds_, n, timeout);
    return 0;
}

static int l_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    int r;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (core_ioctl(fd, req, arg, &r))
        return r;
    if (lx_fd_is_sock(fd))
        return lx_net_ioctl(fd, req, arg);
    if (req == 0x541B && arg) {          /* FIONREAD */
        *(int *)arg = 0;
        return 0;
    }
    if (lx_verbose)
        LOG("ioctl(%d, 0x%lx): not a device", fd, req);
    SET_ERRNO(25);                       /* ENOTTY */
    return -1;
}

/* termios: on the machine's ports as TCGETS / TCSETS+n / TCFLSH ioctls (as on Linux) */
static int l_tcgetattr(int fd, void *t)
{
    int r;

    if (core_ioctl(fd, 0x5401, t, &r))
        return r;
    memset(t, 0, 60);
    return fd <= 2 ? 0 : 0;
}
static int l_tcsetattr(int fd, int act, const void *t)
{
    int r;

    if (core_ioctl(fd, 0x5402 + act, (void *)t, &r))
        return r;
    return 0;
}
static int l_tcflush(int fd, int q)
{
    int r;

    if (core_ioctl(fd, 0x540B, (void *)(intptr_t)q, &r))
        return r;
    return 0;
}
static int l_tcdrain(int fd) { (void)fd; return 0; }
/* struct termios: c_cflag at 8, c_ispeed 52, c_ospeed 56; CBAUD 0x100f */
static int l_cfsetospeed(void *t, unsigned s)
{
    uint32_t *c = (uint32_t *)((char *)t + 8);

    *c = (*c & ~0x100fu) | (s & 0x100f);
    *(uint32_t *)((char *)t + 56) = s;
    return 0;
}
static int l_cfsetispeed(void *t, unsigned s) { *(uint32_t *)((char *)t + 52) = s; return 0; }
static unsigned l_cfgetospeed(const void *t) { return *(const uint32_t *)((const char *)t + 8) & 0x100f; }
static unsigned l_cfgetispeed(const void *t) { return l_cfgetospeed(t); }
static void l_cfmakeraw(void *t)
{
    uint32_t *f = t;

    f[0] &= ~0x5ebu;
    f[1] &= ~1u;
    f[3] &= ~0x804bu;
    f[2] = (f[2] & ~0x130u) | 0x30;
}

/* ---- stat ----------------------------------------------------------------------------------- */

static uint32_t path_ino(const char *s)
{
    uint32_t h = 2166136261u;

    for (; *s; s++)
        h = (h ^ (uint8_t)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s)) * 16777619u;
    return h ? h : 1;
}

static int stat64_win(const char *win, struct l_stat64 *st)
{
    WIN32_FILE_ATTRIBUTE_DATA d;

    if (!GetFileAttributesExA(win, GetFileExInfoStandard, &d)) {
        SET_ERRNO(ENOENT);
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_dev = 0x801;
    st->st_ino = st->__st_ino = path_ino(win);
    st->st_mode = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0040755 :
                  ((d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0100555 : 0100755);
    st->st_nlink = 1;
    st->st_size = ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_atim.tv_sec = ft_unix(&d.ftLastAccessTime);
    st->st_mtim.tv_sec = ft_unix(&d.ftLastWriteTime);
    st->st_ctim.tv_sec = ft_unix(&d.ftCreationTime);
    return 0;
}

static int dev_stat(const char *path, struct l_stat64 *st)
{
    int fd;

    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = 0020666;               /* character device */
    st->st_nlink = 1;
    st->st_ino = st->__st_ino = path_ino(path);
    st->st_blksize = 4096;
    return 0;
}

static int stat64_path(const char *path, struct l_stat64 *st)
{
    char win[MAX_PATH];

    if (!path) {
        SET_ERRNO(EFAULT);
        return -1;
    }
    if (!strncmp(path, "/dev/", 5))
        return dev_stat(path, st);
    if (lx_path(path, win, sizeof(win))) {
        SET_ERRNO(ENOENT);
        return -1;
    }
    return stat64_win(win, st);
}

static int stat64_fd(int fd, struct l_stat64 *st)
{
    struct _stati64 s;

    if (bad_fd(fd))
        return -1;
    memset(st, 0, sizeof(*st));
    if (fds[fd].type != FD_CRT) {
        st->st_mode = fds[fd].type == FD_DIR ? 0040755 : 0020666;
        st->st_nlink = 1;
        return 0;
    }
    if (_fstati64(fds[fd].crt, &s)) {
        SET_ERRNO(EBADF);
        return -1;
    }
    st->st_dev = 0x801;
    st->st_ino = st->__st_ino = 1000 + fd;
    st->st_mode = (s.st_mode & _S_IFDIR) ? 0040755 : (s.st_mode & _S_IFCHR) ? 0020666 :
                  (s.st_mode & _S_IFIFO) ? 0010666 : 0100755;
    st->st_nlink = 1;
    st->st_size = s.st_size;
    st->st_blksize = 4096;
    st->st_blocks = (s.st_size + 511) / 512;
    st->st_atim.tv_sec = (int32_t)s.st_atime;
    st->st_mtim.tv_sec = (int32_t)s.st_mtime;
    st->st_ctim.tv_sec = (int32_t)s.st_ctime;
    return 0;
}

static int to_stat32(int r, const struct l_stat64 *s, struct l_stat *st)
{
    if (r)
        return r;
    if (s->st_size > 0x7fffffff) {
        SET_ERRNO(L_EOVERFLOW);
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_dev = s->st_dev;
    st->st_ino = (uint32_t)s->st_ino;
    st->st_mode = s->st_mode;
    st->st_nlink = s->st_nlink;
    st->st_uid = s->st_uid;
    st->st_gid = s->st_gid;
    st->st_rdev = s->st_rdev;
    st->st_size = (int32_t)s->st_size;
    st->st_blksize = s->st_blksize;
    st->st_blocks = (int32_t)s->st_blocks;
    st->st_atim = s->st_atim;
    st->st_mtim = s->st_mtim;
    st->st_ctim = s->st_ctim;
    return 0;
}

static int l___xstat(int v, const char *p, struct l_stat *st) { struct l_stat64 s; (void)v; return to_stat32(stat64_path(p, &s), &s, st); }
static int l___xstat64(int v, const char *p, struct l_stat64 *st) { (void)v; return stat64_path(p, st); }
static int l___fxstat(int v, int fd, struct l_stat *st) { struct l_stat64 s; (void)v; return to_stat32(stat64_fd(fd, &s), &s, st); }
static int l___fxstat64(int v, int fd, struct l_stat64 *st) { (void)v; return stat64_fd(fd, st); }
static int l_stat(const char *p, struct l_stat *st) { return l___xstat(3, p, st); }
static int l_fstat(int fd, struct l_stat *st) { return l___fxstat(3, fd, st); }

static int l_access(const char *path, int mode)
{
    char win[MAX_PATH];

    (void)mode;
    if (!strncmp(path, "/dev/", 5) || core_vfile(path))
        return 0;
    if (lx_path(path, win, sizeof(win)) || !exists(win)) {
        SET_ERRNO(ENOENT);
        return -1;
    }
    return 0;
}

static int l_unlink(const char *path)
{
    char win[MAX_PATH];

    if (lx_path(path, win, sizeof(win)) || !DeleteFileA(win)) {
        SET_ERRNO(ENOENT);
        return -1;
    }
    return 0;
}

static int l_mkdir(const char *path, int mode)
{
    char win[MAX_PATH];

    (void)mode;
    if (lx_path(path, win, sizeof(win)))
        return -1;
    if (!CreateDirectoryA(win, NULL)) {
        SET_ERRNO(GetLastError() == ERROR_ALREADY_EXISTS ? EEXIST : ENOENT);
        return -1;
    }
    return 0;
}

static int l_rmdir(const char *path)
{
    char win[MAX_PATH];

    if (lx_path(path, win, sizeof(win)) || !RemoveDirectoryA(win))
        return -1;
    return 0;
}

static int l_chdir(const char *path)
{
    char win[MAX_PATH];

    if (lx_path(path, win, sizeof(win)) || !SetCurrentDirectoryA(win)) {
        SET_ERRNO(ENOENT);
        return -1;
    }
    return 0;
}

static char *l_getcwd(char *buf, size_t size)
{
    char win[MAX_PATH], lin[MAX_PATH];

    GetCurrentDirectoryA(sizeof(win), win);
    linux_form(win, lin, sizeof(lin));
    if (!buf) {
        size = size ? size : strlen(lin) + 1;
        buf = l_malloc(size);
    }
    if (strlen(lin) + 1 > size) {
        SET_ERRNO(ERANGE);
        return NULL;
    }
    strcpy(buf, lin);
    return buf;
}

static char *l_realpath(const char *path, char *out)
{
    char full[1024], win[MAX_PATH];
    char *parts[128], *p, *save = NULL;
    int n = 0, i;
    char res[1024] = "";

    if (path[0] == '/')
        snprintf(full, sizeof(full), "%s", path);
    else {
        char cwd[MAX_PATH];

        l_getcwd(cwd, sizeof(cwd));
        snprintf(full, sizeof(full), "%s/%s", cwd, path);
    }
    for (p = strtok_r(full, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (!strcmp(p, "."))
            continue;
        if (!strcmp(p, "..")) {
            if (n)
                n--;
            continue;
        }
        if (n < 128)
            parts[n++] = p;
    }
    for (i = 0; i < n; i++) {
        strcat(res, "/");
        strcat(res, parts[i]);
    }
    if (!n)
        strcpy(res, "/");
    if (lx_path(res, win, sizeof(win)) || !exists(win)) {
        SET_ERRNO(ENOENT);
        return NULL;
    }
    if (!out)
        out = l_malloc(4096);
    strcpy(out, res);
    return out;
}

/* ---- directories ------------------------------------------------------------------------- */

struct ldir {
    HANDLE h;
    WIN32_FIND_DATAA fd;
    int have;
    long pos;
    char pattern[MAX_PATH], dir[MAX_PATH];
    struct l_dirent e;
    struct l_dirent64 e64;
};

static void *l_opendir(const char *path)
{
    char win[MAX_PATH];
    struct ldir *d;
    DWORD a;

    /* Linux always has /dev, /proc and /sys: empty here (Unity lists /dev/input, ...) */
    if (!strncmp(path, "/dev", 4) || !strncmp(path, "/proc", 5) || !strncmp(path, "/sys", 4)) {
        d = calloc(1, sizeof(*d));
        d->h = INVALID_HANDLE_VALUE;
        return d;
    }
    if (lx_path(path, win, sizeof(win)) || (a = GetFileAttributesA(win)) == INVALID_FILE_ATTRIBUTES) {
        SET_ERRNO(ENOENT);
        return NULL;
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SET_ERRNO(ENOTDIR);
        return NULL;
    }
    d = calloc(1, sizeof(*d));
    snprintf(d->dir, sizeof(d->dir), "%s", win);
    snprintf(d->pattern, sizeof(d->pattern), "%s\\*", win);
    d->h = FindFirstFileA(d->pattern, &d->fd);
    d->have = d->h != INVALID_HANDLE_VALUE;
    return d;
}

static int next_entry(struct ldir *d, uint8_t *type, uint32_t *ino, char *name)
{
    char full[MAX_PATH];

    if (!d || !d->have)
        return 0;
    snprintf(name, 256, "%s", d->fd.cFileName);
    snprintf(full, sizeof(full), "%s\\%s", d->dir, d->fd.cFileName);
    *ino = path_ino(full);
    if (d->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        *type = 0;                        /* DT_UNKNOWN: stat follows the WSL link */
    else
        *type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
    d->have = FindNextFileA(d->h, &d->fd);
    d->pos++;
    return 1;
}

static struct l_dirent *l_readdir(struct ldir *d)
{
    uint32_t ino;

    if (!d)
        return NULL;
    memset(&d->e, 0, sizeof(d->e));
    if (!next_entry(d, &d->e.d_type, &ino, d->e.d_name))
        return NULL;
    d->e.d_ino = ino;
    d->e.d_off = d->pos;
    d->e.d_reclen = sizeof(d->e);
    return &d->e;
}

static struct l_dirent64 *l_readdir64(struct ldir *d)
{
    uint32_t ino;

    if (!d)
        return NULL;
    memset(&d->e64, 0, sizeof(d->e64));
    if (!next_entry(d, &d->e64.d_type, &ino, d->e64.d_name))
        return NULL;
    d->e64.d_ino = ino;
    d->e64.d_off = d->pos;
    d->e64.d_reclen = sizeof(d->e64);
    return &d->e64;
}

static int l_closedir(struct ldir *d)
{
    if (!d)
        return -1;
    if (d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    free(d);
    return 0;
}

static void l_rewinddir(struct ldir *d)
{
    if (d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    d->h = FindFirstFileA(d->pattern, &d->fd);
    d->have = d->h != INVALID_HANDLE_VALUE;
    d->pos = 0;
}

static long l_telldir(struct ldir *d) { return d->pos; }

static void l_seekdir(struct ldir *d, long pos)
{
    uint8_t t;
    uint32_t ino;
    char name[256];

    l_rewinddir(d);
    while (d->pos < pos && next_entry(d, &t, &ino, name))
        ;
}

static int l_dirfd(struct ldir *d) { (void)d; return -1; }

/* readdir_r (Speed Driver 3): the entry copied out, *res NULL at the end */
static int l_readdir_r(struct ldir *d, struct l_dirent *entry, struct l_dirent **res)
{
    struct l_dirent *e = l_readdir(d);

    if (e) {
        memcpy(entry, e, sizeof(*entry));
        *res = entry;
    } else {
        *res = NULL;
    }
    return 0;
}

/* scandir: the game's filter and compare are i386 cdecl, called directly; entries from l_malloc
   so the game frees them */
static int l_alphasort(const struct l_dirent **a, const struct l_dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int l_scandir(const char *path, struct l_dirent ***list, int (*filter)(const struct l_dirent *),
                     int (*compar)(const struct l_dirent **, const struct l_dirent **))
{
    struct ldir *d = l_opendir(path);
    struct l_dirent *e, **v = NULL, **nv;
    int n = 0, cap = 0;

    if (!d)
        return -1;
    while ((e = l_readdir(d))) {
        if (filter && !filter(e))
            continue;
        if (n == cap) {
            cap = cap ? 2 * cap : 32;
            nv = l_malloc(cap * sizeof(*v));
            if (!nv)
                break;
            if (v) {
                memcpy(nv, v, n * sizeof(*v));
                l_free(v);
            }
            v = nv;
        }
        if (!(v[n] = l_malloc(sizeof(*e))))
            break;
        memcpy(v[n++], e, sizeof(*e));
    }
    l_closedir(d);
    if (compar && n > 1)
        qsort(v, n, sizeof(*v), (int (*)(const void *, const void *))compar);
    *list = v;
    return n;
}

/* ---- mmap ----------------------------------------------------------------------------------- */

#define L_MAP_FIXED 0x10
#define L_MAP_ANONYMOUS 0x20
static struct { void *p; size_t len; } maps[256];

static void *do_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off)
{
    void *r, *p;
    int c, i;

    (void)prot;
    if (fd >= 0 && core_mmap(fd, len, off, &r))
        return r;
    p = VirtualAlloc((flags & L_MAP_FIXED) ? addr : NULL, len, MEM_RESERVE | MEM_COMMIT,
                     PAGE_EXECUTE_READWRITE);
    if (!p && (flags & L_MAP_FIXED)) {
        p = VirtualAlloc(addr, len, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p)
            memset(p, 0, len);
    }
    if (!p) {
        SET_ERRNO(ENOMEM);
        return (void *)-1;
    }
    if (!(flags & L_MAP_ANONYMOUS) && fd >= 0) {
        c = lx_crt_fd(fd);
        if (c >= 0) {
            int64_t cur = _lseeki64(c, 0, SEEK_CUR);

            _lseeki64(c, off, SEEK_SET);
            _read(c, p, (unsigned)len);
            _lseeki64(c, cur, SEEK_SET);
        }
    }
    for (i = 0; i < 256; i++)
        if (!maps[i].p) {
            maps[i].p = p;
            maps[i].len = len;
            break;
        }
    return p;
}

static void *l_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    return do_mmap(addr, len, prot, flags, fd, off);
}

static void *l_mmap64(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    return do_mmap(addr, len, prot, flags, fd, off);
}

static int l_munmap(void *p, size_t len)
{
    int i;

    (void)len;
    for (i = 0; i < 256; i++)
        if (maps[i].p == p) {
            VirtualFree(p, 0, MEM_RELEASE);
            maps[i].p = NULL;
            return 0;
        }
    return 0;
}

/* MREMAP_MAYMOVE (1): a new mapping and a copy */
static void *l_mremap(void *old, size_t oldlen, size_t len, int flags, ...)
{
    void *p;

    if (!(flags & 1)) {
        SET_ERRNO(ENOMEM);
        return (void *)-1;
    }
    p = do_mmap(NULL, len, 3, L_MAP_ANONYMOUS | 2, -1, 0);
    if (p == (void *)-1)
        return p;
    memcpy(p, old, oldlen < len ? oldlen : len);
    l_munmap(old, oldlen);
    return p;
}

static int l_mprotect(void *p, size_t len, int prot) { (void)p; (void)len; (void)prot; return 0; }
static int l_msync(void *p, size_t len, int fl) { (void)p; (void)len; (void)fl; return 0; }
static int l_madvise(void *p, size_t len, int a) { (void)p; (void)len; (void)a; return 0; }
static int l_mlock(const void *p, size_t len) { (void)p; (void)len; return 0; }

/* ---- time ------------------------------------------------------------------------------------- */

static int32_t l_time(int32_t *t)
{
    int32_t v = (int32_t)(unix_100ns() / 10000000);

    if (t)
        *t = v;
    return v;
}

static int l_gettimeofday(struct l_timeval *tv, void *tz)
{
    int64_t u = unix_100ns();

    if (tv) {
        tv->tv_sec = (int32_t)(u / 10000000);
        tv->tv_usec = (int32_t)(u % 10000000 / 10);
    }
    if (tz)
        memset(tz, 0, 8);
    return 0;
}

static int l_clock_gettime(int clk, struct l_timespec *ts)
{
    if (clk == 0) {                     /* CLOCK_REALTIME */
        int64_t u = unix_100ns();

        ts->tv_sec = (int32_t)(u / 10000000);
        ts->tv_nsec = (int32_t)(u % 10000000 * 100);
    } else {
        double t = lx_now() + 1000.0;   /* monotonic, never 0 */

        ts->tv_sec = (int32_t)t;
        ts->tv_nsec = (int32_t)((t - (int32_t)t) * 1e9);
    }
    return 0;
}

int l_clock_gettime_pub(int clk, struct l_timespec *ts) { return l_clock_gettime(clk, ts); }

static int l_clock_getres(int clk, struct l_timespec *ts)
{
    (void)clk;
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 100;
    }
    return 0;
}

static long l_clock(void) { return (long)(lx_now() * 1000000); }

static long l_times(void *buf)
{
    if (buf)
        memset(buf, 0, 16);
    return (long)(lx_now() * 100);
}

static int l_ftime(struct l_timeb *tb)
{
    int64_t u = unix_100ns();
    TIME_ZONE_INFORMATION tzi;

    GetTimeZoneInformation(&tzi);
    tb->time = (int32_t)(u / 10000000);
    tb->millitm = (uint16_t)(u % 10000000 / 10000);
    tb->timezone = (int16_t)tzi.Bias;
    tb->dstflag = 0;
    return 0;
}

static int l_nanosleep(const struct l_timespec *req, struct l_timespec *rem)
{
    lx_usleep((int64_t)req->tv_sec * 1000000 + req->tv_nsec / 1000);
    if (rem)
        rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

static int l_clock_nanosleep(int clk, int flags, const struct l_timespec *req, struct l_timespec *rem)
{
    if (flags & 1) {                    /* TIMER_ABSTIME */
        struct l_timespec now;
        int64_t us;

        l_clock_gettime(clk, &now);
        us = (int64_t)(req->tv_sec - now.tv_sec) * 1000000 + (req->tv_nsec - now.tv_nsec) / 1000;
        lx_usleep(us);
        return 0;
    }
    return l_nanosleep(req, rem);
}

static int l_usleep(unsigned us) { lx_usleep(us); return 0; }
static unsigned l_sleep(unsigned s) { lx_usleep((int64_t)s * 1000000); return 0; }

static char tzname_std[64] = "UTC";

static void tm_from_crt(const struct tm *c, struct l_tm *t, int32_t gmtoff)
{
    t->tm_sec = c->tm_sec;
    t->tm_min = c->tm_min;
    t->tm_hour = c->tm_hour;
    t->tm_mday = c->tm_mday;
    t->tm_mon = c->tm_mon;
    t->tm_year = c->tm_year;
    t->tm_wday = c->tm_wday;
    t->tm_yday = c->tm_yday;
    t->tm_isdst = c->tm_isdst;
    t->tm_gmtoff = gmtoff;
    t->tm_zone = tzname_std;
}

/* any time as UTC fields (the CRT refuses those before 1970; glibc does not: Mono's time-zone
   scan walks back hour by hour) */
static void tm_civil(int64_t v, struct l_tm *out)
{
    int64_t days = v >= 0 ? v / 86400 : -((-v + 86399) / 86400), s = v - days * 86400;
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int leap;

    y += m <= 2;
    leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    memset(out, 0, sizeof(*out));
    out->tm_sec = (int)(s % 60);
    out->tm_min = (int)(s / 60 % 60);
    out->tm_hour = (int)(s / 3600);
    out->tm_mday = (int)d;
    out->tm_mon = (int)m - 1;
    out->tm_year = (int)(y - 1900);
    out->tm_wday = (int)(((days % 7) + 11) % 7);       /* 1970-01-01 was a Thursday */
    out->tm_yday = (int)((m > 2 ? (153 * (m - 3) + 2) / 5 + 59 + leap : (m == 2 ? 31 : 0)) + d - 1);
}

static struct l_tm *l_gmtime_r(const int32_t *t, struct l_tm *out)
{
    __time64_t v = *t;
    struct tm c;

    if (_gmtime64_s(&c, &v))
        tm_civil(v, out);
    else
        tm_from_crt(&c, out, 0);
    out->tm_zone = "GMT";
    return out;
}

static struct l_tm *l_localtime_r(const int32_t *t, struct l_tm *out)
{
    __time64_t v = *t;
    struct tm c, g;

    if (_localtime64_s(&c, &v) || _gmtime64_s(&g, &v)) {
        __time64_t now = _time64(NULL);
        int32_t off = 0;

        if (!_localtime64_s(&c, &now)) {               /* today's standard offset */
            c.tm_isdst = 0;
            off = (int32_t)(_mkgmtime64(&c) - now);
        }
        tm_civil(v + off, out);
        out->tm_gmtoff = off;
        out->tm_zone = tzname_std;
        return out;
    }
    g = c;                                  /* _mkgmtime64 clears tm_isdst */
    tm_from_crt(&c, out, (int32_t)(_mkgmtime64(&g) - v));
    return out;
}

static struct l_tm *l_gmtime(const int32_t *t) { static __thread struct l_tm r; return l_gmtime_r(t, &r); }
static struct l_tm *l_localtime(const int32_t *t) { static __thread struct l_tm r; return l_localtime_r(t, &r); }

static int32_t l_mktime(struct l_tm *t)
{
    struct tm c, g;
    __time64_t v;

    memset(&c, 0, sizeof(c));
    c.tm_sec = t->tm_sec;
    c.tm_min = t->tm_min;
    c.tm_hour = t->tm_hour;
    c.tm_mday = t->tm_mday;
    c.tm_mon = t->tm_mon;
    c.tm_year = t->tm_year;
    c.tm_isdst = t->tm_isdst;
    v = _mktime64(&c);
    if (v == -1)
        return -1;
    g = c;                                  /* glibc fills tm_gmtoff: Mono's time-zone scan reads it */
    tm_from_crt(&c, t, (int32_t)(_mkgmtime64(&g) - v));
    return (int32_t)v;
}

static int32_t l_timegm(struct l_tm *t)
{
    struct tm c;

    memset(&c, 0, sizeof(c));
    c.tm_sec = t->tm_sec;
    c.tm_min = t->tm_min;
    c.tm_hour = t->tm_hour;
    c.tm_mday = t->tm_mday;
    c.tm_mon = t->tm_mon;
    c.tm_year = t->tm_year;
    return (int32_t)_mkgmtime64(&c);
}

static char *l_asctime_r(const struct l_tm *t, char *buf)
{
    static const char wd[] = "SunMonTueWedThuFriSat", mo[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

    sprintf(buf, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", wd + 3 * (t->tm_wday % 7), mo + 3 * (t->tm_mon % 12),
            t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, 1900 + t->tm_year);
    return buf;
}
static char *l_asctime(const struct l_tm *t) { static char b[32]; return l_asctime_r(t, b); }
static char *l_ctime(const int32_t *t) { return l_asctime(l_localtime(t)); }
static double l_difftime(int32_t a, int32_t b) { return (double)a - b; }

/* the games set the system clock from the operator menu: not on a desktop */
static int l_settimeofday(const void *tv, const void *tz) { (void)tv; (void)tz; return 0; }
static int l_stime(const int32_t *t) { (void)t; return 0; }
static void l_tzset(void) { }

/* ---- signals: libc_thread.c delivers them ------------------------------------------------------- */

static void *l_signal(int sig, void *h) { return lx_signal(sig, h); }
static int l_sigaction(int sig, const void *act, void *old) { return lx_sigaction(sig, act, old); }

static int l_sigemptyset(uint32_t *s) { memset(s, 0, 128); return 0; }
static int l_sigfillset(uint32_t *s) { memset(s, 0xff, 128); return 0; }
static int l_sigaddset(uint32_t *s, int sig) { if (sig > 0 && sig <= 64) s[(sig - 1) / 32] |= 1u << ((sig - 1) % 32); return 0; }
static int l_sigdelset(uint32_t *s, int sig) { if (sig > 0 && sig <= 64) s[(sig - 1) / 32] &= ~(1u << ((sig - 1) % 32)); return 0; }
static int l_sigismember(const uint32_t *s, int sig) { return sig > 0 && sig <= 64 && (s[(sig - 1) / 32] >> ((sig - 1) % 32) & 1); }
static int l_sigprocmask(int how, const void *set, void *old) { return lx_sigmask(how, set, old); }

static int l_raise(int sig)
{
    void *h = lx_sig_handler(sig);

    LOG("raise(%d)", sig);
    if (lx_raise(sig) || h == (void *)1)    /* a handler ran, or SIG_IGN */
        return 0;
    if (sig != 17 && sig != 28 && sig != 23)   /* not CHLD/WINCH/URG */
        ExitProcess(128 + sig);
    return 0;
}
static int l_getpid(void) { return (int)GetCurrentProcessId(); }

static int l_kill(int pid, int sig)
{
    if (pid == l_getpid() || pid == 0)
        return sig ? l_raise(sig) : 0;
    SET_ERRNO(3);                       /* ESRCH */
    return -1;
}

static unsigned l_alarm(unsigned s) { (void)s; return 0; }
static int l_pause(void) { Sleep(INFINITE); return -1; }

/* ---- processes: none -------------------------------------------------------------------------- */

static int l_fork(void) { LOG("fork(): not supported"); SET_ERRNO(L_ENOSYS); return -1; }
static int l_execvp(const char *f, char *const *a) { (void)a; LOG("exec %s: not supported", f); SET_ERRNO(L_ENOSYS); return -1; }
static int l_execv(const char *f, char *const *a) { return l_execvp(f, a); }
static void tmp_rm(const char *win)
{
    DWORD a = GetFileAttributesA(win);
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH], sub[MAX_PATH];
    HANDLE h;

    if (a == INVALID_FILE_ATTRIBUTES)
        return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesA(win, FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileA(win))
            RemoveDirectoryA(win);
        return;
    }
    snprintf(pat, sizeof(pat), "%s\\*", win);
    if ((h = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
                snprintf(sub, sizeof(sub), "%s\\%s", win, fd.cFileName);
                tmp_rm(sub);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(win);
}

static int tmp_copy(const char *from, const char *to)
{
    DWORD a = GetFileAttributesA(from);
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH], f[MAX_PATH], t[MAX_PATH];
    HANDLE h;
    int ret = 0;

    if (!(a & FILE_ATTRIBUTE_DIRECTORY))
        return CopyFileA(from, to, FALSE) ? 0 : -1;
    CreateDirectoryA(to, NULL);
    snprintf(pat, sizeof(pat), "%s\\*", from);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        snprintf(f, sizeof(f), "%s\\%s", from, fd.cFileName);
        snprintf(t, sizeof(t), "%s\\%s", to, fd.cFileName);
        ret |= tmp_copy(f, t);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ret;
}

/* cp / rm into /tmp (tmp_dir()), core/tmpcmd.h; wildcards in the last component */
static int tmp_cmd(char **argv, int argc)
{
    char src[MAX_PATH], dst[MAX_PATH], dir[MAX_PATH], from[MAX_PATH], to[MAX_PATH], *e;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int i, isdir, ret = 0;
    DWORD a;

    for (i = 1; i < argc - (argv[0][0] == 'c'); i++) {
        if (lx_path(argv[i], src, sizeof(src)))
            return -1;
        for (e = src + strlen(src); e > src && e[-1] == '\\'; )
            *--e = 0;
        snprintf(dir, sizeof(dir), "%s", src);
        if ((e = strrchr(dir, '\\')))
            e[1] = 0;
        else
            dir[0] = 0;
        if ((h = FindFirstFileA(src, &fd)) == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(from, sizeof(from), "%s%s", dir, fd.cFileName);
            if (argv[0][0] == 'r') {
                tmp_rm(from);
                continue;
            }
            if (lx_path(argv[argc - 1], dst, sizeof(dst)))
                return -1;
            a = GetFileAttributesA(dst);
            isdir = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ||
                    argv[argc - 1][strlen(argv[argc - 1]) - 1] == '/';
            for (e = dst + strlen(dst); e > dst && e[-1] == '\\'; )
                *--e = 0;
            snprintf(to, sizeof(to), isdir ? "%s\\%s" : "%s", dst, fd.cFileName);
            tmp_rm(to);
            ret |= tmp_copy(from, to);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return ret;
}

/*
 * system(): nothing runs, except cp and rm into /tmp with IGS_NO_SYSTEM
 * (We Dance Online 2's RAM disk), which run in-process.
 */
static int l_system(const char *cmd)
{
    char buf[1024], *argv[TMPCMD_MAX];
    int argc;

    if (!cmd)
        return 0;
    if (getenv("IGS_NO_SYSTEM") && (argc = tmpcmd_parse(cmd, buf, sizeof(buf), argv)))
        return tmp_cmd(argv, argc) ? 256 : 0;
    LOG("system(\"%s\"): not run", cmd);
    return -1;
}
static int l_waitpid(int pid, int *st, int opt) { (void)pid; (void)st; (void)opt; SET_ERRNO(10); return -1; }
static int l_wait(int *st) { return l_waitpid(-1, st, 0); }
/* popen: only output the machine provides (igs_machine.vfile "|command") */
static void *l_popen(const char *cmd, const char *m)
{
    char name[512];
    void *f;

    snprintf(name, sizeof(name), "|%s", cmd ? cmd : "");
    if (m && m[0] == 'r' && (f = lx_vfile_open(name)))
        return f;
    LOG("popen(\"%s\"): not supported", cmd);
    return NULL;
}
static int l_pclose(void *f) { return f ? lx_vfile_close(f) : -1; }
static int l_getppid(void) { return 1; }
static int l_getuid(void) { return 0; }
static int l_setsid(void) { return l_getpid(); }
static int l_umask(int m) { (void)m; return 022; }
static int l_nice(int n) { (void)n; return 0; }

struct l_utsname { char s[6][65]; };
static int l_uname(struct l_utsname *u)
{
    memset(u, 0, sizeof(*u));
    strcpy(u->s[0], "Linux");
    strcpy(u->s[1], "igs");
    strcpy(u->s[2], "2.6.27");
    strcpy(u->s[3], "#1 SMP");
    strcpy(u->s[4], "i686");
    return 0;
}
static int l_gethostname(char *b, size_t n) { snprintf(b, n, "igs"); return 0; }

static long l_sysconf(int name)
{
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    switch (name) {
    case 2: return 100;                          /* _SC_CLK_TCK */
    case 4: return 1024;                         /* _SC_OPEN_MAX */
    case 30: return 4096;                        /* _SC_PAGESIZE */
    case 83: case 84: return si.dwNumberOfProcessors;
    case 85: return 262144;                      /* _SC_PHYS_PAGES: 1 GiB */
    default: return -1;
    }
}
static int l_getpagesize(void) { return 4096; }
static int l_get_nprocs(void) { return (int)l_sysconf(84); }

/* the cabinet's /etc/mtab, users, port I/O and SysV shared memory: none here */
static void *l_setmntent(const char *f, const char *m) { (void)f; (void)m; return NULL; }
static void *l_getmntent(void *f) { (void)f; return NULL; }
static int l_endmntent(void *f) { (void)f; return 1; }
static void *l_getpwnam(const char *n) { (void)n; return NULL; }
static void *l_getpwuid(int u) { (void)u; return NULL; }
/* the _r forms: no entry (Mono then takes $HOME) */
static int l_getpw_r(int u, void *pw, char *buf, size_t n, void **res) { (void)u; (void)pw; (void)buf; (void)n; *res = NULL; return 0; }
static int l_getresuid(uint32_t *r, uint32_t *e, uint32_t *s) { *r = *e = *s = 0; return 0; }
static int l_setresuid(uint32_t r, uint32_t e, uint32_t s) { (void)r; (void)e; (void)s; return 0; }

/* readlink: /proc/self/exe is the game's executable; the sets' links are copies */
const char *lx_argv0;

static int l_readlink(const char *path, char *buf, size_t n)
{
    char res[1024];
    size_t l;

    if (strcmp(path, "/proc/self/exe") || !lx_argv0) {
        SET_ERRNO(EINVAL);
        return -1;
    }
    if (lx_argv0[0] == '/') {
        snprintf(res, sizeof(res), "%s", lx_argv0);
    } else {
        char cwd[MAX_PATH];

        l_getcwd(cwd, sizeof(cwd));
        snprintf(res, sizeof(res), "%s/%s", cwd, lx_argv0[0] == '.' && lx_argv0[1] == '/' ? lx_argv0 + 2 : lx_argv0);
    }
    l = strlen(res);
    if (l > n)
        l = n;
    memcpy(buf, res, l);                   /* no terminator, as the kernel's */
    return (int)l;
}

/* glob (glob_t: pathc, pathv, offs, flags): * ? in the last component */
static int glob_match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (; *s; s++)
                if (glob_match(p + 1, s))
                    return 1;
            return glob_match(p + 1, s);
        }
        if (!*s || (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)))
            return 0;
    }
    return !*s;
}

static int l_glob(const char *pat, int flags, void *errf, uint32_t *g)
{
    const char *slash = strrchr(pat, '/');
    char dir[1024], win[MAX_PATH], spec[MAX_PATH];
    char **v = NULL;
    size_t n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h;

    (void)flags;
    (void)errf;
    snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - pat) : 1, slash ? pat : ".");
    memset(g, 0, 16);
    if (!lx_path(dir[0] ? dir : "/", win, sizeof(win))) {
        snprintf(spec, sizeof(spec), "%s\\*", win);
        if ((h = FindFirstFileA(spec, &fd)) != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] != '.' && glob_match(slash ? slash + 1 : pat, fd.cFileName)) {
                    v = realloc(v, (n + 2) * sizeof(char *));
                    v[n] = l_malloc(strlen(dir) + strlen(fd.cFileName) + 2);
                    sprintf(v[n++], "%s/%s", dir, fd.cFileName);
                    v[n] = NULL;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if (!n)
        return 3;                          /* GLOB_NOMATCH */
    g[0] = (uint32_t)n;
    g[1] = (uint32_t)(uintptr_t)v;
    return 0;
}

static void l_globfree(uint32_t *g)
{
    char **v = (char **)(uintptr_t)g[1];
    uint32_t i;

    for (i = 0; v && i < g[0]; i++)
        free(v[i]);
    free(v);
    g[0] = g[1] = 0;
}

/* ftw: nothing walked (the games only clean their own temporary folders) */
static int l_ftw(const char *d, void *fn, int n) { (void)d; (void)fn; (void)n; return 0; }

/* struct sysinfo (i386, 64 bytes), memory in 4 KiB units */
static int l_sysinfo(uint32_t *s)
{
    MEMORYSTATUSEX m;

    memset(s, 0, 64);
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    s[0] = GetTickCount() / 1000;
    s[4] = (uint32_t)(m.ullTotalPhys >> 12);
    s[5] = (uint32_t)(m.ullAvailPhys >> 12);
    s[8] = (uint32_t)(m.ullTotalPageFile >> 12);
    s[9] = (uint32_t)(m.ullAvailPageFile >> 12);
    ((uint16_t *)s)[20] = 1;               /* procs */
    s[13] = 4096;                          /* mem_unit */
    return 0;
}

static int l_truncate(const char *path, long len)
{
    char win[MAX_PATH];
    HANDLE h;
    LARGE_INTEGER at;
    int ok;

    if (lx_path(path, win, sizeof(win)) ||
        (h = CreateFileA(win, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)) ==
            INVALID_HANDLE_VALUE) {
        SET_ERRNO(ENOENT);
        return -1;
    }
    at.QuadPart = len;
    ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok ? 0 : -1;
}

static int l_sched_cpucount(size_t size, const uint8_t *set)
{
    int n = 0;
    size_t i;

    for (i = 0; i < size; i++)
        n += __builtin_popcount(set[i]);
    return n;
}

/* not here: epoll, SysV semaphores, POSIX shared memory, sendfile (Mono falls back) */
static int l_enosys(void) { SET_ERRNO(L_ENOSYS); return -1; }
static int l_zero(void) { return 0; }
static int l_getdtablesize(void) { return 1024; }
static int l_sigrtmin(void) { return 34; }
static int l_sigrtmax(void) { return 64; }

/* struct statvfs64 (i386): bsize, frsize, 6 x u64 counts, fsid, pad, flag, namemax */
static int l_statvfs64(const char *p, uint32_t *s)
{
    uint64_t *c = (uint64_t *)(s + 2);

    (void)p;
    memset(s, 0, 96);
    s[0] = s[1] = 4096;
    c[0] = c[1] = c[2] = 1u << 20;
    c[3] = c[4] = c[5] = 1u << 16;
    s[17] = 255;
    return 0;
}

/* We Dance Online 2's network code: no interfaces here */
static int l_getifaddrs(void **ifa) { *ifa = NULL; SET_ERRNO(L_ENOSYS); return -1; }
static void l_freeifaddrs(void *ifa) { (void)ifa; }

static int l_ftruncate64(int fd, long long len)
{
    int crt = lx_crt_fd(fd);

    if (crt < 0) {
        SET_ERRNO(9);                      /* EBADF */
        return -1;
    }
    return _chsize_s(crt, len) ? -1 : 0;
}
static int l_ftruncate(int fd, long len) { return l_ftruncate64(fd, len); }

/* file times are not kept */
static int l_utimes(const char *path, const void *tv) { (void)path; (void)tv; return 0; }

/* POSIX basename: the last component, trailing slashes cut */
static char *l___xpg_basename(char *p)
{
    char *e, *s;

    if (!p || !*p)
        return ".";
    for (e = p + strlen(p); e > p + 1 && e[-1] == '/'; )
        *--e = 0;
    s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}
static int l_setuid(int u) { (void)u; return 0; }
/* port I/O: only for a machine that emulates it (in/out are trapped, core_win.c) */
static int l_ioperm(unsigned long from, unsigned long n, int on)
{
    (void)from; (void)n; (void)on;
    if (core_ports())
        return 0;
    SET_ERRNO(EPERM);
    return -1;
}

static int l_iopl(int level)
{
    (void)level;
    if (core_ports())
        return 0;
    SET_ERRNO(EPERM);
    return -1;
}
/* Speed Driver 3's odds and ends: no login name, no passwd entries, no
   configuration strings (0: none), no symbolic links on the sets */
static const char *l_getlogin(void) { return NULL; }
static void l_endpwent(void) { }
static size_t l_confstr(int name, char *buf, size_t n) { (void)name; (void)buf; (void)n; SET_ERRNO(EINVAL); return 0; }
static int l_symlink(const char *from, const char *to) { (void)from; (void)to; SET_ERRNO(EPERM); return -1; }

/* fegetround / fesetround: the x87 and SSE rounding modes (glibc FE_* = x87 RC bits) */
static int l_fegetround(void)
{
    unsigned short cw;

    __asm__ volatile ("fnstcw %0" : "=m"(cw));
    return cw & 0xc00;
}

static int l_fesetround(int mode)
{
    unsigned short cw;
    unsigned mx;

    if (mode & ~0xc00)
        return -1;
    __asm__ volatile ("fnstcw %0" : "=m"(cw));
    cw = (cw & ~0xc00) | mode;
    __asm__ volatile ("fldcw %0" : : "m"(cw));
    __asm__ volatile ("stmxcsr %0" : "=m"(mx));
    mx = (mx & ~0x6000u) | (unsigned)mode << 3;
    __asm__ volatile ("ldmxcsr %0" : : "m"(mx));
    return 0;
}

static int l_shm_fail(void) { SET_ERRNO(L_ENOSYS); return -1; }
static void *l_shmat(int id, const void *a, int f) { (void)id; (void)a; (void)f; SET_ERRNO(L_ENOSYS); return (void *)-1; }

static char *l_tmpnam(char *s)
{
    static char buf[64];
    static LONG n;

    if (!s)
        s = buf;
    snprintf(s, 20, "/tmp/igs%05ld", (long)InterlockedIncrement(&n));
    return s;
}
static void l_sync(void) { }
static int l_fsync(int fd) { (void)fd; return 0; }
static int l_getrlimit(int r, uint32_t *rl) { (void)r; rl[0] = rl[1] = 0xffffffff; return 0; }
static int l_setrlimit(int r, const uint32_t *rl) { (void)r; (void)rl; return 0; }
static int l_getrusage(int who, void *ru) { (void)who; memset(ru, 0, 72); return 0; }

/*
 * libpci (pciutils 3): an empty bus.  struct pci_access is zeroed; its
 * device list (offset 44) stays NULL.  Fish Hunter Final 6 reads the class,
 * vendor and device ids of its cabinet's cards into its FPGA key, which the
 * emulated FPGA does not use (the list's mask decides).
 */
static void *l_pci_alloc(void) { return calloc(1, 256); }
static void l_pci_init(void *a) { (void)a; }
static void l_pci_scan_bus(void *a) { (void)a; }
static void l_pci_cleanup(void *a) { free(a); }
static int l_pci_fill_info(void *d, int flags) { (void)d; return flags; }
static uint8_t l_pci_read_byte(void *d, int pos) { (void)d; (void)pos; return 0; }
static char *l_pci_lookup_name(void *a, char *buf, int size, int flags, ...)
{
    va_list ap;
    unsigned v, d;

    (void)a;
    va_start(ap, flags);
    v = va_arg(ap, unsigned);
    d = va_arg(ap, unsigned);
    va_end(ap);
    /* PCI_LOOKUP_DEVICE (2) with no id database: the numeric form */
    snprintf(buf, size, flags & 2 ? "Device %04x" : "Vendor %04x", flags & 2 ? d : v);
    return buf;
}

static int l_net_fail(void) { SET_ERRNO(L_ENOSYS); return -1; }
static void *l_net_null(void) { SET_ERRNO(L_ENOSYS); return NULL; }
static void l_if_freenameindex(void *p) { (void)p; }
static char *l_inet_ntoa(uint32_t a)
{
    static char buf[16];

    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a & 0xff, a >> 8 & 0xff, a >> 16 & 0xff, a >> 24);
    return buf;
}
static uint32_t l_inet_addr(const char *s)
{
    unsigned a, b, c, d;

    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return 0xffffffff;
    return a | b << 8 | c << 16 | (uint32_t)d << 24;
}
static int l_inet_aton(const char *s, uint32_t *a)
{
    uint32_t v = l_inet_addr(s);

    if (v == 0xffffffff && strcmp(s, "255.255.255.255"))
        return 0;
    *a = v;
    return 1;
}
/* getaddrinfo / getnameinfo: EAI_FAIL, there is no network */
static int l_gai_fail(void) { return -4; }
static void l_freeaddrinfo(void *p) { (void)p; }
static const char *l_gai_strerror(int e) { (void)e; return "no network"; }
/* inet_pton: not a valid address (the games only parse their own settings) */
static int l_inet_pton(int af, const char *s, void *d) { (void)af; (void)s; (void)d; return 0; }
static uint16_t l_htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static uint32_t l_htonl(uint32_t v)
{
    return v << 24 | (v & 0xff00) << 8 | (v >> 8 & 0xff00) | v >> 24;
}
/* statvfs (32-bit struct, 72 bytes): plenty of room */
static int l_statvfs(const char *p, uint32_t *s)
{
    (void)p;
    memset(s, 0, 72);
    s[0] = s[1] = 4096;                        /* f_bsize, f_frsize */
    s[2] = s[3] = s[4] = 1u << 20;             /* f_blocks, f_bfree, f_bavail */
    s[5] = s[6] = s[7] = 1u << 16;             /* f_files, f_ffree, f_favail */
    s[10] = 255;                               /* f_namemax */
    return 0;
}
static double l_drand48(void) { return (double)(rand() & 0x7fff) / 32768.0 + (double)(rand() & 0x7fff) / (32768.0 * 32768.0); }
static char *l_gcvt(double v, int n, char *buf) { sprintf(buf, "%.*g", n, v); return buf; }
/* mkstemp: the X's become a counter */
static int l_mkstemp(char *t)
{
    static LONG n;
    size_t len = strlen(t);
    int i, fd;

    if (len < 6 || strcmp(t + len - 6, "XXXXXX")) {
        SET_ERRNO(EINVAL);
        return -1;
    }
    for (i = 0; i < 1000; i++) {
        snprintf(t + len - 6, 7, "%06ld", (long)(InterlockedIncrement(&n) % 1000000));
        if ((fd = l_open(t, 2 | L_O_CREAT | L_O_EXCL, 0600)) >= 0)
            return fd;
    }
    return -1;
}
/* modes are not kept (Go Go Ball 2 chmods its logs) */
static int l_chmod(const char *p, unsigned m) { (void)p; (void)m; return 0; }

/* syscall(): only what libstdc++ uses (futex for static-init guards, gettid) */
static long l_syscall(long n, long a, long b, long c)
{
    switch (n) {
    case 240:                                  /* futex */
        if ((b & 0x7f) == 0) {                 /* FUTEX_WAIT */
            if (*(volatile int *)a == (int)c)
                Sleep(1);
            return 0;
        }
        return 0;                              /* FUTEX_WAKE: waiters poll */
    case 224:                                  /* gettid */
        return GetCurrentThreadId();
    case 20:
        return l_getpid();
    }
    LOG("syscall(%ld): not supported", n);
    SET_ERRNO(L_ENOSYS);
    return -1;
}

/* ---- setjmp / longjmp (jmp_buf: ebx esi edi ebp esp eip, mask_was_saved) ---------------------- */

__asm__(
    ".globl _l__setjmp\n"
    "_l__setjmp:\n"
    "    movl 4(%esp), %eax\n"
    "    movl %ebx, 0(%eax)\n"
    "    movl %esi, 4(%eax)\n"
    "    movl %edi, 8(%eax)\n"
    "    movl %ebp, 12(%eax)\n"
    "    leal 4(%esp), %ecx\n"
    "    movl %ecx, 16(%eax)\n"
    "    movl (%esp), %ecx\n"
    "    movl %ecx, 20(%eax)\n"
    "    movl $0, 24(%eax)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".globl _l_longjmp\n"
    "_l_longjmp:\n"
    "    movl 4(%esp), %edx\n"
    "    movl 8(%esp), %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    incl %eax\n"
    "1:  movl 0(%edx), %ebx\n"
    "    movl 4(%edx), %esi\n"
    "    movl 8(%edx), %edi\n"
    "    movl 12(%edx), %ebp\n"
    "    movl 16(%edx), %esp\n"
    "    jmp *20(%edx)\n");
void l__setjmp(void);
void l_longjmp(void);

const struct nsym sys_syms[] = {
    ND(environ, &lx_environ), ND(__environ, &lx_environ),
    N(open), NA(open64, l_open), NA(__open, l_open), NA(__open64, l_open), N(read), N(write),
    N(close), N(lseek), N(lseek64), NA(__lseek64, l_lseek64), N(pread), N(writev), N(readv),
    N(dup), N(dup2), N(pipe), N(fcntl), N(isatty), N(poll), N(ioctl),
    N(tcgetattr), N(tcsetattr), N(tcflush), N(tcdrain), N(cfsetospeed), N(cfsetispeed),
    N(cfgetospeed), N(cfgetispeed), N(cfmakeraw),
    N(__xstat), N(__xstat64), NA(__lxstat, l___xstat), NA(__lxstat64, l___xstat64),
    N(__fxstat), N(__fxstat64), N(stat), NA(lstat, l_stat), N(fstat), N(access), N(unlink),
    N(mkdir), N(rmdir), N(chdir), N(getcwd), N(realpath),
    N(opendir), N(readdir), N(readdir64), N(readdir_r), N(closedir),
    N(getlogin), N(endpwent), N(confstr), N(symlink), N(fegetround), N(fesetround), N(rewinddir), N(telldir), N(seekdir), N(dirfd),
    N(scandir), N(alphasort),
    N(mmap), N(mmap64), N(munmap), N(mprotect), N(msync), N(madvise), N(mlock), NA(munlock, l_mlock),
    N(time), N(gettimeofday), N(clock_gettime), N(clock_getres), N(clock), N(times), N(ftime),
    N(nanosleep), N(clock_nanosleep), N(usleep), N(sleep), N(gmtime), N(gmtime_r), N(localtime),
    N(localtime_r), N(mktime), N(timegm), N(asctime), N(asctime_r), N(ctime), N(difftime),
    N(settimeofday), N(stime), N(tzset),
    N(signal), NA(sysv_signal, l_signal), NA(bsd_signal, l_signal), N(sigaction), N(sigemptyset),
    N(sigfillset), N(sigaddset), N(sigdelset), N(sigismember), N(sigprocmask), N(raise), N(kill),
    N(alarm), N(pause),
    N(fork), NA(vfork, l_fork), N(execvp), N(execv), NA(execve, l_execv), N(system), N(waitpid),
    N(wait), N(popen), N(pclose), N(getpid), N(getppid), N(getuid), NA(geteuid, l_getuid),
    NA(getgid, l_getuid), NA(getegid, l_getuid), N(setsid), N(umask), N(nice), N(uname),
    N(gethostname), N(sysconf), N(getpagesize), N(sync), N(fsync), NA(fdatasync, l_fsync),
    N(getrlimit), N(setrlimit), N(getrusage), N(get_nprocs), NA(get_nprocs_conf, l_get_nprocs),
    N(setmntent), N(getmntent), N(endmntent), N(getpwnam), N(getpwuid), N(setuid), N(ioperm), N(iopl),
    N(getifaddrs), N(freeifaddrs), N(ftruncate), N(ftruncate64), N(utimes), N(__xpg_basename),
    NA(shmget, l_shm_fail), N(shmat), NA(shmdt, l_shm_fail), NA(shmctl, l_shm_fail), N(tmpnam),
    N(mremap),
    NA(getpwuid_r, l_getpw_r), NA(getpwnam_r, l_getpw_r), NA(getgrgid_r, l_getpw_r), NA(getgrnam_r, l_getpw_r),
    N(getresuid), N(setresuid), N(readlink), N(glob), N(globfree), N(ftw), N(sysinfo), N(truncate),
    NA(__sched_cpucount, l_sched_cpucount), NA(epoll_create, l_enosys), NA(epoll_ctl, l_enosys),
    NA(epoll_wait, l_enosys), NA(semget, l_enosys), NA(semctl, l_enosys), NA(semop, l_enosys),
    NA(shm_open, l_enosys), NA(shm_unlink, l_enosys), NA(ftok, l_enosys), NA(sendfile64, l_enosys),
    NA(sendfile, l_enosys), NA(setitimer, l_zero), NA(sched_setaffinity, l_zero),
    NA(posix_fadvise64, l_zero), NA(posix_fadvise, l_zero), N(statvfs64),
    NA(__libc_current_sigrtmin, l_sigrtmin), NA(__libc_current_sigrtmax, l_sigrtmax),
    NA(__realpath_chk, l_realpath), NA(execl, l_execv), NA(__sysv_signal, l_signal),
    NA(getservbyname, l_net_null), N(getdtablesize),
    /* socket, connect, send, recv...: net.c */
    NA(bind, l_net_fail), NA(listen, l_net_fail), NA(accept, l_net_fail),
    NA(gethostbyname, l_net_null), NA(gethostbyaddr, l_net_null), NA(if_nameindex, l_net_null), N(if_freenameindex), N(inet_ntoa),
    N(inet_addr), N(inet_aton),
    NA(getaddrinfo, l_gai_fail), NA(getnameinfo, l_gai_fail), N(freeaddrinfo), N(gai_strerror),
    NA(socketpair, l_net_fail), NA(sendmsg, l_net_fail), NA(recvmsg, l_net_fail),
    NA(sockatmark, l_net_fail), NA(isfdtype, l_net_fail),
    NA(if_indextoname, l_net_null), NA(inet_ntop, l_net_null), N(inet_pton), N(htons),
    NA(ntohs, l_htons), N(htonl), NA(ntohl, l_htonl), N(statvfs), N(drand48), N(gcvt),
    N(mkstemp), NA(mkstemp64, l_mkstemp),
    N(chmod), N(syscall),
    N(pci_alloc), N(pci_init), N(pci_scan_bus), N(pci_cleanup), N(pci_fill_info), N(pci_read_byte),
    N(pci_lookup_name),
    N(_setjmp), NA(setjmp, l__setjmp), NA(__sigsetjmp, l__setjmp), N(longjmp),
    NA(_longjmp, l_longjmp), NA(siglongjmp, l_longjmp), NA(__longjmp_chk, l_longjmp),
    { NULL, NULL },
};

void sys_init(void)
{
    InitializeCriticalSection(&fd_lock);
    lx_now();
}
