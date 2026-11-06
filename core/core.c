/*
 * IGS loader core: LD_PRELOAD library for an unmodified 32-bit IGS game.
 *
 * Machine-independent:
 *   - loads the machine plugin ($IGS_MACHINE) and routes its device files
 *     (open/ioctl/mmap/close, read/write and termios for serial ports) to it;
 *   - MMIO traps: PROT_NONE mappings whose accesses are emulated in a
 *     SIGSEGV handler (the games' register accessors are plain movs);
 *   - libc fixes: 32-bit readdir/stat on large inode numbers, an empty OSS
 *     mixer, no system clock changes;
 *   - IP networking: none (IGS_NO_NET), or only to a local stand-in for the
 *     game's server (IGS_NET);
 *   - SDL: key events to the machine, windowed mode, frame pacing, the
 *     overlay (drawn with fixed-function GL before each swap);
 *   - games with SDL linked in statically (E2000): the same through X11
 *     (XNextEvent key events) and GLX (glXSwapBuffers, found via dlsym), and
 *     OSS /dev/dsp played through PulseAudio;
 *   - SDL_mixer: buffer override and logging;
 *   - boards with a frame buffer of their own (E1000): port I/O traps and
 *     iopl, mmap at an offset, lseek on machine files, and a window that
 *     shows the machine's frames (present).
 * See wiki Loader.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "igs.h"
#include "font.h"
#include "tmpcmd.h"

static struct igs_core core;
static const struct igs_machine *machine;

static void core_present(const void *px, int w, int h, int pitch);

typedef void (*sighandler_fn)(int);

#define LOG(...) core_log(__VA_ARGS__)

static void core_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("[igs] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char *core_env(const char *name, const char *def)
{
    const char *v = getenv(name);

    return v && *v ? v : def;
}

static long core_env_long(const char *name, long def)
{
    const char *v = getenv(name);

    return v && *v ? atol(v) : def;
}

static double core_now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

#define REAL(ret, name, args) \
    static ret (*real_##name) args; \
    if (!real_##name) real_##name = (ret (*) args)dlsym(RTLD_NEXT, #name)

static int core_placeholder_fd(void)
{
    REAL(int, open, (const char *, int, ...));
    return real_open("/dev/null", O_RDWR);
}

/* ---- MMIO traps ------------------------------------------------------------ */

#define MMIO_MAX 8
static struct mmio {
    uint8_t *base;
    size_t len;
    void *opaque;
    uint32_t (*read)(void *, uint32_t, int);
    void (*write)(void *, uint32_t, int, uint32_t);
} mmio[MMIO_MAX];
static int nmmio;
static struct sigaction old_segv;

/* i386 gregs index of register number 0..7 (eax ecx edx ebx esp ebp esi edi) */
static const int greg_of[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI };

static uint32_t get_reg(ucontext_t *uc, int r, int size)
{
    if (size == 1)  /* al cl dl bl ah ch dh bh */
        return (uc->uc_mcontext.gregs[greg_of[r & 3]] >> (r & 4 ? 8 : 0)) & 0xff;
    return uc->uc_mcontext.gregs[greg_of[r]] & (size == 2 ? 0xffff : 0xffffffff);
}

static void set_reg(ucontext_t *uc, int r, int size, uint32_t v)
{
    greg_t *g;

    if (size == 1) {
        g = &uc->uc_mcontext.gregs[greg_of[r & 3]];
        if (r & 4)
            *g = (*g & ~0xff00) | ((v & 0xff) << 8);
        else
            *g = (*g & ~0xff) | (v & 0xff);
        return;
    }
    g = &uc->uc_mcontext.gregs[greg_of[r]];
    *g = size == 2 ? (*g & ~0xffff) | (v & 0xffff) : v;
}

/* length of a ModRM memory operand (ModRM byte included) */
static int modrm_len(const uint8_t *p)
{
    int mod = p[0] >> 6, rm = p[0] & 7, len = 1;

    if (rm == 4) {
        len++;
        if (mod == 0 && (p[1] & 7) == 5)
            len += 4;
    } else if (mod == 0 && rm == 5) {
        len += 4;
    }
    if (mod == 1)
        len += 1;
    else if (mod == 2)
        len += 4;
    return len;
}

/*
 * x86 port I/O (in/out with dx or an imm8 port, 8/16/32-bit) for machines
 * with port_in/port_out: without iopl the instruction faults (SIGSEGV).
 */
static int port_access(ucontext_t *uc)
{
    const uint8_t *ip = (const uint8_t *)uc->uc_mcontext.gregs[REG_EIP];
    int n = 0, size = 4, op;
    uint16_t port = uc->uc_mcontext.gregs[REG_EDX] & 0xffff;

    if (!machine || !machine->port_in || !machine->port_out)
        return 0;
    if (ip[0] == 0x66) {
        size = 2;
        n = 1;
    }
    switch (op = ip[n]) {
    case 0xe4: case 0xe5: case 0xe6: case 0xe7:   /* in/out al|eax, imm8 */
        port = ip[n + 1];
        n++;
        /* fall through */
    case 0xec: case 0xed: case 0xee: case 0xef:   /* in/out al|eax, dx */
        if (!(op & 1))
            size = 1;
        if (op & 2)
            machine->port_out(port, size, get_reg(uc, 0, size));
        else
            set_reg(uc, 0, size, machine->port_in(port, size));
        uc->uc_mcontext.gregs[REG_EIP] += n + 1;
        return 1;
    }
    return 0;
}

static void segv_handler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uint8_t *a = si->si_addr;
    const uint8_t *ip;
    struct mmio *m = NULL;
    int size = 4, reg, n, i;
    uint32_t off;

    if (port_access(uc))
        return;
    for (i = 0; i < nmmio; i++)
        if (a >= mmio[i].base && a < mmio[i].base + mmio[i].len)
            m = &mmio[i];
    if (!m) {
        /* not ours: hand over to whatever was installed before */
        LOG("SIGSEGV at %p, eip %p", (void *)a, (void *)uc->uc_mcontext.gregs[REG_EIP]);
        if (old_segv.sa_flags & SA_SIGINFO)
            old_segv.sa_sigaction(sig, si, ctx);
        else if (old_segv.sa_handler != SIG_DFL && old_segv.sa_handler != SIG_IGN)
            old_segv.sa_handler(sig);
        else {
            REAL(sighandler_fn, signal, (int, sighandler_fn));
            real_signal(SIGSEGV, SIG_DFL);
            raise(SIGSEGV);
        }
        return;
    }
    off = a - m->base;
    ip = (const uint8_t *)uc->uc_mcontext.gregs[REG_EIP];
    n = 0;
    if (ip[0] == 0x66) {
        size = 2;
        n = 1;
    }
    switch (ip[n]) {
    case 0x8b:  /* mov r, r/m */
    case 0x8a:
        if (ip[n] == 0x8a)
            size = 1;
        reg = (ip[n + 1] >> 3) & 7;
        set_reg(uc, reg, size, m->read(m->opaque, off, size));
        n += 1 + modrm_len(ip + n + 1);
        break;
    case 0x89:  /* mov r/m, r */
    case 0x88:
        if (ip[n] == 0x88)
            size = 1;
        reg = (ip[n + 1] >> 3) & 7;
        m->write(m->opaque, off, size, get_reg(uc, reg, size));
        n += 1 + modrm_len(ip + n + 1);
        break;
    case 0x0f:  /* movzx r32, r/m8 / r/m16 */
        if (ip[n + 1] != 0xb6 && ip[n + 1] != 0xb7)
            goto unknown;
        size = ip[n + 1] == 0xb6 ? 1 : 2;
        reg = (ip[n + 2] >> 3) & 7;
        set_reg(uc, reg, 4, m->read(m->opaque, off, size));
        n += 2 + modrm_len(ip + n + 2);
        break;
    case 0xc7:  /* mov r/m, imm16/32 */
    case 0xc6: {
        int ml;
        uint32_t imm = 0;

        if (ip[n] == 0xc6)
            size = 1;
        ml = modrm_len(ip + n + 1);
        memcpy(&imm, ip + n + 1 + ml, size);
        m->write(m->opaque, off, size, imm);
        n += 1 + ml + size;
        break;
    }
    default:
    unknown:
        LOG("MMIO access at +0x%x by unsupported instruction %02x %02x %02x %02x at %p",
            off, ip[0], ip[1], ip[2], ip[3], (void *)ip);
        abort();
    }
    uc->uc_mcontext.gregs[REG_EIP] += n;
}

/*
 * Keep the trap handler installed.  SDL 1.2's parachute re-installs whatever
 * SIGSEGV handler it found with signal(), which drops SA_SIGINFO and leaves
 * the trap handler without its context.  While traps exist, the game's own
 * SIGSEGV handler is recorded as the fallback instead of replacing ours.
 */
static int traps_on;                   /* the trap handler is installed */

sighandler_fn signal(int sig, sighandler_fn h)
{
    REAL(sighandler_fn, signal, (int, sighandler_fn));
    sighandler_fn prev;

    if (sig != SIGSEGV || !traps_on)
        return real_signal(sig, h);
    prev = old_segv.sa_flags & SA_SIGINFO ? SIG_DFL : old_segv.sa_handler;
    if (h == (sighandler_fn)segv_handler)
        return prev;              /* our own handler handed back */
    memset(&old_segv, 0, sizeof(old_segv));
    old_segv.sa_handler = h;
    return prev;
}

int sigaction(int sig, const struct sigaction *sa, struct sigaction *old)
{
    REAL(int, sigaction, (int, const struct sigaction *, struct sigaction *));

    if (sig != SIGSEGV || !traps_on)
        return real_sigaction(sig, sa, old);
    if (old)
        *old = old_segv;
    if (sa && sa->sa_sigaction != segv_handler)
        old_segv = *sa;
    return 0;
}

static void traps_install(void)
{
    REAL(int, sigaction, (int, const struct sigaction *, struct sigaction *));
    struct sigaction sa;

    if (traps_on)
        return;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction(SIGSEGV, &sa, &old_segv);
    traps_on = 1;
}

static void *core_mmio_map(size_t len, void *opaque,
                           uint32_t (*read)(void *, uint32_t, int),
                           void (*write)(void *, uint32_t, int, uint32_t))
{
    void *p;

    if (nmmio == MMIO_MAX)
        return MAP_FAILED;
    len = (len + 0xfff) & ~(size_t)0xfff;
    p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return p;
    mmio[nmmio++] = (struct mmio){ p, len, opaque, read, write };
    traps_install();
    return p;
}

