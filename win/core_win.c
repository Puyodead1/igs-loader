/*
 * IGS loader, Windows core: what core/core.c does on Linux.
 *   - loads the machine plugin ($IGS_MACHINE, a DLL here) and routes its
 *     device files (open/ioctl/mmap/close, read/write, termios) to it;
 *   - MMIO traps: PAGE_NOACCESS regions whose accesses (the games' plain
 *     movs) are emulated in a vectored exception handler; port I/O (in/out,
 *     privileged instructions) for machines that emulate it (E1000);
 *   - an empty OSS mixer, OSS /dev/dsp on waveOut, IGS_NULL_DEVS;
 *   - the overlay (fixed-function GL before each swap), IGS_SHOT frame dumps,
 *     IGS_SCRIPT key presses, the frame cap, the window title;
 *   - crash reports for faults in ELF code (module+offset, registers).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>

#include "lx.h"
#include "../core/igs.h"
#include "../core/font.h"

static struct igs_core core;
static const struct igs_machine *machine;
int lx_verbose, lx_lddebug;
static CRITICAL_SECTION log_lock;

void lx_log(const char *fmt, ...)
{
    va_list ap;

    EnterCriticalSection(&log_lock);
    va_start(ap, fmt);
    fputs("[igs] ", stderr);
    __mingw_vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
    LeaveCriticalSection(&log_lock);
}

const char *lx_env(const char *name, const char *def)
{
    const char *v = getenv(name);

    return v && *v ? v : def;
}

long lx_env_long(const char *name, long def)
{
    const char *v = getenv(name);

    return v && *v ? atol(v) : def;
}

static int core_placeholder_fd(void) { return lx_fd_new_null(); }

/* ---- MMIO traps --------------------------------------------------------------------------- */

#define MMIO_MAX 8
static struct mmio {
    uint8_t *base;
    size_t len;
    void *opaque;
    uint32_t (*read)(void *, uint32_t, int);
    void (*write)(void *, uint32_t, int, uint32_t);
} mmio[MMIO_MAX];
static int nmmio;

static DWORD *reg_ptr(CONTEXT *c, int r)
{
    switch (r & 7) {
    case 0: return &c->Eax;
    case 1: return &c->Ecx;
    case 2: return &c->Edx;
    case 3: return &c->Ebx;
    case 4: return &c->Esp;
    case 5: return &c->Ebp;
    case 6: return &c->Esi;
    default: return &c->Edi;
    }
}

static uint32_t get_reg(CONTEXT *c, int r, int size)
{
    if (size == 1)      /* al cl dl bl ah ch dh bh */
        return (*reg_ptr(c, r & 3) >> (r & 4 ? 8 : 0)) & 0xff;
    return *reg_ptr(c, r) & (size == 2 ? 0xffff : 0xffffffff);
}

