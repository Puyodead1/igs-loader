/*
 * IGS loader, Windows core: libGL / GLX.
 *
 * Linux GL entry points are cdecl, Windows' are stdcall (the callee pops
 * its arguments).  Every GL function the ELF side binds to is a 16-byte
 * stub that loads its target and jumps to one shared thunk:
 *
 *   pop the return address into a TLS slot, save esp in another,
 *   call the stdcall target, restore esp, push the return address, ret.
 *
 * No argument counts needed; GL never calls back, so one save per thread is
 * enough.  The slots are TlsAlloc indices read through fs:[0xE10 + 4*i]
 * (TEB.TlsSlots).  Targets come from opengl32's exports (GL 1.1) or
 * wglGetProcAddress (later, once a context exists); unknown ones return 0.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "lx.h"

#define MAXGL 4096

static DWORD slot_ret, slot_esp;
static uint8_t *common;
static HMODULE ogl;
static struct glent { const char *name; void *stub; } gltab[MAXGL];
static void *volatile cells[MAXGL];
static int ngl;
static CRITICAL_SECTION gl_lock;
static uint8_t *ret0;              /* xor eax, eax; ret */

static void *wgl_get(const char *name)
{
    void *p;

    if (!wglGetCurrentContext())
        return NULL;
    p = (void *)wglGetProcAddress(name);
    if ((uintptr_t)p <= 3 || p == (void *)-1)
        return NULL;
    return p;
}

/* first call through stub i: find the target */
static void *__attribute__((used)) gl_resolve(int i)
{
    const char *name = gltab[i].name;
    void *p = (void *)GetProcAddress(ogl, name);

    if (!p)
        p = wgl_get(name);
    if (!p) {
        if (!wglGetCurrentContext())
            return ret0;              /* no context yet: try again next time */
        LOG("GL: %s is not available (calls return 0)", name);
        p = ret0;
    }
    cells[i] = p;
    return p;
}

void gl_init(void)
{
    uint8_t *c;
    int32_t rel;
    uint32_t r, s;

    InitializeCriticalSection(&gl_lock);
    ogl = LoadLibraryA("opengl32.dll");
    slot_ret = TlsAlloc();
    slot_esp = TlsAlloc();
    if (slot_ret >= 64 || slot_esp >= 64)
        LOG("GL: TLS slots %lu/%lu out of the TEB's range", slot_ret, slot_esp);
    r = 0xE10 + 4 * slot_ret;
    s = 0xE10 + 4 * slot_esp;
    ret0 = stub_alloc(4);
    ret0[0] = 0x31; ret0[1] = 0xc0; ret0[2] = 0xc3;
    c = common = stub_alloc(64);
    memcpy(c, "\x85\xc0\x75\x09\x52\xe8", 6);              /* test; jnz go; push edx; call */
    rel = (int32_t)((uint8_t *)gl_resolve - (c + 10));
    memcpy(c + 6, &rel, 4);
    memcpy(c + 10, "\x83\xc4\x04\x59", 4);                  /* add esp, 4; go: pop ecx */
    memcpy(c + 14, "\x64\x89\x0d", 3); memcpy(c + 17, &r, 4);   /* mov fs:[R], ecx */
    memcpy(c + 21, "\x64\x89\x25", 3); memcpy(c + 24, &s, 4);   /* mov fs:[S], esp */
    memcpy(c + 28, "\xff\xd0", 2);                              /* call eax */
    memcpy(c + 30, "\x64\x8b\x25", 3); memcpy(c + 33, &s, 4);   /* mov esp, fs:[S] */
    memcpy(c + 37, "\x64\xff\x35", 3); memcpy(c + 40, &r, 4);   /* push dword fs:[R] */
    c[44] = 0xc3;                                               /* ret */
}