/* igs_core.hook: jmp rel32 to fn over the game function's entry */
static int core_hook(uint32_t addr, void *fn)
{
    uintptr_t pg = addr & ~(uintptr_t)0xfff;
    uint8_t *p = (uint8_t *)(uintptr_t)addr;
    int32_t rel = (int32_t)((uintptr_t)fn - (addr + 5));

    if (mprotect((void *)pg, ((addr + 5 - pg) + 0xfff) & ~(uintptr_t)0xfff, PROT_READ | PROT_WRITE | PROT_EXEC))
        return -1;
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    mprotect((void *)pg, ((addr + 5 - pg) + 0xfff) & ~(uintptr_t)0xfff, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + 5);
    return 0;
}

/* igs_core.hook_lib: hooks applied when their library is dlopened */
static struct lib_hook { const char *soname, *sym; void *fn; } lib_hooks[32];
static int nlib_hooks;

static void lib_hooks_apply(void *h, const char *file)
{
    const char *base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
    int i;

    for (i = 0; i < nlib_hooks; i++) {
        void *a;

        if (!strstr(base, lib_hooks[i].soname) || !(a = dlsym(h, lib_hooks[i].sym)))
            continue;
        if (core_hook((uint32_t)(uintptr_t)a, lib_hooks[i].fn))
            core_log("hook %s in %s failed", lib_hooks[i].sym, base);
        else if (core.verbose)
            core_log("hooked %s in %s", lib_hooks[i].sym, base);
    }
}

void *dlopen(const char *file, int mode)
{
    REAL(void *, dlopen, (const char *, int));
    void *h = real_dlopen(file, mode);

    if (h && file && nlib_hooks)
        lib_hooks_apply(h, file);
    return h;
}

static int core_hook_lib(const char *soname, const char *sym, void *fn)
{
    REAL(void *, dlopen, (const char *, int));
    void *h;

    if (nlib_hooks == (int)(sizeof(lib_hooks) / sizeof(lib_hooks[0])))
        return -1;
    lib_hooks[nlib_hooks++] = (struct lib_hook){ soname, sym, fn };
    if ((h = real_dlopen(soname, RTLD_NOW | RTLD_NOLOAD)))
        lib_hooks_apply(h, soname);
    return 0;
}

/* port I/O is the machine's (port_in/port_out): the trap handler takes in/out */
int iopl(int level)
{
    REAL(int, iopl, (int));

    if (machine && machine->port_in)
        return 0;
    return real_iopl(level);
}

int ioperm(unsigned long from, unsigned long num, int on)
{
    REAL(int, ioperm, (unsigned long, unsigned long, int));

    if (machine && machine->port_in)
        return 0;
    return real_ioperm(from, num, on);
}

/* ---- overlay (fixed-function GL) ------------------------------------------ */

/*
 * GL entry points come from SDL_GL_GetProcAddress: the games export data
 * symbols with GL names, so a plain dlsym could find those instead.
 */
#define GL_ALL_ATTRIB_BITS        0x000fffff
#define GL_CLIENT_ALL_ATTRIB_BITS 0xffffffff
#define GL_VIEWPORT               0x0ba2
#define GL_MODELVIEW              0x1700
#define GL_PROJECTION             0x1701
#define GL_TEXTURE_2D             0x0de1
#define GL_DEPTH_TEST             0x0b71
#define GL_LIGHTING               0x0b50
#define GL_FOG                    0x0b60
#define GL_CULL_FACE              0x0b44
#define GL_ALPHA_TEST             0x0bc0
#define GL_STENCIL_TEST           0x0b90
#define GL_SCISSOR_TEST           0x0c11
#define GL_BLEND                  0x0be2
#define GL_SRC_ALPHA              0x0302
#define GL_ONE_MINUS_SRC_ALPHA    0x0303
#define GL_QUADS                  0x0007
#define GL_UNPACK_ALIGNMENT       0x0cf5
#define GL_CURRENT_PROGRAM        0x8b8d
#define GL_ACTIVE_TEXTURE         0x84e0
#define GL_TEXTURE0               0x84c0

static struct {
    int ok;
    void (*PushAttrib)(unsigned), (*PopAttrib)(void);
    void (*PushClientAttrib)(unsigned), (*PopClientAttrib)(void);
    void (*GetIntegerv)(unsigned, int *);
    void (*MatrixMode)(unsigned), (*PushMatrix)(void), (*PopMatrix)(void), (*LoadIdentity)(void);
    void (*Ortho)(double, double, double, double, double, double);
    void (*Enable)(unsigned), (*Disable)(unsigned), (*BlendFunc)(unsigned, unsigned);
    void (*Color4f)(float, float, float, float);
    void (*Begin)(unsigned), (*End)(void), (*Vertex2f)(float, float);
    void (*WindowPos2i)(int, int);
    void (*Bitmap)(int, int, float, float, float, float, const unsigned char *);
    void (*PixelStorei)(unsigned, int);
    void (*UseProgram)(unsigned);
    void (*ActiveTexture)(unsigned);
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void (*Viewport)(int, int, int, int);
} gl;

static int gl_load_from(void *(*gpa)(const char *))
{
#define L(f) if (!(*(void **)&gl.f = gpa ? gpa("gl" #f) : NULL)) return -1
    L(PushAttrib); L(PopAttrib); L(PushClientAttrib); L(PopClientAttrib);
    L(GetIntegerv); L(MatrixMode); L(PushMatrix); L(PopMatrix); L(LoadIdentity);
    L(Ortho); L(Enable); L(Disable); L(BlendFunc); L(Color4f);
    L(Begin); L(End); L(Vertex2f); L(WindowPos2i); L(Bitmap); L(PixelStorei);
    L(UseProgram); L(ActiveTexture); L(ReadPixels); L(Viewport);
#undef L
    return 0;
}

/*
 * SDL's loader first.  SDL linked into the game and not exported, or a
 * game that only uses SDL for audio (its GL comes from Ogre's GLX render
 * system): ask GLX directly.
 */
static int gl_load(void)
{
    if (gl_load_from(dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress")) == 0)
        return 0;
    return gl_load_from(dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"));
}

static void core_fill(int x, int y, int w, int h, float r, float g, float b, float a)
{
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.Color4f(r, g, b, a);
    gl.Begin(GL_QUADS);
    gl.Vertex2f(x, y);
    gl.Vertex2f(x + w, y);
    gl.Vertex2f(x + w, y + h);
    gl.Vertex2f(x, y + h);
    gl.End();
    gl.Disable(GL_BLEND);
}

static void core_text(int x, int y, const char *t, float r, float g, float b)
{
    gl.Color4f(r, g, b, 1);
    gl.WindowPos2i(x, y);
    for (; *t; t++) {
        int c = (unsigned char)*t;

        if (c < 32 || c > 126)
            c = '?';
        gl.Bitmap(16, FONT_H, 0, 0, FONT_W, 0, font_bits[c - 32]);
    }
}

/*
 * IGS_SHOT=<prefix>: write the frame as <prefix><n>.ppm every IGS_SHOT_EVERY
 * frames (default 600, i.e. 10 s): see the game without a screen capture.
 */
static void screenshot(void)
{
    static long frame, every = -1;
    static int n;
    const char *prefix = getenv("IGS_SHOT");
    char path[512];
    unsigned char *px;
    int vp[4], y;
    FILE *f;

    if (!prefix || gl.ok <= 0)
        return;
    if (every < 0)
        every = core_env_long("IGS_SHOT_EVERY", 600);
    if (++frame % (every > 0 ? every : 600))
        return;
    gl.GetIntegerv(GL_VIEWPORT, vp);
    px = malloc((size_t)vp[2] * vp[3] * 3);
    if (!px)
        return;
    gl.PixelStorei(0x0d05, 1);                            /* GL_PACK_ALIGNMENT */
    gl.ReadPixels(0, 0, vp[2], vp[3], 0x1907, 0x1401, px);  /* GL_RGB, UNSIGNED_BYTE */
    snprintf(path, sizeof(path), "%s%d.ppm", prefix, n++);
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "P6\n%d %d\n255\n", vp[2], vp[3]);
        for (y = vp[3] - 1; y >= 0; y--)
            fwrite(px + (size_t)y * vp[2] * 3, 3, vp[2], f);
        fclose(f);
        LOG("frame %ld -> %s", frame, path);
    }
    free(px);
}

/*
 * Origin bottom-left, in viewport pixels; all GL state saved and restored.
 * The viewport is the largest seen: some screens end their frame on a small
 * one (We Dance's login: 480x32).
 */
static void overlay(void)
{
    static int full[4];
    int vp[4], prog = 0, active = 0, i;

    if (!machine || !machine->draw)
        return;
    if (gl.ok == 0)
        gl.ok = gl_load() == 0 ? 1 : -1;
    if (gl.ok < 0)
        return;
    gl.GetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] * vp[3] >= full[2] * full[3])
        memcpy(full, vp, sizeof(full));
    else
        memcpy(vp, full, sizeof(vp));
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    gl.PushAttrib(GL_ALL_ATTRIB_BITS);
    gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
    gl.PushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    gl.UseProgram(0);
    for (i = 3; i >= 0; i--) {
        gl.ActiveTexture(GL_TEXTURE0 + i);
        gl.Disable(GL_TEXTURE_2D);
    }
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_LIGHTING);
    gl.Disable(GL_FOG);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_ALPHA_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.MatrixMode(GL_PROJECTION);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.Ortho(0, vp[2], 0, vp[3], -1, 1);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.LoadIdentity();

    machine->draw(vp[2], vp[3]);
    screenshot();

    gl.MatrixMode(GL_MODELVIEW);
    gl.PopMatrix();
    gl.MatrixMode(GL_PROJECTION);
    gl.PopMatrix();
    gl.PopClientAttrib();
    gl.PopAttrib();
    gl.ActiveTexture(active ? active : GL_TEXTURE0);
    gl.UseProgram(prog);
}

/* ---- start-up: load the machine plugin ------------------------------------- */

__attribute__((constructor)) static void core_init(void)
{
    const char *path = getenv("IGS_MACHINE");
    igs_machine_entry_fn entry;
    void *h;

    core = (struct igs_core){
        .version = IGS_API_VERSION,
        .log = core_log,
        .verbose = getenv("IGS_VERBOSE") != NULL,
        .env = core_env,
        .env_long = core_env_long,
        .now = core_now,
        .placeholder_fd = core_placeholder_fd,
        .mmio_map = core_mmio_map,
        .fill = core_fill,
        .text = core_text,
        .font_w = FONT_W,
        .font_h = FONT_H,
        .present = core_present,
        .hook = core_hook,
        .hook_lib = core_hook_lib,
    };
    /* the games' own diagnostics go to stdout: keep them in order with ours */
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!path)
        return;
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    entry = h ? (igs_machine_entry_fn)dlsym(h, IGS_MACHINE_ENTRY) : NULL;
    machine = entry ? entry(&core) : NULL;
    if (!machine || machine->version != IGS_API_VERSION) {
        LOG("cannot load machine %s: %s", path, h ? "bad plugin" : dlerror());
        machine = NULL;
        return;
    }
    LOG("machine %s", machine->name);
    if (machine->port_in)
        traps_install();
}