static void set_reg(CONTEXT *c, int r, int size, uint32_t v)
{
    DWORD *g;

    if (size == 1) {
        g = reg_ptr(c, r & 3);
        if (r & 4)
            *g = (*g & ~0xff00) | ((v & 0xff) << 8);
        else
            *g = (*g & ~0xff) | (v & 0xff);
        return;
    }
    g = reg_ptr(c, r);
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

/* emulate the mov at Eip against an MMIO region; 0 if not one */
static int mmio_access(CONTEXT *c, uint8_t *a)
{
    struct mmio *m = NULL;
    const uint8_t *ip = (const uint8_t *)c->Eip;
    int size = 4, reg, n = 0, i;
    uint32_t off;

    for (i = 0; i < nmmio; i++)
        if (a >= mmio[i].base && a < mmio[i].base + mmio[i].len)
            m = &mmio[i];
    if (!m)
        return 0;
    off = (uint32_t)(a - m->base);
    if (ip[0] == 0x66) {
        size = 2;
        n = 1;
    }
    switch (ip[n]) {
    case 0x8b:       /* mov r, r/m */
    case 0x8a:
        if (ip[n] == 0x8a)
            size = 1;
        reg = (ip[n + 1] >> 3) & 7;
        set_reg(c, reg, size, m->read(m->opaque, off, size));
        n += 1 + modrm_len(ip + n + 1);
        break;
    case 0x89:       /* mov r/m, r */
    case 0x88:
        if (ip[n] == 0x88)
            size = 1;
        reg = (ip[n + 1] >> 3) & 7;
        m->write(m->opaque, off, size, get_reg(c, reg, size));
        n += 1 + modrm_len(ip + n + 1);
        break;
    case 0x0f:       /* movzx r32, r/m8 / r/m16 */
        if (ip[n + 1] != 0xb6 && ip[n + 1] != 0xb7)
            goto unknown;
        size = ip[n + 1] == 0xb6 ? 1 : 2;
        reg = (ip[n + 2] >> 3) & 7;
        set_reg(c, reg, 4, m->read(m->opaque, off, size));
        n += 2 + modrm_len(ip + n + 2);
        break;
    case 0xc7:       /* mov r/m, imm16/32 */
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
        fflush(NULL);
        ExitProcess(134);
    }
    c->Eip += n;
    return 1;
}

/*
 * x86 port I/O (in/out with dx or an imm8 port, 8/16/32-bit) for machines
 * with port_in/port_out: from user mode the instruction raises a privileged
 * instruction exception.
 */
static int port_access(CONTEXT *c)
{
    const uint8_t *ip = (const uint8_t *)c->Eip;
    int n = 0, size = 4, op;
    uint16_t port = (uint16_t)c->Edx;

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
            machine->port_out(port, size, get_reg(c, 0, size));
        else
            set_reg(c, 0, size, machine->port_in(port, size));
        c->Eip += n + 1;
        return 1;
    }
    return 0;
}

int core_ports(void)
{
    return machine && machine->port_in;
}

int elf_owns(uint32_t addr);

