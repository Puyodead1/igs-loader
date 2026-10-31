/*
 * IGS loader, Windows core: Xlib, Xext and GLX on Win32/WGL.
 *
 * What the X11 games use: Gold Captain's built-in SDL 1.2 (X11 video, GLX
 * through dlopen("libGL.so.1")), Ogre 1.6's GLX render system and OIS
 * (Hero of Robots).  Structures follow the i386 Xlib ABI (Display, Screen,
 * Visual, XEvent...), since the games read them through Xlib's macros.
 *
 *   - Displays: one queue of XEvents each; any number of connections (Ogre
 *     opens two, OIS a third).  ConnectionNumber is a placeholder fd that
 *     select() reports readable while events are queued.
 *   - Windows: X windows are Win32 windows owned by one UI thread, which
 *     pumps their messages and turns them into X events for every display
 *     that selected them.  Top-level windows are captioned windows, children
 *     are child windows (SDL's GL window inside its WM window).
 *   - Keyboard: X keycode = scancode + 8 (+ 0x80 extended), keysyms from the
 *     virtual keys.  Every key also goes to the machine plugin (as core.c's
 *     XNextEvent hook does); auto-repeat is dropped.
 *   - GLX 1.2 and 1.3 on WGL: visuals and FBConfigs are a few fixed RGBA
 *     configurations; a context gets its pixel format and HGLRC at its first
 *     glXMakeCurrent.  glXSwapBuffers runs the core's overlay, IGS_SHOT and
 *     frame cap first.
 *   - Grabs, pointer warps and auto-repeat changes are no-ops; extensions
 *     (XShm, XF86VidMode, RandR...) are absent; Xt/Xaw (Ogre's config
 *     dialog) are empty stand-ins.
 * See docs/IGS_LOADER.md, "Windows".
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>

#include "lx.h"

void core_caption(const char *t);

typedef uint32_t XID;

#define ROOT_WIN      0x15a
#define DEFAULT_CMAP  0x20
#define ROOT_VISUAL   0x21

/* event types and masks */
#define KeyPress 2
#define KeyRelease 3
#define ButtonPress 4
#define ButtonRelease 5
#define MotionNotify 6
#define FocusIn 9
#define FocusOut 10
#define KeymapNotify 11
#define Expose 12
#define VisibilityNotify 15
#define DestroyNotify 17
#define UnmapNotify 18
#define MapNotify 19
#define ReparentNotify 21
#define ConfigureNotify 22
#define ClientMessage 33

#define KeyPressMask        (1L << 0)
#define KeyReleaseMask      (1L << 1)
#define ButtonPressMask     (1L << 2)
#define ButtonReleaseMask   (1L << 3)
#define PointerMotionMask   (1L << 6)
#define KeymapStateMask     (1L << 14)
#define ExposureMask        (1L << 15)
#define VisibilityChangeMask (1L << 16)
#define StructureNotifyMask (1L << 17)
#define SubstructureNotifyMask (1L << 19)
#define FocusChangeMask     (1L << 21)

/* ---- Xlib structures (i386) --------------------------------------------------------------- */

typedef struct {
    void *ext_data;
    uint32_t visualid;
    int c_class;
    uint32_t red_mask, green_mask, blue_mask;
    int bits_per_rgb, map_entries;
} XVisual;

typedef struct { int depth, nvisuals; XVisual *visuals; } XDepth;

typedef struct {
    void *ext_data;
    void *display;
    XID root;
    int width, height, mwidth, mheight, ndepths;
    XDepth *depths;
    int root_depth;
    XVisual *root_visual;
    void *default_gc;
    XID cmap;
    uint32_t white_pixel, black_pixel;
    int max_maps, min_maps, backing_store, save_unders;
    long root_input_mask;
} XScreen;

typedef struct {
    XVisual *visual;
    uint32_t visualid;
    int screen, depth, c_class;
    uint32_t red_mask, green_mask, blue_mask;
    int colormap_size, bits_per_rgb;
} XVisualInfo;

typedef struct { int depth, bits_per_pixel, scanline_pad; } XPixFmt;

typedef struct {
    int x, y, width, height, border_width, depth;
    XVisual *visual;
    XID root;
    int c_class, bit_gravity, win_gravity, backing_store;
    uint32_t backing_planes, backing_pixel;
    int save_under;
    XID colormap;
    int map_installed, map_state;
    long all_event_masks, your_event_mask, do_not_propagate_mask;
    int override_redirect;
    XScreen *screen;
} XWindowAttributes;

typedef struct {
    XID background_pixmap;
    uint32_t background_pixel;
    XID border_pixmap;
    uint32_t border_pixel;
    int bit_gravity, win_gravity, backing_store;
    uint32_t backing_planes, backing_pixel;
    int save_under;
    long event_mask, do_not_propagate_mask;
    int override_redirect;
    XID colormap, cursor;
} XSetWindowAttributes;

typedef struct { unsigned char *value; XID encoding; int format; uint32_t nitems; } XTextProperty;
typedef struct { uint32_t pixel; uint16_t red, green, blue; char flags, pad; } XColor;

typedef struct XImage {
    int width, height, xoffset, format;
    char *data;
    int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel;
    uint32_t red_mask, green_mask, blue_mask;
    void *obdata;
    void *create_image, *destroy_image, *get_pixel, *put_pixel, *sub_image, *add_pixel;
} XImage;

typedef struct { int max_keypermod; uint8_t *modifiermap; } XModifierKeymap;
typedef struct { void *next; void *display; void *codes; void *data; } XExtDisplayInfo;
typedef struct { XExtDisplayInfo *head, *cur; int ndisplays; } XExtensionInfo;

_Static_assert(sizeof(XVisual) == 32, "Visual");
_Static_assert(sizeof(XScreen) == 80, "Screen");
_Static_assert(sizeof(XVisualInfo) == 40, "XVisualInfo");
_Static_assert(sizeof(XWindowAttributes) == 92, "XWindowAttributes");
_Static_assert(sizeof(XSetWindowAttributes) == 60, "XSetWindowAttributes");
_Static_assert(sizeof(XImage) == 88, "XImage");
_Static_assert(sizeof(XColor) == 12, "XColor");

/* ---- displays ------------------------------------------------------------------------------- */

#define DPY_PUBLIC 2048            /* the _XDisplay as Xlib's macros read it (lock at 1220/1232) */
#define QMAX 1024

struct qev { uint8_t ev[96]; long mask; };

struct xdpy {
    uint8_t pub[DPY_PUBLIC];
    XScreen screen;
    XPixFmt formats[3];
    int fd, closed;
    uint32_t serial;
    struct qev q[QMAX];
    int qhead, qn;
    CRITICAL_SECTION lock;
    HANDLE ev;
    struct xdpy *next;
};

#define DPY_FIELD(d, off) (*(uint32_t *)((d)->pub + (off)))

static CRITICAL_SECTION xlock;
static struct xdpy *dpys;
static XVisual root_visual = { NULL, ROOT_VISUAL, 4, 0xff0000, 0xff00, 0xff, 8, 256 };
static XDepth root_depths[1] = { { 24, 1, &root_visual } };
static XID next_xid = 0x00400001;
static int x_inited;
static void *err_handler, *ioerr_handler;

static XID x_intern(const char *name);

static XID new_xid(void)
{
    XID id;

    EnterCriticalSection(&xlock);
    id = next_xid++;
    LeaveCriticalSection(&xlock);
    return id;
}

static XID x_resource_alloc(void *dpy) { (void)dpy; return new_xid(); }

static struct xdpy *dpy_from_fd(int fd)
{
    struct xdpy *d;

    for (d = dpys; d; d = d->next)
        if (!d->closed && d->fd == fd)
            return d;
    return NULL;
}

static void q_push(struct xdpy *d, uint8_t *ev, long mask)
{
    struct qev *e;

    if (d->closed)
        return;
    EnterCriticalSection(&d->lock);
    if (d->qn == QMAX) {               /* full: drop the oldest */
        d->qhead = (d->qhead + 1) % QMAX;
        d->qn--;
    }
    e = &d->q[(d->qhead + d->qn) % QMAX];
    memcpy(e->ev, ev, 96);
    *(uint32_t *)(e->ev + 4) = ++d->serial;
    *(void **)(e->ev + 12) = d;
    e->mask = mask;
    d->qn++;
    DPY_FIELD(d, 88) = d->qn;          /* qlen */
    LeaveCriticalSection(&d->lock);
    SetEvent(d->ev);
}

/* the first queued event that match() accepts (removed if take); 1 if found */
typedef int (*q_match_fn)(const struct qev *e, void *arg);

static int q_find(struct xdpy *d, q_match_fn match, void *arg, uint8_t *out, int take)
{
    int i, found = 0;

    EnterCriticalSection(&d->lock);
    for (i = 0; i < d->qn; i++) {
        struct qev *e = &d->q[(d->qhead + i) % QMAX];

        if (match && !match(e, arg))
            continue;
        if (out)
            memcpy(out, e->ev, 96);
        if (take) {
            int j;

            for (j = i; j > 0; j--)
                d->q[(d->qhead + j) % QMAX] = d->q[(d->qhead + j - 1) % QMAX];
            d->qhead = (d->qhead + 1) % QMAX;
            d->qn--;
            DPY_FIELD(d, 88) = d->qn;
        }
        found = 1;
        break;
    }
    LeaveCriticalSection(&d->lock);
    return found;
}

static void q_wait(struct xdpy *d, q_match_fn match, void *arg, uint8_t *out, int take)
{
    while (!q_find(d, match, arg, out, take))
        WaitForSingleObject(d->ev, 10);
}

/* ---- windows ------------------------------------------------------------------------------- */

#define MAXWIN 256
#define MAXSEL 8

struct xwin {
    XID id;
    struct xwin *parent;               /* NULL: the root */
    int x, y, w, h, border, depth, c_class;
    XVisual *visual;
    XID cmap, cursor;
    int mapped, override;
    int pinned;                        /* kept windowed: moves are ignored */
    struct { struct xdpy *d; long mask; } sel[MAXSEL];
    int nsel;
    HWND hwnd;
    HDC hdc;
    int pf;
    int wm_delete;
    struct xdpy *proto_dpy;
};

static struct xwin *wins[MAXWIN];
static int nwins;
static struct xwin *title_win;         /* the top-level window the game titled */
static XID hidden_cursors[64];
static int nhidden;

static struct xwin *win_get(XID id)
{
    int i;

    for (i = 0; i < nwins; i++)
        if (wins[i]->id == id)
            return wins[i];
    return NULL;
}

static struct xwin *win_top(struct xwin *w)
{
    while (w && w->parent)
        w = w->parent;
    return w;
}

static long sel_mask(struct xwin *w, struct xdpy *d)
{
    int i;

    for (i = 0; w && i < w->nsel; i++)
        if (w->sel[i].d == d)
            return w->sel[i].mask;
    return 0;
}

static void sel_set(struct xwin *w, struct xdpy *d, long mask)
{
    int i;

    for (i = 0; i < w->nsel; i++)
        if (w->sel[i].d == d) {
            w->sel[i].mask = mask;
            return;
        }
    if (w->nsel < MAXSEL) {
        w->sel[w->nsel].d = d;
        w->sel[w->nsel].mask = mask;
        w->nsel++;
    }
}

/* deliver to every display that selected mask on w (window field at 16) */
static void deliver(struct xwin *w, uint8_t *ev, long mask)
{
    int i;

    for (i = 0; i < w->nsel; i++)
        if (w->sel[i].mask & mask) {
            *(XID *)(ev + 16) = w->id;
            q_push(w->sel[i].d, ev, mask);
        }
}

/* structure events: to w (StructureNotify) and its parent (SubstructureNotify) */
static void deliver_structure(struct xwin *w, uint8_t *ev)
{
    int i;

    *(XID *)(ev + 20) = w->id;
    for (i = 0; i < w->nsel; i++)
        if (w->sel[i].mask & StructureNotifyMask) {
            *(XID *)(ev + 16) = w->id;
            q_push(w->sel[i].d, ev, StructureNotifyMask | SubstructureNotifyMask);
        }
    for (i = 0; w->parent && i < w->parent->nsel; i++)
        if (w->parent->sel[i].mask & SubstructureNotifyMask) {
            *(XID *)(ev + 16) = w->parent->id;
            q_push(w->parent->sel[i].d, ev, StructureNotifyMask | SubstructureNotifyMask);
        }
}

/* ---- the UI thread: owns the Win32 windows -------------------------------------------------- */