/* ---- device files ------------------------------------------------------------ */

#define FD_MAX 16
static int machine_fd[FD_MAX], nmachine_fd;
static int mixer_fd = -1;

/*
 * OSS /dev/dsp for games with SDL's OSS driver built in.  The game writes to
 * a pipe; a thread plays it through PulseAudio (libpulse-simple), which also
 * paces the writes.  Only the ioctls SDL 1.2's "dsp" driver uses.
 */
static struct {
    int fd, rd;                     /* the game's write end, our read end */
    int rate, channels, fmt;        /* fmt: OSS AFMT_U8 (8) or AFMT_S16_LE (16) */
    int fragsize, nfrags;
} dsp = { -1, -1, 8000, 1, 8, 4096, 4 };

struct pa_spec { int format; uint32_t rate; uint8_t channels; };
struct pa_attr { uint32_t maxlength, tlength, prebuf, minreq, fragsize; };

static void *dsp_thread(void *arg)
{
    void *(*pa_new)(const char *, const char *, int, const char *, const char *,
                    const struct pa_spec *, const void *, const struct pa_attr *, int *);
    int (*pa_write)(void *, const void *, size_t, int *);
    void (*pa_free)(void *);
    void *lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL), *pa = NULL;
    int rd = (int)(intptr_t)arg, err, bps = 0;
    char buf[4096];
    ssize_t n;

    *(void **)&pa_new = lib ? dlsym(lib, "pa_simple_new") : NULL;
    *(void **)&pa_write = lib ? dlsym(lib, "pa_simple_write") : NULL;
    *(void **)&pa_free = lib ? dlsym(lib, "pa_simple_free") : NULL;
    while ((n = read(rd, buf, sizeof(buf))) > 0) {
        /* IGS_NO_AUDIO=1 (test runs): no stream */
        if (!pa && !bps && pa_new && !getenv("IGS_NO_AUDIO")) {
            /* the ioctls are done by the first write: open the stream now */
            struct pa_spec spec = { dsp.fmt == 16 ? 3 : 0, dsp.rate, dsp.channels };
            struct pa_attr attr = { (uint32_t)-1, 0, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1 };

            bps = dsp.rate * dsp.channels * (dsp.fmt == 16 ? 2 : 1);
            attr.tlength = bps * core_env_long("IGS_DSP_LATENCY_MS", 60) / 1000;
            pa = pa_new(NULL, "IGS", 1, NULL, "game", &spec, NULL, &attr, &err);
            LOG("dsp: %d Hz, %d ch, %d bit -> PulseAudio %s", dsp.rate, dsp.channels,
                dsp.fmt == 16 ? 16 : 8, pa ? "ok" : "failed");
        }
        if (pa) {
            pa_write(pa, buf, n, &err);
        } else {
            /* no PulseAudio: drain at the real rate so the game keeps time */
            if (!bps)
                bps = dsp.rate * dsp.channels * (dsp.fmt == 16 ? 2 : 1);
            usleep(n * 1000000LL / (bps ? bps : 8000));
        }
    }
    if (pa)
        pa_free(pa);
    REAL(int, close, (int));
    real_close(rd);
    return NULL;
}

static int dsp_open(void)
{
    pthread_t t;
    int p[2];

    if (pipe(p))
        return -1;
    fcntl(p[1], F_SETPIPE_SZ, 4096);
    dsp.fd = p[1];
    dsp.rd = p[0];
    pthread_create(&t, NULL, dsp_thread, (void *)(intptr_t)p[0]);
    pthread_detach(t);
    return p[1];
}

static int dsp_ioctl(unsigned long req, int *arg)
{
    switch (req) {
    case 0xC0045002: dsp.rate = *arg; break;                         /* SPEED */
    case 0xC0045003: dsp.channels = *arg ? 2 : 1; break;             /* STEREO */
    case 0xC0045006: dsp.channels = *arg >= 2 ? 2 : 1; *arg = dsp.channels; break;
    case 0xC0045005:                                                 /* SETFMT */
        if (*arg != 8 && *arg != 16)
            *arg = 16;
        dsp.fmt = *arg;
        break;
    case 0x8004500B: *arg = 8 | 16; break;                           /* GETFMTS */
    case 0xC004500A:                                                 /* SETFRAGMENT */
        dsp.fragsize = 1 << (*arg & 0xffff);
        dsp.nfrags = (*arg >> 16) & 0x7fff;
        break;
    case 0xC0045004: *arg = dsp.fragsize; break;                     /* GETBLKSIZE */
    case 0x8010500C:                                                 /* GETOSPACE */
        arg[0] = arg[1] = dsp.nfrags;
        arg[2] = dsp.fragsize;
        arg[3] = dsp.nfrags * dsp.fragsize;
        break;
    case 0x8004500F: *arg = 0; break;                                /* GETCAPS */
    default: break;                                                  /* RESET, SYNC, POST, ... */
    }
    return 0;
}

static int is_machine_fd(int fd)
{
    int i;

    for (i = 0; fd >= 0 && i < nmachine_fd; i++)
        if (machine_fd[i] == fd)
            return 1;
    return 0;
}

static int null_fd[FD_MAX], nnull_fd;

static int is_null_fd(int fd)
{
    int i;

    for (i = 0; fd >= 0 && i < nnull_fd; i++)
        if (null_fd[i] == fd)
            return 1;
    return 0;
}

/* is path one of the space-separated $IGS_NULL_DEVS? */
static int null_dev(const char *path)
{
    const char *list = getenv("IGS_NULL_DEVS");
    size_t n = strlen(path);

    while (list && *list) {
        const char *end = strchr(list, ' ');
        size_t len = end ? (size_t)(end - list) : strlen(list);

        if (len == n && !strncmp(list, path, n))
            return 1;
        list = end ? end + 1 : NULL;
    }
    return 0;
}

/*
 * IGS_SET_DIRS: space-separated cabinet directories outside DATA that the
 * game opens by absolute path (e.g. Speed Driver 2's /data); they resolve
 * inside the set ($IGS_SET), as on Windows.  /tmp is an overlay, since the
 * host's libraries use it too (Xauthority): a path resolves inside the set
 * when it exists there or is being created (create).
 */
static const char *set_path_c(const char *path, char *buf, size_t size, int create)
{
    const char *list = getenv("IGS_SET_DIRS"), *set = getenv("IGS_SET");

    if (!path || path[0] != '/' || !set)
        return path;
    while (list && *list) {
        const char *end = strchr(list, ' ');
        size_t len = end ? (size_t)(end - list) : strlen(list);

        if (len && !strncmp(path, list, len) && (path[len] == '/' || !path[len])) {
            struct stat64 st;

            snprintf(buf, size, "%s%s", set, path);
            if (!create && len == 4 && !strncmp(list, "/tmp", 4) && lstat64(buf, &st))
                return path;
            return buf;
        }
        list = end ? end + 1 : NULL;
    }
    return path;
}

static const char *set_path(const char *path, char *buf, size_t size)
{
    return set_path_c(path, buf, size, 0);
}

static int route_open(const char *path, int *fd)
{
    if (!path)
        return 0;
    if (machine && machine->handles(path)) {
        *fd = machine->open(path);
        if (*fd >= 0 && !is_machine_fd(*fd) && nmachine_fd < FD_MAX)
            machine_fd[nmachine_fd++] = *fd;
        return 1;
    }
    /*
     * OSS /dev/mixer: the games list the channels at start-up and set each
     * to 100%, and exit if the device cannot be opened.  An empty mixer will do.
     */
    if (!strcmp(path, "/dev/mixer")) {
        *fd = mixer_fd = core_placeholder_fd();
        return 1;
    }
    if (!strcmp(path, "/dev/dsp")) {
        *fd = dsp_open();
        return 1;
    }
    /*
     * IGS_NULL_DEVS: optional peripherals (lamp boards...).  Writes are
     * dropped and reads fail with EAGAIN, as from a port with nothing on it
     * (an EOF would make the games' read loops spin forever).
     */
    if (null_dev(path)) {
        *fd = core_placeholder_fd();
        if (*fd >= 0 && nnull_fd < FD_MAX)
            null_fd[nnull_fd++] = *fd;
        return 1;
    }
    return 0;
}

int open(const char *path, int flags, ...)
{
    REAL(int, open, (const char *, int, ...));
    va_list ap;
    int mode, fd;
    char buf[512];

    if (route_open(path, &fd))
        return fd;
    path = set_path_c(path, buf, sizeof(buf), (flags & O_CREAT) != 0);
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    REAL(int, open64, (const char *, int, ...));
    va_list ap;
    int mode, fd;
    char buf[512];

    if (route_open(path, &fd))
        return fd;
    path = set_path_c(path, buf, sizeof(buf), (flags & O_CREAT) != 0);
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    return real_open64(path, flags, mode);
}

int ioctl(int fd, unsigned long req, ...)
{
    REAL(int, ioctl, (int, unsigned long, void *));
    va_list ap;
    void *arg;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (is_machine_fd(fd))
        return machine->ioctl(fd, req, arg);
    if (fd >= 0 && fd == dsp.fd && ((req >> 8) & 0xff) == 'P')
        return arg ? dsp_ioctl(req, arg) : 0;
    if (fd >= 0 && fd == mixer_fd && ((req >> 8) & 0xff) == 'M') {
        if ((req & 0x80000000u) && arg)  /* reads: no channels, volume 0 */
            *(int *)arg = 0;
        return 0;
    }
    return real_ioctl(fd, req, arg);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    REAL(void *, mmap, (void *, size_t, int, int, int, off_t));

    if (is_machine_fd(fd))
        return machine->mmap_at ? machine->mmap_at(fd, len, (uint32_t)off) : machine->mmap(fd, len);
    return real_mmap(addr, len, prot, flags, fd, off);
}

off_t lseek(int fd, off_t off, int whence)
{
    REAL(off_t, lseek, (int, off_t, int));

    if (nmachine_fd && machine->seek && is_machine_fd(fd))
        return machine->seek(fd, off, whence);
    return real_lseek(fd, off, whence);
}

