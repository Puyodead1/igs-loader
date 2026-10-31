/*
 * IGS loader, Windows core: shared declarations.
 *
 * The .exe loads the game's i386 Linux ELF binary and its non-system
 * libraries (libstdc++, SDL_mixer, zlib...) itself, and provides the
 * OS-boundary libraries natively: glibc (libc/libm/libpthread/libdl), SDL 1.2,
 * libGL/GLX (thunks to opengl32) and empty stand-ins for X11 helpers.
 * Native functions are named l_<linux name>; Linux structures are declared
 * here with the i386 glibc layout (l_ prefix).  See docs/IGS_LOADER.md.
 */
#ifndef LX_H
#define LX_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- native symbol tables (what ELF modules bind to) ----------------------- */

struct nsym {
    const char *name;
    void *addr;
};

/* N(x): l_x; NF(x): the CRT's x as is; NA(x, f): x is f; ND(x, p): data at p */
#define N(name)        { #name, (void *)l_##name }
#define NF(name)       { #name, (void *)name }
#define NA(name, f)    { #name, (void *)(f) }
#define ND(name, p)    { #name, (void *)(p) }

extern const struct nsym libc_syms[], sys_syms[], thread_syms[], elf_syms[],
    sdl_syms[], gl_syms[];
void libc_init(void);
void sys_init(void);
void thread_init(void);
void elf_reserve_low(void);
int elf_owns(uint32_t addr);
extern char **lx_environ;
void l_exit(int code) __attribute__((noreturn));
void *lx_thread_create(void *fn, void *arg, size_t stack);
void lx_thread_wait(void *t, int *ret);
void lx_thread_kill(void *t);
uint32_t lx_thread_id(void *t);

/* ---- logging and configuration ------------------------------------------------ */

void lx_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) lx_log(__VA_ARGS__)
extern int lx_verbose;     /* IGS_VERBOSE */
extern int lx_lddebug;     /* IGS_LDDEBUG: loader detail */
const char *lx_env(const char *name, const char *def);
long lx_env_long(const char *name, long def);
double lx_now(void);       /* monotonic seconds */
void lx_sleep_until(double t);   /* precise sleep to an lx_now() time */
void lx_usleep(int64_t us);

/* ---- calling ELF code ------------------------------------------------------------ */

/*
 * ELF code is i386 SysV: it may assume a 16-byte aligned stack at calls.
 * Every Windows -> ELF call (callbacks, thread entries, init functions)
 * goes through elf_call, which aligns the stack and copies the arguments.
 */
uint32_t elf_call(void *fn, int nargs, const uint32_t *args);
uint64_t elf_call64(void *fn, int nargs, const uint32_t *args);   /* edx:eax */
static inline uint32_t elf_call0(void *f) { return elf_call(f, 0, NULL); }
static inline uint32_t elf_call1(void *f, uint32_t a) { return elf_call(f, 1, &a); }
static inline uint32_t elf_call2(void *f, uint32_t a, uint32_t b)
{ uint32_t v[2] = { a, b }; return elf_call(f, 2, v); }
static inline uint32_t elf_call3(void *f, uint32_t a, uint32_t b, uint32_t c)
{ uint32_t v[3] = { a, b, c }; return elf_call(f, 3, v); }
static inline uint32_t elf_call4(void *f, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ uint32_t v[4] = { a, b, c, d }; return elf_call(f, 4, v); }
#define U32(p) ((uint32_t)(uintptr_t)(p))

/* ---- the ELF loader (elf.c) ---------------------------------------------------- */

struct elf_mod;
void elf_add_search(const char *path);        /* a dir or one library file */
void elf_set_native(const struct nsym *const *tables);
struct elf_mod *elf_load_exe(const char *path);
int elf_link_all(int check_only);             /* resolve + relocate; missing count */
void elf_run_inits(int argc, char **argv, char **envp);
void elf_start(struct elf_mod *exe, int argc, char **argv, char **envp) __attribute__((noreturn));
void *elf_dlopen(const char *name, int flags);
void *elf_dlsym(void *h, const char *name);
int elf_dlclose(void *h);
const char *elf_dlerror(void);
int elf_iterate_phdr(void *cb, void *data);
void *elf_tls_get_addr(uint32_t *ti);
int elf_describe(uint32_t addr, char *buf, size_t n);   /* "module+0x1234" */
void *native_lookup(const char *name);
void *stub_alloc(size_t n);                   /* executable memory */

/* ---- libc pieces shared between files ------------------------------------------ */

void *l_malloc(size_t n);
void l_free(void *p);
void *l_calloc(size_t n, size_t m);
void *l_realloc(void *p, size_t n);
char *l_strdup(const char *s);
int *l___errno_location(void);
#define SET_ERRNO(e) (*l___errno_location() = (e))
int lx_errno_from_crt(int e);

