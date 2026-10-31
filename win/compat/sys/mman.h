/* <sys/mman.h> for the plugins built as Windows DLLs: anonymous mappings only */
#ifndef IGS_COMPAT_MMAN_H
#define IGS_COMPAT_MMAN_H

#include <stddef.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *)-1)

__declspec(dllimport) void *__stdcall VirtualAlloc(void *, unsigned long, unsigned long, unsigned long);
__declspec(dllimport) int __stdcall VirtualFree(void *, unsigned long, unsigned long);

static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    void *p;

    (void)addr;
    (void)prot;
    (void)off;
    if (!(flags & MAP_ANONYMOUS) || fd != -1)
        return MAP_FAILED;
    p = VirtualAlloc(NULL, len, 0x3000 /* MEM_COMMIT | MEM_RESERVE */, 0x04 /* PAGE_READWRITE */);
    return p ? p : MAP_FAILED;
}

static inline int munmap(void *p, size_t len)
{
    (void)len;
    return VirtualFree(p, 0, 0x8000 /* MEM_RELEASE */) ? 0 : -1;
}

#endif