ssize_t read(int fd, void *buf, size_t n)
{
    REAL(ssize_t, read, (int, void *, size_t));

    if (nnull_fd && is_null_fd(fd)) {
        errno = EAGAIN;
        return -1;
    }
    if (nmachine_fd && machine->read && is_machine_fd(fd))
        return machine->read(fd, buf, n);
    return real_read(fd, buf, n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
    REAL(ssize_t, write, (int, const void *, size_t));

    if (nmachine_fd && machine->write && is_machine_fd(fd))
        return machine->write(fd, buf, n);
    return real_write(fd, buf, n);
}

/*
 * termios on the machine's serial ports.  glibc issues these ioctls
 * internally, past the ioctl() above, so hand them over here: TCGETS
 * 0x5401, TCSETS 0x5402 + optional_actions, TCFLSH 0x540B.
 */
int tcgetattr(int fd, struct termios *t)
{
    REAL(int, tcgetattr, (int, struct termios *));

    if (is_machine_fd(fd))
        return machine->ioctl(fd, 0x5401, t);
    return real_tcgetattr(fd, t);
}

int tcsetattr(int fd, int act, const struct termios *t)
{
    REAL(int, tcsetattr, (int, int, const struct termios *));

    if (is_machine_fd(fd))
        return machine->ioctl(fd, 0x5402 + act, (void *)t);
    return real_tcsetattr(fd, act, t);
}

int tcflush(int fd, int queue)
{
    REAL(int, tcflush, (int, int));

    if (is_machine_fd(fd))
        return machine->ioctl(fd, 0x540B, (void *)(intptr_t)queue);
    return real_tcflush(fd, queue);
}

int close(int fd)
{
    REAL(int, close, (int));
    int i;

    for (i = 0; i < nnull_fd; i++)
        if (null_fd[i] == fd)
            null_fd[i] = null_fd[--nnull_fd];
    if (is_machine_fd(fd)) {
        machine->close(fd);  /* the fd stays reserved for the device */
        return 0;
    }
    if (fd >= 0 && fd == mixer_fd)
        mixer_fd = -1;
    if (fd >= 0 && fd == dsp.fd)
        dsp.fd = -1;   /* the thread sees EOF and stops */
    return real_close(fd);
}

/* data files the machine wants to know about (e.g. which stage is loading) */
static void note_fopen(const char *path, void *f)
{
    if (f && path && machine && machine->file_opened)
        machine->file_opened(path);
}

/* a file the machine provides (igs_machine.vfile), as a memory stream */
static FILE *vfile_open(const char *name)
{
    const char *txt = name && machine && machine->vfile ? machine->vfile(name) : NULL;
    FILE *f;

    if (!txt || !(f = fmemopen(NULL, strlen(txt) + 1, "w+")))
        return NULL;
    fputs(txt, f);
    rewind(f);
    if (core.verbose)
        LOG("%s: provided by the machine", name);
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    REAL(FILE *, fopen, (const char *, const char *));
    char buf[512];
    int w = mode && strpbrk(mode, "wa+") != NULL;
    FILE *f = w ? NULL : vfile_open(path);

    if (f)
        return f;
    f = real_fopen(set_path_c(path, buf, sizeof(buf), w), mode);
    note_fopen(path, f);
    return f;
}

FILE *fopen64(const char *path, const char *mode)
{
    REAL(FILE *, fopen64, (const char *, const char *));
    char buf[512];
    int w = mode && strpbrk(mode, "wa+") != NULL;
    FILE *f = w ? NULL : vfile_open(path);

    if (f)
        return f;
    f = real_fopen64(set_path_c(path, buf, sizeof(buf), w), mode);
    note_fopen(path, f);
    return f;
}

DIR *opendir(const char *path)
{
    REAL(DIR *, opendir, (const char *));
    char buf[512];

    return real_opendir(set_path(path, buf, sizeof(buf)));
}

int remove(const char *path)
{
    REAL(int, remove, (const char *));
    char buf[512];

    return real_remove(set_path(path, buf, sizeof(buf)));
}

int chmod(const char *path, mode_t mode)
{
    REAL(int, chmod, (const char *, mode_t));
    char buf[512];

    return real_chmod(set_path(path, buf, sizeof(buf)), mode);
}

/*
 * IGS_NET=host[:port] (port 10000): the game's own server, e.g. igs-online.
 * Outgoing TCP to the ports in IGS_NET_PORTS (default 10000, the game
 * server's port in We Dance's info/ip) goes to host:port whatever address
 * the game asked for; every other IP connection, name lookup and datagram
 * is refused, so nothing reaches a real IGS server.  Datagram sockets may
 * be made (the games ioctl their interfaces on one), raw ones not.  Takes
 * precedence over IGS_NO_NET.
 */
static struct {
    int on;
    struct sockaddr_in to;
    int ports[8], nports;
} net;

static void net_init(void)
{
    static int done;
    const char *v = getenv("IGS_NET"), *p;
    char host[256];
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
    REAL(int, getaddrinfo, (const char *, const char *, const struct addrinfo *, struct addrinfo **));
    int port = 10000;
    size_t n;

    if (done++ || !v || !*v)
        return;
    p = strrchr(v, ':');
    n = p ? (size_t)(p - v) : strlen(v);
    if (n >= sizeof(host))
        n = sizeof(host) - 1;
    memcpy(host, v, n);
    host[n] = 0;
    if (p)
        port = atoi(p + 1);
    if (real_getaddrinfo(host, NULL, &hints, &ai)) {
        LOG("IGS_NET=%s: cannot resolve %s; no network", v, host);
        net.on = -1;
        return;
    }
    net.to = *(struct sockaddr_in *)ai->ai_addr;
    net.to.sin_port = htons((uint16_t)port);
    freeaddrinfo(ai);
    for (p = core_env("IGS_NET_PORTS", "10000"); *p && net.nports < 8; ) {
        net.ports[net.nports++] = atoi(p);
        p += strcspn(p, ",");
        p += *p == ',';
    }
    net.on = 1;
    LOG("IGS_NET: game server %s:%d (ports %s)", inet_ntoa(net.to.sin_addr), port,
        core_env("IGS_NET_PORTS", "10000"));
}

/* 1: connect to the server instead, 0: AF_UNIX (as is), -1: refused */
static int net_route(const struct sockaddr *a)
{
    int i, port;

    if (!a || a->sa_family == AF_UNIX)
        return 0;
    if (a->sa_family != AF_INET || net.on != 1)
        return -1;
    port = ntohs(((const struct sockaddr_in *)a)->sin_port);
    for (i = 0; i < net.nports; i++)
        if (port == net.ports[i])
            return 1;
    return -1;
}

int connect(int fd, const struct sockaddr *a, socklen_t len)
{
    REAL(int, connect, (int, const struct sockaddr *, socklen_t));
    char to[32];

    net_init();
    if (!net.on)
        return real_connect(fd, a, len);
    switch (net_route(a)) {
    case 0:
        return real_connect(fd, a, len);
    case 1:
        snprintf(to, sizeof(to), "%s:%d", inet_ntoa(((const struct sockaddr_in *)a)->sin_addr),
                 ntohs(((const struct sockaddr_in *)a)->sin_port));
        LOG("connect %s -> IGS_NET", to);
        return real_connect(fd, (const struct sockaddr *)&net.to, sizeof(net.to));
    default:
        if (a && a->sa_family == AF_INET)
            LOG("connect %s:%d refused (IGS_NET)", inet_ntoa(((const struct sockaddr_in *)a)->sin_addr),
                ntohs(((const struct sockaddr_in *)a)->sin_port));
        errno = ECONNREFUSED;
        return -1;
    }
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *a, socklen_t len)
{
    REAL(ssize_t, sendto, (int, const void *, size_t, int, const struct sockaddr *, socklen_t));

    net_init();
    if (net.on && a && a->sa_family != AF_UNIX) {
        errno = EACCES;
        return -1;
    }
    return real_sendto(fd, buf, n, flags, a, len);
}

ssize_t sendmsg(int fd, const struct msghdr *m, int flags)
{
    REAL(ssize_t, sendmsg, (int, const struct msghdr *, int));

    net_init();
    if (net.on && m && m->msg_name && ((const struct sockaddr *)m->msg_name)->sa_family != AF_UNIX) {
        errno = EACCES;
        return -1;
    }
    return real_sendmsg(fd, m, flags);
}

/*
 * IGS_NO_NET=1: no IP networking for the game, as on Windows (We Dance
 * Online 2 logs in to its servers, fetches web files and pings).  Unix
 * sockets stay: X11, Wayland and PulseAudio use them.
 */
int socket(int domain, int type, int proto)
{
    REAL(int, socket, (int, int, int));

    net_init();
    if (net.on && domain != AF_UNIX) {
        if (net.on == 1 && domain == AF_INET && ((type & 0xf) == SOCK_STREAM || (type & 0xf) == SOCK_DGRAM))
            return real_socket(domain, type, proto);
        errno = EACCES;
        return -1;
    }
    if (getenv("IGS_NO_NET") && domain != AF_UNIX) {
        static int logged;

        if (!logged++)
            LOG("socket(%d, %d, %d): no network (IGS_NO_NET; logged once)", domain, type, proto);
        errno = EACCES;
        return -1;
    }
    return real_socket(domain, type, proto);
}

struct hostent *gethostbyname(const char *name)
{
    REAL(struct hostent *, gethostbyname, (const char *));

    net_init();
    if (net.on || getenv("IGS_NO_NET")) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    return real_gethostbyname(name);
}

int getaddrinfo(const char *node, const char *svc, const struct addrinfo *hints, struct addrinfo **res)
{
    REAL(int, getaddrinfo, (const char *, const char *, const struct addrinfo *, struct addrinfo **));

    net_init();
    if (net.on || getenv("IGS_NO_NET"))
        return EAI_FAIL;
    return real_getaddrinfo(node, svc, hints, res);
}

static int tmp_rm_one(const char *p, const struct stat64 *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    return flag == FTW_DP ? rmdir(p) : unlink(p);
}

static void tmp_rm(const char *p)
{
    struct stat64 st;

    if (lstat64(p, &st))
        return;
    if (S_ISDIR(st.st_mode))
        nftw64(p, tmp_rm_one, 16, FTW_DEPTH | FTW_PHYS);
    else
        unlink(p);
}

/*
 * cp / rm into the game's /tmp (core/tmpcmd.h), which must resolve inside
 * the set (IGS_SET_DIRS has /tmp): the host's /tmp is never touched.  A
 * copy is a symlink to its source.
 */
static int tmp_cmd(char **argv, int argc)
{
    char tmp[512], dst[512], src[512], abs[PATH_MAX], to[1024], *name, *e;
    struct stat64 st;
    glob64_t g;
    int i, dir, ret = 0;
    size_t k;

    if (set_path_c("/tmp", tmp, sizeof(tmp), 1) != tmp)
        return -1;
    mkdir(tmp, 0755);
    for (i = 1; i < argc - (argv[0][0] == 'c'); i++) {
        const char *s = argv[i][0] == '/' ? set_path_c(argv[i], src, sizeof(src), 1) : argv[i];

        if (glob64(s, 0, NULL, &g))
            continue;
        for (k = 0; k < g.gl_pathc; k++) {
            if (argv[0][0] == 'r') {
                tmp_rm(g.gl_pathv[k]);
                continue;
            }
            if (!realpath(g.gl_pathv[k], abs)) {
                ret = -1;
                continue;
            }
            set_path_c(argv[argc - 1], dst, sizeof(dst), 1);
            dir = (stat64(dst, &st) == 0 && S_ISDIR(st.st_mode)) || dst[strlen(dst) - 1] == '/';
            for (e = dst + strlen(dst); e > dst + 1 && e[-1] == '/'; )
                *--e = 0;
            name = strrchr(abs, '/');
            snprintf(to, sizeof(to), "%s%s", dst, dir ? name : "");
            tmp_rm(to);
            if (symlink(abs, to))
                ret = -1;
        }
        globfree64(&g);
    }
    return ret;
}

/*
 * IGS_NO_SYSTEM=1: the game's shell commands configure the cabinet (Speed
 * Driver 2: ifconfig, route, pppoe, hwclock): not run, as on Windows.
 * popen fails, which such games take as "no network".  cp and rm into /tmp
 * run in-process (tmp_cmd).
 */
/*
 * Commands that configure the cabinet's system never run on the host, with
 * or without IGS_NO_SYSTEM: the clock after a server's time (Percussion
 * Master 3: "hwclock --systohc"), the network (fixed-IP setting: killall
 * dhclient, ifconfig, route), the disk (hdparm), power.
 */
static int host_cmd(const char *cmd)
{
    static const char *const deny[] = {
        "hwclock", "date", "ifconfig", "route", "killall", "dhclient", "hdparm", "ntpdate",
        "reboot", "shutdown", "halt", "poweroff", "init", "ip", "pppd", "pppoe",
    };
    size_t i, n;

    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    for (i = 0; i < sizeof(deny) / sizeof(deny[0]); i++) {
        n = strlen(deny[i]);
        if (!strncmp(cmd, deny[i], n) && (!cmd[n] || cmd[n] == ' ' || cmd[n] == '\t' || cmd[n] == ';'))
            return 1;
        if (cmd[0] == '/' && strstr(cmd, deny[i]) == strrchr(cmd, '/') + 1)   /* /sbin/hwclock ... */
            return 1;
    }
    return 0;
}

int system(const char *cmd)
{
    REAL(int, system, (const char *));
    static int logged;
    char buf[1024], *argv[TMPCMD_MAX];
    int argc;

    if (cmd && host_cmd(cmd)) {
        LOG("system(\"%s\"): not run (configures the host)", cmd);
        return 0;
    }
    if (getenv("IGS_NO_SYSTEM") && cmd) {
        if ((argc = tmpcmd_parse(cmd, buf, sizeof(buf), argv))) {
            if (core.verbose)
                LOG("system(\"%s\"): in-process", cmd);
            return tmp_cmd(argv, argc) ? 256 : 0;
        }
        if (logged++ < 8)
            LOG("system(\"%s\"): not run%s", cmd, logged == 8 ? " (no more logged)" : "");
        return 0;
    }
    return real_system(cmd);
}

static FILE *vpipe[8];                 /* popen streams the machine provided */
static int nvpipe;

FILE *popen(const char *cmd, const char *mode)
{
    REAL(FILE *, popen, (const char *, const char *));

    if (getenv("IGS_NO_SYSTEM")) {
        char name[512];
        FILE *f;

        snprintf(name, sizeof(name), "|%s", cmd ? cmd : "");
        if (mode && mode[0] == 'r' && nvpipe < (int)(sizeof(vpipe) / sizeof(vpipe[0])) &&
            (f = vfile_open(name))) {
            vpipe[nvpipe++] = f;
            return f;
        }
        LOG("popen(\"%s\"): not run", cmd);
        errno = ENOENT;
        return NULL;
    }
    return real_popen(cmd, mode);
}

int pclose(FILE *f)
{
    REAL(int, pclose, (FILE *));
    int i;

    for (i = 0; i < nvpipe; i++)
        if (vpipe[i] == f) {
            vpipe[i] = vpipe[--nvpipe];
            fclose(f);
            return 0;
        }
    return real_pclose(f);
}

/*
 * The games set the system clock from the operator menu or a server's login
 * reply (We Dance: settimeofday, then system("hwclock -w")): not on a desktop.
 */
int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tv;
    (void)tz;
    return 0;
}