/* Linux errno values that differ from the CRT's */
#define L_EDEADLK      35
#define L_ENAMETOOLONG 36
#define L_ENOSYS       38
#define L_ENOTEMPTY    39
#define L_ENOTSUP      95
#define L_EAFNOSUPPORT 97
#define L_ETIMEDOUT    110
#define L_EOVERFLOW    75

/* paths: a Linux path to a Windows one (root fs, DATA, /mnt/<d>/); 0 = ok */
int lx_path(const char *in, char *out, size_t n);
int lx_resolve(char *path, size_t n);          /* follow WSL symlinks in place */
void lx_set_dirs(const char *data, const char *rootfs);
const char *lx_data_dir(void);

/* fds: the Linux fd table (libc_sys.c) */
int lx_fd_new_null(void);      /* a /dev/null-like fd (core placeholder) */
int lx_fd_from_crt(int crtfd);

/* stdio FILE stand-in */
typedef struct LFILE {
    uint32_t magic;
    FILE *f;
    int fd;          /* Linux fd, -1 until fileno() asks */
} LFILE;
#define LFILE_MAGIC 0x4c46494cu
LFILE *lfile_wrap(FILE *f);
FILE *lfile_crt(LFILE *lf);

/* ---- core (core_win.c) ------------------------------------------------------------- */

void core_init(void);                         /* load the machine plugin */
int core_route_open(const char *path, int *fd);
int core_ioctl(int fd, unsigned long req, void *arg, int *ret);   /* 1 if handled */
int core_mmap(int fd, size_t len, void **ret);
int core_read(int fd, void *buf, size_t n, long *ret);
int core_write(int fd, const void *buf, size_t n, long *ret);
int core_close(int fd);                       /* 1 if the fd stays reserved */
void core_note_fopen(const char *path);
void core_key(int sym, int down);
void core_frame(void);                        /* overlay + pacing, before a swap */
void core_set_window(void *hwnd);
void core_gl_ready(void);
void core_install_veh(void);

/* ---- SDL / GL glue -------------------------------------------------------------------- */

void *gl_proc(const char *name);              /* cdecl-callable GL entry (thunk) */
void gl_init(void);
int sdl_window_title(const char *s);

/* ---- Linux structures (i386 glibc) ---------------------------------------------- */

#define PACKED __attribute__((packed))

struct l_timespec { int32_t tv_sec, tv_nsec; };
struct l_timeval { int32_t tv_sec, tv_usec; };

struct l_stat {                       /* struct stat, __xstat ver 3: 88 bytes */
    uint64_t st_dev;
    uint16_t __pad1;
    uint16_t __pad1b;
    uint32_t st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev;
    uint16_t __pad2;
    uint16_t __pad2b;
    int32_t st_size, st_blksize, st_blocks;
    struct l_timespec st_atim, st_mtim, st_ctim;
    uint32_t __unused4, __unused5;
} PACKED;

struct l_stat64 {                     /* struct stat64: 96 bytes */
    uint64_t st_dev;
    uint32_t __pad1;
    uint32_t __st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev;
    uint32_t __pad2;
    int64_t st_size;
    int32_t st_blksize;
    int64_t st_blocks;
    struct l_timespec st_atim, st_mtim, st_ctim;
    uint64_t st_ino;
} PACKED;

struct l_dirent {                     /* 268 bytes */
    uint32_t d_ino;
    int32_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
    uint8_t __pad;
} PACKED;

struct l_dirent64 {                   /* 280 bytes */
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
    uint8_t __pad[5];
} PACKED;

struct l_tm {                         /* 44 bytes: glibc has tm_gmtoff / tm_zone */
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    int32_t tm_gmtoff;
    const char *tm_zone;
};

struct l_timeb { int32_t time; uint16_t millitm; int16_t timezone, dstflag; uint16_t pad; } PACKED;

_Static_assert(sizeof(struct l_stat) == 88, "stat");
_Static_assert(sizeof(struct l_stat64) == 96, "stat64");
_Static_assert(offsetof(struct l_stat64, st_size) == 44, "stat64.st_size");
_Static_assert(offsetof(struct l_stat64, st_ino) == 88, "stat64.st_ino");
_Static_assert(sizeof(struct l_dirent) == 268, "dirent");
_Static_assert(offsetof(struct l_dirent64, d_name) == 19, "dirent64");
_Static_assert(sizeof(struct l_tm) == 44, "tm");

/* Linux open flags */
#define L_O_ACCMODE  3
#define L_O_CREAT    0x40
#define L_O_EXCL     0x80
#define L_O_TRUNC    0x200
#define L_O_APPEND   0x400
#define L_O_NONBLOCK 0x800
#define L_O_DIRECTORY 0x10000

#endif
