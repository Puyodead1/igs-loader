/* <dlfcn.h> for the plugins built as Windows DLLs: LoadLibrary / GetProcAddress */
#ifndef IGS_COMPAT_DLFCN_H
#define IGS_COMPAT_DLFCN_H

#include <stdio.h>

#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL  0

__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) int __stdcall FreeLibrary(void *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);

static inline void *dlopen(const char *path, int flags)
{
    (void)flags;
    return LoadLibraryA(path);
}

static inline void *dlsym(void *h, const char *name)
{
    return GetProcAddress(h, name);
}

static inline int dlclose(void *h)
{
    return FreeLibrary(h) ? 0 : -1;
}

static inline const char *dlerror(void)
{
    static char buf[32];

    snprintf(buf, sizeof(buf), "Windows error %lu", GetLastError());
    return buf;
}

#endif