int stime(const time_t *t)
{
    (void)t;
    return 0;
}

/*
 * The wall clock never runs backwards for the game.  WSL steps it when it
 * resyncs with the host; Ogre 1.4's frame time is an unsigned difference of
 * gettimeofday() readings, so a step back becomes a frame of about 4.3
 * million seconds, and its texture-animation controllers then spin forever
 * subtracting 1.0 from a float that large (Speed Rider).
 */
int gettimeofday(struct timeval *tv, void *tz)
{
    REAL(int, gettimeofday, (struct timeval *, void *));
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static long long last;
    static int logged;
    long long us;
    int r = real_gettimeofday(tv, tz);

    if (r)
        return r;
    us = (long long)tv->tv_sec * 1000000 + tv->tv_usec;
    pthread_mutex_lock(&lock);
    if (us < last) {
        if (!logged++)
            LOG("clock stepped back %lld us; held (logged once)", last - us);
        us = last;
        tv->tv_sec = (time_t)(us / 1000000);
        tv->tv_usec = (suseconds_t)(us % 1000000);
    }
    last = us;
    pthread_mutex_unlock(&lock);
    return r;
}

/*
 * 32-bit directory and stat calls.  The games use the old non-LFS readdir()
 * and __xstat(); on WSL's Windows-drive mounts inode numbers exceed 32 bits,
 * so those fail with EOVERFLOW (Ocean Party's file index came up empty).
 * Serve them from the 64-bit calls with the inode number truncated.
 */
struct dirent *readdir(DIR *dir)
{
    REAL(struct dirent64 *, readdir64, (DIR *));
    static __thread struct dirent out;
    struct dirent64 *e = real_readdir64(dir);

    if (!e)
        return NULL;
    out.d_ino = (__ino_t)e->d_ino;
    out.d_off = (__off_t)e->d_off;
    out.d_reclen = sizeof(out);
    out.d_type = e->d_type;
    snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
    return &out;
}

static int stat_from64(const struct stat64 *s64, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_dev = s64->st_dev;
    st->st_ino = (__ino_t)s64->st_ino;
    st->st_mode = s64->st_mode;
    st->st_nlink = s64->st_nlink;
    st->st_uid = s64->st_uid;
    st->st_gid = s64->st_gid;
    st->st_rdev = s64->st_rdev;
    if (s64->st_size > 0x7fffffff) {
        errno = EOVERFLOW;
        return -1;
    }
    st->st_size = s64->st_size;
    st->st_blksize = s64->st_blksize;
    st->st_blocks = s64->st_blocks;
    st->st_atim = s64->st_atim;
    st->st_mtim = s64->st_mtim;
    st->st_ctim = s64->st_ctim;
    return 0;
}

int __xstat(int ver, const char *path, struct stat *st)
{
    struct stat64 s64;
    char buf[512];

    (void)ver;
    return stat64(set_path(path, buf, sizeof(buf)), &s64) ? -1 : stat_from64(&s64, st);
}

int __lxstat(int ver, const char *path, struct stat *st)
{
    struct stat64 s64;
    char buf[512];

    (void)ver;
    return lstat64(set_path(path, buf, sizeof(buf)), &s64) ? -1 : stat_from64(&s64, st);
}

int __fxstat(int ver, int fd, struct stat *st)
{
    struct stat64 s64;

    (void)ver;
    return fstat64(fd, &s64) ? -1 : stat_from64(&s64, st);
}

/*
 * The same for host libraries built against glibc 2.33+, which call the
 * plain symbols: SDL2's SDL_RWFromFile fstat()s the file it opened and
 * refused every WAV of Panda Family's ("not a regular file").
 */
int stat(const char *path, struct stat *st)
{
    struct stat64 s64;

    return stat64(path, &s64) ? -1 : stat_from64(&s64, st);
}

int lstat(const char *path, struct stat *st)
{
    struct stat64 s64;

    return lstat64(path, &s64) ? -1 : stat_from64(&s64, st);
}

int fstat(int fd, struct stat *st)
{
    struct stat64 s64;

    return fstat64(fd, &s64) ? -1 : stat_from64(&s64, st);
}

/* ---- SDL --------------------------------------------------------------------- */

/*
 * SDL 1.2 SDL_KeyboardEvent: Uint8 type, which, state, then SDL_keysym
 * {Uint8 scancode; SDLKey sym; ...}.  SDL_keysym holds ints, so it is
 * 4-aligned (offset 4) and sym sits at offset 8.
 */
int SDL_PollEvent(void *ev)
{
    REAL(int, SDL_PollEvent, (void *));
    int r = real_SDL_PollEvent(ev);

    if (r && ev && machine && machine->key) {
        uint8_t type = *(uint8_t *)ev;

        if (type == 2 || type == 3) {  /* SDL_KEYDOWN, SDL_KEYUP */
            int sym;

            memcpy(&sym, (uint8_t *)ev + 8, 4);
            machine->key(sym, type == 2);
        }
    }
    return r;
}

static int sdl_video;                  /* the game set its video mode through SDL */

/* windowed by default; IGS_FULLSCREEN=1 keeps the game's fullscreen request */
void *SDL_SetVideoMode(int w, int h, int bpp, uint32_t flags)
{
    REAL(void *, SDL_SetVideoMode, (int, int, int, uint32_t));

    if (!getenv("IGS_FULLSCREEN"))
        flags &= ~0x80000000u;  /* SDL_FULLSCREEN */
    LOG("video mode %dx%d bpp %d flags 0x%x", w, h, bpp, flags);
    sdl_video = 1;
    return real_SDL_SetVideoMode(w, h, bpp, flags);
}

/*
 * Frame pacing.  The cabinets run vsync'd at 60 Hz and the games advance one
 * step per frame; uncapped on a desktop they run fast and saturate every CPU,
 * which starves WSLg's audio path.  IGS_FPS_CAP sets the cap (0 = off),
 * IGS_FPS=1 logs the rate every 5 s.
 */
