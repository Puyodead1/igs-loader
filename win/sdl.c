/*
 * IGS loader, Windows core: SDL 1.2, natively on Win32.
 *
 * What the games and the cabinet's SDL_mixer import: an OpenGL window (WGL),
 * keyboard events with SDL 1.2 key symbols, audio through waveOut with the
 * SDL 1.2 callback model, RWops, WAV loading and format conversion, threads,
 * timers, shared-object loading (the ELF loader).  Structures are laid out
 * as SDL 1.2 on i386 Linux (SDL_Event, SDL_AudioSpec, SDL_RWops,
 * SDL_Surface, SDL_VideoInfo, SDL_AudioCVT).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>

#include "lx.h"

void core_caption(const char *t);
void *gl_get_proc_address(const char *name);

/* ---- errors -------------------------------------------------------------------------------- */

static __thread char sdl_err[512];

static void l_SDL_SetError(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    __mingw_vsnprintf(sdl_err, sizeof(sdl_err), fmt, ap);
    va_end(ap);
    if (lx_verbose)
        LOG("SDL error: %s", sdl_err);
}

static char *l_SDL_GetError(void) { return sdl_err; }
static void l_SDL_ClearError(void) { sdl_err[0] = 0; }

static void l_SDL_Error(int code)
{
    static const char *const msg[] = {
        "Out of memory", "Error reading from datastream", "Error writing to datastream",
        "Error seeking in datastream", "Unknown SDL error",
    };

    l_SDL_SetError("%s", msg[code >= 0 && code < 5 ? code : 4]);
}

/* ---- init, time --------------------------------------------------------------------------------- */

static double t_init = -1;
static uint32_t inited;

static uint32_t l_SDL_GetTicks(void)
{
    if (t_init < 0)
        t_init = lx_now();
    return (uint32_t)((lx_now() - t_init) * 1000);
}

static void l_SDL_Delay(uint32_t ms) { lx_usleep((int64_t)ms * 1000); }

static int l_SDL_Init(uint32_t flags)
{
    if (t_init < 0)
        t_init = lx_now();
    inited |= flags;
    LOG("SDL_Init(0x%x)", flags);
    return 0;
}

static int l_SDL_InitSubSystem(uint32_t flags) { inited |= flags; return 0; }
static void l_SDL_QuitSubSystem(uint32_t flags) { inited &= ~flags; }
static uint32_t l_SDL_WasInit(uint32_t flags) { return inited & flags; }
static void l_SDL_CloseAudio(void);
static void l_SDL_Quit(void)
{
    LOG("SDL_Quit");
    l_SDL_CloseAudio();
}

static const uint8_t sdl_version[3] = { 1, 2, 13 };
static const uint8_t *l_SDL_Linked_Version(void) { return sdl_version; }

/* ---- the event queue ----------------------------------------------------------------------------- */

#define EV_SIZE 20
#define EV_MAX 256
static uint8_t evq[EV_MAX][EV_SIZE];
static int ev_head, ev_tail;
static CRITICAL_SECTION ev_lock;
static uint8_t keystate[512];
static int modstate, key_repeat;

static void ev_push(const uint8_t *ev)
{
    EnterCriticalSection(&ev_lock);
    if ((ev_tail + 1) % EV_MAX != ev_head) {
        memcpy(evq[ev_tail], ev, EV_SIZE);
        ev_tail = (ev_tail + 1) % EV_MAX;
    }
    LeaveCriticalSection(&ev_lock);
}

static int ev_pop(uint8_t *ev, int remove)
{
    int r = 0;

    EnterCriticalSection(&ev_lock);
    if (ev_head != ev_tail) {
        if (ev)
            memcpy(ev, evq[ev_head], EV_SIZE);
        if (remove)
            ev_head = (ev_head + 1) % EV_MAX;
        r = 1;
    }
    LeaveCriticalSection(&ev_lock);
    return r;
}