static void crash_report(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    char where[160];
    uint32_t *sp = (uint32_t *)c->Esp;
    int i, shown = 0;

    elf_describe(c->Eip, where, sizeof(where));
    LOG("fault 0x%08lx at %s (eip 0x%08lx)%s 0x%08lx", er->ExceptionCode, where, c->Eip,
        er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ?
            (er->ExceptionInformation[0] == 8 ? " executing" :
             er->ExceptionInformation[0] ? " writing" : " reading") : "",
        er->NumberParameters >= 2 ? (unsigned long)er->ExceptionInformation[1] : 0ul);
    LOG("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    /* return addresses into ELF code on the stack: a rough backtrace */
    for (i = 0; i < 2048 && shown < 12; i++) {
        MEMORY_BASIC_INFORMATION mbi;

        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            break;
        if (elf_owns(sp[i])) {
            elf_describe(sp[i], where, sizeof(where));
            LOG("  stack +0x%03x: %s", i * 4, where);
            shown++;
        }
    }
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;

    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 && nmmio &&
        mmio_access(ep->ContextRecord, (uint8_t *)er->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (code == EXCEPTION_PRIV_INSTRUCTION && port_access(ep->ContextRecord))
        return EXCEPTION_CONTINUE_EXECUTION;
    /* ELF code has no exception handlers of its own: a fault there is fatal */
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
         code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT) &&
        elf_owns(ep->ContextRecord->Eip)) {
        crash_report(ep);
        fflush(NULL);
        ExitProcess(139);
    }
    /* IGS_VERBOSE: first-chance faults in native code too (drivers may handle their own) */
    if ((lx_verbose && code == EXCEPTION_ACCESS_VIOLATION) || code == EXCEPTION_STACK_OVERFLOW) {
        LOG("first-chance fault in native code:");
        crash_report(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI last_chance(EXCEPTION_POINTERS *ep)
{
    LOG("unhandled exception:");
    crash_report(ep);
    fflush(NULL);
    ExitProcess(139);
    return EXCEPTION_EXECUTE_HANDLER;
}

void core_install_veh(void)
{
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(last_chance);
}

static void *core_mmio_map(size_t len, void *opaque, uint32_t (*read)(void *, uint32_t, int),
                           void (*write)(void *, uint32_t, int, uint32_t))
{
    void *p;

    if (nmmio == MMIO_MAX)
        return (void *)-1;
    len = (len + 0xfff) & ~(size_t)0xfff;
    /* high addresses, as on Linux (E1000's game ANDs its mapped pointers) */
    p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_NOACCESS);
    if (!p)
        return (void *)-1;
    mmio[nmmio].base = p;
    mmio[nmmio].len = len;
    mmio[nmmio].opaque = opaque;
    mmio[nmmio].read = read;
    mmio[nmmio].write = write;
    nmmio++;
    return p;
}

/* ---- overlay (fixed-function GL) ------------------------------------------------------------ */

#ifndef APIENTRY
#define APIENTRY __stdcall
#endif
static struct {
    int ok;
    void (APIENTRY *WindowPos2i)(int, int);
    void (APIENTRY *UseProgram)(unsigned);
    void (APIENTRY *ActiveTexture)(unsigned);
} glx;
static HWND game_hwnd;

void core_set_window(void *hwnd) { game_hwnd = hwnd; }

void core_gl_ready(void)
{
    *(void **)&glx.WindowPos2i = (void *)wglGetProcAddress("glWindowPos2i");
    *(void **)&glx.UseProgram = (void *)wglGetProcAddress("glUseProgram");
    *(void **)&glx.ActiveTexture = (void *)wglGetProcAddress("glActiveTexture");
    glx.ok = glx.WindowPos2i ? 1 : -1;
    if (glx.ok < 0)
        LOG("GL: no glWindowPos2i: no overlay text");
}

static void core_fill(int x, int y, int w, int h, float r, float g, float b, float a)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
    glDisable(GL_BLEND);
}

static void core_text(int x, int y, const char *t, float r, float g, float b)
{
    if (glx.ok <= 0)
        return;
    glColor4f(r, g, b, 1);
    glx.WindowPos2i(x, y);
    for (; *t; t++) {
        int c = (unsigned char)*t;

        if (c < 32 || c > 126)
            c = '?';
        glBitmap(16, FONT_H, 0, 0, FONT_W, 0, font_bits[c - 32]);
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
    static char prefix[MAX_PATH];
    const char *p = getenv("IGS_SHOT");
    char path[MAX_PATH + 16];
    unsigned char *px;
    int vp[4], y;
    FILE *f;

    if (!p)
        return;
    if (every < 0) {
        every = lx_env_long("IGS_SHOT_EVERY", 600);
        if (p[0] == '/') {
            if (lx_path(p, prefix, sizeof(prefix)))
                snprintf(prefix, sizeof(prefix), "%s", p);
        } else {
            snprintf(prefix, sizeof(prefix), "%s", p);
        }
    }
    if (++frame % (every > 0 ? every : 600))
        return;
    glGetIntegerv(GL_VIEWPORT, vp);
    px = malloc((size_t)vp[2] * vp[3] * 3);
    if (!px)
        return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, vp[2], vp[3], GL_RGB, GL_UNSIGNED_BYTE, px);
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

#define GL_CURRENT_PROGRAM_ 0x8b8d
#define GL_ACTIVE_TEXTURE_ 0x84e0
#define GL_TEXTURE0_ 0x84c0

/* origin bottom-left, in viewport pixels; all GL state saved and restored */
static void overlay(void)
{
    int vp[4], prog = 0, active = 0, i;

    if (!machine || !machine->draw || glx.ok == 0) {
        screenshot();
        return;
    }
    glGetIntegerv(GL_VIEWPORT, vp);
    if (glx.UseProgram)
        glGetIntegerv(GL_CURRENT_PROGRAM_, &prog);
    if (glx.ActiveTexture)
        glGetIntegerv(GL_ACTIVE_TEXTURE_, &active);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    if (glx.UseProgram)
        glx.UseProgram(0);
    for (i = 3; i >= 0; i--) {
        if (glx.ActiveTexture)
            glx.ActiveTexture(GL_TEXTURE0_ + i);
        else if (i)
            continue;
        glDisable(GL_TEXTURE_2D);
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, vp[2], 0, vp[3], -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    machine->draw(vp[2], vp[3]);
    screenshot();

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    if (glx.ActiveTexture)
        glx.ActiveTexture(active ? active : GL_TEXTURE0_);
    if (glx.UseProgram)
        glx.UseProgram(prog);
}

/* ---- IGS_SCRIPT, frame pacing, title ------------------------------------------------------------ */

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
        LOG("script %.1f s key %d%s", now, held, hold ? " (held)" : "");
        release = hold ? 1e9 : now + 0.2;
        p = end ? end + 1 : NULL;
        if (hold && p && *p)
            release = atof(p);
    }
}

/*
 * Window title: the game, the machine's status line and the frame rate,
 * refreshed every second.  IGS_TITLE replaces the name ($IGS_NAME, the
 * manifest's NAME); IGS_TITLE=0 leaves the game's own title.
 */
static char game_caption[256];

static int title_on(void)
{
    static int on = -1;

    if (on < 0)
        on = strcmp(lx_env("IGS_TITLE", ""), "0") != 0;
    return on;
}

void core_caption(const char *t)
{
    snprintf(game_caption, sizeof(game_caption), "%s", t);
    if (!title_on())
        sdl_window_title(game_caption);
}

static void title_tick(void)
{
    static int frames;
    static double t0;
    double now = lx_now();
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
             lx_env("IGS_TITLE", lx_env("IGS_NAME", machine ? machine->name : "IGS")),
             *status ? " | " : "", status, frames / (now - t0));
    frames = 0;
    t0 = now;
    if (lx_verbose)
        LOG("title: %s", buf);
    sdl_window_title(buf);
}