static void frame_end(void);
static void title_tick(void);

/*
 * IGS_SCRIPT="sec:key[+],...": press keys on a timetable from the first
 * frame, for testing.  key is an SDL 1.2 key symbol number, a character,
 * or one of up down left right space enter ctrl alt shift esc f1..f12; a
 * press lasts 0.2 s, or with '+' until the next entry.
 */
static int script_sym(const char *name, size_t n)
{
    static const struct { const char *name; int sym; } names[] = {
        { "up", 273 }, { "down", 274 }, { "right", 275 }, { "left", 276 },
        { "space", 32 }, { "enter", 13 }, { "ctrl", 306 }, { "alt", 308 },
        { "shift", 304 }, { "esc", 27 },
    };
    unsigned i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i].name) == n && !strncmp(names[i].name, name, n))
            return names[i].sym;
    if (n >= 2 && name[0] == 'f' && name[1] >= '1' && name[1] <= '9')
        return 281 + atoi(name + 1);               /* SDLK_F1 = 282 */
    if (n == 1)
        return (unsigned char)name[0];
    return atoi(name);
}

static void script_step(double now)
{
    static const char *p;
    static int held = -1, inited;
    static double release;

    if (!inited) {
        inited = 1;
        p = getenv("IGS_SCRIPT");
    }
    if (held >= 0 && now >= release) {
        machine->key(held, 0);
        held = -1;
    }
    while (p && *p) {
        const char *colon = strchr(p, ':'), *end;
        size_t n;
        int hold;

        if (!colon || atof(p) > now)
            return;
        end = strchr(colon, ',');
        n = end ? (size_t)(end - colon - 1) : strlen(colon + 1);
        hold = n && colon[n] == '+';
        if (held >= 0)
            machine->key(held, 0);
        held = script_sym(colon + 1, n - hold);
        machine->key(held, 1);
        core_log("script %.1f s key %d%s", now, held, hold ? " (held)" : "");
        release = hold ? 1e9 : now + 0.2;
        p = end ? end + 1 : NULL;
        if (hold && p && *p)
            release = atof(p);
    }
}

void SDL_GL_SwapBuffers(void)
{
    REAL(void, SDL_GL_SwapBuffers, (void));

    overlay();
    frame_end();
    real_SDL_GL_SwapBuffers();
}

/*
 * Boards that scan out a frame buffer of their own (E1000, no SDL video in
 * the game): the core opens an SDL GL window at the first frame and shows
 * each frame the machine hands over as a texture, then runs the overlay,
 * the key events and the frame cap as a game's swap would.
 */
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_LINEAR                 0x2601
#define GL_RGB                    0x1907
#define GL_UNSIGNED_SHORT_5_6_5   0x8363
#define GL_UNPACK_ROW_LENGTH      0x0cf2

static struct {
    int w, h;
    unsigned tex;
    void (*GenTextures)(int, unsigned *), (*BindTexture)(unsigned, unsigned);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexCoord2f)(float, float), (*Viewport)(int, int, int, int);
} fbw;

static void fb_window(int w, int h)
{
    int (*init_sub)(uint32_t) = dlsym(RTLD_DEFAULT, "SDL_InitSubSystem");
    void *(*gpa)(const char *) = dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");

    if (!init_sub || !gpa || init_sub(0x20) < 0 ||          /* SDL_INIT_VIDEO */
        !SDL_SetVideoMode(w, h, 0, 0x2)) {                  /* SDL_OPENGL */
        LOG("cannot open the window for the frame buffer");
        exit(1);
    }
    if (gl_load() || !(*(void **)&fbw.GenTextures = gpa("glGenTextures")) ||
        !(*(void **)&fbw.BindTexture = gpa("glBindTexture")) ||
        !(*(void **)&fbw.TexParameteri = gpa("glTexParameteri")) ||
        !(*(void **)&fbw.TexImage2D = gpa("glTexImage2D")) ||
        !(*(void **)&fbw.TexSubImage2D = gpa("glTexSubImage2D")) ||
        !(*(void **)&fbw.TexCoord2f = gpa("glTexCoord2f")) ||
        !(*(void **)&fbw.Viewport = gpa("glViewport"))) {
        LOG("frame buffer window: GL entry points missing");
        exit(1);
    }
    gl.ok = 1;
    fbw.GenTextures(1, &fbw.tex);
    fbw.BindTexture(GL_TEXTURE_2D, fbw.tex);
    fbw.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    fbw.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    fbw.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    fbw.w = w;
    fbw.h = h;
}

static void core_present(const void *px, int w, int h, int pitch)
{
    REAL(void, SDL_GL_SwapBuffers, (void));
    uint8_t ev[64];

    if (!fbw.w)
        fb_window(w, h);
    /* SDL_PollEvent above hands the keys to the machine */
    while (SDL_PollEvent(ev))
        if (ev[0] == 12) {                                  /* SDL_QUIT */
            LOG("window closed");
            exit(0);
        }
    fbw.Viewport(0, 0, fbw.w, fbw.h);
    fbw.BindTexture(GL_TEXTURE_2D, fbw.tex);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 2);
    fbw.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w < fbw.w ? w : fbw.w, h < fbw.h ? h : fbw.h,
                      GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.Enable(GL_TEXTURE_2D);
    gl.Color4f(1, 1, 1, 1);
    gl.Begin(GL_QUADS);
    fbw.TexCoord2f(0, 0); gl.Vertex2f(-1, 1);
    fbw.TexCoord2f(1, 0); gl.Vertex2f(1, 1);
    fbw.TexCoord2f(1, 1); gl.Vertex2f(1, -1);
    fbw.TexCoord2f(0, 1); gl.Vertex2f(-1, -1);
    gl.End();
    gl.Disable(GL_TEXTURE_2D);
    overlay();
    frame_end();
    real_SDL_GL_SwapBuffers();
}

/* overlay, then sleep to the frame cap and log the rate; the caller swaps */
static void frame_end(void)
{
    static int frames, on = -1;
    static long period_ns;
    static struct timespec t0, next;
    struct timespec now;

    if (on < 0) {
        long hz = core_env_long("IGS_FPS_CAP", 60);

        period_ns = hz > 0 ? 1000000000L / hz : 0;
        on = getenv("IGS_FPS") != NULL;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        next = t0;
    }
    if (machine && machine->key) {
        static struct timespec start;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!start.tv_sec)
            start = now;
        script_step((now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9);
    }
    title_tick();
    if (period_ns) {
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1)  /* far behind: resync, don't burst */
            next = now;
        else
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    if (!on)
        return;
    frames++;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - t0.tv_sec >= 5) {
        double dt = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;

        LOG("%.1f fps", frames / dt);
        frames = 0;
        t0 = now;
    }
}

/* ---- X11 / GLX: games with SDL linked in --------------------------------------- */

/* X keysym -> SDL 1.2 key symbol (what the machine and game plugins use) */
static int sdl_key(unsigned long ks)
{
    if (ks >= 0x20 && ks <= 0x7e)
        return ks >= 'A' && ks <= 'Z' ? (int)ks + 32 : (int)ks;
    if (ks >= 0xffbe && ks <= 0xffc9)
        return 282 + (ks - 0xffbe);            /* F1..F12 */
    if (ks >= 0xffb0 && ks <= 0xffb9)
        return 256 + (ks - 0xffb0);            /* keypad 0..9 */
    switch (ks) {
    case 0xff08: return 8;       case 0xff09: return 9;
    case 0xff0d: return 13;      case 0xff1b: return 27;
    case 0xffff: return 127;     case 0xff50: return 278;
    case 0xff51: return 276;     case 0xff52: return 273;
    case 0xff53: return 275;     case 0xff54: return 274;
    case 0xff55: return 280;     case 0xff56: return 281;
    case 0xff57: return 279;     case 0xff63: return 277;
    case 0xffae: return 266;     case 0xff8d: return 271;
    case 0xffe1: return 304;     case 0xffe2: return 303;
    case 0xffe3: return 306;     case 0xffe4: return 305;
    case 0xffe9: return 308;     case 0xffea: return 307;
    default: return 0;
    }
}

/*
 * IGS_SCREEN=WxH: the cabinet's X screen size.  A game that asks for a
 * fullscreen window without a mode switch gets the screen's size (Ogre 1.6's
 * GLX window, Percussion Master 3), while its menus keep their own layout
 * for the cabinet's mode; the screen then reports the cabinet's size (struct
 * Screen on i386: width at 12, height at 16, millimetres at 20 and 24).
 */