/* Windows virtual key -> SDL 1.2 key symbol */
static int vk_sdl(WPARAM vk, LPARAM lp)
{
    int ext = (lp >> 24) & 1, sc = (lp >> 16) & 0xff;

    if (vk >= 'A' && vk <= 'Z')
        return (int)vk + 32;
    if (vk >= '0' && vk <= '9')
        return (int)vk;
    if (vk >= VK_F1 && vk <= VK_F15)
        return 282 + (int)(vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return 256 + (int)(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_BACK: return 8;
    case VK_TAB: return 9;
    case VK_CLEAR: return 12;
    case VK_RETURN: return ext ? 271 : 13;
    case VK_PAUSE: return 19;
    case VK_ESCAPE: return 27;
    case VK_SPACE: return 32;
    case VK_OEM_7: return 39;
    case VK_OEM_COMMA: return 44;
    case VK_OEM_MINUS: return 45;
    case VK_OEM_PERIOD: return 46;
    case VK_OEM_2: return 47;
    case VK_OEM_1: return 59;
    case VK_OEM_PLUS: return 61;
    case VK_OEM_4: return 91;
    case VK_OEM_5: return 92;
    case VK_OEM_6: return 93;
    case VK_OEM_3: return 96;
    case VK_DELETE: return 127;
    case VK_DECIMAL: return 266;
    case VK_DIVIDE: return 267;
    case VK_MULTIPLY: return 268;
    case VK_SUBTRACT: return 269;
    case VK_ADD: return 270;
    case VK_UP: return 273;
    case VK_DOWN: return 274;
    case VK_RIGHT: return 275;
    case VK_LEFT: return 276;
    case VK_INSERT: return 277;
    case VK_HOME: return 278;
    case VK_END: return 279;
    case VK_PRIOR: return 280;
    case VK_NEXT: return 281;
    case VK_NUMLOCK: return 300;
    case VK_CAPITAL: return 301;
    case VK_SCROLL: return 302;
    case VK_SHIFT: return sc == 0x36 ? 303 : 304;
    case VK_CONTROL: return ext ? 305 : 306;
    case VK_MENU: return ext ? 307 : 308;
    case VK_LWIN: return 311;
    case VK_RWIN: return 312;
    case VK_SNAPSHOT: return 316;
    case VK_APPS: return 319;
    }
    return 0;
}

static int mod_bit(int sym)
{
    switch (sym) {
    case 304: return 0x0001;
    case 303: return 0x0002;
    case 306: return 0x0040;
    case 305: return 0x0080;
    case 308: return 0x0100;
    case 307: return 0x0200;
    }
    return 0;
}

/* a key event, as from the keyboard (also IGS_SCRIPT through core_key? no: machine only) */
static void key_event(int sym, int down, int scancode)
{
    uint8_t ev[EV_SIZE] = { 0 };

    if (sym <= 0 || sym >= 512)
        return;
    keystate[sym] = down;
    if (down)
        modstate |= mod_bit(sym);
    else
        modstate &= ~mod_bit(sym);
    ev[0] = down ? 2 : 3;           /* SDL_KEYDOWN / SDL_KEYUP */
    ev[2] = down;
    ev[4] = (uint8_t)scancode;
    memcpy(ev + 8, &sym, 4);
    memcpy(ev + 12, &modstate, 4);
    if (sym < 128) {
        uint16_t u = (uint16_t)sym;

        memcpy(ev + 16, &u, 2);
    }
    ev_push(ev);
}

/* ---- video ------------------------------------------------------------------------------------------ */

static HWND hwnd;
static HDC hdc;
static HGLRC hglrc;
static int gl_attr[20] = { 3, 3, 2, 0, 0, 1, 16, 0 };
static int cursor_shown = 1, mouse_buttons, closes, fullscreen_mode;
static uint8_t surface[60], pixfmt[40], videoinfo[20];
static char caption[256];

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE: {
        uint8_t ev[EV_SIZE] = { 12 };     /* SDL_QUIT */

        if (++closes >= 2) {
            LOG("window closed twice: exiting");
            l_exit(0);
        }
        LOG("window close: SDL_QUIT sent (close again to force)");
        ev_push(ev);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((lp & (1 << 30)) && !key_repeat)
            return 0;
        if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
            break;                        /* Alt+F4 */
        key_event(vk_sdl(wp, lp), 1, (lp >> 16) & 0xff);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        key_event(vk_sdl(wp, lp), 0, (lp >> 16) & 0xff);
        return 0;
    case WM_SYSCHAR:
    case WM_CHAR:
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 1 :
                (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 2 : 3;
        int down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        uint8_t ev[EV_SIZE] = { 0 };
        uint16_t x = (uint16_t)(short)LOWORD(lp), y = (uint16_t)(short)HIWORD(lp);

        if (down)
            mouse_buttons |= 1 << (b - 1);
        else
            mouse_buttons &= ~(1 << (b - 1));
        ev[0] = down ? 5 : 6;
        ev[2] = (uint8_t)b;
        ev[3] = (uint8_t)down;
        memcpy(ev + 4, &x, 2);
        memcpy(ev + 6, &y, 2);
        ev_push(ev);
        return 0;
    }
    case WM_SETCURSOR:
        if (!cursor_shown && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_KILLFOCUS: {
        int s;

        /* release whatever was held, as the keys come up elsewhere */
        for (s = 1; s < 512; s++)
            if (keystate[s])
                key_event(s, 0, 0);
        break;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void pump(void)
{
    MSG m;

    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

static void fill_pixfmt(int bpp)
{
    memset(pixfmt, 0, sizeof(pixfmt));
    pixfmt[4] = (uint8_t)bpp;
    pixfmt[5] = (uint8_t)(bpp / 8);
    *(uint32_t *)(pixfmt + 16) = 0x00ff0000;
    *(uint32_t *)(pixfmt + 20) = 0x0000ff00;
    *(uint32_t *)(pixfmt + 24) = 0x000000ff;
    pixfmt[10] = 16;
    pixfmt[11] = 8;
    pixfmt[12] = 0;
    pixfmt[36] = 255;
}

static void *l_SDL_GetVideoInfo(void)
{
    uint32_t flags = 1 | 2;               /* hw_available, wm_available */
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);

    fill_pixfmt(32);
    memcpy(videoinfo, &flags, 4);
    *(uint32_t *)(videoinfo + 4) = 256 * 1024;
    *(void **)(videoinfo + 8) = pixfmt;
    memcpy(videoinfo + 12, &w, 4);
    memcpy(videoinfo + 16, &h, 4);
    return videoinfo;
}

static void *l_SDL_GetVideoSurface(void) { return hwnd ? surface : NULL; }

static int make_context(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    BOOL (WINAPI *swap_interval)(int);

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | (gl_attr[5] ? PFD_DOUBLEBUFFER : 0);
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = (BYTE)gl_attr[3];
    pfd.cDepthBits = (BYTE)(gl_attr[6] ? gl_attr[6] : 16);
    pfd.cStencilBits = (BYTE)gl_attr[7];
    pfd.iLayerType = PFD_MAIN_PLANE;
    hdc = GetDC(hwnd);
    pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf || !SetPixelFormat(hdc, pf, &pfd)) {
        l_SDL_SetError("no OpenGL pixel format (error %lu)", GetLastError());
        return -1;
    }
    hglrc = wglCreateContext(hdc);
    if (!hglrc || !wglMakeCurrent(hdc, hglrc)) {
        l_SDL_SetError("cannot create an OpenGL context (error %lu)", GetLastError());
        return -1;
    }
    DescribePixelFormat(hdc, pf, sizeof(pfd), &pfd);
    LOG("OpenGL: %s, %s, %s; depth %d stencil %d alpha %d",
        (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
        (const char *)glGetString(GL_VERSION), pfd.cDepthBits, pfd.cStencilBits, pfd.cAlphaBits);
    /* the frame cap paces the game (IGS_VSYNC=1: also wait for vblank) */
    *(void **)&swap_interval = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval)
        swap_interval(lx_env_long("IGS_VSYNC", 0) ? 1 : 0);
    return 0;
}

static void *l_SDL_SetVideoMode(int w, int h, int bpp, uint32_t flags)
{
    static int registered;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, w, h };
    int fs = (flags & 0x80000000u) && lx_env("IGS_FULLSCREEN", NULL);
    uint16_t pitch;

    LOG("video mode %dx%d bpp %d flags 0x%x%s", w, h, bpp, flags, fs ? " (fullscreen)" : "");
    if (!registered) {
        WNDCLASSA wc;

        memset(&wc, 0, sizeof(wc));
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon = LoadIcon(GetModuleHandleA(NULL), MAKEINTRESOURCE(1));
        wc.lpszClassName = "IGSLoader";
        RegisterClassA(&wc);
        registered = 1;
    }
    if (fs) {
        DEVMODEA dm;

        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = w;
        dm.dmPelsHeight = h;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        if (ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL) {
            fullscreen_mode = 1;
            style = WS_POPUP;
        } else {
            LOG("fullscreen %dx%d not available: windowed", w, h);
        }
    }
    if (!hwnd) {
        AdjustWindowRect(&r, style, FALSE);
        hwnd = CreateWindowExA(0, "IGSLoader", caption[0] ? caption : lx_env("IGS_NAME", "IGS"),
                               style, fs ? 0 : CW_USEDEFAULT, fs ? 0 : CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top, NULL, NULL,
                               GetModuleHandleA(NULL), NULL);
        if (!hwnd) {
            l_SDL_SetError("cannot create a window (error %lu)", GetLastError());
            return NULL;
        }
        /* IGS_BACKGROUND=1: open without taking the keyboard focus (test runs) */
        ShowWindow(hwnd, lx_env("IGS_BACKGROUND", NULL) ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (make_context())
            return NULL;
        core_set_window(hwnd);
        core_gl_ready();
    } else {
        AdjustWindowRect(&r, style, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    pump();
    fill_pixfmt(bpp ? bpp : 32);
    memset(surface, 0, sizeof(surface));
    *(uint32_t *)surface = flags & ~(fs ? 0 : 0x80000000u);
    *(void **)(surface + 4) = pixfmt;
    memcpy(surface + 8, &w, 4);
    memcpy(surface + 12, &h, 4);
    pitch = (uint16_t)(w * (bpp ? bpp : 32) / 8);
    memcpy(surface + 16, &pitch, 2);
    *(uint16_t *)(surface + 36) = (uint16_t)w;
    *(uint16_t *)(surface + 38) = (uint16_t)h;
    *(int32_t *)(surface + 56) = 1;
    return surface;
}

static void *l_SDL_ListModes(void *fmt, uint32_t flags) { (void)fmt; (void)flags; return (void *)-1; }
static int l_SDL_VideoModeOK(int w, int h, int bpp, uint32_t f) { (void)w; (void)h; (void)f; return bpp ? bpp : 32; }
static char *l_SDL_VideoDriverName(char *buf, int n) { snprintf(buf, n, "windib"); return buf; }
static char *l_SDL_AudioDriverName(char *buf, int n) { snprintf(buf, n, "waveout"); return buf; }

static int l_SDL_GL_SetAttribute(int attr, int v)
{
    if (attr >= 0 && attr < 20)
        gl_attr[attr] = v;
    return 0;
}

static int l_SDL_GL_GetAttribute(int attr, int *v)
{
    *v = attr >= 0 && attr < 20 ? gl_attr[attr] : 0;
    return 0;
}

static int l_SDL_GL_LoadLibrary(const char *p) { (void)p; return 0; }
static void *l_SDL_GL_GetProcAddress(const char *name) { return gl_get_proc_address(name); }

static void l_SDL_GL_SwapBuffers(void)
{
    pump();
    core_frame();
    SwapBuffers(hdc);
}

/*
 * igs_core.present: a board that scans out its own frame buffer (E1000; the
 * game sets no video mode).  The window opens at the first frame; each frame
 * is an RGB565 texture, then the overlay, the frame cap and the swap.  The
 * game polls no events: the keys go to the machine from the queue here.
 */
void sdl_present(const void *px, int w, int h, int pitch)
{
    static GLuint tex;
    static int tw, th;
    uint8_t e[EV_SIZE];

    if (!hwnd) {
        if (!l_SDL_SetVideoMode(w, h, 32, 2)) {         /* SDL_OPENGL */
            LOG("cannot open the window for the frame buffer: %s", sdl_err);
            l_exit(1);
        }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, 0x8363, NULL);   /* 5_6_5 */
        tw = w;
        th = h;
    }
    pump();
    while (ev_pop(e, 1)) {
        if (e[0] == 2 || e[0] == 3) {
            int sym;

            memcpy(&sym, e + 8, 4);
            core_key(sym, e[0] == 2);
        } else if (e[0] == 12) {                          /* SDL_QUIT */
            LOG("window closed");
            l_exit(0);
        }
    }
    glViewport(0, 0, tw, th);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w < tw ? w : tw, h < th ? h : th, GL_RGB, 0x8363, px);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, 1);
    glTexCoord2f(1, 0); glVertex2f(1, 1);
    glTexCoord2f(1, 1); glVertex2f(1, -1);
    glTexCoord2f(0, 1); glVertex2f(-1, -1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    core_frame();
    SwapBuffers(hdc);
}

int x11_window_title(const char *s);

/* the core's window title: on the SDL window, else on the X11 game's */
int sdl_window_title(const char *s)
{
    if (!hwnd)
        return x11_window_title(s);
    SetWindowTextA(hwnd, s);
    return 0;
}

static void l_SDL_WM_SetCaption(const char *title, const char *icon)
{
    (void)icon;
    if (title)
        snprintf(caption, sizeof(caption), "%s", title);
    core_caption(caption);
}

static void l_SDL_WM_GetCaption(char **t, char **i)
{
    if (t)
        *t = caption;
    if (i)
        *i = caption;
}

static int l_SDL_WM_GrabInput(int mode) { return mode == -1 ? 0 : mode; }
static int l_SDL_WM_IconifyWindow(void) { return 0; }
static int l_SDL_WM_ToggleFullScreen(void *s) { (void)s; return 0; }
static void l_SDL_WM_SetIcon(void *i, void *m) { (void)i; (void)m; }

static int l_SDL_ShowCursor(int t)
{
    int prev = cursor_shown;

    if (t >= 0)
        cursor_shown = t;
    return prev;
}

static uint8_t l_SDL_GetMouseState(int *x, int *y)
{
    POINT p = { 0, 0 };

    if (hwnd) {
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
    }
    if (x)
        *x = p.x;
    if (y)
        *y = p.y;
    return (uint8_t)mouse_buttons;
}

static void l_SDL_WarpMouse(uint16_t x, uint16_t y) { (void)x; (void)y; }

static void l_SDL_PumpEvents(void) { pump(); }

static int l_SDL_PollEvent(uint8_t *ev)
{
    uint8_t e[EV_SIZE];

    pump();
    if (!ev_pop(e, ev != NULL))
        return 0;
    if (ev) {
        memcpy(ev, e, EV_SIZE);
        if (e[0] == 2 || e[0] == 3) {
            int sym;

            memcpy(&sym, e + 8, 4);
            core_key(sym, e[0] == 2);
        }
    }
    return 1;
}

static int l_SDL_WaitEvent(uint8_t *ev)
{
    while (!l_SDL_PollEvent(ev))
        lx_usleep(5000);
    return 1;
}

static int l_SDL_PushEvent(const uint8_t *ev) { ev_push(ev); return 0; }
static uint8_t *l_SDL_GetKeyState(int *n) { if (n) *n = 323; return keystate; }
static int l_SDL_GetModState(void) { return modstate; }
static int l_SDL_EnableKeyRepeat(int d, int i) { (void)i; key_repeat = d > 0; return 0; }
static int l_SDL_EnableUNICODE(int e) { (void)e; return 0; }
static uint8_t l_SDL_GetAppState(void) { return 7; }

/* ---- RWops (32 bytes: seek read write close, type, hidden) --------------------------------- */

struct rw {
    void *seek, *read, *write, *close;
    uint32_t type;
    union {
        struct { int autoclose; FILE *fp; } stdio;
        struct { uint8_t *base, *here, *stop; } mem;
    } h;
};
_Static_assert(sizeof(struct rw) == 32, "SDL_RWops");

static int rw_seek(struct rw *rw, int off, int wh) { return (int)elf_call3(rw->seek, U32(rw), off, wh); }
static int rw_read(struct rw *rw, void *p, int s, int n) { return (int)elf_call4(rw->read, U32(rw), U32(p), s, n); }
static int rw_write(struct rw *rw, const void *p, int s, int n) { return (int)elf_call4(rw->write, U32(rw), U32(p), s, n); }
static int rw_close(struct rw *rw) { return (int)elf_call1(rw->close, U32(rw)); }

static void *l_SDL_AllocRW(void) { struct rw *rw = l_malloc(sizeof(*rw)); if (!rw) l_SDL_Error(0); return rw; }
static void l_SDL_FreeRW(void *rw) { l_free(rw); }

static int mem_seek(struct rw *rw, int off, int wh)
{
    uint8_t *p = wh == 0 ? rw->h.mem.base + off : wh == 1 ? rw->h.mem.here + off : rw->h.mem.stop + off;

    if (p < rw->h.mem.base)
        p = rw->h.mem.base;
    if (p > rw->h.mem.stop)
        p = rw->h.mem.stop;
    rw->h.mem.here = p;
    return (int)(p - rw->h.mem.base);
}

static int mem_read(struct rw *rw, void *dst, int size, int n)
{
    int avail = (int)(rw->h.mem.stop - rw->h.mem.here), total = size * n;

    if (size <= 0 || n <= 0)
        return 0;
    if (total > avail)
        total = avail - avail % size;
    memcpy(dst, rw->h.mem.here, total);
    rw->h.mem.here += total;
    return total / size;
}

static int mem_write(struct rw *rw, const void *src, int size, int n)
{
    int avail = (int)(rw->h.mem.stop - rw->h.mem.here), total = size * n;

    if (size <= 0 || n <= 0)
        return 0;
    if (total > avail)
        total = avail - avail % size;
    memcpy(rw->h.mem.here, src, total);
    rw->h.mem.here += total;
    return total / size;
}

static int mem_write_ro(struct rw *rw, const void *src, int size, int n)
{
    (void)rw; (void)src; (void)size; (void)n;
    l_SDL_SetError("Can't write to read-only memory");
    return -1;
}

static int mem_close(struct rw *rw) { l_free(rw); return 0; }

static void *l_SDL_RWFromMem(void *mem, int size)
{
    struct rw *rw = l_SDL_AllocRW();

    if (!rw)
        return NULL;
    rw->seek = mem_seek;
    rw->read = mem_read;
    rw->write = mem_write;
    rw->close = mem_close;
    rw->type = 4;
    rw->h.mem.base = rw->h.mem.here = mem;
    rw->h.mem.stop = (uint8_t *)mem + size;
    return rw;
}

static void *l_SDL_RWFromConstMem(const void *mem, int size)
{
    struct rw *rw = l_SDL_RWFromMem((void *)mem, size);

    if (rw)
        rw->write = mem_write_ro;
    return rw;
}

static int file_seek(struct rw *rw, int off, int wh)
{
    if (fseek(rw->h.stdio.fp, off, wh)) {
        l_SDL_Error(3);
        return -1;
    }
    return (int)ftell(rw->h.stdio.fp);
}
static int file_read(struct rw *rw, void *p, int s, int n) { return (int)fread(p, s, n, rw->h.stdio.fp); }
static int file_write(struct rw *rw, const void *p, int s, int n) { return (int)fwrite(p, s, n, rw->h.stdio.fp); }
static int file_close(struct rw *rw)
{
    if (rw->h.stdio.autoclose)
        fclose(rw->h.stdio.fp);
    l_free(rw);
    return 0;
}

static void *rw_from_crt(FILE *f, int autoclose)
{
    struct rw *rw = l_SDL_AllocRW();

    if (!rw)
        return NULL;
    rw->seek = file_seek;
    rw->read = file_read;
    rw->write = file_write;
    rw->close = file_close;
    rw->type = 2;
    rw->h.stdio.autoclose = autoclose;
    rw->h.stdio.fp = f;
    return rw;
}

static void *l_SDL_RWFromFile(const char *file, const char *mode)
{
    char win[MAX_PATH], m[8];
    FILE *f;

    snprintf(m, sizeof(m), "%.5s%s", mode, strchr(mode, 'b') ? "" : "b");
    if (!file || lx_path(file, win, sizeof(win)) || !(f = fopen(win, m))) {
        l_SDL_SetError("Couldn't open %s", file ? file : "(null)");
        return NULL;
    }
    core_note_fopen(file);
    return rw_from_crt(f, 1);
}

static void *l_SDL_RWFromFP(LFILE *lf, int autoclose) { return rw_from_crt(lfile_crt(lf), autoclose); }

static uint16_t l_SDL_ReadLE16(struct rw *rw) { uint16_t v = 0; rw_read(rw, &v, 2, 1); return v; }
static uint16_t l_SDL_ReadBE16(struct rw *rw) { uint16_t v = 0; rw_read(rw, &v, 2, 1); return (uint16_t)(v >> 8 | v << 8); }
static uint32_t l_SDL_ReadLE32(struct rw *rw) { uint32_t v = 0; rw_read(rw, &v, 4, 1); return v; }
static uint32_t l_SDL_ReadBE32(struct rw *rw) { uint32_t v = 0; rw_read(rw, &v, 4, 1); return __builtin_bswap32(v); }
static int l_SDL_WriteLE16(struct rw *rw, uint16_t v) { return rw_write(rw, &v, 2, 1); }
static int l_SDL_WriteBE16(struct rw *rw, uint16_t v) { v = (uint16_t)(v >> 8 | v << 8); return rw_write(rw, &v, 2, 1); }
static int l_SDL_WriteLE32(struct rw *rw, uint32_t v) { return rw_write(rw, &v, 4, 1); }
static int l_SDL_WriteBE32(struct rw *rw, uint32_t v) { v = __builtin_bswap32(v); return rw_write(rw, &v, 4, 1); }

/* ---- shared objects, threads, locks ----------------------------------------------------------- */

static void *l_SDL_LoadObject(const char *f)
{
    void *h = elf_dlopen(f, 0);

    if (!h)
        l_SDL_SetError("Failed loading %s: %s", f, elf_dlerror());
    return h;
}

static void *l_SDL_LoadFunction(void *h, const char *n)
{
    void *p = elf_dlsym(h, n);

    if (!p)
        l_SDL_SetError("Failed loading %s", n);
    return p;
}

static void l_SDL_UnloadObject(void *h) { (void)h; }

static void *l_SDL_CreateThread(void *fn, void *data) { return lx_thread_create(fn, data, 0); }
static void l_SDL_WaitThread(void *t, int *status) { lx_thread_wait(t, status); }
static void l_SDL_KillThread(void *t) { lx_thread_kill(t); }
static uint32_t l_SDL_ThreadID(void) { return GetCurrentThreadId(); }
static uint32_t l_SDL_GetThreadID(void *t) { return lx_thread_id(t); }

static void *l_SDL_CreateMutex(void)
{
    CRITICAL_SECTION *cs = malloc(sizeof(*cs));

    InitializeCriticalSection(cs);
    return cs;
}
static void l_SDL_DestroyMutex(void *m) { if (m) { DeleteCriticalSection(m); free(m); } }
static int l_SDL_mutexP(void *m) { EnterCriticalSection(m); return 0; }
static int l_SDL_mutexV(void *m) { LeaveCriticalSection(m); return 0; }

static void *l_SDL_CreateSemaphore(uint32_t v) { return CreateSemaphoreA(NULL, v, 0x7fffffff, NULL); }
static void l_SDL_DestroySemaphore(void *s) { if (s) CloseHandle(s); }
static int l_SDL_SemWait(void *s) { return WaitForSingleObject(s, INFINITE) == WAIT_OBJECT_0 ? 0 : -1; }
static int l_SDL_SemTryWait(void *s) { return WaitForSingleObject(s, 0) == WAIT_OBJECT_0 ? 0 : 1; }
static int l_SDL_SemWaitTimeout(void *s, uint32_t ms) { return WaitForSingleObject(s, ms) == WAIT_OBJECT_0 ? 0 : 1; }
static int l_SDL_SemPost(void *s) { return ReleaseSemaphore(s, 1, NULL) ? 0 : -1; }

static void *l_SDL_CreateCond(void) { CONDITION_VARIABLE *c = malloc(sizeof(*c)); InitializeConditionVariable(c); return c; }
static void l_SDL_DestroyCond(void *c) { free(c); }
static int l_SDL_CondSignal(void *c) { WakeConditionVariable(c); return 0; }
static int l_SDL_CondBroadcast(void *c) { WakeAllConditionVariable(c); return 0; }
static int l_SDL_CondWait(void *c, void *m) { return SleepConditionVariableCS(c, m, INFINITE) ? 0 : -1; }
static int l_SDL_CondWaitTimeout(void *c, void *m, uint32_t ms) { return SleepConditionVariableCS(c, m, ms) ? 0 : 1; }

/* ---- audio ---------------------------------------------------------------------------------------- */

/* SDL_AudioSpec: freq 0, format 4, channels 6, silence 7, samples 8, size 12, callback 16, userdata 20 */
#define NBUF 3
static struct {
    int open, freq, fmt, ch, samples, size, bufsize, quit;
    volatile int paused;
    uint8_t silence;
    void *cb, *ud;
    HWAVEOUT wo;
    WAVEHDR hdr[NBUF];
    HANDLE ev, thread;
    CRITICAL_SECTION lock;
    long callbacks, nonsilent;
    int peak;
} au;

static int fmt_bytes(int f) { return (f & 0xff) / 8; }

static void audio_stats(const uint8_t *buf, int len)
{
    static double t0;
    int peak = 0, i;

    if (au.fmt == 0x8010) {
        for (i = 0; i + 1 < len; i += 2) {
            int s = abs(*(const int16_t *)(buf + i));

            if (s > peak)
                peak = s;
        }
    } else {
        for (i = 0; i < len; i++) {
            int s = abs((int)buf[i] - au.silence) * 256;

            if (s > peak)
                peak = s;
        }
    }
    au.callbacks++;
    if (peak > au.peak)
        au.peak = peak;
    if (peak > 0 && au.nonsilent++ == 0)
        LOG("audio: first non-silent buffer after %ld callbacks (peak %d)", au.callbacks, peak);
    if (lx_env("IGS_AUDIO_LOG", NULL) && lx_now() - t0 >= 5) {
        LOG("audio: %ld callbacks, %ld non-silent, peak %d", au.callbacks, au.nonsilent, au.peak);
        au.peak = 0;
        t0 = lx_now();
    }
}

static void audio_fill(uint8_t *buf)
{
    int off;

    memset(buf, au.silence, au.bufsize);
    if (au.paused)
        return;
    for (off = 0; off + au.size <= au.bufsize; off += au.size) {
        EnterCriticalSection(&au.lock);
        elf_call3(au.cb, U32(au.ud), U32(buf + off), au.size);
        LeaveCriticalSection(&au.lock);
    }
    audio_stats(buf, au.bufsize);
}

static DWORD WINAPI audio_main(void *arg)
{
    int i;

    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (!au.wo) {
        /* no audio device: the callbacks still run, at the real rate */
        double next = lx_now(), period = (double)au.bufsize / (au.freq * au.ch * fmt_bytes(au.fmt));

        while (!au.quit) {
            audio_fill((uint8_t *)au.hdr[0].lpData);
            next += period;
            lx_sleep_until(next);
        }
        return 0;
    }
    for (i = 0; i < NBUF; i++) {
        audio_fill((uint8_t *)au.hdr[i].lpData);
        waveOutWrite(au.wo, &au.hdr[i], sizeof(WAVEHDR));
    }
    while (!au.quit) {
        WaitForSingleObject(au.ev, 100);
        for (i = 0; i < NBUF && !au.quit; i++) {
            if (!(au.hdr[i].dwFlags & WHDR_DONE))
                continue;
            au.hdr[i].dwFlags &= ~WHDR_DONE;
            audio_fill((uint8_t *)au.hdr[i].lpData);
            waveOutWrite(au.wo, &au.hdr[i], sizeof(WAVEHDR));
        }
    }
    return 0;
}

static int l_SDL_OpenAudio(uint8_t *want, uint8_t *got)
{
    WAVEFORMATEX wf;
    uint16_t fmt, samples;
    int i, per;
    MMRESULT r;

    if (au.open) {
        l_SDL_SetError("Audio device is already opened");
        return -1;
    }
    memcpy(&fmt, want + 4, 2);
    memcpy(&samples, want + 8, 2);
    samples = (uint16_t)lx_env_long("IGS_AUDIO_CHUNK", samples);
    au.freq = *(int32_t *)want;
    au.ch = want[6] ? want[6] : 2;
    au.fmt = fmt == 0x0008 || fmt == 0x8010 ? fmt : 0x8010;
    au.samples = samples ? samples : 1024;
    au.silence = au.fmt == 0x0008 ? 0x80 : 0;
    au.size = au.samples * au.ch * fmt_bytes(au.fmt);
    memcpy(&au.cb, want + 16, 4);
    memcpy(&au.ud, want + 20, 4);
    /* waveOut buffers of at least ~23 ms, filled by several callbacks */
    per = (1024 + au.samples - 1) / au.samples;
    au.bufsize = au.size * (per < 1 ? 1 : per);
    want[7] = au.silence;
    *(uint32_t *)(want + 12) = au.size;
    if (got) {
        memcpy(got, want, 24);
        memcpy(got + 4, &au.fmt, 2);
        got[6] = (uint8_t)au.ch;
        memcpy(got + 8, &au.samples, 2);
    } else if (au.fmt != fmt) {
        LOG("audio: format 0x%x played as S16 without conversion", fmt);
    }
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)au.ch;
    wf.nSamplesPerSec = au.freq;
    wf.wBitsPerSample = (WORD)(fmt_bytes(au.fmt) * 8);
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    au.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    r = waveOutOpen(&au.wo, WAVE_MAPPER, &wf, (DWORD_PTR)au.ev, 0, CALLBACK_EVENT);
    LOG("audio open %d Hz fmt 0x%x ch %d samples %d (callback %d bytes, %d per buffer) -> %s",
        au.freq, au.fmt, au.ch, au.samples, au.size, au.bufsize / au.size,
        r ? "no device: silent" : "waveOut");
    if (r)                      /* the games exit without audio: play into nothing instead */
        au.wo = NULL;
    InitializeCriticalSection(&au.lock);
    for (i = 0; i < NBUF; i++) {
        memset(&au.hdr[i], 0, sizeof(WAVEHDR));
        au.hdr[i].lpData = malloc(au.bufsize);
        au.hdr[i].dwBufferLength = au.bufsize;
        if (au.wo)
            waveOutPrepareHeader(au.wo, &au.hdr[i], sizeof(WAVEHDR));
    }
    au.paused = 1;
    au.quit = 0;
    au.open = 1;
    au.thread = CreateThread(NULL, 1 << 20, audio_main, NULL, 0, NULL);
    return 0;
}

static void l_SDL_PauseAudio(int p) { au.paused = p; }
static int l_SDL_GetAudioStatus(void) { return !au.open ? 0 : au.paused ? 2 : 1; }
static void l_SDL_LockAudio(void) { if (au.open) EnterCriticalSection(&au.lock); }
static void l_SDL_UnlockAudio(void) { if (au.open) LeaveCriticalSection(&au.lock); }

static void l_SDL_CloseAudio(void)
{
    int i;

    if (!au.open)
        return;
    au.quit = 1;
    SetEvent(au.ev);
    WaitForSingleObject(au.thread, 1000);
    if (au.wo)
        waveOutReset(au.wo);
    for (i = 0; i < NBUF; i++) {
        if (au.wo)
            waveOutUnprepareHeader(au.wo, &au.hdr[i], sizeof(WAVEHDR));
        free(au.hdr[i].lpData);
    }
    if (au.wo)
        waveOutClose(au.wo);
    CloseHandle(au.ev);
    au.open = 0;
}

/* SDL 1.2's mixer: add with volume (0..128), clipped */
static void l_SDL_MixAudio(uint8_t *dst, const uint8_t *src, uint32_t len, int vol)
{
    uint32_t i;

    if (!vol)
        return;
    if (au.fmt == 0x0008) {
        for (i = 0; i < len; i++) {
            int s = ((int)dst[i] - 128) + ((((int)src[i] - 128) * vol) >> 7);

            dst[i] = (uint8_t)((s < -128 ? -128 : s > 127 ? 127 : s) + 128);
        }
        return;
    }
    for (i = 0; i + 1 < len; i += 2) {
        int s = *(int16_t *)(dst + i) + ((*(const int16_t *)(src + i) * vol) >> 7);

        *(int16_t *)(dst + i) = (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
    }
}

/* SDL_AudioCVT (84 bytes); ours keeps channels and rates in filters[0..3] */
struct cvt {
    int32_t needed;
    uint16_t src_format, dst_format;
    double rate_incr;
    uint8_t *buf;
    int32_t len, len_cvt, len_mult;
    double len_ratio;
    int32_t filters[10];
    int32_t filter_index;
} PACKED;
_Static_assert(sizeof(struct cvt) == 84, "SDL_AudioCVT");

static int l_SDL_BuildAudioCVT(struct cvt *c, uint16_t sf, uint8_t sc, int sr, uint16_t df, uint8_t dc, int dr)
{
    double ratio;

    memset(c, 0, sizeof(*c));
    c->src_format = sf;
    c->dst_format = df;
    c->filters[0] = sc;
    c->filters[1] = dc;
    c->filters[2] = sr;
    c->filters[3] = dr;
    ratio = (double)(fmt_bytes(df) * dc) / (fmt_bytes(sf) * sc) * ((double)dr / sr);
    c->rate_incr = (double)sr / dr;
    c->len_ratio = ratio;
    c->len_mult = (int)ceil(ratio) + 1;
    c->needed = sf != df || sc != dc || sr != dr;
    return c->needed ? 1 : 0;
}

static float get_sample(const uint8_t *p, int fmt)
{
    switch (fmt) {
    case 0x0008: return (p[0] - 128) / 128.0f;
    case 0x8008: return (int8_t)p[0] / 128.0f;
    case 0x0010: return ((p[0] | p[1] << 8) - 32768) / 32768.0f;
    case 0x8010: return (int16_t)(p[0] | p[1] << 8) / 32768.0f;
    case 0x1010: return ((p[1] | p[0] << 8) - 32768) / 32768.0f;
    case 0x9010: return (int16_t)(p[1] | p[0] << 8) / 32768.0f;
    }
    return 0;
}

static void put_sample(uint8_t *p, int fmt, float v)
{
    int s;

    if (v > 1)
        v = 1;
    if (v < -1)
        v = -1;
    switch (fmt) {
    case 0x0008: p[0] = (uint8_t)(v * 127 + 128); break;
    case 0x8008: p[0] = (uint8_t)(int8_t)(v * 127); break;
    default:
        s = (int)(v * 32767);
        if (!(fmt & 0x8000))
            s += 32768;
        if (fmt & 0x1000) {
            p[0] = (uint8_t)(s >> 8);
            p[1] = (uint8_t)s;
        } else {
            p[0] = (uint8_t)s;
            p[1] = (uint8_t)(s >> 8);
        }
    }
}

static int l_SDL_ConvertAudio(struct cvt *c)
{
    int sc = c->filters[0], dc = c->filters[1], sr = c->filters[2], dr = c->filters[3];
    int sb = fmt_bytes(c->src_format) * sc, db = fmt_bytes(c->dst_format) * dc;
    int frames = c->len / sb, out_frames, i, ch;
    float *in, *mid;

    if (!c->needed) {
        c->len_cvt = c->len;
        return 0;
    }
    if (!c->buf || frames <= 0) {
        c->len_cvt = 0;
        return 0;
    }
    in = malloc(sizeof(float) * frames * dc + 16);
    for (i = 0; i < frames; i++) {
        float s[8];

        for (ch = 0; ch < sc && ch < 8; ch++)
            s[ch] = get_sample(c->buf + i * sb + ch * fmt_bytes(c->src_format), c->src_format);
        for (ch = 0; ch < dc; ch++) {
            if (sc == 1)
                in[i * dc + ch] = s[0];
            else if (dc == 1)
                in[i * dc + ch] = (s[0] + s[1]) * 0.5f;
            else
                in[i * dc + ch] = s[ch < sc ? ch : sc - 1];
        }
    }
    out_frames = (int)((int64_t)frames * dr / sr);
    mid = in;
    if (sr != dr) {
        mid = malloc(sizeof(float) * out_frames * dc + 16);
        for (i = 0; i < out_frames; i++) {
            double pos = (double)i * sr / dr;
            int a = (int)pos, b = a + 1 < frames ? a + 1 : a;
            float f = (float)(pos - a);

            for (ch = 0; ch < dc; ch++)
                mid[i * dc + ch] = in[a * dc + ch] * (1 - f) + in[b * dc + ch] * f;
        }
    }
    for (i = 0; i < out_frames; i++)
        for (ch = 0; ch < dc; ch++)
            put_sample(c->buf + i * db + ch * fmt_bytes(c->dst_format), c->dst_format, mid[i * dc + ch]);
    c->len_cvt = out_frames * db;
    if (mid != in)
        free(mid);
    free(in);
    return 0;
}

static void *l_SDL_LoadWAV_RW(struct rw *src, int freesrc, uint8_t *spec, uint8_t **buf, uint32_t *len)
{
    uint8_t hdr[12], ch[8], fmt[40];
    int have_fmt = 0, ok = 0;
    uint16_t tag = 0, chans = 0, bits = 0;
    uint32_t rate = 0;

    if (!src) {
        l_SDL_SetError("no source");
        return NULL;
    }
    *buf = NULL;
    if (rw_read(src, hdr, 12, 1) != 1 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        l_SDL_SetError("Unrecognized file type (not WAVE)");
        goto out;
    }
    while (rw_read(src, ch, 8, 1) == 1) {
        uint32_t clen;

        memcpy(&clen, ch + 4, 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint32_t n = clen < sizeof(fmt) ? clen : sizeof(fmt);

            memset(fmt, 0, sizeof(fmt));
            rw_read(src, fmt, n, 1);
            if (clen > n)
                rw_seek(src, clen - n, 1);
            memcpy(&tag, fmt, 2);
            memcpy(&chans, fmt + 2, 2);
            memcpy(&rate, fmt + 4, 4);
            memcpy(&bits, fmt + 14, 2);
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt) {
                l_SDL_SetError("WAVE data before fmt");
                goto out;
            }
            if (tag == 0xfffe)
                tag = 1;                      /* WAVE_FORMAT_EXTENSIBLE: PCM in practice */
            if (tag != 1 || (bits != 8 && bits != 16)) {
                l_SDL_SetError("Unknown WAVE data format: 0x%.4x, %d bit", tag, bits);
                goto out;
            }
            *buf = l_malloc(clen ? clen : 1);
            *len = rw_read(src, *buf, 1, clen);
            ok = 1;
            break;
        } else {
            rw_seek(src, clen + (clen & 1), 1);
        }
    }
    if (!ok) {
        if (!sdl_err[0])
            l_SDL_SetError("No WAVE data");
        goto out;
    }
    memset(spec, 0, 24);
    *(int32_t *)spec = rate;
    *(uint16_t *)(spec + 4) = bits == 8 ? 0x0008 : 0x8010;
    spec[6] = (uint8_t)chans;
    *(uint16_t *)(spec + 8) = 4096;
out:
    if (freesrc)
        rw_close(src);
    if (!ok) {
        l_free(*buf);
        *buf = NULL;
        return NULL;
    }
    return spec;
}

static void l_SDL_FreeWAV(uint8_t *buf) { l_free(buf); }

static int l_SDL_putenv(const char *s) { return _putenv(s); }
static char *l_SDL_getenv(const char *n) { return getenv(n); }

const struct nsym sdl_syms[] = {
    N(SDL_SetError), N(SDL_GetError), N(SDL_ClearError), N(SDL_Error),
    N(SDL_GetTicks), N(SDL_Delay), N(SDL_Init), N(SDL_InitSubSystem), N(SDL_QuitSubSystem),
    N(SDL_WasInit), N(SDL_Quit), N(SDL_Linked_Version),
    N(SDL_GetVideoInfo), N(SDL_GetVideoSurface), N(SDL_SetVideoMode), N(SDL_ListModes),
    N(SDL_VideoModeOK), N(SDL_VideoDriverName), N(SDL_AudioDriverName),
    N(SDL_GL_SetAttribute), N(SDL_GL_GetAttribute), N(SDL_GL_LoadLibrary), N(SDL_GL_GetProcAddress),
    N(SDL_GL_SwapBuffers), N(SDL_WM_SetCaption), N(SDL_WM_GetCaption), N(SDL_WM_GrabInput),
    N(SDL_WM_IconifyWindow), N(SDL_WM_ToggleFullScreen), N(SDL_WM_SetIcon), N(SDL_ShowCursor),
    N(SDL_GetMouseState), N(SDL_WarpMouse), N(SDL_PumpEvents), N(SDL_PollEvent), N(SDL_WaitEvent),
    N(SDL_PushEvent), N(SDL_GetKeyState), N(SDL_GetModState), N(SDL_EnableKeyRepeat),
    N(SDL_EnableUNICODE), N(SDL_GetAppState),
    N(SDL_AllocRW), N(SDL_FreeRW), N(SDL_RWFromMem), N(SDL_RWFromConstMem), N(SDL_RWFromFile),
    N(SDL_RWFromFP), N(SDL_ReadLE16), N(SDL_ReadBE16), N(SDL_ReadLE32), N(SDL_ReadBE32),
    N(SDL_WriteLE16), N(SDL_WriteBE16), N(SDL_WriteLE32), N(SDL_WriteBE32),
    N(SDL_LoadObject), N(SDL_LoadFunction), N(SDL_UnloadObject),
    N(SDL_CreateThread), N(SDL_WaitThread), N(SDL_KillThread), N(SDL_ThreadID), N(SDL_GetThreadID),
    N(SDL_CreateMutex), N(SDL_DestroyMutex), N(SDL_mutexP), N(SDL_mutexV),
    NA(SDL_LockMutex, l_SDL_mutexP), NA(SDL_UnlockMutex, l_SDL_mutexV),
    N(SDL_CreateSemaphore), N(SDL_DestroySemaphore), N(SDL_SemWait), N(SDL_SemTryWait),
    N(SDL_SemWaitTimeout), N(SDL_SemPost), N(SDL_CreateCond), N(SDL_DestroyCond), N(SDL_CondSignal),
    N(SDL_CondBroadcast), N(SDL_CondWait), N(SDL_CondWaitTimeout),
    N(SDL_OpenAudio), N(SDL_PauseAudio), N(SDL_GetAudioStatus), N(SDL_LockAudio), N(SDL_UnlockAudio),
    N(SDL_CloseAudio), N(SDL_MixAudio), N(SDL_BuildAudioCVT), N(SDL_ConvertAudio),
    N(SDL_LoadWAV_RW), N(SDL_FreeWAV), N(SDL_putenv), N(SDL_getenv),
    { NULL, NULL },
};

void sdl_init(void)
{
    InitializeCriticalSection(&ev_lock);
}

void sdl_shutdown(void)
{
    if (fullscreen_mode)
        ChangeDisplaySettingsA(NULL, 0);
}