/*
 * Frame pacing.  The cabinets run vsync'd at 60 Hz and the games advance one
 * step per frame.  IGS_FPS_CAP sets the cap (0 = off), IGS_FPS=1 logs the
 * rate every 5 s.
 */
static void frame_end(void)
{
    static int frames, on = -1;
    static double period, t0, next, start;
    double now;

    if (on < 0) {
        long hz = lx_env_long("IGS_FPS_CAP", 60);

        period = hz > 0 ? 1.0 / hz : 0;
        on = getenv("IGS_FPS") != NULL;
        t0 = next = start = lx_now();
    }
    if (machine && machine->key)
        script_step(lx_now() - start);
    title_tick();
    if (period > 0) {
        next += period;
        now = lx_now();
        if (now > next + 1)              /* far behind: resync, don't burst */
            next = now;
        else
            lx_sleep_until(next);
    }
    if (!on)
        return;
    frames++;
    now = lx_now();
    if (now - t0 >= 5) {
        LOG("%.1f fps", frames / (now - t0));
        frames = 0;
        t0 = now;
    }
}

void core_frame(void)
{
    overlay();
    frame_end();
}

void core_key(int sym, int down)
{
    if (machine && machine->key)
        machine->key(sym, down);
}

/* ---- OSS /dev/dsp on waveOut ------------------------------------------------------------------------- */

/*
 * For games with SDL's OSS driver built in (E2000).  The game's writes are
 * queued as waveOut buffers and block while about IGS_DSP_LATENCY_MS
 * (default 60) is queued, which paces the game's audio thread as a real
 * /dev/dsp does.  The device opens at the first write, after the ioctls.
 * Only the ioctls SDL 1.2's "dsp" driver uses (core.c's set).
 */
#define DSP_BUFS 32
#define DSP_CHUNK 16384
static struct {
    int fd, rate, channels, fmt, fragsize, nfrags, bps;
    HWAVEOUT wo;
    HANDLE ev;
    WAVEHDR hdr[DSP_BUFS];
    int next, heard, peak;
} dsp = { -1, 8000, 1, 8, 4096, 4, 0, NULL, NULL, { { 0 } }, 0, 0, 0 };

