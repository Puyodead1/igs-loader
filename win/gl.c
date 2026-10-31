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

/* ---- GLX (only what the games and GLEW ask) ------------------------------------------------ */

static int noop_swap_interval(int i) { (void)i; return 0; }

static void *l_glXGetProcAddressARB(const char *name)
{
    void *p;

    if (!name)
        return NULL;
    if (!strcmp(name, "glXSwapIntervalSGI") || !strcmp(name, "glXSwapIntervalMESA"))
        return (void *)noop_swap_interval;
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
    { NULL, NULL },
};