static void x11_cabinet_screen(void *dpy)
{
    REAL(int *, XScreenOfDisplay, (void *, int));
    REAL(int, XDefaultScreen, (void *));
    const char *v = getenv("IGS_SCREEN");
    int w, h, *scr;

    if (!dpy || !v || sscanf(v, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0 ||
        !real_XScreenOfDisplay || !real_XDefaultScreen)
        return;
    scr = real_XScreenOfDisplay(dpy, real_XDefaultScreen(dpy));
    if (!scr)
        return;
    scr[3] = w;
    scr[4] = h;
    scr[5] = w * 254 / 960;
    scr[6] = h * 254 / 960;
}

/* ...and no RandR, whose modes are the host's (Ogre then takes the screen's size) */
int XQueryExtension(void *dpy, const char *name, int *op, int *ev, int *err)
{
    REAL(int, XQueryExtension, (void *, const char *, int *, int *, int *));

    if (name && !strcmp(name, "RANDR") && getenv("IGS_SCREEN"))
        return 0;
    return real_XQueryExtension(dpy, name, op, ev, err);
}

/*
 * A window manager's fullscreen state (_NET_WM_STATE_FULLSCREEN, as a
 * property or a client message) makes WSLg size the game's window to the
 * desktop, which Ogre then takes for its frame while its menus keep the
 * cabinet's layout.  Windowed by default, as SDL's fullscreen flag;
 * IGS_FULLSCREEN=1 keeps the requests.
 */
static int is_atom(void *dpy, unsigned long atom, const char *name)
{
    REAL(char *, XGetAtomName, (void *, unsigned long));
    REAL(int, XFree, (void *));
    char *n = atom && real_XGetAtomName ? real_XGetAtomName(dpy, atom) : NULL;
    int same = n && !strcmp(n, name);

    if (n && real_XFree)
        real_XFree(n);
    return same;
}

int XChangeProperty(void *dpy, unsigned long w, unsigned long prop, unsigned long type, int format,
                    int mode, const unsigned char *data, int n)
{
    REAL(int, XChangeProperty, (void *, unsigned long, unsigned long, unsigned long, int, int,
                                const unsigned char *, int));
    int i;

    if (!getenv("IGS_FULLSCREEN") && format == 32 && data && is_atom(dpy, prop, "_NET_WM_STATE"))
        for (i = 0; i < n; i++)
            if (is_atom(dpy, ((const unsigned long *)data)[i], "_NET_WM_STATE_FULLSCREEN"))
                return 1;
    return real_XChangeProperty(dpy, w, prop, type, format, mode, data, n);
}

int XSendEvent(void *dpy, unsigned long w, int propagate, long mask, void *ev)
{
    REAL(int, XSendEvent, (void *, unsigned long, int, long, void *));
    /* XClientMessageEvent on i386: type at 0, message_type at 20, data.l at 28 */
    const unsigned long *e = ev;

    if (!getenv("IGS_FULLSCREEN") && e && *(const int *)ev == 33 /* ClientMessage */ &&
        is_atom(dpy, e[5], "_NET_WM_STATE") &&
        (is_atom(dpy, e[8], "_NET_WM_STATE_FULLSCREEN") || is_atom(dpy, e[9], "_NET_WM_STATE_FULLSCREEN")))
        return 1;
    return real_XSendEvent(dpy, w, propagate, mask, ev);
}

/*
 * libX11 >= 1.8 turns on its display locks for every client.  SDL 1.2 built
 * into a 2008 game carries Xlib internals of its own (extension code with
 * LockDisplay macros) and deadlocks in the first XMatchVisualInfo.  Clear the
 * display's lock pointers (struct _XDisplay: lock at 1220, lock_fns at 1232 on
 * i386, an ABI fixed since X11R6) so Xlib runs single-threaded, as it did on
 * the cabinet.  IGS_X11_LOCKS=1 keeps them.
 */
void *XOpenDisplay(const char *name)
{
    REAL(void *, XOpenDisplay, (const char *));
    void *dpy = real_XOpenDisplay(name);

    if (dpy && !getenv("IGS_X11_LOCKS")) {
        *(void **)((char *)dpy + 1220) = NULL;
        *(void **)((char *)dpy + 1232) = NULL;
    }
    x11_cabinet_screen(dpy);
    return dpy;
}

/*
 * A cabinet owns its X server; a desktop window must not.  OIS (Ogre's
 * input library) grabs the keyboard and pointer, warps the pointer to the
 * window centre and turns key auto-repeat off server-wide (never restored
 * when the game is killed).  These become no-ops; IGS_X11_GRAB=1 keeps them.
 */
static int x11_grab(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("IGS_X11_GRAB") != NULL;
    return on;
}

int XGrabPointer(void *dpy, unsigned long w, int owner, unsigned mask, int pmode, int kmode,
                 unsigned long confine, unsigned long cursor, unsigned long time)
{
    REAL(int, XGrabPointer, (void *, unsigned long, int, unsigned, int, int,
                             unsigned long, unsigned long, unsigned long));

    if (x11_grab())
        return real_XGrabPointer(dpy, w, owner, mask, pmode, kmode, confine, cursor, time);
    return 0;  /* GrabSuccess */
}

int XGrabKeyboard(void *dpy, unsigned long w, int owner, int pmode, int kmode, unsigned long time)
{
    REAL(int, XGrabKeyboard, (void *, unsigned long, int, int, int, unsigned long));

    if (x11_grab())
        return real_XGrabKeyboard(dpy, w, owner, pmode, kmode, time);
    return 0;
}

int XWarpPointer(void *dpy, unsigned long src, unsigned long dst, int sx, int sy,
                 unsigned sw, unsigned sh, int dx, int dy)
{
    REAL(int, XWarpPointer, (void *, unsigned long, unsigned long, int, int,
                             unsigned, unsigned, int, int));

    if (x11_grab())
        return real_XWarpPointer(dpy, src, dst, sx, sy, sw, sh, dx, dy);
    return 1;
}

int XAutoRepeatOff(void *dpy)
{
    REAL(int, XAutoRepeatOff, (void *));

    if (x11_grab())
        return real_XAutoRepeatOff(dpy);
    return 1;
}

/*
 * Window title: the game, the machine's status line and the frame rate,
 * refreshed every second ("Hero of Robots m100fa | insert coin | 58 fps").
 * IGS_TITLE replaces the name ($IGS_NAME, the manifest's NAME); IGS_TITLE=0
 * leaves the game's own title.  SDL games get it through SDL_WM_SetCaption
 * on their render thread; X11 games (SDL built in, Ogre) on an X connection
 * of the core's own, set on the window the game titled.
 */
static unsigned long title_window;     /* the X11 window the game titled */

static int title_on(void)
{
    static int on = -1;

    if (on < 0)
        on = strcmp(core_env("IGS_TITLE", ""), "0") != 0;
    return on;
}

static void (*real_sdl_caption)(const char *, const char *);

void SDL_WM_SetCaption(const char *title, const char *icon)
{
    if (!real_sdl_caption)
        *(void **)&real_sdl_caption = dlsym(RTLD_NEXT, "SDL_WM_SetCaption");
    if (!title_on() && real_sdl_caption)
        real_sdl_caption(title, icon);
}

static void title_x11(const char *s)
{
    REAL(void *, XOpenDisplay, (const char *));
    REAL(int, XStoreName, (void *, unsigned long, const char *));
    REAL(unsigned long, XInternAtom, (void *, const char *, int));
    REAL(int, XChangeProperty, (void *, unsigned long, unsigned long, unsigned long, int, int,
                                const unsigned char *, int));
    REAL(int, XFlush, (void *));
    static void *dpy;
    static unsigned long net_wm_name, utf8;

    if (!title_window)
        return;
    if (!dpy) {
        dpy = real_XOpenDisplay(NULL);
        if (!dpy)
            return;
        net_wm_name = real_XInternAtom(dpy, "_NET_WM_NAME", 0);
        utf8 = real_XInternAtom(dpy, "UTF8_STRING", 0);
    }
    real_XStoreName(dpy, title_window, s);
    real_XChangeProperty(dpy, title_window, net_wm_name, utf8, 8, 0 /* PropModeReplace */,
                         (const unsigned char *)s, strlen(s));
    real_XFlush(dpy);
}

static void title_tick(void)
{
    static int frames;
    static double t0;
    double now = core_now();
    char status[128] = "", buf[256];

    frames++;
    if (!title_on())
        return;
    if (!t0)
        t0 = now;
    if (now - t0 < 1)
        return;
    if (machine && machine->status)
        machine->status(status, sizeof(status));
    snprintf(buf, sizeof(buf), "%s%s%s | %.0f fps",
             core_env("IGS_TITLE", core_env("IGS_NAME", machine ? machine->name : "IGS")),
             *status ? " | " : "", status, frames / (now - t0));
    frames = 0;
    t0 = now;
    if (core.verbose)
        LOG("title: %s (window 0x%lx, %s)", buf, title_window, sdl_video ? "SDL" : "X11");
    if (sdl_video) {
        if (!real_sdl_caption)
            *(void **)&real_sdl_caption = dlsym(RTLD_NEXT, "SDL_WM_SetCaption");
        if (real_sdl_caption)
            real_sdl_caption(buf, buf);
    } else {
        title_x11(buf);
    }
}

/*
 * The game's own title and window-manager hint calls name its top-level
 * window (SDL 1.2 sets hints on it even when the game sets no caption).
 */
int XSetWMHints(void *dpy, unsigned long w, void *hints)
{
    REAL(int, XSetWMHints, (void *, unsigned long, void *));

    title_window = w;
    return real_XSetWMHints(dpy, w, hints);
}

int XSetClassHint(void *dpy, unsigned long w, void *hint)
{
    REAL(int, XSetClassHint, (void *, unsigned long, void *));

    title_window = w;
    return real_XSetClassHint(dpy, w, hint);
}

int XStoreName(void *dpy, unsigned long w, const char *name)
{
    REAL(int, XStoreName, (void *, unsigned long, const char *));

    title_window = w;
    return real_XStoreName(dpy, w, name);
}

void XSetWMName(void *dpy, unsigned long w, void *prop)
{
    REAL(void, XSetWMName, (void *, unsigned long, void *));

    title_window = w;
    real_XSetWMName(dpy, w, prop);
}

void XSetWMProperties(void *dpy, unsigned long w, void *name, void *icon, char **argv, int argc,
                      void *hints, void *wm, void *cls)
{
    REAL(void, XSetWMProperties, (void *, unsigned long, void *, void *, char **, int,
                                  void *, void *, void *));

    if (name)
        title_window = w;
    real_XSetWMProperties(dpy, w, name, icon, argv, argc, hints, wm, cls);
}

void Xutf8SetWMProperties(void *dpy, unsigned long w, const char *name, const char *icon,
                          char **argv, int argc, void *hints, void *wm, void *cls)
{
    REAL(void, Xutf8SetWMProperties, (void *, unsigned long, const char *, const char *,
                                      char **, int, void *, void *, void *));

    if (name)
        title_window = w;
    real_Xutf8SetWMProperties(dpy, w, name, icon, argv, argc, hints, wm, cls);
}

void XmbSetWMProperties(void *dpy, unsigned long w, const char *name, const char *icon,
                        char **argv, int argc, void *hints, void *wm, void *cls)
{
    REAL(void, XmbSetWMProperties, (void *, unsigned long, const char *, const char *,
                                    char **, int, void *, void *, void *));

    if (name)
        title_window = w;
    real_XmbSetWMProperties(dpy, w, name, icon, argv, argc, hints, wm, cls);
}

/*
 * XEvent: int type first; KeyPress = 2, KeyRelease = 3, FocusOut = 10.  Keys
 * held when the window loses the focus come up elsewhere: released here, as
 * the Windows loader does, so none stays pressed for the game.
 */
int XNextEvent(void *dpy, void *ev)
{
    REAL(int, XNextEvent, (void *, void *));
    REAL(unsigned long, XLookupKeysym, (void *, int));
    static uint8_t held[1024];
    int r = real_XNextEvent(dpy, ev);
    int type = *(int *)ev, s;

    if ((type == 2 || type == 3) && machine && machine->key) {
        int sym = sdl_key(real_XLookupKeysym(ev, 0));

        if (sym) {
            if (sym > 0 && sym < 1024)
                held[sym] = type == 2;
            machine->key(sym, type == 2);
        }
    } else if (type == 10 && machine && machine->key) {
        for (s = 1; s < 1024; s++)
            if (held[s]) {
                held[s] = 0;
                machine->key(s, 0);
            }
    }
    return r;
}

static void (*real_glXSwapBuffers)(void *, unsigned long);

static void hook_glXSwapBuffers(void *dpy, unsigned long drawable)
{
    overlay();
    frame_end();
    real_glXSwapBuffers(dpy, drawable);
}

/* the same for games whose GLX code links glXSwapBuffers (Ogre's GL render system) */
void glXSwapBuffers(void *dpy, unsigned long drawable)
{
    if (!real_glXSwapBuffers) {
        static void *(*real_dlsym)(void *, const char *);

        if (!real_dlsym)
            *(void **)&real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real_dlsym)
            *(void **)&real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
        *(void **)&real_glXSwapBuffers = real_dlsym(RTLD_NEXT, "glXSwapBuffers");
    }
    hook_glXSwapBuffers(dpy, drawable);
}