static int dsp_open(void)
{
    dsp.fd = core_placeholder_fd();
    dsp.rate = 8000;
    dsp.channels = 1;
    dsp.fmt = 8;
    dsp.fragsize = 4096;
    dsp.nfrags = 4;
    return dsp.fd;
}

static void dsp_ioctl(unsigned long req, int *arg)
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
}

static int dsp_start(void)
{
    WAVEFORMATEX wf;
    MMRESULT r;
    int i;

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)dsp.channels;
    wf.nSamplesPerSec = dsp.rate;
    wf.wBitsPerSample = (WORD)(dsp.fmt == 16 ? 16 : 8);
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    dsp.bps = wf.nAvgBytesPerSec;
    dsp.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    r = waveOutOpen(&dsp.wo, WAVE_MAPPER, &wf, (DWORD_PTR)dsp.ev, 0, CALLBACK_EVENT);
    if (r == WAVERR_BADFORMAT) {
        /* an odd rate (Gold Captain asks 22000 Hz): the nearest standard one, 0.2% off */
        static const int rates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000 };
        int i, best = 0;

        for (i = 1; i < 7; i++)
            if (abs(rates[i] - dsp.rate) < abs(rates[best] - dsp.rate))
                best = i;
        wf.nSamplesPerSec = rates[best];
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        r = waveOutOpen(&dsp.wo, WAVE_MAPPER, &wf, (DWORD_PTR)dsp.ev, 0, CALLBACK_EVENT);
        dsp.bps = wf.nAvgBytesPerSec;
    }
    LOG("dsp: %d Hz, %d ch, %d bit -> waveOut %lu Hz %s (%u)", dsp.rate, dsp.channels,
        wf.wBitsPerSample, wf.nSamplesPerSec, r ? "failed" : "ok", r);
    if (r) {
        dsp.wo = NULL;
        return -1;
    }
    for (i = 0; i < DSP_BUFS; i++) {
        memset(&dsp.hdr[i], 0, sizeof(WAVEHDR));
        dsp.hdr[i].lpData = malloc(DSP_CHUNK);
    }
    dsp.next = 0;
    return 0;
}

/* the first non-silent write; IGS_AUDIO_LOG=1: writes and peak level every 5 s */
static void dsp_stats(const uint8_t *buf, size_t n)
{
    static double t0;
    static long writes;
    int peak = 0;
    size_t i;

    for (i = 0; i + 1 < n; i += 2) {
        int s = dsp.fmt == 16 ? abs(*(const int16_t *)(buf + i)) : abs(buf[i] - 128) * 256;

        if (s > peak)
            peak = s;
    }
    writes++;
    if (peak > 64 && !dsp.heard++)
        LOG("dsp: first non-silent write after %ld writes (peak %d)", writes, peak);
    if (peak > dsp.peak)
        dsp.peak = peak;
    if (lx_env("IGS_AUDIO_LOG", NULL) && lx_now() - t0 >= 5) {
        LOG("dsp: %ld writes, peak %d", writes, dsp.peak);
        dsp.peak = 0;
        t0 = lx_now();
    }
}