/* a cdecl-callable entry for a GL function name */
void *gl_proc(const char *name)
{
    uint8_t *st;
    int i;
    int32_t rel;
    void *cell;

    if (!common)
        gl_init();
    EnterCriticalSection(&gl_lock);
    for (i = 0; i < ngl; i++)
        if (!strcmp(gltab[i].name, name)) {
            LeaveCriticalSection(&gl_lock);
            return gltab[i].stub;
        }
    if (ngl == MAXGL) {
        LeaveCriticalSection(&gl_lock);
        return NULL;
    }
    i = ngl;
    gltab[i].name = strdup(name);
    cells[i] = (void *)GetProcAddress(ogl, name);
    st = stub_alloc(16);
    st[0] = 0xba;                                  /* mov edx, i */
    memcpy(st + 1, &i, 4);
    st[5] = 0xa1;                                  /* mov eax, [cell] */
    cell = (void *)&cells[i];
    memcpy(st + 6, &cell, 4);
    st[10] = 0xe9;                                 /* jmp common */
    rel = (int32_t)(common - (st + 15));
    memcpy(st + 11, &rel, 4);
    gltab[i].stub = st;
    ngl++;
    LeaveCriticalSection(&gl_lock);
    return st;
}

/*
 * ---- EXT_paletted_texture / EXT_shared_texture_palette -----------------------------------
 *
 * Gold Captain needs glColorTableEXT (it exits without it) and draws its water
 * as GL_COLOR_INDEX8_EXT textures under a shared palette set after binding.
 * Current drivers have neither, so the loader keeps each paletted texture's
 * indices and uploads it as RGBA, again whenever the palette changes.
 */
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_COLOR_INDEX 0x1900
#define GL_COLOR_INDEX8_EXT 0x80E5
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define MAXPAL 64

static struct paltex {
    unsigned id;
    int w, h, level, border;
    uint8_t *idx;
    uint8_t used[1024];                /* the palette it was last expanded with */
} paltex[MAXPAL];
static uint8_t palette[1024];
static unsigned bound2d;