/*
 * IGS_CG_ARB=1: NVIDIA-only Cg profiles on any GL.  Speed Rider's shaders
 * ask Cg for vp40 / fp40 only (OPTION NV_vertex_program3 /
 * NV_fragment_program2 ARB programs), which Mesa cannot load.  The GL then
 * claims those two extensions, so Ogre selects the profiles, and Cg compiles
 * them as plain arbvp1 / arbfp1.
 */
static void *real_next(const char *name)
{
    static void *(*real_dlsym)(void *, const char *);

    if (!real_dlsym)
        *(void **)&real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!real_dlsym)
        *(void **)&real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
    return real_dlsym(RTLD_NEXT, name);
}

const unsigned char *glGetString(unsigned name)
{
    static const unsigned char *(*real)(unsigned);
    static char *ext;
    static int on = -1;
    const unsigned char *s;

    if (!real)
        *(void **)&real = real_next("glGetString");
    s = real ? real(name) : NULL;
    if (on < 0)
        on = getenv("IGS_CG_ARB") && atoi(getenv("IGS_CG_ARB"));
    if (!on || name != 0x1F03 || !s)          /* GL_EXTENSIONS */
        return s;
    /* the trailing space: older GLEW (Ogre 1.6's RenderSystem_GL) never
       matches the string's last name */
    if (!ext && asprintf(&ext, "%s GL_NV_vertex_program3 GL_NV_fragment_program2 ", (const char *)s) < 0)
        ext = NULL;
    return ext ? (const unsigned char *)ext : s;
}

void *cgCreateProgram(void *ctx, int type, const char *src, int profile, const char *entry, const char **args)
{
    static void *(*real)(void *, int, const char *, int, const char *, const char **);

    if (!real)
        *(void **)&real = real_next("cgCreateProgram");
    if (getenv("IGS_CG_ARB") && atoi(getenv("IGS_CG_ARB"))) {
        if (profile == 7001)                   /* CG_PROFILE_VP40 -> ARBVP1 */
            profile = 6150;
        else if (profile == 6151)              /* CG_PROFILE_FP40 -> ARBFP1 */
            profile = 7000;
    }
    return real ? real(ctx, type, src, profile, entry, args) : NULL;
}

/*
 * Swap-interval requests go out as GLX vendor-private requests with the
 * context tag, which WSLg's Xwayland rejects (GLXBadContextTag) for a direct
 * context.  The frame cap paces the game instead.
 */
static int noop_swap_interval(int interval)
{
    (void)interval;
    return 0;
}

static int is_swap_interval(const char *name)
{
    return !strcmp(name, "glXSwapIntervalSGI") || !strcmp(name, "glXSwapIntervalMESA");
}

/*
 * $IGS_VRAM_MB: the video memory the GL memory queries report at least
 * (NVX_gpu_memory_info, ATI_meminfo, in KiB).  Unity 4 sizes its texture
 * budget from them; a host driver reporting little makes it shrink every
 * large texture, and its compressed ones come out striped (Crazy Shoot: the
 * cabinet's GT218 has 512 MB).
 */
void glGetIntegerv(unsigned pname, int *v)
{
    REAL(void, glGetIntegerv, (unsigned, int *));
    static long vram = -1;

    real_glGetIntegerv(pname, v);
    if (vram < 0)
        vram = core.env_long ? core.env_long("IGS_VRAM_MB", 0) : 0;
    if (!vram || !v)
        return;
    switch (pname) {
    case 0x9047: case 0x9048: case 0x9049:   /* dedicated, total, current (NVX), KiB */
    case 0x87FB: case 0x87FC: case 0x87FD:   /* VBO, texture, renderbuffer free (ATI), KiB */
        if (v[0] < vram * 1024)
            v[0] = (int)(vram * 1024);
        break;
    }
}

static void *gl_override(const char *name)
{
    if (is_swap_interval(name))
        return (void *)noop_swap_interval;
    if (!strcmp(name, "glGetIntegerv"))
        return (void *)glGetIntegerv;
    return NULL;
}

/* the same entry points asked for by name from GLX */
void *glXGetProcAddressARB(const unsigned char *name)
{
    REAL(void *, glXGetProcAddressARB, (const unsigned char *));
    void *o = gl_override((const char *)name);

    return o ? o : real_glXGetProcAddressARB(name);
}

void *glXGetProcAddress(const unsigned char *name)
{
    REAL(void *, glXGetProcAddress, (const unsigned char *));
    void *o = gl_override((const char *)name);

    return o ? o : real_glXGetProcAddress(name);
}

/*
 * SDL finds glXSwapBuffers with dlsym on its own dlopen'ed libGL, which an
 * LD_PRELOAD definition cannot shadow: hand out the hook from dlsym instead.
 * The real dlsym is reached through dlvsym (GLIBC_2.34, or 2.0 before it).
 */
void *dlsym(void *h, const char *name)
{
    static void *(*real)(void *, const char *);

    if (!real) {
        *(void **)&real = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real)
            *(void **)&real = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
    }
    if (!strcmp(name, "glXSwapBuffers")) {
        void *f = real(h, name);

        if (!f)
            return NULL;
        /* a global-scope handle finds our own definition first: take the next */
        if (f == (void *)glXSwapBuffers)
            f = real(RTLD_NEXT, name);
        if (!f)
            return NULL;
        *(void **)&real_glXSwapBuffers = f;
        return (void *)hook_glXSwapBuffers;
    }
    if (is_swap_interval(name))
        return real(h, name) ? (void *)noop_swap_interval : NULL;
    if (!strcmp(name, "glGetIntegerv") && h != RTLD_NEXT)
        return real(h, name) ? (void *)glGetIntegerv : NULL;
    return real(h, name);
}

/* ---- SDL_mixer: buffer override and logging (IGS_AUDIO_LOG=1) -------------- */

static int audio_log(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("IGS_AUDIO_LOG") != NULL;
    return on;
}

int Mix_OpenAudio(int freq, uint16_t format, int channels, int chunksize)
{
    REAL(int, Mix_OpenAudio, (int, uint16_t, int, int));
    char *(*drv_name)(char *, int) = dlsym(RTLD_DEFAULT, "SDL_AudioDriverName");
    char name[32] = "?";
    int r;

    /* the games' own 512 samples work best on WSLg; IGS_AUDIO_CHUNK overrides */
    chunksize = core_env_long("IGS_AUDIO_CHUNK", chunksize);
    r = real_Mix_OpenAudio(freq, format, channels, chunksize);
    if (drv_name)
        drv_name(name, sizeof(name));
    LOG("audio open %d Hz fmt 0x%x ch %d chunk %d driver %s -> %d",
        freq, format, channels, chunksize, name, r);
    return r;
}

/*
 * A heap RIFF image wrapped with a size that cannot be its own (negative,
 * or far past its RIFF length) gets its allocation's size: Fish Hunter 6
 * M607CN's sound loader (0x8062E10) passes a stale stack word as the size,
 * read before its decrypt call stores it.  The cabinet's SDL 1.2 and the
 * Windows loader's reader stop at the data; the host mixer does not.
 */
void *SDL_RWFromMem(void *mem, int size)
{
    REAL(void *, SDL_RWFromMem, (void *, int));
    const uint8_t *p = mem;

    if (p && !memcmp(p, "RIFF", 4)) {
        uint32_t len = 8 + ((uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24);

        if (size < 12 || (uint32_t)size > len + 0x10000u) {
            size_t n = malloc_usable_size(mem);

            if (n >= 12 && n <= 0x7FFFFFFF) {
                LOG("RIFF at %p: size %d -> %zu (its allocation)", mem, size, n);
                size = (int)n;
            }
        }
    }
    return real_SDL_RWFromMem(mem, size);
}

void *Mix_LoadWAV_RW(void *src, int freesrc)
{
    REAL(void *, Mix_LoadWAV_RW, (void *, int));
    void *r = real_Mix_LoadWAV_RW(src, freesrc);

    if (audio_log() && !r) {
        char *(*err)(void) = dlsym(RTLD_DEFAULT, "SDL_GetError");

        LOG("load wav -> NULL %s", err ? err() : "");
    }
    return r;
}

int Mix_PlayChannelTimed(int ch, void *chunk, int loops, int ticks)
{
    REAL(int, Mix_PlayChannelTimed, (int, void *, int, int));
    int r = real_Mix_PlayChannelTimed(ch, chunk, loops, ticks);

    if (audio_log()) {
        char *(*err)(void) = r < 0 ? dlsym(RTLD_DEFAULT, "SDL_GetError") : NULL;

        LOG("%.2f play ch %d loops %d ticks %d -> %d%s%s", core_now(), ch, loops, ticks, r,
            err ? " " : "", err ? err() : "");
    }
    return r;
}

int Mix_FadeInChannelTimed(int ch, void *chunk, int loops, int ms, int ticks)
{
    REAL(int, Mix_FadeInChannelTimed, (int, void *, int, int, int));
    int r = real_Mix_FadeInChannelTimed(ch, chunk, loops, ms, ticks);

    if (audio_log())
        LOG("%.2f fadein ch %d loops %d ms %d -> %d", core_now(), ch, loops, ms, r);
    return r;
}

int Mix_HaltChannel(int ch)
{
    REAL(int, Mix_HaltChannel, (int));

    if (audio_log())
        LOG("%.2f halt ch %d", core_now(), ch);
    return real_Mix_HaltChannel(ch);
}

int Mix_FadeOutChannel(int ch, int ms)
{
    REAL(int, Mix_FadeOutChannel, (int, int));

    if (audio_log())
        LOG("%.2f fadeout ch %d ms %d", core_now(), ch, ms);
    return real_Mix_FadeOutChannel(ch, ms);
}

int Mix_Volume(int ch, int vol)
{
    REAL(int, Mix_Volume, (int, int));
    static int last[64];

    if (audio_log() && vol >= 0 && ch >= 0 && ch < 64 && last[ch] != vol + 1) {
        last[ch] = vol + 1;
        LOG("%.2f volume ch %d = %d", core_now(), ch, vol);
    }
    return real_Mix_Volume(ch, vol);
}