#define WM_X_OP (WM_APP + 1)
enum { OP_CREATE, OP_SHOW, OP_HIDE, OP_DESTROY, OP_GEOMETRY, OP_RAISE, OP_REPARENT, OP_TITLE };
struct uireq { int op; struct xwin *w; const char *text; };

static HWND ui_hwnd;
static uint8_t keydown[256];
static uint32_t keymap[256][2];        /* keycode -> keysym, unshifted / shifted */
static int closes;

#define TOP_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN)

static int background(void)
{
    return lx_env("IGS_BACKGROUND", NULL) != NULL;
}

/* X keysym -> SDL 1.2 key symbol (core.c's table: what the plugins use) */
static int sdl_key(uint32_t ks)
{
    if (ks >= 0x20 && ks <= 0x7e)
        return ks >= 'A' && ks <= 'Z' ? (int)ks + 32 : (int)ks;
    if (ks >= 0xffbe && ks <= 0xffc9)
        return 282 + (ks - 0xffbe);
    if (ks >= 0xffb0 && ks <= 0xffb9)
        return 256 + (ks - 0xffb0);
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

/* Windows virtual key -> X keysym (unshifted) */
static uint32_t vk_keysym(UINT vk, int sc, int ext)
{
    if (vk >= 'A' && vk <= 'Z')
        return vk + 32;
    if (vk >= '0' && vk <= '9')
        return vk;
    if (vk >= VK_F1 && vk <= VK_F24)
        return 0xffbe + (vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return 0xffb0 + (vk - VK_NUMPAD0);
    switch (vk) {
    case VK_BACK: return 0xff08;
    case VK_TAB: return 0xff09;
    case VK_RETURN: return ext ? 0xff8d : 0xff0d;
    case VK_PAUSE: return 0xff13;
    case VK_ESCAPE: return 0xff1b;
    case VK_SPACE: return ' ';
    case VK_PRIOR: return 0xff55;
    case VK_NEXT: return 0xff56;
    case VK_END: return 0xff57;
    case VK_HOME: return 0xff50;
    case VK_LEFT: return 0xff51;
    case VK_UP: return 0xff52;
    case VK_RIGHT: return 0xff53;
    case VK_DOWN: return 0xff54;
    case VK_SNAPSHOT: return 0xff61;
    case VK_INSERT: return 0xff63;
    case VK_DELETE: return 0xffff;
    case VK_LWIN: return 0xffeb;
    case VK_RWIN: return 0xffec;
    case VK_APPS: return 0xff67;
    case VK_MULTIPLY: return 0xffaa;
    case VK_ADD: return 0xffab;
    case VK_SUBTRACT: return 0xffad;
    case VK_DECIMAL: return 0xffae;
    case VK_DIVIDE: return 0xffaf;
    case VK_NUMLOCK: return 0xff7f;
    case VK_SCROLL: return 0xff14;
    case VK_CAPITAL: return 0xffe5;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return sc == 0x36 ? 0xffe2 : 0xffe1;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return ext ? 0xffe4 : 0xffe3;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return ext ? 0xffea : 0xffe9;
    case VK_OEM_1: return ';';
    case VK_OEM_PLUS: return '=';
    case VK_OEM_COMMA: return ',';
    case VK_OEM_MINUS: return '-';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2: return '/';
    case VK_OEM_3: return '`';
    case VK_OEM_4: return '[';
    case VK_OEM_5: return '\\';
    case VK_OEM_6: return ']';
    case VK_OEM_7: return '\'';
    }
    return 0;
}

/* the US layout's shifted keysym */
static uint32_t shifted(uint32_t ks)
{
    static const char from[] = "1234567890-=[]\\;',./`", to[] = "!@#$%^&*()_+{}|:\"<>?~";
    const char *p;

    if (ks >= 'a' && ks <= 'z')
        return ks - 32;
    if (ks && ks < 0x80 && (p = strchr(from, (int)ks)))
        return (uint8_t)to[p - from];
    return ks;
}

static void keymap_init(void)
{
    int kc;

    for (kc = 8; kc < 256; kc++) {
        int sc = (kc - 8) & 0x7f, ext = (kc - 8) & 0x80;
        UINT vk = MapVirtualKeyA(sc | (ext ? 0xe000 : 0), 3 /* MAPVK_VSC_TO_VK_EX */);
        uint32_t ks = vk ? vk_keysym(vk, sc, ext != 0) : 0;

        keymap[kc][0] = ks;
        keymap[kc][1] = shifted(ks);
    }
}

/* X modifier state from the keys held (before this event) */
static unsigned key_state(void)
{
    unsigned s = 0;

    if (keydown[50] || keydown[62])
        s |= 1;                                    /* Shift */
    if (GetKeyState(VK_CAPITAL) & 1)
        s |= 2;                                    /* Lock */
    if (keydown[37] || keydown[165])
        s |= 4;                                    /* Control */
    if (keydown[64] || keydown[192])
        s |= 8;                                    /* Mod1 (Alt) */
    if (GetKeyState(VK_NUMLOCK) & 1)
        s |= 16;                                   /* Mod2 (NumLock) */
    return s;
}

static void pointer_pos(struct xwin *w, int *x, int *y, int *rx, int *ry)
{
    POINT p = { 0, 0 };

    GetCursorPos(&p);
    *rx = p.x;
    *ry = p.y;
    if (w && w->hwnd)
        ScreenToClient(w->hwnd, &p);
    *x = p.x;
    *y = p.y;
}

/* the windows of top's tree, top first (creation order) */
static int tree_of(struct xwin *top, struct xwin **out)
{
    int i, n = 0;

    for (i = 0; i < nwins; i++)
        if (win_top(wins[i]) == top)
            out[n++] = wins[i];
    return n;
}

/* a key on top-level window top: to each display's first window in the tree that wants it */
static void key_event(struct xwin *top, int kc, int down)
{
    struct xwin *tree[MAXWIN];
    uint8_t ev[96];
    uint32_t ks = keymap[kc][0];
    long mask = down ? KeyPressMask : KeyReleaseMask;
    struct xdpy *d;
    int n, i, x, y, rx, ry;

    memset(ev, 0, sizeof(ev));
    ev[0] = down ? KeyPress : KeyRelease;
    *(XID *)(ev + 20) = ROOT_WIN;
    *(uint32_t *)(ev + 28) = GetMessageTime();
    pointer_pos(top, &x, &y, &rx, &ry);
    *(int *)(ev + 32) = x;
    *(int *)(ev + 36) = y;
    *(int *)(ev + 40) = rx;
    *(int *)(ev + 44) = ry;
    *(unsigned *)(ev + 48) = key_state();
    *(unsigned *)(ev + 52) = kc;
    *(int *)(ev + 56) = 1;
    keydown[kc] = (uint8_t)down;
    EnterCriticalSection(&xlock);
    n = top ? tree_of(top, tree) : 0;
    for (d = dpys; d; d = d->next)
        for (i = 0; i < n; i++)
            if (sel_mask(tree[i], d) & mask) {
                *(XID *)(ev + 16) = tree[i]->id;
                q_push(d, ev, mask);
                break;
            }
    LeaveCriticalSection(&xlock);
    if (sdl_key(ks))
        core_key(sdl_key(ks), down);
}

static void release_all(struct xwin *top)
{
    int kc;

    for (kc = 8; kc < 256; kc++)
        if (keydown[kc])
            key_event(top, kc, 0);
}

/* pointer events: from w up to the root, the first window each display selected */
static void pointer_event(struct xwin *w, int type, int button, long mask)
{
    uint8_t ev[96];
    struct xdpy *d;
    int x, y, rx, ry;

    memset(ev, 0, sizeof(ev));
    ev[0] = (uint8_t)type;
    *(XID *)(ev + 20) = ROOT_WIN;
    *(uint32_t *)(ev + 28) = GetMessageTime();
    pointer_pos(w, &x, &y, &rx, &ry);
    *(int *)(ev + 40) = rx;
    *(int *)(ev + 44) = ry;
    *(unsigned *)(ev + 48) = key_state();
    *(unsigned *)(ev + 52) = button;
    *(int *)(ev + 56) = 1;
    EnterCriticalSection(&xlock);
    for (d = dpys; d; d = d->next) {
        struct xwin *c;
        int cx = x, cy = y;

        for (c = w; c; c = c->parent) {
            if (sel_mask(c, d) & mask) {
                *(XID *)(ev + 16) = c->id;
                *(int *)(ev + 32) = cx;
                *(int *)(ev + 36) = cy;
                q_push(d, ev, mask);
                break;
            }
            cx += c->x;
            cy += c->y;
        }
    }
    LeaveCriticalSection(&xlock);
}

static void focus_event(struct xwin *top, int in)
{
    uint8_t ev[96];
    int i, kc;

    memset(ev, 0, sizeof(ev));
    ev[0] = in ? FocusIn : FocusOut;
    *(int *)(ev + 24) = 3;                         /* NotifyNonlinear */
    EnterCriticalSection(&xlock);
    deliver(top, ev, FocusChangeMask);
    if (in) {
        memset(ev, 0, sizeof(ev));
        ev[0] = KeymapNotify;
        for (kc = 0; kc < 256; kc++)
            if (keydown[kc])
                ev[20 + kc / 8] |= 1 << (kc % 8);
        for (i = 0; i < top->nsel; i++)
            if (top->sel[i].mask & KeymapStateMask)
                q_push(top->sel[i].d, ev, KeymapStateMask);
    }
    LeaveCriticalSection(&xlock);
}

static int cursor_hidden(XID c)
{
    int i;

    for (i = 0; c && i < nhidden; i++)
        if (hidden_cursors[i] == c)
            return 1;
    return 0;
}

static LRESULT CALLBACK x_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct xwin *w = (struct xwin *)GetWindowLongPtrA(h, GWLP_USERDATA), *top = win_top(w);

    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int sc = (lp >> 16) & 0x7f, ext = (lp >> 24) & 1, kc = sc + (ext ? 0x80 : 0) + 8;
        int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        uint32_t ks;

        if (!w)
            break;
        if (down && wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
            break;                                 /* Alt+F4 */
        if (down && keydown[kc])
            return 0;                              /* auto-repeat */
        if (!down && !keydown[kc])
            return 0;
        if ((ks = vk_keysym((UINT)wp, sc, ext))) {
            keymap[kc][0] = ks;
            keymap[kc][1] = shifted(ks);
        }
        key_event(top, kc, down);
        return 0;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: {
        int down = msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN;
        int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 1 :
                (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 2 : 3;

        if (w)
            pointer_event(w, down ? ButtonPress : ButtonRelease, b,
                          down ? ButtonPressMask : ButtonReleaseMask);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (w)
            pointer_event(w, MotionNotify, 0, PointerMotionMask);
        return 0;
    case WM_SETFOCUS:
        if (w && w == top)
            focus_event(top, 1);
        break;
    case WM_KILLFOCUS:
        if (w && w == top) {
            release_all(top);
            focus_event(top, 0);
        }
        break;
    case WM_CLOSE:
        if (++closes >= 2 || !top) {
            LOG("window closed twice: exiting");
            l_exit(0);
        }
        if (top->wm_delete && top->proto_dpy) {
            uint8_t ev[96];
            XID protocols = x_intern("WM_PROTOCOLS"), del = x_intern("WM_DELETE_WINDOW");

            memset(ev, 0, sizeof(ev));
            ev[0] = ClientMessage;
            *(XID *)(ev + 16) = top->id;
            *(XID *)(ev + 20) = protocols;
            *(int *)(ev + 24) = 32;
            *(XID *)(ev + 28) = del;
            *(uint32_t *)(ev + 32) = GetMessageTime();
            q_push(top->proto_dpy, ev, 0);
            LOG("window close: WM_DELETE_WINDOW sent (close again to force)");
        } else {
            LOG("window close: the game has no WM_DELETE_WINDOW (close again to exit)");
        }
        return 0;
    case WM_PAINT:
        ValidateRect(h, NULL);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            struct xwin *c;

            for (c = w; c; c = c->parent)
                if (c->cursor) {
                    if (cursor_hidden(c->cursor)) {
                        SetCursor(NULL);
                        return TRUE;
                    }
                    break;
                }
        }
        break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static LRESULT ui_op(struct uireq *r)
{
    struct xwin *w = r->w;
    RECT rc;

    switch (r->op) {
    case OP_CREATE: {
        DWORD style, ex = 0;
        HWND parent = NULL;
        int x, y, cw, ch;

        if (w->parent && w->parent->hwnd) {
            style = WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
            parent = w->parent->hwnd;
            x = w->x;
            y = w->y;
            cw = w->w;
            ch = w->h;
        } else if (w->override) {
            style = WS_POPUP | WS_CLIPCHILDREN;
            ex = WS_EX_TOOLWINDOW;
            x = w->x;
            y = w->y;
            cw = w->w;
            ch = w->h;
        } else {
            style = TOP_STYLE;
            SetRect(&rc, 0, 0, w->w, w->h);
            AdjustWindowRect(&rc, style, FALSE);
            x = w->x || w->y ? w->x : CW_USEDEFAULT;
            y = w->x || w->y ? w->y : CW_USEDEFAULT;
            cw = rc.right - rc.left;
            ch = rc.bottom - rc.top;
        }
        w->hwnd = CreateWindowExA(ex, "IGSLoaderX", r->text, style, x, y, cw, ch, parent, NULL,
                                  GetModuleHandleA(NULL), NULL);
        if (!w->hwnd) {
            LOG("X11: cannot create a window (error %lu)", GetLastError());
            return 0;
        }
        SetWindowLongPtrA(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
        w->hdc = GetDC(w->hwnd);
        return 1;
    }
    case OP_SHOW:
        if (!w->hwnd)
            return 0;
        if (w->parent)
            ShowWindow(w->hwnd, SW_SHOWNA);
        else if (w->override && !lx_env("IGS_FULLSCREEN", NULL))
            return 0;                      /* SDL's fullscreen window: windowed instead */
        else if (background())               /* behind everything, without the focus */
            SetWindowPos(w->hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        else
            ShowWindow(w->hwnd, SW_SHOW);
        return 1;
    case OP_HIDE:
        if (w->hwnd)
            ShowWindow(w->hwnd, SW_HIDE);
        return 1;
    case OP_DESTROY:
        if (w->hwnd) {
            SetWindowLongPtrA(w->hwnd, GWLP_USERDATA, 0);
            DestroyWindow(w->hwnd);
        }
        return 1;
    case OP_GEOMETRY:
        if (!w->hwnd)
            return 0;
        if (w->parent || w->override) {
            MoveWindow(w->hwnd, w->x, w->y, w->w, w->h, TRUE);
        } else {
            SetRect(&rc, 0, 0, w->w, w->h);
            AdjustWindowRect(&rc, GetWindowLongA(w->hwnd, GWL_STYLE), FALSE);
            SetWindowPos(w->hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 1;
    case OP_RAISE:
        if (w->hwnd && !background())
            SetWindowPos(w->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 1;
    case OP_REPARENT:
        if (!w->hwnd)
            return 0;
        if (w->parent && w->parent->hwnd) {
            SetWindowLongA(w->hwnd, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN |
                           (w->mapped ? WS_VISIBLE : 0));
            SetParent(w->hwnd, w->parent->hwnd);
            MoveWindow(w->hwnd, w->x, w->y, w->w, w->h, TRUE);
        }
        return 1;
    case OP_TITLE:
        if (w->hwnd)
            SetWindowTextA(w->hwnd, r->text);
        return 1;
    }
    return 0;
}

static LRESULT CALLBACK ui_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_X_OP)
        return ui_op((struct uireq *)lp);
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI ui_main(void *ready)
{
    WNDCLASSA wc;
    MSG m;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = x_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(GetModuleHandleA(NULL), MAKEINTRESOURCE(1));
    wc.lpszClassName = "IGSLoaderX";
    RegisterClassA(&wc);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ui_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "IGSLoaderXUI";
    RegisterClassA(&wc);
    ui_hwnd = CreateWindowExA(0, "IGSLoaderXUI", "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                              GetModuleHandleA(NULL), NULL);
    SetEvent(ready);
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

static LRESULT ui(int op, struct xwin *w, const char *text)
{
    struct uireq r = { op, w, text };

    return SendMessageA(ui_hwnd, WM_X_OP, 0, (LPARAM)&r);
}

static void x_start(void)
{
    HANDLE ready;

    EnterCriticalSection(&xlock);
    if (x_inited) {
        LeaveCriticalSection(&xlock);
        return;
    }
    keymap_init();
    ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 1 << 20, ui_main, ready, 0, NULL));
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    x_inited = 1;
    LeaveCriticalSection(&xlock);
}

/* the core's title (name | status | fps) on the game's top-level window */
int x11_window_title(const char *s)
{
    struct xwin *w = title_win;
    int i;

    if (!x_inited)
        return -1;
    for (i = 0; !w && i < nwins; i++)
        if (!wins[i]->parent && wins[i]->mapped && !wins[i]->override)
            w = wins[i];
    if (!w)
        return -1;
    ui(OP_TITLE, w, s);
    return 0;
}

/* ---- atoms -------------------------------------------------------------------------------------- */

static struct { char *name; } atoms[1024];
static int natoms = 69;
static const char *const predefined[69] = {
    NULL, "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP", "CURSOR",
    "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5",
    "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE",
    "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP",
    "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID", "WINDOW", "WM_COMMAND",
    "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS",
    "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE",
    "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION",
    "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT",
    "QUAD_WIDTH", "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME",
    "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR",
};

static XID atom_find(const char *name, int create)
{
    int i;

    for (i = 1; i < 69; i++)
        if (!strcmp(predefined[i], name))
            return i;
    for (i = 69; i < natoms; i++)
        if (!strcmp(atoms[i].name, name))
            return i;
    if (!create || natoms == 1024)
        return 0;
    atoms[natoms].name = strdup(name);
    return natoms++;
}

static XID x_intern(const char *name)
{
    XID a;

    EnterCriticalSection(&xlock);
    a = atom_find(name, 1);
    LeaveCriticalSection(&xlock);
    return a;
}

static XID l_XInternAtom(void *dpy, const char *name, int only_if_exists)
{
    XID a;

    (void)dpy;
    EnterCriticalSection(&xlock);
    a = atom_find(name, !only_if_exists);
    LeaveCriticalSection(&xlock);
    return a;
}

static int l_XInternAtoms(void *dpy, char **names, int n, int only, XID *out)
{
    int i;

    for (i = 0; i < n; i++)
        out[i] = l_XInternAtom(dpy, names[i], only);
    return 1;
}

static char *l_XGetAtomName(void *dpy, XID a)
{
    const char *s = a < 69 ? predefined[a] : (int)a < natoms ? atoms[a].name : NULL;

    (void)dpy;
    return s ? l_strdup(s) : NULL;
}

/* ---- connection --------------------------------------------------------------------------------- */

static void *l_XOpenDisplay(const char *name)
{
    struct xdpy *d;
    static char vendor[] = "IGS loader", dname[] = ":0.0";
    static XPixFmt fmts[3] = { { 1, 1, 32 }, { 8, 8, 32 }, { 24, 32, 32 } };
    static uint32_t gc_dummy[24];
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);

    (void)name;
    x_start();
    d = calloc(1, sizeof(*d));
    InitializeCriticalSection(&d->lock);
    d->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    d->fd = lx_fd_new_null();
    memcpy(d->formats, fmts, sizeof(fmts));
    d->screen.display = d;
    d->screen.root = ROOT_WIN;
    d->screen.width = w;
    d->screen.height = h;
    d->screen.mwidth = w * 254 / 960;
    d->screen.mheight = h * 254 / 960;
    d->screen.ndepths = 1;
    d->screen.depths = root_depths;
    d->screen.root_depth = 24;
    d->screen.root_visual = &root_visual;
    d->screen.default_gc = gc_dummy;
    d->screen.cmap = DEFAULT_CMAP;
    d->screen.white_pixel = 0xffffff;
    d->screen.black_pixel = 0;
    d->screen.max_maps = d->screen.min_maps = 1;
    DPY_FIELD(d, 8) = d->fd;
    DPY_FIELD(d, 16) = 11;                          /* proto_major_version */
    *(char **)(d->pub + 24) = vendor;
    *(void **)(d->pub + 44) = (void *)x_resource_alloc;
    DPY_FIELD(d, 48) = 0;                           /* byte_order: LSBFirst */
    DPY_FIELD(d, 52) = 32;                          /* bitmap_unit */
    DPY_FIELD(d, 56) = 32;                          /* bitmap_pad */
    DPY_FIELD(d, 64) = 3;                           /* nformats */
    *(XPixFmt **)(d->pub + 68) = d->formats;
    DPY_FIELD(d, 76) = 40300000;                    /* release */
    DPY_FIELD(d, 116) = 65535;                      /* max_request_size */
    *(char **)(d->pub + 128) = dname;
    DPY_FIELD(d, 132) = 0;                          /* default_screen */
    DPY_FIELD(d, 136) = 1;                          /* nscreens */
    *(XScreen **)(d->pub + 140) = &d->screen;
    DPY_FIELD(d, 152) = 8;                          /* min_keycode */
    DPY_FIELD(d, 156) = 255;                        /* max_keycode */
    EnterCriticalSection(&xlock);
    d->next = dpys;
    dpys = d;
    LeaveCriticalSection(&xlock);
    LOG("X11: display %p opened (fd %d)", (void *)d, d->fd);
    return d;
}

static int l_XCloseDisplay(struct xdpy *d)
{
    int i, j;

    if (!d)
        return 0;
    EnterCriticalSection(&xlock);
    d->closed = 1;
    for (i = 0; i < nwins; i++)
        for (j = 0; j < wins[i]->nsel; j++)
            if (wins[i]->sel[j].d == d)
                wins[i]->sel[j].mask = 0;
    LeaveCriticalSection(&xlock);
    return 0;
}

static char *l_XDisplayName(const char *s) { static char n[] = ":0.0"; return s && *s ? (char *)s : n; }
static int l_XConnectionNumber(struct xdpy *d) { return d->fd; }
static int l_XDefaultScreen(void *d) { (void)d; return 0; }
static XScreen *l_XDefaultScreenOfDisplay(struct xdpy *d) { return &d->screen; }
static XScreen *l_XScreenOfDisplay(struct xdpy *d, int n) { (void)n; return &d->screen; }
static int l_XScreenCount(void *d) { (void)d; return 1; }
static XID l_XRootWindow(void *d, int s) { (void)d; (void)s; return ROOT_WIN; }
static XID l_XDefaultRootWindow(void *d) { (void)d; return ROOT_WIN; }
static XID l_XRootWindowOfScreen(void *s) { (void)s; return ROOT_WIN; }
static XVisual *l_XDefaultVisual(void *d, int s) { (void)d; (void)s; return &root_visual; }
static int l_XDefaultDepth(void *d, int s) { (void)d; (void)s; return 24; }
static XID l_XDefaultColormap(void *d, int s) { (void)d; (void)s; return DEFAULT_CMAP; }
static int l_XDisplayWidth(struct xdpy *d, int s) { (void)s; return d->screen.width; }
static int l_XDisplayHeight(struct xdpy *d, int s) { (void)s; return d->screen.height; }
static uint32_t l_XBlackPixel(void *d, int s) { (void)d; (void)s; return 0; }
static uint32_t l_XWhitePixel(void *d, int s) { (void)d; (void)s; return 0xffffff; }
static int l_XScreenNumberOfScreen(void *s) { (void)s; return 0; }
static int l_XFlush(void *d) { (void)d; return 1; }
static int l_XSync(void *d, int discard) { (void)d; (void)discard; return 1; }
static int l_XFree(void *p) { l_free(p); return 1; }
static int l_XBell(void *d, int p) { (void)d; (void)p; return 1; }

static void *l_XSetErrorHandler(void *h) { void *o = err_handler; err_handler = h; return o; }
static void *l_XSetIOErrorHandler(void *h) { void *o = ioerr_handler; ioerr_handler = h; return o; }

static int l_XGetErrorDatabaseText(void *d, const char *name, const char *msg, const char *def,
                                   char *buf, int n)
{
    (void)d; (void)name; (void)msg;
    if (buf && n > 0)
        snprintf(buf, n, "%s", def ? def : "");
    return 1;
}

static int l_XGetErrorText(void *d, int code, char *buf, int n)
{
    (void)d;
    if (buf && n > 0)
        snprintf(buf, n, "X error %d", code);
    return 1;
}

/* ---- events ----------------------------------------------------------------------------------- */

static int l_XPending(struct xdpy *d) { return d->qn; }
static int l_XEventsQueued(struct xdpy *d, int mode) { (void)mode; return d->qn; }
static int l_XQLength(struct xdpy *d) { return d->qn; }

static int l_XNextEvent(struct xdpy *d, uint8_t *ev)
{
    q_wait(d, NULL, NULL, ev, 1);
    return 0;
}

static int l_XPeekEvent(struct xdpy *d, uint8_t *ev)
{
    q_wait(d, NULL, NULL, ev, 0);
    return 1;
}

struct match { XID win; long mask; int type; void *pred, *dpy, *arg; };

static int m_mask(const struct qev *e, void *a)
{
    struct match *m = a;

    return (e->mask & m->mask) && (!m->win || *(const XID *)(e->ev + 16) == m->win);
}

static int m_type(const struct qev *e, void *a)
{
    struct match *m = a;

    return e->ev[0] == m->type && (!m->win || *(const XID *)(e->ev + 16) == m->win);
}

static int m_pred(const struct qev *e, void *a)
{
    struct match *m = a;
    uint8_t copy[96];

    memcpy(copy, e->ev, 96);
    return (int)elf_call3(m->pred, U32(m->dpy), U32(copy), U32(m->arg));
}

static int l_XMaskEvent(struct xdpy *d, long mask, uint8_t *ev)
{
    struct match m = { 0, mask, 0, NULL, NULL, NULL };

    q_wait(d, m_mask, &m, ev, 1);
    return 0;
}

static int l_XCheckMaskEvent(struct xdpy *d, long mask, uint8_t *ev)
{
    struct match m = { 0, mask, 0, NULL, NULL, NULL };

    return q_find(d, m_mask, &m, ev, 1);
}

static int l_XWindowEvent(struct xdpy *d, XID w, long mask, uint8_t *ev)
{
    struct match m = { w, mask, 0, NULL, NULL, NULL };

    q_wait(d, m_mask, &m, ev, 1);
    return 0;
}

static int l_XCheckWindowEvent(struct xdpy *d, XID w, long mask, uint8_t *ev)
{
    struct match m = { w, mask, 0, NULL, NULL, NULL };

    return w && q_find(d, m_mask, &m, ev, 1);
}

static int l_XCheckTypedEvent(struct xdpy *d, int type, uint8_t *ev)
{
    struct match m = { 0, 0, type, NULL, NULL, NULL };

    return q_find(d, m_type, &m, ev, 1);
}

static int l_XCheckTypedWindowEvent(struct xdpy *d, XID w, int type, uint8_t *ev)
{
    struct match m = { w, 0, type, NULL, NULL, NULL };

    return w && q_find(d, m_type, &m, ev, 1);
}

/* the predicate is ELF code; it runs with the queue locked (it must not call Xlib) */
static int l_XIfEvent(struct xdpy *d, uint8_t *ev, void *pred, void *arg)
{
    struct match m = { 0, 0, 0, pred, d, arg };

    q_wait(d, m_pred, &m, ev, 1);
    return 0;
}

static int l_XCheckIfEvent(struct xdpy *d, uint8_t *ev, void *pred, void *arg)
{
    struct match m = { 0, 0, 0, pred, d, arg };

    return q_find(d, m_pred, &m, ev, 1);
}

static int l_XPeekIfEvent(struct xdpy *d, uint8_t *ev, void *pred, void *arg)
{
    struct match m = { 0, 0, 0, pred, d, arg };

    q_wait(d, m_pred, &m, ev, 0);
    return 0;
}

static int l_XPutBackEvent(struct xdpy *d, uint8_t *ev)
{
    EnterCriticalSection(&d->lock);
    if (d->qn < QMAX) {
        d->qhead = (d->qhead + QMAX - 1) % QMAX;
        memcpy(d->q[d->qhead].ev, ev, 96);
        d->q[d->qhead].mask = 0;
        d->qn++;
        DPY_FIELD(d, 88) = d->qn;
    }
    LeaveCriticalSection(&d->lock);
    SetEvent(d->ev);
    return 0;
}

static int l_XSendEvent(struct xdpy *d, XID w, int propagate, long mask, uint8_t *ev)
{
    struct xwin *win;
    uint8_t copy[96];
    int i;

    (void)propagate;
    memcpy(copy, ev, 96);
    *(int *)(copy + 8) = 1;                        /* send_event */
    EnterCriticalSection(&xlock);
    win = win_get(w);
    if (win) {
        if (!mask && win->proto_dpy)
            q_push(win->proto_dpy, copy, 0);
        for (i = 0; mask && i < win->nsel; i++)
            if (win->sel[i].mask & mask)
                q_push(win->sel[i].d, copy, mask);
    }
    LeaveCriticalSection(&xlock);
    (void)d;
    return 1;
}

static int l_XSelectInput(struct xdpy *d, XID w, long mask)
{
    struct xwin *win;

    EnterCriticalSection(&xlock);
    if ((win = win_get(w)))
        sel_set(win, d, mask);
    LeaveCriticalSection(&xlock);
    if (lx_verbose)
        LOG("X11: XSelectInput(%p, 0x%x, 0x%lx)", (void *)d, w, mask);
    return 1;
}

/* ---- windows ------------------------------------------------------------------------------- */

static XID l_XCreateWindow(struct xdpy *d, XID parent, int x, int y, unsigned w, unsigned h,
                           unsigned border, int depth, unsigned cls, XVisual *visual,
                           unsigned long mask, XSetWindowAttributes *a)
{
    struct xwin *win = calloc(1, sizeof(*win));

    win->id = new_xid();
    win->parent = parent == ROOT_WIN ? NULL : win_get(parent);
    win->x = x;
    win->y = y;
    win->w = w ? (int)w : 1;
    win->h = h ? (int)h : 1;
    win->border = border;
    win->depth = depth ? depth : 24;
    win->c_class = cls ? (int)cls : 1;
    win->visual = visual ? visual : &root_visual;
    win->cmap = DEFAULT_CMAP;
    if (a) {
        if (mask & 0x0200)                             /* CWOverrideRedirect */
            win->override = a->override_redirect;
        if (mask & 0x0800)                             /* CWEventMask */
            sel_set(win, d, a->event_mask);
        if (mask & 0x2000)                             /* CWColormap */
            win->cmap = a->colormap;
        if (mask & 0x4000)                             /* CWCursor */
            win->cursor = a->cursor;
    }
    EnterCriticalSection(&xlock);
    if (nwins < MAXWIN)
        wins[nwins++] = win;
    LeaveCriticalSection(&xlock);
    ui(OP_CREATE, win, lx_env("IGS_NAME", "IGS"));
    LOG("X11: window 0x%x %dx%d+%d+%d in 0x%x%s, visual 0x%x", win->id, win->w, win->h, x, y, parent,
        win->override ? " (override-redirect)" : "", win->visual->visualid);
    return win->id;
}

static XID l_XCreateSimpleWindow(struct xdpy *d, XID parent, int x, int y, unsigned w, unsigned h,
                                 unsigned bw, uint32_t border, uint32_t bg)
{
    (void)border; (void)bg;
    return l_XCreateWindow(d, parent, x, y, w, h, bw, 0, 1, NULL, 0, NULL);
}

static int l_XChangeWindowAttributes(struct xdpy *d, XID w, unsigned long mask, XSetWindowAttributes *a)
{
    struct xwin *win;

    EnterCriticalSection(&xlock);
    if ((win = win_get(w)) && a) {
        if (mask & 0x0200)
            win->override = a->override_redirect;
        if (mask & 0x0800)
            sel_set(win, d, a->event_mask);
        if (mask & 0x2000)
            win->cmap = a->colormap;
        if (mask & 0x4000)
            win->cursor = a->cursor;
    }
    LeaveCriticalSection(&xlock);
    return 1;
}

static void structure_event(struct xwin *w, int type)
{
    uint8_t ev[96];

    memset(ev, 0, sizeof(ev));
    ev[0] = (uint8_t)type;
    if (type == ConfigureNotify) {
        *(int *)(ev + 24) = w->x;
        *(int *)(ev + 28) = w->y;
        *(int *)(ev + 32) = w->w;
        *(int *)(ev + 36) = w->h;
        *(int *)(ev + 40) = w->border;
        *(int *)(ev + 48) = w->override;
    } else if (type == MapNotify) {
        *(int *)(ev + 24) = w->override;
    }
    EnterCriticalSection(&xlock);
    deliver_structure(w, ev);
    LeaveCriticalSection(&xlock);
}

static int l_XMapWindow(struct xdpy *d, XID id)
{
    struct xwin *w = win_get(id);
    uint8_t ev[96];

    (void)d;
    if (!w)
        return 1;
    if (w->mapped)
        return 1;
    w->mapped = 1;
    ui(OP_SHOW, w, NULL);
    structure_event(w, MapNotify);
    if (!w->parent)
        structure_event(w, ConfigureNotify);
    memset(ev, 0, sizeof(ev));
    ev[0] = VisibilityNotify;                     /* state 0: VisibilityUnobscured */
    EnterCriticalSection(&xlock);
    deliver(w, ev, VisibilityChangeMask);
    memset(ev, 0, sizeof(ev));
    ev[0] = Expose;
    *(int *)(ev + 28) = w->w;
    *(int *)(ev + 32) = w->h;
    deliver(w, ev, ExposureMask);
    LeaveCriticalSection(&xlock);
    LOG("X11: map 0x%x", id);
    return 1;
}

static int l_XMapRaised(struct xdpy *d, XID id)
{
    l_XMapWindow(d, id);
    if (win_get(id))
        ui(OP_RAISE, win_get(id), NULL);
    return 1;
}

static int l_XMapSubwindows(struct xdpy *d, XID id)
{
    int i;

    for (i = 0; i < nwins; i++)
        if (wins[i]->parent && wins[i]->parent->id == id)
            l_XMapWindow(d, wins[i]->id);
    return 1;
}

static int l_XUnmapWindow(struct xdpy *d, XID id)
{
    struct xwin *w = win_get(id);

    (void)d;
    if (!w || !w->mapped)
        return 1;
    w->mapped = 0;
    ui(OP_HIDE, w, NULL);
    structure_event(w, UnmapNotify);
    return 1;
}

static int l_XDestroyWindow(struct xdpy *d, XID id)
{
    struct xwin *w = win_get(id);
    int i;

    (void)d;
    if (!w)
        return 1;
    /* children first */
    for (i = nwins - 1; i >= 0; i--)
        if (i < nwins && wins[i]->parent == w)
            l_XDestroyWindow(d, wins[i]->id);
    structure_event(w, DestroyNotify);
    ui(OP_DESTROY, w, NULL);
    EnterCriticalSection(&xlock);
    for (i = 0; i < nwins; i++)
        if (wins[i] == w) {
            memmove(wins + i, wins + i + 1, (nwins - i - 1) * sizeof(*wins));
            nwins--;
            break;
        }
    if (title_win == w)
        title_win = NULL;
    LeaveCriticalSection(&xlock);
    /* the struct stays allocated: other threads may still hold it */
    w->hwnd = NULL;
    return 1;
}

static int geometry(XID id, int x, int y, int wd, int ht, int move, int size)
{
    struct xwin *w = win_get(id);

    if (!w)
        return 1;
    if (w->pinned)
        move = 0;
    if (move) {
        w->x = x;
        w->y = y;
    }
    if (size) {
        w->w = wd > 0 ? wd : 1;
        w->h = ht > 0 ? ht : 1;
    }
    ui(OP_GEOMETRY, w, NULL);
    structure_event(w, ConfigureNotify);
    return 1;
}

static int l_XMoveWindow(void *d, XID w, int x, int y) { (void)d; return geometry(w, x, y, 0, 0, 1, 0); }
static int l_XResizeWindow(void *d, XID w, unsigned wd, unsigned h) { (void)d; return geometry(w, 0, 0, wd, h, 0, 1); }
static int l_XMoveResizeWindow(void *d, XID w, int x, int y, unsigned wd, unsigned h)
{
    (void)d;
    return geometry(w, x, y, wd, h, 1, 1);
}

/* XWindowChanges: x y width height border_width sibling stack_mode */
static int l_XConfigureWindow(void *d, XID id, unsigned mask, int *c)
{
    struct xwin *w = win_get(id);

    (void)d;
    if (!w || !c)
        return 1;
    return geometry(id, mask & 1 ? c[0] : w->x, mask & 2 ? c[1] : w->y, mask & 4 ? c[2] : w->w,
                    mask & 8 ? c[3] : w->h, (mask & 3) != 0, (mask & 12) != 0);
}

static int l_XRaiseWindow(void *d, XID id)
{
    (void)d;
    if (win_get(id))
        ui(OP_RAISE, win_get(id), NULL);
    return 1;
}

static int l_XLowerWindow(void *d, XID id) { (void)d; (void)id; return 1; }
static int l_XIconifyWindow(void *d, XID id, int s) { (void)d; (void)id; (void)s; return 1; }

static int l_XReparentWindow(void *d, XID id, XID parent, int x, int y)
{
    struct xwin *w = win_get(id), *p = parent == ROOT_WIN ? NULL : win_get(parent);
    uint8_t ev[96];

    (void)d;
    if (!w)
        return 1;
    LOG("X11: reparent 0x%x into 0x%x at %d,%d%s", id, parent, x, y,
        p && p->override && !lx_env("IGS_FULLSCREEN", NULL) ? " (fullscreen window: kept windowed)" : "");
    if (p && p->override && !lx_env("IGS_FULLSCREEN", NULL)) {
        /*
         * SDL's fullscreen window stays hidden: the GL window stays in its WM
         * window, which takes its size, and keeps its place there
         */
        struct xwin *top = win_top(w);

        w->pinned = 1;
        if (top && top != w) {
            w->x = w->y = 0;
            ui(OP_GEOMETRY, w, NULL);
            geometry(top->id, 0, 0, w->w, w->h, 0, 1);
        }
        return 1;
    }
    w->parent = p;
    w->x = x;
    w->y = y;
    ui(OP_REPARENT, w, NULL);
    memset(ev, 0, sizeof(ev));
    ev[0] = ReparentNotify;
    *(XID *)(ev + 24) = parent;
    *(int *)(ev + 28) = x;
    *(int *)(ev + 32) = y;
    EnterCriticalSection(&xlock);
    deliver_structure(w, ev);
    LeaveCriticalSection(&xlock);
    return 1;
}

static int l_XGetWindowAttributes(struct xdpy *d, XID id, XWindowAttributes *a)
{
    struct xwin *w = win_get(id);

    memset(a, 0, sizeof(*a));
    a->root = ROOT_WIN;
    a->screen = &d->screen;
    a->visual = &root_visual;
    a->colormap = DEFAULT_CMAP;
    a->depth = 24;
    a->c_class = 1;
    if (id == ROOT_WIN || !w) {
        a->width = d->screen.width;
        a->height = d->screen.height;
        a->map_state = 2;
        return id == ROOT_WIN;
    }
    a->x = w->x;
    a->y = w->y;
    a->width = w->w;
    a->height = w->h;
    a->border_width = w->border;
    a->depth = w->depth;
    a->visual = w->visual;
    a->c_class = w->c_class;
    a->colormap = w->cmap;
    a->map_installed = 1;
    a->map_state = w->mapped ? 2 : 0;              /* IsViewable / IsUnmapped */
    a->your_event_mask = sel_mask(w, d);
    a->all_event_masks = a->your_event_mask;
    a->override_redirect = w->override;
    return 1;
}

static int l_XGetGeometry(struct xdpy *d, XID id, XID *root, int *x, int *y, unsigned *wd,
                          unsigned *ht, unsigned *bw, unsigned *depth)
{
    XWindowAttributes a;

    l_XGetWindowAttributes(d, id, &a);
    if (root) *root = ROOT_WIN;
    if (x) *x = a.x;
    if (y) *y = a.y;
    if (wd) *wd = a.width;
    if (ht) *ht = a.height;
    if (bw) *bw = a.border_width;
    if (depth) *depth = a.depth;
    return 1;
}

static int l_XQueryTree(void *d, XID id, XID *root, XID *parent, XID **children, unsigned *n)
{
    struct xwin *w = win_get(id);
    int i, k = 0;

    (void)d;
    if (root)
        *root = ROOT_WIN;
    if (parent)
        *parent = w && w->parent ? w->parent->id : ROOT_WIN;
    if (children) {
        *children = l_malloc((nwins + 1) * sizeof(XID));
        for (i = 0; i < nwins; i++)
            if ((id == ROOT_WIN && !wins[i]->parent) || (w && wins[i]->parent == w))
                (*children)[k++] = wins[i]->id;
        if (!k) {
            l_free(*children);
            *children = NULL;
        }
    }
    if (n)
        *n = k;
    return 1;
}

static int l_XTranslateCoordinates(void *d, XID src, XID dst, int x, int y, int *dx, int *dy, XID *child)
{
    struct xwin *s = win_get(src), *t = win_get(dst);
    POINT p = { x, y };

    (void)d;
    if (s && s->hwnd)
        ClientToScreen(s->hwnd, &p);
    if (t && t->hwnd)
        ScreenToClient(t->hwnd, &p);
    if (dx) *dx = p.x;
    if (dy) *dy = p.y;
    if (child) *child = 0;
    return 1;
}

static int l_XQueryPointer(void *d, XID id, XID *root, XID *child, int *rx, int *ry, int *wx, int *wy,
                           unsigned *mask)
{
    int x, y, sx, sy;

    (void)d;
    pointer_pos(win_get(id), &x, &y, &sx, &sy);
    if (root) *root = ROOT_WIN;
    if (child) *child = 0;
    if (rx) *rx = sx;
    if (ry) *ry = sy;
    if (wx) *wx = x;
    if (wy) *wy = y;
    if (mask) *mask = key_state();
    return 1;
}

static int l_XGetInputFocus(void *d, XID *focus, int *revert)
{
    int i;

    (void)d;
    if (focus) {
        *focus = 1;                                /* PointerRoot */
        for (i = 0; i < nwins; i++)
            if (wins[i]->hwnd && wins[i]->hwnd == GetForegroundWindow())
                *focus = wins[i]->id;
    }
    if (revert)
        *revert = 2;
    return 1;
}

/* ---- window manager hints, titles, properties --------------------------------------------------- */

static void set_title(XID id, const char *s)
{
    struct xwin *w = win_top(win_get(id));

    if (!w || !s)
        return;
    title_win = w;
    core_caption(s);
}

static int l_XStoreName(void *d, XID w, const char *s) { (void)d; set_title(w, s); return 1; }
static int l_XSetIconName(void *d, XID w, const char *s) { (void)d; (void)w; (void)s; return 1; }

static int l_XFetchName(void *d, XID w, char **out)
{
    (void)d; (void)w;
    *out = l_strdup(lx_env("IGS_NAME", "IGS"));
    return 1;
}

static void l_XSetWMName(void *d, XID w, XTextProperty *tp)
{
    (void)d;
    if (tp && tp->value)
        set_title(w, (const char *)tp->value);
}

static void l_XSetWMIconName(void *d, XID w, XTextProperty *tp) { (void)d; (void)w; (void)tp; }

static int get_text(XTextProperty *tp)
{
    const char *s = lx_env("IGS_NAME", "IGS");

    tp->value = (unsigned char *)l_strdup(s);
    tp->encoding = 31;                             /* XA_STRING */
    tp->format = 8;
    tp->nitems = strlen(s);
    return 1;
}

static int l_XGetWMName(void *d, XID w, XTextProperty *tp) { (void)d; (void)w; return get_text(tp); }
static int l_XGetWMIconName(void *d, XID w, XTextProperty *tp) { (void)d; (void)w; return get_text(tp); }

static int l_XStringListToTextProperty(char **list, int n, XTextProperty *tp)
{
    size_t len = 0, o = 0;
    int i;
    char *v;

    for (i = 0; i < n; i++)
        len += strlen(list[i]) + 1;
    v = l_malloc(len ? len : 1);
    v[0] = 0;
    for (i = 0; i < n; i++) {
        strcpy(v + o, list[i]);
        o += strlen(list[i]) + 1;
    }
    tp->value = (unsigned char *)v;
    tp->encoding = 31;
    tp->format = 8;
    tp->nitems = len ? (uint32_t)len - 1 : 0;
    return 1;
}

static void l_XSetWMProperties(void *d, XID w, XTextProperty *name, XTextProperty *icon, char **argv,
                               int argc, void *normal, void *wm, void *cls)
{
    (void)icon; (void)argv; (void)argc; (void)normal; (void)wm; (void)cls;
    if (name)
        l_XSetWMName(d, w, name);
}

static void l_Xutf8SetWMProperties(void *d, XID w, const char *name, const char *icon, char **argv,
                                   int argc, void *normal, void *wm, void *cls)
{
    (void)d; (void)icon; (void)argv; (void)argc; (void)normal; (void)wm; (void)cls;
    if (name)
        set_title(w, name);
}

static int l_XSetWMProtocols(struct xdpy *d, XID id, XID *list, int n)
{
    struct xwin *w = win_get(id);
    XID del = l_XInternAtom(d, "WM_DELETE_WINDOW", 0);
    int i;

    for (i = 0; w && i < n; i++)
        if (list[i] == del) {
            w->wm_delete = 1;
            w->proto_dpy = d;
        }
    return 1;
}

/* WM_NAME (39) or _NET_WM_NAME: the game's title */
static int l_XChangeProperty(void *d, XID w, XID prop, XID type, int format, int mode,
                             const unsigned char *data, int n)
{
    (void)type; (void)mode;
    if (format == 8 && data && (prop == 39 || prop == l_XInternAtom(d, "_NET_WM_NAME", 0))) {
        char buf[256];

        snprintf(buf, sizeof(buf), "%.*s", n, (const char *)data);
        set_title(w, buf);
    }
    return 1;
}

static int l_XGetWindowProperty(void *d, XID w, XID prop, long off, long len, int del, XID req,
                                XID *type, int *format, uint32_t *n, uint32_t *after, unsigned char **data)
{
    (void)d; (void)w; (void)prop; (void)off; (void)len; (void)del; (void)req;
    if (type) *type = 0;
    if (format) *format = 0;
    if (n) *n = 0;
    if (after) *after = 0;
    if (data) *data = NULL;
    return 0;                                      /* Success, property absent */
}

static void *l_XAllocSizeHints(void) { return l_calloc(1, 72); }
static void *l_XAllocWMHints(void) { return l_calloc(1, 36); }
static void *l_XAllocClassHint(void) { return l_calloc(1, 8); }
static void *l_XGetWMHints(void *d, XID w) { (void)d; (void)w; return l_calloc(1, 36); }
static int l_XGetWMNormalHints(void *d, XID w, void *h, long *supplied)
{
    (void)d; (void)w;
    memset(h, 0, 72);
    if (supplied)
        *supplied = 0;
    return 0;
}

static int x_ok(void) { return 1; }               /* hints, cursors, grabs undone... */
static int x_zero(void) { return 0; }             /* GrabSuccess, Success, False */
static void *x_null(void) { return NULL; }

/* ---- cursors, pixmaps, GCs, colours, images ----------------------------------------------------- */

static XID l_XCreatePixmap(void) { return new_xid(); }

/* a pixmap cursor is the invisible one games make to hide the pointer */
static XID l_XCreatePixmapCursor(void)
{
    XID id = new_xid();

    EnterCriticalSection(&xlock);
    if (nhidden < 64)
        hidden_cursors[nhidden++] = id;
    LeaveCriticalSection(&xlock);
    return id;
}

static XID l_XCreateFontCursor(void) { return new_xid(); }

static int l_XDefineCursor(void *d, XID w, XID c)
{
    struct xwin *win = win_get(w);

    (void)d;
    if (win)
        win->cursor = c;
    return 1;
}

static int l_XUndefineCursor(void *d, XID w) { return l_XDefineCursor(d, w, 0); }

static void *l_XCreateGC(void)
{
    uint32_t *gc = l_calloc(1, 96);

    gc[1] = new_xid();
    return gc;
}

static XID l_XCreateColormap(void) { return new_xid(); }

static uint32_t rgb_pixel(const XColor *c)
{
    return (uint32_t)(c->red >> 8) << 16 | (uint32_t)(c->green >> 8) << 8 | (c->blue >> 8);
}

static int l_XAllocColor(void *d, XID cmap, XColor *c)
{
    (void)d; (void)cmap;
    c->pixel = rgb_pixel(c);
    return 1;
}

static int parse_color(const char *name, XColor *c)
{
    static const struct { const char *n; uint32_t rgb; } named[] = {
        { "black", 0 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x00ff00 },
        { "blue", 0x0000ff }, { "gray", 0xbebebe }, { "grey", 0xbebebe }, { "yellow", 0xffff00 },
    };
    uint32_t rgb = 0;
    unsigned i;

    if (name[0] == '#' && strlen(name) == 7)
        rgb = (uint32_t)strtoul(name + 1, NULL, 16);
    else
        for (i = 0; i < sizeof(named) / sizeof(named[0]); i++)
            if (!_stricmp(named[i].n, name))
                rgb = named[i].rgb;
    c->red = (uint16_t)(((rgb >> 16) & 0xff) * 257);
    c->green = (uint16_t)(((rgb >> 8) & 0xff) * 257);
    c->blue = (uint16_t)((rgb & 0xff) * 257);
    c->flags = 7;
    c->pixel = rgb;
    return 1;
}

static int l_XParseColor(void *d, XID cmap, const char *name, XColor *c)
{
    (void)d; (void)cmap;
    return parse_color(name, c);
}

static int l_XAllocNamedColor(void *d, XID cmap, const char *name, XColor *scr, XColor *exact)
{
    (void)d; (void)cmap;
    if (scr)
        parse_color(name, scr);
    if (exact)
        parse_color(name, exact);
    return 1;
}

static int l_XQueryColors(void *d, XID cmap, XColor *c, int n)
{
    int i;

    (void)d; (void)cmap;
    for (i = 0; i < n; i++) {
        c[i].red = (uint16_t)(((c[i].pixel >> 16) & 0xff) * 257);
        c[i].green = (uint16_t)(((c[i].pixel >> 8) & 0xff) * 257);
        c[i].blue = (uint16_t)((c[i].pixel & 0xff) * 257);
        c[i].flags = 7;
    }
    return 1;
}

static int x_destroy_image(XImage *im)
{
    if (im) {
        l_free(im->data);
        l_free(im);
    }
    return 1;
}

static uint32_t x_get_pixel(XImage *im, int x, int y)
{
    if (!im->data || im->bits_per_pixel != 32)
        return 0;
    return *(uint32_t *)(im->data + y * im->bytes_per_line + x * 4);
}

static int x_put_pixel(XImage *im, int x, int y, uint32_t p)
{
    if (im->data && im->bits_per_pixel == 32)
        *(uint32_t *)(im->data + y * im->bytes_per_line + x * 4) = p;
    return 1;
}

static int l_XInitImage(XImage *im)
{
    im->destroy_image = (void *)x_destroy_image;
    im->get_pixel = (void *)x_get_pixel;
    im->put_pixel = (void *)x_put_pixel;
    return 1;
}

static XImage *l_XCreateImage(void *d, XVisual *v, unsigned depth, int format, int offset, char *data,
                              unsigned w, unsigned h, int pad, int bpl)
{
    XImage *im = l_calloc(1, sizeof(*im));
    int bpp = format == 2 ? (depth > 16 ? 32 : depth > 8 ? 16 : 8) : 1;   /* ZPixmap */

    (void)d;
    im->width = w;
    im->height = h;
    im->xoffset = offset;
    im->format = format;
    im->data = data;
    im->bitmap_unit = 32;
    im->bitmap_pad = pad ? pad : 32;
    im->depth = depth;
    im->bits_per_pixel = bpp;
    im->bytes_per_line = bpl ? bpl : (int)((w * bpp + im->bitmap_pad - 1) / im->bitmap_pad * im->bitmap_pad / 8);
    if (v) {
        im->red_mask = v->red_mask;
        im->green_mask = v->green_mask;
        im->blue_mask = v->blue_mask;
    }
    l_XInitImage(im);
    return im;
}

static void *l_XGetModifierMapping(void *d)
{
    /* Shift, Lock, Control, Mod1 (Alt), Mod2 (NumLock), Mod3..5: two keycodes each */
    static const uint8_t map[16] = { 50, 62, 66, 0, 37, 165, 64, 192, 205, 0, 0, 0, 0, 0, 0, 0 };
    XModifierKeymap *m = l_malloc(sizeof(*m));

    (void)d;
    m->max_keypermod = 2;
    m->modifiermap = l_malloc(16);
    memcpy(m->modifiermap, map, 16);
    return m;
}

static int l_XFreeModifiermap(XModifierKeymap *m)
{
    if (m) {
        l_free(m->modifiermap);
        l_free(m);
    }
    return 1;
}

/* ---- keyboard ----------------------------------------------------------------------------------- */

static uint32_t l_XKeycodeToKeysym(void *d, unsigned kc, int index)
{
    (void)d;
    if (kc > 255 || index < 0 || index > 1)
        return 0;
    return keymap[kc][index];
}

static uint8_t l_XKeysymToKeycode(void *d, uint32_t ks)
{
    int kc;

    (void)d;
    if (ks >= 'A' && ks <= 'Z')
        ks += 32;
    for (kc = 8; kc < 256; kc++)
        if (keymap[kc][0] == ks || keymap[kc][1] == ks)
            return (uint8_t)kc;
    return 0;
}

static uint32_t l_XLookupKeysym(uint8_t *ev, int index)
{
    return l_XKeycodeToKeysym(NULL, *(unsigned *)(ev + 52), index);
}

static int l_XLookupString(uint8_t *ev, char *buf, int n, uint32_t *keysym, void *status)
{
    unsigned state = *(unsigned *)(ev + 48), kc = *(unsigned *)(ev + 52);
    uint32_t ks = kc < 256 ? keymap[kc][state & 1 ? 1 : 0] : 0;
    int c = -1;

    (void)status;
    if ((state & 2) && ks >= 'a' && ks <= 'z')
        ks -= 32;
    else if ((state & 2) && ks >= 'A' && ks <= 'Z')
        ks += 32;
    if (keysym)
        *keysym = ks;
    if (ks >= 0x20 && ks <= 0x7e)
        c = (state & 4) && ((ks | 32) >= 'a' && (ks | 32) <= 'z') ? (int)(ks & 0x1f) : (int)ks;
    else if (ks == 0xff0d || ks == 0xff8d)
        c = '\r';
    else if (ks == 0xff08 || ks == 0xff09 || ks == 0xff1b)
        c = (int)(ks & 0xff);
    else if (ks == 0xffff)
        c = 127;
    if (c < 0 || !buf || n < 1)
        return 0;
    buf[0] = (char)c;
    return 1;
}

static char *l_XKeysymToString(uint32_t ks)
{
    static const struct { uint32_t ks; const char *s; } names[] = {
        { ' ', "space" }, { 0xff08, "BackSpace" }, { 0xff09, "Tab" }, { 0xff0d, "Return" },
        { 0xff1b, "Escape" }, { 0xffff, "Delete" }, { 0xff50, "Home" }, { 0xff51, "Left" },
        { 0xff52, "Up" }, { 0xff53, "Right" }, { 0xff54, "Down" }, { 0xff55, "Prior" },
        { 0xff56, "Next" }, { 0xff57, "End" }, { 0xff63, "Insert" }, { 0xff8d, "KP_Enter" },
        { 0xffe1, "Shift_L" }, { 0xffe2, "Shift_R" }, { 0xffe3, "Control_L" },
        { 0xffe4, "Control_R" }, { 0xffe9, "Alt_L" }, { 0xffea, "Alt_R" },
    };
    static char one[128][2], fkeys[24][4];
    unsigned i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].ks == ks)
            return (char *)names[i].s;
    if (ks > 0x20 && ks < 0x7f) {
        one[ks][0] = (char)ks;
        return one[ks];
    }
    if (ks >= 0xffbe && ks < 0xffbe + 24) {
        snprintf(fkeys[ks - 0xffbe], 4, "F%u", ks - 0xffbe + 1);
        return fkeys[ks - 0xffbe];
    }
    return NULL;
}

static uint32_t l_XStringToKeysym(const char *s)
{
    uint32_t ks;

    if (!s)
        return 0;
    if (s[0] && !s[1])
        return (uint8_t)s[0];
    for (ks = 0xff00; ks <= 0xffff; ks++) {
        const char *n = l_XKeysymToString(ks);

        if (n && !strcmp(n, s))
            return ks;
    }
    return 0;
}

static int l_XQueryKeymap(void *d, char keys[32])
{
    int kc;

    (void)d;
    memset(keys, 0, 32);
    for (kc = 0; kc < 256; kc++)
        if (keydown[kc])
            keys[kc / 8] |= (char)(1 << (kc % 8));
    return 1;
}

static int l_XDisplayKeycodes(void *d, int *mn, int *mx)
{
    (void)d;
    if (mn) *mn = 8;
    if (mx) *mx = 255;
    return 1;
}

static uint32_t *l_XGetKeyboardMapping(void *d, uint8_t first, int count, int *per)
{
    uint32_t *m = l_calloc(count ? count : 1, 2 * sizeof(uint32_t));
    int i;

    (void)d;
    for (i = 0; i < count && first + i < 256; i++) {
        m[2 * i] = keymap[first + i][0];
        m[2 * i + 1] = keymap[first + i][1];
    }
    if (per)
        *per = 2;
    return m;
}

/* XKeyboardState: key_click_percent bell_percent bell_pitch bell_duration led_mask global_auto_repeat auto_repeats[32] */
static int l_XGetKeyboardControl(void *d, uint8_t *ks)
{
    (void)d;
    memset(ks, 0, 56);
    *(int *)(ks + 20) = 1;                         /* AutoRepeatModeOn */
    return 1;
}

static int l_XGetPointerControl(void *d, int *num, int *den, int *thr)
{
    (void)d;
    if (num) *num = 2;
    if (den) *den = 1;
    if (thr) *thr = 4;
    return 1;
}

/* ---- visuals ----------------------------------------------------------------------------------------- */

/* GL configurations: each has its own X visual */
struct glcfg {
    int id, dbl, alpha, depth, stencil;
    XVisual vis;
};

#define MAXCFG 16
static struct glcfg cfgs[MAXCFG];
static int ncfg;

static struct glcfg *cfg_get(int dbl, int alpha, int depth, int stencil)
{
    int i;
    struct glcfg *c;

    EnterCriticalSection(&xlock);
    for (i = 0; i < ncfg; i++)
        if (cfgs[i].dbl == dbl && cfgs[i].alpha == alpha && cfgs[i].depth == depth &&
            cfgs[i].stencil == stencil) {
            LeaveCriticalSection(&xlock);
            return &cfgs[i];
        }
    i = ncfg < MAXCFG ? ncfg++ : MAXCFG - 1;
    c = &cfgs[i];
    c->id = i + 1;
    c->dbl = dbl;
    c->alpha = alpha;
    c->depth = depth;
    c->stencil = stencil;
    c->vis = root_visual;
    c->vis.visualid = 0x22 + i;
    LeaveCriticalSection(&xlock);
    return c;
}

static struct glcfg *cfg_of_visual(uint32_t id)
{
    int i;

    for (i = 0; i < ncfg; i++)
        if (cfgs[i].vis.visualid == id)
            return &cfgs[i];
    return NULL;
}

static void fill_vi(XVisualInfo *vi, XVisual *v)
{
    memset(vi, 0, sizeof(*vi));
    vi->visual = v;
    vi->visualid = v->visualid;
    vi->depth = 24;
    vi->c_class = 4;                                  /* TrueColor */
    vi->red_mask = v->red_mask;
    vi->green_mask = v->green_mask;
    vi->blue_mask = v->blue_mask;
    vi->colormap_size = 256;
    vi->bits_per_rgb = 8;
}

static XVisualInfo *l_XGetVisualInfo(void *d, long mask, XVisualInfo *t, int *n)
{
    XVisualInfo *out = l_malloc((ncfg + 1) * sizeof(XVisualInfo)), vi;
    int i, k = 0;

    (void)d;
    for (i = -1; i < ncfg; i++) {
        fill_vi(&vi, i < 0 ? &root_visual : &cfgs[i].vis);
        if (t && (((mask & 1) && t->visualid != vi.visualid) || ((mask & 2) && t->screen != 0) ||
                  ((mask & 4) && t->depth != 24) || ((mask & 8) && t->c_class != 4)))
            continue;
        out[k++] = vi;
    }
    *n = k;
    if (!k) {
        l_free(out);
        return NULL;
    }
    return out;
}

static int l_XMatchVisualInfo(void *d, int screen, int depth, int cls, XVisualInfo *vi)
{
    (void)d; (void)screen;
    if (depth != 24 || cls != 4)
        return 0;
    fill_vi(vi, &root_visual);
    return 1;
}

static uint32_t l_XVisualIDFromVisual(XVisual *v) { return v ? v->visualid : 0; }

static XPixFmt *l_XListPixmapFormats(struct xdpy *d, int *n)
{
    XPixFmt *f = l_malloc(sizeof(d->formats));

    memcpy(f, d->formats, sizeof(d->formats));
    *n = 3;
    return f;
}

/* ---- extensions: none --------------------------------------------------------------------------- */

static int l_XQueryExtension(void *d, const char *name, int *op, int *ev, int *err)
{
    (void)d;
    if (op) *op = 0;
    if (ev) *ev = 0;
    if (err) *err = 0;
    if (lx_verbose)
        LOG("X11: extension %s: absent", name);
    return 0;
}

static void *l_XextCreateExtension(void) { return l_calloc(1, sizeof(XExtensionInfo)); }
static void l_XextDestroyExtension(void *e) { l_free(e); }

static XExtDisplayInfo *l_XextFindDisplay(XExtensionInfo *e, void *dpy)
{
    XExtDisplayInfo *i;

    for (i = e ? e->head : NULL; i; i = i->next)
        if (i->display == dpy)
            return i;
    return NULL;
}

/* codes NULL: XextHasExtension() is false, the extension's calls give up */
static XExtDisplayInfo *l_XextAddDisplay(XExtensionInfo *e, void *dpy, const char *name, void *hooks,
                                         int nevents, void *data)
{
    XExtDisplayInfo *i = l_calloc(1, sizeof(*i));

    (void)hooks; (void)nevents;
    if (lx_verbose)
        LOG("X11: extension %s: absent", name ? name : "?");
    i->display = dpy;
    i->data = data;
    if (e) {
        i->next = e->head;
        e->head = i;
        e->cur = i;
        e->ndisplays++;
    }
    return i;
}

static int l_XextRemoveDisplay(XExtensionInfo *e, void *dpy)
{
    XExtDisplayInfo **p;

    for (p = e ? &e->head : NULL; p && *p; p = (XExtDisplayInfo **)&(*p)->next)
        if ((*p)->display == dpy) {
            XExtDisplayInfo *i = *p;

            *p = i->next;
            if (e->cur == i)
                e->cur = NULL;
            e->ndisplays--;
            l_free(i);
            return 1;
        }
    return 0;
}

/* ---- GLX ------------------------------------------------------------------------------------------ */

#define GLX_USE_GL 1
#define GLX_BUFFER_SIZE 2
#define GLX_LEVEL 3
#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_STEREO 6
#define GLX_AUX_BUFFERS 7
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_STENCIL_SIZE 13
#define GLX_CONFIG_CAVEAT 0x20
#define GLX_X_VISUAL_TYPE 0x22
#define GLX_TRANSPARENT_TYPE 0x23
#define GLX_NONE 0x8000
#define GLX_TRUE_COLOR 0x8002
#define GLX_VISUAL_ID 0x800B
#define GLX_SCREEN 0x800C
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_RENDER_TYPE 0x8011
#define GLX_X_RENDERABLE 0x8012
#define GLX_FBCONFIG_ID 0x8013
#define GLX_RGBA_TYPE 0x8014
#define GLX_WIDTH 0x801D
#define GLX_HEIGHT 0x801E
#define GLX_SAMPLE_BUFFERS 100000
#define GLX_SAMPLES 100001
#define GLX_DONT_CARE (-1)

struct glxctx {
    HGLRC rc;
    struct glcfg *cfg;
    struct glxctx *share;
};

static __thread struct glxctx *cur_ctx;
static __thread XID cur_draw;
static __thread struct xdpy *cur_dpy;
static int gl_announced;

/* a GLX attribute of a configuration; -1 if unknown */
static int cfg_attr(const struct glcfg *c, int a)
{
    switch (a) {
    case GLX_USE_GL: case GLX_RGBA: case GLX_X_RENDERABLE: return 1;
    case GLX_BUFFER_SIZE: return c->alpha ? 32 : 24;
    case GLX_LEVEL: case GLX_STEREO: case GLX_AUX_BUFFERS: case GLX_SCREEN: return 0;
    case GLX_DOUBLEBUFFER: return c->dbl;
    case GLX_RED_SIZE: case GLX_GREEN_SIZE: case GLX_BLUE_SIZE: return 8;
    case GLX_ALPHA_SIZE: return c->alpha;
    case GLX_DEPTH_SIZE: return c->depth;
    case GLX_STENCIL_SIZE: return c->stencil;
    case 14: case 15: case 16: case 17: return 0;      /* accumulation */
    case GLX_CONFIG_CAVEAT: case GLX_TRANSPARENT_TYPE: return GLX_NONE;
    case GLX_X_VISUAL_TYPE: return GLX_TRUE_COLOR;
    case GLX_VISUAL_ID: return (int)c->vis.visualid;
    case GLX_DRAWABLE_TYPE: return 1;                  /* GLX_WINDOW_BIT */
    case GLX_RENDER_TYPE: return 1;                    /* GLX_RGBA_BIT */
    case GLX_FBCONFIG_ID: return c->id;
    case GLX_SAMPLE_BUFFERS: case GLX_SAMPLES: return 0;
    case 0x8016: case 0x8017: case 0x8018: return 0;   /* max pbuffer */
    case 0x20B2: return 0;                             /* sRGB capable */
    }
    return -1;
}

static XVisualInfo *l_glXChooseVisual(void *d, int screen, const int *attr)
{
    int dbl = 0, alpha = 0, depth = 0, stencil = 0;
    XVisualInfo *vi;
    struct glcfg *c;

    (void)d; (void)screen;
    for (; attr && *attr; attr++) {
        switch (*attr) {
        case GLX_USE_GL: case GLX_RGBA: case GLX_STEREO: break;
        case GLX_DOUBLEBUFFER: dbl = 1; break;
        case GLX_ALPHA_SIZE: alpha = *++attr; break;
        case GLX_DEPTH_SIZE: depth = *++attr; break;
        case GLX_STENCIL_SIZE: stencil = *++attr; break;
        default: attr++; break;
        }
    }
    c = cfg_get(dbl, alpha > 0 ? 8 : 0, depth > 24 ? 32 : 24, stencil > 0 ? 8 : 0);
    vi = l_malloc(sizeof(*vi));
    fill_vi(vi, &c->vis);
    LOG("GLX: visual 0x%x: %s, alpha %d, depth %d, stencil %d", c->vis.visualid,
        dbl ? "double" : "single", c->alpha, c->depth, c->stencil);
    return vi;
}

static int l_glXGetConfig(void *d, XVisualInfo *vi, int a, int *v)
{
    struct glcfg *c = vi ? cfg_of_visual(vi->visualid) : NULL;
    struct glcfg root = { 0, 1, 0, 24, 8, { NULL, ROOT_VISUAL, 4, 0, 0, 0, 8, 256 } };
    int r;

    (void)d;
    r = cfg_attr(c ? c : &root, a);
    if (r == -1)
        return 2;                                      /* GLX_BAD_ATTRIBUTE */
    *v = r;
    return 0;
}

/* the FBConfigs: double- and single-buffered RGBA8, depth 24, stencil 8 */
static void fb_init(void)
{
    if (!ncfg)
        cfg_get(1, 8, 24, 8);
    cfg_get(0, 8, 24, 8);
}

static int fb_ok(const struct glcfg *c, const int *a)
{
    for (; a && *a; a += 2) {
        int want = a[1], have = cfg_attr(c, a[0]);

        if (want == GLX_DONT_CARE || have == -1)
            continue;
        switch (a[0]) {
        case GLX_DOUBLEBUFFER: case GLX_STEREO: case GLX_X_VISUAL_TYPE: case GLX_FBCONFIG_ID:
        case GLX_LEVEL: case GLX_CONFIG_CAVEAT: case GLX_TRANSPARENT_TYPE: case GLX_X_RENDERABLE:
            if (have != want)
                return 0;
            break;
        case GLX_DRAWABLE_TYPE: case GLX_RENDER_TYPE:
            if ((have & want) != want)
                return 0;
            break;
        default:
            if (have < want)
                return 0;
        }
    }
    return 1;
}

static struct glcfg **l_glXChooseFBConfig(void *d, int screen, const int *attr, int *n)
{
    struct glcfg **out;
    int i, k = 0;

    (void)d; (void)screen;
    fb_init();
    out = l_malloc((ncfg + 1) * sizeof(*out));
    for (i = 0; i < ncfg; i++)
        if (fb_ok(&cfgs[i], attr))
            out[k++] = &cfgs[i];
    *n = k;
    if (!k) {
        l_free(out);
        return NULL;
    }
    return out;
}

static struct glcfg **l_glXGetFBConfigs(void *d, int screen, int *n)
{
    return l_glXChooseFBConfig(d, screen, NULL, n);
}

static int l_glXGetFBConfigAttrib(void *d, struct glcfg *c, int a, int *v)
{
    int r = c ? cfg_attr(c, a) : -1;

    (void)d;
    if (r == -1)
        return 2;
    *v = r;
    return 0;
}

static XVisualInfo *l_glXGetVisualFromFBConfig(void *d, struct glcfg *c)
{
    XVisualInfo *vi;

    (void)d;
    if (!c)
        return NULL;
    vi = l_malloc(sizeof(*vi));
    fill_vi(vi, &c->vis);
    return vi;
}

static struct glxctx *new_ctx(struct glcfg *c, struct glxctx *share)
{
    struct glxctx *x = calloc(1, sizeof(*x));

    x->cfg = c;
    x->share = share;
    return x;
}

static struct glxctx *l_glXCreateContext(void *d, XVisualInfo *vi, struct glxctx *share, int direct)
{
    struct glcfg *c = vi ? cfg_of_visual(vi->visualid) : NULL;

    (void)d; (void)direct;
    if (!c)
        c = cfg_get(1, 0, 24, 8);
    return new_ctx(c, share);
}

static struct glxctx *l_glXCreateNewContext(void *d, struct glcfg *c, int type, struct glxctx *share,
                                            int direct)
{
    (void)d; (void)type; (void)direct;
    return new_ctx(c ? c : cfg_get(1, 8, 24, 8), share);
}

static void set_pixel_format(struct xwin *w, const struct glcfg *c)
{
    PIXELFORMATDESCRIPTOR pfd;

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | (c->dbl ? PFD_DOUBLEBUFFER : 0);
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = (BYTE)c->alpha;
    pfd.cDepthBits = (BYTE)c->depth;
    pfd.cStencilBits = (BYTE)c->stencil;
    pfd.iLayerType = PFD_MAIN_PLANE;
    w->pf = ChoosePixelFormat(w->hdc, &pfd);
    if (!w->pf || !SetPixelFormat(w->hdc, w->pf, &pfd))
        LOG("GLX: no pixel format for window 0x%x (error %lu)", w->id, GetLastError());
}

static int l_glXMakeCurrent(struct xdpy *d, XID draw, struct glxctx *x)
{
    struct xwin *w;

    if (!x || !draw) {
        wglMakeCurrent(NULL, NULL);
        cur_ctx = NULL;
        cur_draw = 0;
        cur_dpy = NULL;
        return 1;
    }
    w = win_get(draw);
    if (!w || !w->hdc) {
        LOG("GLX: glXMakeCurrent on unknown drawable 0x%x", draw);
        return 0;
    }
    if (!w->pf)
        set_pixel_format(w, x->cfg);
    if (!x->rc) {
        x->rc = wglCreateContext(w->hdc);
        if (!x->rc) {
            LOG("GLX: cannot create a context (error %lu)", GetLastError());
            return 0;
        }
        if (x->share && x->share->rc)
            wglShareLists(x->share->rc, x->rc);
    }
    if (!wglMakeCurrent(w->hdc, x->rc)) {
        LOG("GLX: glXMakeCurrent failed (error %lu)", GetLastError());
        return 0;
    }
    cur_ctx = x;
    cur_draw = draw;
    cur_dpy = d;
    if (!gl_announced) {
        BOOL (WINAPI *swap_interval)(int);
        PIXELFORMATDESCRIPTOR pfd;

        gl_announced = 1;
        DescribePixelFormat(w->hdc, w->pf, sizeof(pfd), &pfd);
        LOG("OpenGL: %s, %s, %s; depth %d stencil %d alpha %d",
            (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION), pfd.cDepthBits, pfd.cStencilBits, pfd.cAlphaBits);
        *(void **)&swap_interval = (void *)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap_interval)
            swap_interval(lx_env_long("IGS_VSYNC", 0) ? 1 : 0);
        core_set_window(win_top(w)->hwnd);
        core_gl_ready();
    }
    return 1;
}

static int l_glXMakeContextCurrent(struct xdpy *d, XID draw, XID read, struct glxctx *x)
{
    (void)read;
    return l_glXMakeCurrent(d, draw, x);
}

static void l_glXDestroyContext(void *d, struct glxctx *x)
{
    (void)d;
    if (!x)
        return;
    if (cur_ctx == x)
        l_glXMakeCurrent(NULL, 0, NULL);
    if (x->rc)
        wglDeleteContext(x->rc);
    free(x);
}

/* the frame: the core's overlay, IGS_SHOT and frame cap, then the swap */
static void l_glXSwapBuffers(void *d, XID draw)
{
    struct xwin *w = win_get(draw);

    (void)d;
    core_frame();
    if (w && w->hdc)
        SwapBuffers(w->hdc);
}

static void *l_glXGetCurrentContext(void) { return cur_ctx ? (void *)cur_ctx : (void *)wglGetCurrentContext(); }
static XID l_glXGetCurrentDrawable(void) { return cur_draw; }

/* the X display of the current context; GLEW under SDL (no X) gets a dummy one */
static void *l_glXGetCurrentDisplay(void)
{
    static int dummy[64];

    if (cur_dpy)
        return cur_dpy;
    return wglGetCurrentContext() ? dummy : NULL;
}

static int l_glXIsDirect(void *d, void *c) { (void)d; (void)c; return 1; }

static int l_glXQueryContext(void *d, struct glxctx *x, int a, int *v)
{
    (void)d;
    if (!x)
        return 2;
    *v = a == GLX_FBCONFIG_ID ? x->cfg->id : a == GLX_RENDER_TYPE ? GLX_RGBA_TYPE : 0;
    return 0;
}

static void l_glXQueryDrawable(void *d, XID draw, int a, unsigned *v)
{
    struct xwin *w = win_get(draw);

    (void)d;
    *v = !w ? 0 : a == GLX_WIDTH ? (unsigned)w->w : a == GLX_HEIGHT ? (unsigned)w->h : 0;
}

static XID l_glXCreateWindow(void *d, void *c, XID win, const int *a) { (void)d; (void)c; (void)a; return win; }
static void l_glXWaitGL(void) { glFinish(); }

/* ---- select(): the X connection is readable while events are queued -------------------------------- */

static int l_select(int n, uint32_t *rd, uint32_t *wr, uint32_t *ex, struct l_timeval *tv)
{
    double end = tv ? lx_now() + tv->tv_sec + tv->tv_usec / 1e6 : 1e300;
    uint32_t in[32], ready[32];
    int fd, count, xfds = 0, others = 0;

    if (n > 1024)
        n = 1024;
    memset(in, 0, sizeof(in));
    if (rd)
        memcpy(in, rd, (n + 31) / 32 * 4);
    for (fd = 0; fd < n; fd++)
        if (in[fd / 32] >> (fd % 32) & 1) {
            if (dpy_from_fd(fd))
                xfds++;
            else
                others++;
        }
    for (;;) {
        memset(ready, 0, sizeof(ready));
        count = 0;
        for (fd = 0; fd < n; fd++) {
            struct xdpy *d;

            if (!(in[fd / 32] >> (fd % 32) & 1))
                continue;
            d = dpy_from_fd(fd);
            if (!d || d->qn) {                     /* other fds: always readable */
                ready[fd / 32] |= 1u << (fd % 32);
                count++;
            }
        }
        if (wr)
            for (fd = 0; fd < n; fd++)
                if (wr[fd / 32] >> (fd % 32) & 1)
                    count++;
        if (count || lx_now() >= end || (!xfds && !others && !tv))
            break;
        lx_usleep(xfds ? 1000 : (int64_t)((end - lx_now()) * 1e6));
        if (!xfds)
            break;
    }
    if (rd)
        memcpy(rd, ready, (n + 31) / 32 * 4);
    if (ex)
        memset(ex, 0, (n + 31) / 32 * 4);
    return count;
}

/* ---- Xt / Xaw (Ogre's config and error dialogs, never shown): empty -------------------------------- */

static char xt_strings[4096], xt_shell_strings[1024];
static void *widget_class_dummy[64];
static void *session_shell_class = widget_class_dummy, *command_class = widget_class_dummy,
            *menu_button_class = widget_class_dummy, *form_class = widget_class_dummy,
            *label_class = widget_class_dummy, *simple_menu_class = widget_class_dummy,
            *sme_bsb_class = widget_class_dummy;

/* ---- the table ------------------------------------------------------------------------------------- */

#define NOK(name) NA(name, x_ok)
#define NZ(name) NA(name, x_zero)

const struct nsym x11_syms[] = {
    /* connection */
    N(XOpenDisplay), N(XCloseDisplay), N(XDisplayName), N(XConnectionNumber), N(XDefaultScreen),
    N(XDefaultScreenOfDisplay), N(XScreenOfDisplay), N(XScreenCount), N(XRootWindow),
    N(XDefaultRootWindow), N(XRootWindowOfScreen), N(XDefaultVisual), N(XDefaultDepth),
    N(XDefaultColormap), N(XDisplayWidth), N(XDisplayHeight), N(XBlackPixel), N(XWhitePixel),
    N(XScreenNumberOfScreen), N(XFlush), N(XSync), N(XFree), N(XBell), N(XSetErrorHandler),
    N(XSetIOErrorHandler), N(XGetErrorDatabaseText), N(XGetErrorText),
    /* events */
    N(XPending), N(XEventsQueued), N(XQLength), N(XNextEvent), N(XPeekEvent), N(XMaskEvent),
    N(XCheckMaskEvent), N(XWindowEvent), N(XCheckWindowEvent), N(XCheckTypedEvent),
    N(XCheckTypedWindowEvent), N(XIfEvent), N(XCheckIfEvent), N(XPeekIfEvent), N(XPutBackEvent),
    N(XSendEvent), N(XSelectInput),
    /* windows */
    N(XCreateWindow), N(XCreateSimpleWindow), N(XChangeWindowAttributes), N(XMapWindow),
    N(XMapRaised), N(XMapSubwindows), N(XUnmapWindow), N(XDestroyWindow), N(XMoveWindow),
    N(XResizeWindow), N(XMoveResizeWindow), N(XConfigureWindow), N(XRaiseWindow), N(XLowerWindow),
    N(XIconifyWindow), N(XReparentWindow), N(XGetWindowAttributes), N(XGetGeometry), N(XQueryTree),
    N(XTranslateCoordinates), N(XQueryPointer), N(XGetInputFocus),
    NOK(XSetWindowBackground), NOK(XSetWindowBackgroundPixmap), NOK(XSetWindowBorder), NOK(XSetWindowColormap),
    NOK(XClearWindow), NOK(XClearArea), NOK(XSetInputFocus), NOK(XSetTransientForHint),
    /* window manager */
    N(XStoreName), N(XSetIconName), N(XFetchName), N(XSetWMName), N(XSetWMIconName), N(XGetWMName),
    N(XGetWMIconName), N(XStringListToTextProperty), N(XSetWMProperties), N(Xutf8SetWMProperties),
    NA(XmbSetWMProperties, l_Xutf8SetWMProperties), N(XSetWMProtocols), N(XChangeProperty),
    N(XGetWindowProperty), NOK(XDeleteProperty), N(XAllocSizeHints), N(XAllocWMHints),
    N(XAllocClassHint), N(XGetWMHints), N(XGetWMNormalHints), NOK(XSetWMHints), NOK(XSetClassHint),
    NOK(XSetWMNormalHints), NOK(XSetWMSizeHints), NOK(XSetNormalHints), N(XInternAtom), N(XInternAtoms),
    N(XGetAtomName),
    /* grabs and the pointer: a desktop window must not take them */
    NZ(XGrabPointer), NZ(XGrabKeyboard), NOK(XUngrabPointer), NOK(XUngrabKeyboard), NOK(XWarpPointer),
    NOK(XAutoRepeatOff), NOK(XAutoRepeatOn), NOK(XChangePointerControl), N(XGetPointerControl),
    NOK(XChangeActivePointerGrab), NOK(XAllowEvents),
    /* cursors, pixmaps, GCs, colours, images */
    N(XCreatePixmap), NA(XCreateBitmapFromData, l_XCreatePixmap),
    NA(XCreatePixmapFromBitmapData, l_XCreatePixmap), NOK(XFreePixmap), N(XCreatePixmapCursor),
    N(XCreateFontCursor), N(XDefineCursor), N(XUndefineCursor), NOK(XFreeCursor), NOK(XRecolorCursor),
    N(XCreateGC), NA(XFreeGC, l_XFree), NOK(XChangeGC), NOK(XSetForeground), NOK(XSetBackground),
    NOK(XSetFunction), NOK(XFillRectangle), NOK(XCopyArea), NOK(XDrawLine),
    N(XCreateColormap), NOK(XFreeColormap), NOK(XInstallColormap), N(XAllocColor), N(XAllocNamedColor),
    N(XParseColor), N(XQueryColors), NOK(XStoreColors), NOK(XFreeColors),
    N(XCreateImage), N(XInitImage), NOK(XPutImage), NA(XGetImage, x_null),
    N(XGetModifierMapping), N(XFreeModifiermap),
    /* keyboard */
    N(XKeycodeToKeysym), N(XKeysymToKeycode), N(XLookupKeysym), N(XLookupString), N(XKeysymToString),
    N(XStringToKeysym), N(XQueryKeymap), N(XDisplayKeycodes), N(XGetKeyboardMapping),
    N(XGetKeyboardControl), NOK(XChangeKeyboardControl), NOK(XRefreshKeyboardMapping),
    /* visuals */
    N(XGetVisualInfo), N(XMatchVisualInfo), N(XVisualIDFromVisual), N(XListPixmapFormats),
    /* extensions: none (XShm, XF86VidMode, DGA, Xinerama, RandR, XInput...) */
    N(XQueryExtension), NA(XInitExtension, x_null), NA(XListExtensions, x_null), NZ(XMissingExtension),
    N(XextCreateExtension), N(XextDestroyExtension), N(XextFindDisplay), N(XextAddDisplay),
    N(XextRemoveDisplay), NA(XESetWireToEvent, x_null), NA(XESetEventToWire, x_null),
    NA(XESetCloseDisplay, x_null), NA(XESetError, x_null), NA(XESetErrorString, x_null),
    NZ(_XReply), NZ(_XSend), NZ(_XRead), NZ(_XReadPad), NZ(_XEatData), NZ(_XFlush), NZ(_XFlushGCCache),
    NZ(_XSetLastRequestRead), NA(_XGetRequest, x_null),
    NZ(XShmQueryExtension), NZ(XShmAttach), NZ(XShmDetach), NZ(XShmPutImage), NA(XShmCreateImage, x_null),
    NZ(XShmGetEventBase), NZ(XShmQueryVersion),
    NA(XRRGetScreenInfo, x_null), NZ(XRRConfigCurrentConfiguration), NZ(XRRConfigCurrentRate),
    NA(XRRConfigRates, x_null), NA(XRRConfigSizes, x_null), NOK(XRRFreeScreenConfigInfo),
    NZ(XRRSetScreenConfigAndRate), NZ(XRRQueryExtension),
    NZ(XF86VidModeQueryExtension), NZ(XF86VidModeGetAllModeLines), NZ(XF86VidModeSwitchToMode),
    NZ(XF86VidModeSetViewPort), NZ(XF86VidModeGetModeLine), NZ(XF86VidModeLockModeSwitch),
    /* Xt / Xaw */
    NA(XtVaOpenApplication, x_null), NA(XtVaCreateManagedWidget, x_null),
    NA(XtVaCreatePopupShell, x_null), NA(XtDisplay, x_null), NZ(XtAddCallback), NZ(XtAppMainLoop),
    NZ(XtAppSetExitFlag), NZ(XtDestroyWidget), NZ(XtRealizeWidget), NZ(XtUnrealizeWidget),
    NZ(XtVaSetValues), ND(XtStrings, xt_strings), ND(XtShellStrings, xt_shell_strings),
    ND(sessionShellWidgetClass, &session_shell_class), ND(commandWidgetClass, &command_class),
    ND(menuButtonWidgetClass, &menu_button_class), ND(formWidgetClass, &form_class),
    ND(labelWidgetClass, &label_class), ND(simpleMenuWidgetClass, &simple_menu_class),
    ND(smeBSBObjectClass, &sme_bsb_class),
    /* GLX (the rest is in gl.c) */
    N(glXChooseVisual), N(glXGetConfig), N(glXChooseFBConfig), N(glXGetFBConfigs),
    N(glXGetFBConfigAttrib), N(glXGetVisualFromFBConfig), N(glXCreateContext), N(glXCreateNewContext),
    N(glXMakeCurrent), N(glXMakeContextCurrent), N(glXDestroyContext), N(glXSwapBuffers),
    N(glXGetCurrentContext), N(glXGetCurrentDrawable), NA(glXGetCurrentReadDrawable, l_glXGetCurrentDrawable),
    N(glXGetCurrentDisplay), N(glXIsDirect), N(glXQueryContext), N(glXQueryDrawable),
    N(glXCreateWindow), NA(glXDestroyWindow, x_ok), NA(glXCreatePbuffer, x_zero),
    NA(glXDestroyPbuffer, x_ok), NA(glXCreateGLXPixmap, x_zero), NA(glXDestroyGLXPixmap, x_ok),
    N(glXWaitGL), NA(glXWaitX, x_ok), NA(glXUseXFont, x_ok), NA(glXReleaseBuffersMESA, x_ok),
    /* libc: select() knows the X connections */
    N(select),
    { NULL, NULL },
};

void x11_init(void)
{
    InitializeCriticalSection(&xlock);
}