/* the real entry, through its thunk (looked up once) */
#define GLCALL(type, name, args) \
    (*({ static type (*f) args; if (!f) *(void **)&f = gl_proc(#name); f; }))

static struct paltex *paltex_find(unsigned id, int add)
{
    int i, free_slot = -1;

    for (i = 0; i < MAXPAL; i++) {
        if (paltex[i].id == id && paltex[i].idx)
            return &paltex[i];
        if (!paltex[i].idx && free_slot < 0)
            free_slot = i;
    }
    if (!add || free_slot < 0)
        return NULL;
    paltex[free_slot].id = id;
    return &paltex[free_slot];
}

static void paltex_drop(unsigned id)
{
    struct paltex *t = paltex_find(id, 0);

    if (t) {
        free(t->idx);
        t->idx = NULL;
    }
}

/* the bound paletted texture as RGBA with the current palette */
static void paltex_upload(struct paltex *t)
{
    uint8_t *rgba = malloc((size_t)t->w * t->h * 4);
    int i, align = 4;

    if (!rgba)
        return;
    for (i = 0; i < t->w * t->h; i++)
        memcpy(rgba + 4 * i, palette + 4 * t->idx[i], 4);
    memcpy(t->used, palette, sizeof(palette));
    GLCALL(void, glGetIntegerv, (unsigned, int *))(GL_UNPACK_ALIGNMENT, &align);
    GLCALL(void, glPixelStorei, (unsigned, int))(GL_UNPACK_ALIGNMENT, 4);
    GLCALL(void, glTexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *))
        (GL_TEXTURE_2D, t->level, GL_RGBA, t->w, t->h, t->border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    GLCALL(void, glPixelStorei, (unsigned, int))(GL_UNPACK_ALIGNMENT, align);
    free(rgba);
}

static void l_glBindTexture(unsigned target, unsigned id)
{
    if (target == GL_TEXTURE_2D)
        bound2d = id;
    GLCALL(void, glBindTexture, (unsigned, unsigned))(target, id);
}

static void l_glDeleteTextures(int n, const unsigned *ids)
{
    int i;

    for (i = 0; i < n; i++)
        paltex_drop(ids[i]);
    GLCALL(void, glDeleteTextures, (int, const unsigned *))(n, ids);
}

static void l_glTexImage2D(unsigned target, int level, int ifmt, int w, int h, int border,
                           unsigned fmt, unsigned type, const void *pixels)
{
    struct paltex *t;

    if (target != GL_TEXTURE_2D || fmt != GL_COLOR_INDEX || type != GL_UNSIGNED_BYTE) {
        if (target == GL_TEXTURE_2D && level == 0)
            paltex_drop(bound2d);
        GLCALL(void, glTexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *))
            (target, level, ifmt, w, h, border, fmt, type, pixels);
        return;
    }
    if (!(t = paltex_find(bound2d, 1))) {
        LOG("GL: too many paletted textures");
        return;
    }
    if (!t->idx || t->w != w || t->h != h) {
        free(t->idx);
        t->idx = malloc((size_t)w * h);
        if (!t->idx)
            return;
    }
    t->w = w;
    t->h = h;
    t->level = level;
    t->border = border;
    if (pixels) {
        int align = 4, stride, y;

        GLCALL(void, glGetIntegerv, (unsigned, int *))(GL_UNPACK_ALIGNMENT, &align);
        stride = (w + align - 1) / align * align;
        for (y = 0; y < h; y++)
            memcpy(t->idx + (size_t)y * w, (const uint8_t *)pixels + (size_t)y * stride, w);
    } else {
        memset(t->idx, 0, (size_t)w * h);
    }
    paltex_upload(t);
}

/* target GL_SHARED_TEXTURE_PALETTE_EXT (or a texture's own), 256 RGBA bytes */
static void l_glColorTableEXT(unsigned target, unsigned ifmt, int n, unsigned fmt, unsigned type,
                              const void *table)
{
    struct paltex *t;

    (void)target;
    (void)ifmt;
    if (fmt != GL_RGBA || type != GL_UNSIGNED_BYTE || n < 1 || !table)
        return;
    if (n > 256)
        n = 256;
    memcpy(palette, table, (size_t)n * 4);
    if ((t = paltex_find(bound2d, 0)) && memcmp(t->used, palette, sizeof(palette)))
        paltex_upload(t);
}

/* ---- GLX (only what the games and GLEW ask) ------------------------------------------------ */

static int noop_swap_interval(int i) { (void)i; return 0; }

static void *l_glXGetProcAddressARB(const char *name)
{
    void *p;

    if (!name)
        return NULL;
    if (!strcmp(name, "glXSwapIntervalSGI") || !strcmp(name, "glXSwapIntervalMESA"))
        return (void *)noop_swap_interval;
    if (!strcmp(name, "glColorTableEXT"))
        return (void *)l_glColorTableEXT;
    if (!strncmp(name, "glX", 3)) {
        p = native_lookup(name);
        if (lx_verbose && !p)
            LOG("glXGetProcAddress(%s): none", name);
        return p;
    }
    if (name[0] == 'g' && name[1] == 'l') {
        /* only what the driver has: GLEW and the games test for NULL */
        if (!GetProcAddress(ogl, name) && !wgl_get(name))
            return NULL;
        return gl_proc(name);
    }
    return NULL;
}

void *gl_get_proc_address(const char *name)
{
    return l_glXGetProcAddressARB(name);
}

static int l_glXQueryVersion(void *dpy, int *maj, int *min)
{
    (void)dpy;
    if (maj)
        *maj = 1;
    if (min)
        *min = 4;
    return 1;
}

static const char *l_glXGetClientString(void *dpy, int name)
{
    (void)dpy;
    switch (name) {
    case 1: return "IGS loader";
    case 2: return "1.4";
    default: return "GLX_ARB_get_proc_address";
    }
}

static const char *l_glXQueryExtensionsString(void *dpy, int screen)
{
    (void)dpy;
    (void)screen;
    return "GLX_ARB_get_proc_address";
}

static int l_glXQueryExtension(void *dpy, int *e, int *v) { (void)dpy; if (e) *e = 0; if (v) *v = 0; return 1; }

/* contexts, visuals and drawables: x11.c */
const struct nsym gl_syms[] = {
    N(glXGetProcAddressARB), NA(glXGetProcAddress, l_glXGetProcAddressARB),
    N(glXQueryVersion), N(glXGetClientString), N(glXQueryExtensionsString),
    NA(glXQueryServerString, l_glXGetClientString), N(glXQueryExtension),
    N(glBindTexture), N(glDeleteTextures), N(glTexImage2D), N(glColorTableEXT),
    { NULL, NULL },
};