static long dsp_write(const uint8_t *buf, size_t n)
{
    size_t done = 0;
    long target;

    /* IGS_NO_AUDIO=1 (test runs): no device */
    if (!dsp.wo && !dsp.bps && (lx_env("IGS_NO_AUDIO", NULL) || dsp_start())) {
        /* no device: drain at the real rate so the game keeps time */
        dsp.bps = dsp.rate * dsp.channels * (dsp.fmt == 16 ? 2 : 1);
    }
    dsp_stats(buf, n);
    if (!dsp.wo) {
        lx_usleep((int64_t)n * 1000000 / (dsp.bps ? dsp.bps : 8000));
        return (long)n;
    }
    target = dsp.bps * lx_env_long("IGS_DSP_LATENCY_MS", 60) / 1000;
    while (done < n) {
        size_t len = n - done < DSP_CHUNK ? n - done : DSP_CHUNK;
        WAVEHDR *h = &dsp.hdr[dsp.next];

        if ((long)len * 3 > target)
            target = (long)len * 3;
        for (;;) {
            long queued = 0;
            int i;

            for (i = 0; i < DSP_BUFS; i++)
                if ((dsp.hdr[i].dwFlags & WHDR_PREPARED) && !(dsp.hdr[i].dwFlags & WHDR_DONE))
                    queued += dsp.hdr[i].dwBufferLength;
            if ((!(h->dwFlags & WHDR_PREPARED) || (h->dwFlags & WHDR_DONE)) &&
                (!queued || queued + (long)len <= target))
                break;
            WaitForSingleObject(dsp.ev, 5);
        }
        if (h->dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(dsp.wo, h, sizeof(*h));
        memcpy(h->lpData, buf + done, len);
        h->dwBufferLength = (DWORD)len;
        h->dwFlags = 0;
        waveOutPrepareHeader(dsp.wo, h, sizeof(*h));
        waveOutWrite(dsp.wo, h, sizeof(*h));
        dsp.next = (dsp.next + 1) % DSP_BUFS;
        done += len;
    }
    return (long)n;
}

static void dsp_close(void)
{
    int i;

    if (dsp.wo) {
        waveOutReset(dsp.wo);
        for (i = 0; i < DSP_BUFS; i++) {
            if (dsp.hdr[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(dsp.wo, &dsp.hdr[i], sizeof(WAVEHDR));
            free(dsp.hdr[i].lpData);
        }
        waveOutClose(dsp.wo);
        CloseHandle(dsp.ev);
        dsp.wo = NULL;
    }
    dsp.bps = 0;
    dsp.fd = -1;
}

/* ---- device files ------------------------------------------------------------------------------------ */

#define FD_MAX 16
static int machine_fd[FD_MAX], nmachine_fd;
static int mixer_fd = -1;
static int null_fd[FD_MAX], nnull_fd;

static int is_machine_fd(int fd)
{
    int i;

    for (i = 0; fd >= 0 && i < nmachine_fd; i++)
        if (machine_fd[i] == fd)
            return 1;
    return 0;
}

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

int core_route_open(const char *path, int *fd)
{
    if (!path)
        return 0;
    if (machine && machine->handles(path)) {
        *fd = machine->open(path);
        if (*fd >= 0 && !is_machine_fd(*fd) && nmachine_fd < FD_MAX)
            machine_fd[nmachine_fd++] = *fd;
        if (*fd < 0)
            SET_ERRNO(lx_errno_from_crt(errno));
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
        if (dsp.fd >= 0)
            dsp_close();
        *fd = dsp_open();
        return 1;
    }
    /*
     * IGS_NULL_DEVS: optional peripherals (lamp boards...).  Writes are
     * dropped and reads fail with EAGAIN.
     */
    if (null_dev(path)) {
        *fd = core_placeholder_fd();
        if (*fd >= 0 && nnull_fd < FD_MAX)
            null_fd[nnull_fd++] = *fd;
        return 1;
    }
    return 0;
}

int core_ioctl(int fd, unsigned long req, void *arg, int *ret)
{
    if (is_machine_fd(fd)) {
        *ret = machine->ioctl(fd, req, arg);
        if (*ret < 0)
            SET_ERRNO(lx_errno_from_crt(errno));
        return 1;
    }
    if (fd >= 0 && fd == mixer_fd && ((req >> 8) & 0xff) == 'M') {
        if ((req & 0x80000000u) && arg)  /* reads: no channels, volume 0 */
            *(int *)arg = 0;
        *ret = 0;
        return 1;
    }
    if (fd >= 0 && fd == dsp.fd) {
        if (arg && ((req >> 8) & 0xff) == 'P')
            dsp_ioctl(req, arg);
        *ret = 0;
        return 1;
    }
    if (is_null_fd(fd)) {
        *ret = 0;
        return 1;
    }
    return 0;
}

int core_mmap(int fd, size_t len, int64_t off, void **ret)
{
    if (!is_machine_fd(fd))
        return 0;
    *ret = machine->mmap_at ? machine->mmap_at(fd, len, (uint32_t)off) : machine->mmap(fd, len);
    return 1;
}

int core_seek(int fd, long off, int whence, long *ret)
{
    if (!nmachine_fd || !machine->seek || !is_machine_fd(fd))
        return 0;
    *ret = machine->seek(fd, off, whence);
    if (*ret < 0)
        SET_ERRNO(lx_errno_from_crt(errno));
    return 1;
}

int core_read(int fd, void *buf, size_t n, long *ret)
{
    if (nnull_fd && is_null_fd(fd)) {
        SET_ERRNO(11);                   /* EAGAIN */
        *ret = -1;
        return 1;
    }
    if (nmachine_fd && machine->read && is_machine_fd(fd)) {
        *ret = machine->read(fd, buf, n);
        if (*ret < 0)
            SET_ERRNO(lx_errno_from_crt(errno));
        return 1;
    }
    return 0;
}

int core_write(int fd, const void *buf, size_t n, long *ret)
{
    if (fd >= 0 && fd == dsp.fd) {
        *ret = dsp_write(buf, n);
        return 1;
    }
    if (nmachine_fd && machine->write && is_machine_fd(fd)) {
        *ret = machine->write(fd, buf, n);
        return 1;
    }
    return 0;
}

int core_close(int fd)
{
    int i;

    for (i = 0; i < nnull_fd; i++)
        if (null_fd[i] == fd)
            null_fd[i] = null_fd[--nnull_fd];
    if (is_machine_fd(fd)) {
        machine->close(fd);  /* the fd stays reserved for the device */
        return 1;
    }
    if (fd >= 0 && fd == mixer_fd)
        mixer_fd = -1;
    if (fd >= 0 && fd == dsp.fd)
        dsp_close();
    return 0;
}

/* files the cabinet's system provides (igs_machine.vfile) */
const char *core_vfile(const char *name)
{
    return name && machine && machine->vfile ? machine->vfile(name) : NULL;
}

/* data files the machine wants to know about (e.g. which stage is loading) */
void core_note_fopen(const char *path)
{
    if (path && machine && machine->file_opened)
        machine->file_opened(path);
}

/* ---- start-up: load the machine plugin ------------------------------------------------------------ */

static void core_log_fn(const char *fmt, ...)
{
    va_list ap;

    EnterCriticalSection(&log_lock);
    va_start(ap, fmt);
    fputs("[igs] ", stderr);
    __mingw_vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
    LeaveCriticalSection(&log_lock);
}

void core_init(void)
{
    const char *path = getenv("IGS_MACHINE");
    igs_machine_entry_fn entry;
    HMODULE h;

    core.version = IGS_API_VERSION;
    core.log = core_log_fn;
    core.verbose = getenv("IGS_VERBOSE") != NULL;
    core.env = lx_env;
    core.env_long = lx_env_long;
    core.now = lx_now;
    core.placeholder_fd = core_placeholder_fd;
    core.mmio_map = core_mmio_map;
    core.fill = core_fill;
    core.text = core_text;
    core.font_w = FONT_W;
    core.font_h = FONT_H;
    core.present = sdl_present;
    if (!path)
        return;
    h = LoadLibraryA(path);
    entry = h ? (igs_machine_entry_fn)(void *)GetProcAddress(h, IGS_MACHINE_ENTRY) : NULL;
    machine = entry ? entry(&core) : NULL;
    if (!machine || machine->version != IGS_API_VERSION) {
        LOG("cannot load machine %s: %s (error %lu)", path, h ? "bad plugin" : "LoadLibrary failed",
            GetLastError());
        machine = NULL;
        return;
    }
    LOG("machine %s", machine->name);
}

void log_init(void)
{
    InitializeCriticalSection(&log_lock);
}
