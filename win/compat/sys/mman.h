/*
 * <sys/mman.h> for the plugins built as Windows DLLs: anonymous mappings
 * only, placed top-down (high addresses, as Linux's mmap gives a 32-bit
 * process; E1000's game ANDs its mapped pointers together).
 */
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
    p = VirtualAlloc(NULL, len, 0x103000 /* MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN */,
                     0x04 /* PAGE_READWRITE */);
    return p ? p : MAP_FAILED;
}

__declspec(dllimport) int __stdcall VirtualProtect(void *, unsigned long, unsigned long, unsigned long *);

/* the loader maps ELF code read-write-execute already; this makes sure */
static inline int mprotect(void *p, size_t len, int prot)
{
    unsigned long old;

    (void)prot;
    return VirtualProtect(p, len, 0x40 /* PAGE_EXECUTE_READWRITE */, &old) ? 0 : -1;
}

static inline int munmap(void *p, size_t len)
{
    (void)len;
    return VirtualFree(p, 0, 0x8000 /* MEM_RELEASE */) ? 0 : -1;
}

#endif
