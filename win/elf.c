/*
 * IGS loader, Windows core: the i386 ELF loader.
 *
 * Loads the game executable at its link address and its libraries anywhere,
 * resolves symbols in ld.so order (the executable, then DT_NEEDED breadth
 * first) with the native shim libraries (libc, SDL, GL...) taking the place
 * of the first system library in that order, applies the relocations (REL
 * and RELA: Fedora's prelinked libraries carry RELA), dynamic TLS through
 * ___tls_get_addr, and the stack protector fix-up (%gs:0x14 -> %fs:0x14,
 * NT_TIB.ArbitraryUserPointer, a per-thread word).  Libraries are found by
 * DT_SONAME in the search list, never through symlinks.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "lx.h"

typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;
typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; } Phdr;
typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize; } Shdr;
typedef struct { int32_t d_tag; uint32_t d_val; } Dyn;
typedef struct { uint32_t st_name, st_value, st_size; uint8_t st_info, st_other; uint16_t st_shndx; } Sym;
typedef struct { uint32_t r_offset, r_info; } Rel;
typedef struct { uint32_t r_offset, r_info; int32_t r_addend; } Rela;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_PHDR 6
#define PT_TLS 7
#define PF_X 1

#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_INIT 12
#define DT_SONAME 14
#define DT_REL 17
#define DT_RELSZ 18
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_GNU_HASH 0x6ffffef5
#define DT_VERSYM 0x6ffffff0
#define DT_VERDEF 0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#define DT_VERNEED 0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#define R_386_COPY 5
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8
#define R_386_TLS_TPOFF 14
#define R_386_TLS_DTPMOD32 35
#define R_386_TLS_DTPOFF32 36
#define R_386_TLS_TPOFF32 37

#define STB_LOCAL 0
#define STB_WEAK 2
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_TLS 6

struct elf_mod {
    char name[128];             /* DT_SONAME, or the file name */
    char path[MAX_PATH];
    int native, is_exe;
    HMODULE dll;                /* a preloaded native module: its exports */
    uintptr_t bias;             /* load address minus link address */
    uint8_t *map;
    size_t maplen;
    Phdr *phdr;
    int phnum;
    Dyn *dyn;
    Sym *symtab;
    const char *strtab;
    uint32_t nsyms;
    uint32_t *hash, *gnuhash;
    uint16_t *versym;
    const char **vername;
    uint32_t nver;
    struct elf_mod **needed;
    int nneeded;
    uint32_t tls_id, tls_filesz, tls_memsz, tls_align;
    void *tls_image;
    int loaded_deps, linked, inited;
    uint32_t entry;
    struct elf_mod *next;
};

static struct elf_mod *mods, *mods_tail, *exe_mod;
static char dl_error[512];
static CRITICAL_SECTION dl_lock;
static int dl_lock_init;
static unsigned long long dl_adds;

/* ---- executable stubs ---------------------------------------------------------- */

void *stub_alloc(size_t n)
{
    static uint8_t *cur, *end;
    static CRITICAL_SECTION cs;
    static LONG init;
    void *p;

    if (InterlockedExchange(&init, 1) == 0)
        InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    n = (n + 15) & ~(size_t)15;
    if (!cur || cur + n > end) {
        cur = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        end = cur + 0x10000;
    }
    p = cur;
    cur += n;
    LeaveCriticalSection(&cs);
    return p;
}

/* ---- the native symbol table ------------------------------------------------------ */

#define NHASH 4096
static struct nent { const char *name; void *addr; struct nent *next; } *nhash[NHASH];

static uint32_t elf_hash(const char *s)
{
    uint32_t h = 0, g;

    while (*s) {
        h = (h << 4) + (uint8_t)*s++;
        g = h & 0xf0000000;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t gnu_hash(const char *s)
{
    uint32_t h = 5381;

    while (*s)
        h = h * 33 + (uint8_t)*s++;
    return h;
}

void elf_set_native(const struct nsym *const *tables)
{
    int t;

    for (t = 0; tables[t]; t++) {
        const struct nsym *s;

        for (s = tables[t]; s->name; s++) {
            uint32_t h = elf_hash(s->name) % NHASH;
            struct nent *e = malloc(sizeof(*e));

            e->name = s->name;
            e->addr = s->addr;
            e->next = nhash[h];
            nhash[h] = e;
        }
    }
}

void *native_lookup(const char *name)
{
    struct nent *e;

    for (e = nhash[elf_hash(name) % NHASH]; e; e = e->next)
        if (!strcmp(e->name, name))
            return e->addr;
    /* any other GL entry point: a thunk to opengl32 / wglGetProcAddress */
    if (name[0] == 'g' && name[1] == 'l' && isupper((unsigned char)name[2]))
        return gl_proc(name);
    return NULL;
}

/* for preloaded DLLs: the shims behind dlvsym(RTLD_NEXT, ...) (win/compat/dlfcn.h) */
__declspec(dllexport) void *igs_native_lookup(const char *name)
{
    return native_lookup(name);
}

/* libraries the .exe provides natively (or as empty stand-ins) */
static int is_native_lib(const char *name)
{
    static const char *const libs[] = {
        "libc.so", "libm.so", "libpthread.so", "libdl.so", "librt.so", "ld-linux.so",
        "libSDL-1.2.so", "libGL.so", "libXxf86vm.so", "libpci.so", "libncurses.so",
        "libtinfo.so", "libX11.so", "libXext.so", "libXmu.so", "libXi.so", "libXt.so",
        "libSM.so", "libICE.so", "libXrandr.so", "libXrender.so", "libXau.so", "libXdmcp.so",
        "libxcb.so", "libXaw.so", "libXpm.so", "libXfont.so", "libXcursor.so", "libasound.so", NULL,
    };
    int i;

    for (i = 0; libs[i]; i++)
        if (!strncmp(name, libs[i], strlen(libs[i])))
            return 1;
    return 0;
}

/* ---- the library search list --------------------------------------------------- */

static struct cand {
    char path[MAX_PATH];
    char base[128];
    char soname[128];           /* "" = not read yet, "-" = not an i386 ELF */
} *cands;
static int ncands, maxcands;

static void cand_add(const char *path, const char *base)
{
    if (ncands == maxcands) {
        maxcands = maxcands ? maxcands * 2 : 256;
        cands = realloc(cands, maxcands * sizeof(*cands));
    }
    snprintf(cands[ncands].path, MAX_PATH, "%s", path);
    snprintf(cands[ncands].base, sizeof(cands[ncands].base), "%s", base);
    cands[ncands].soname[0] = 0;
    ncands++;
}

void elf_add_search(const char *path)
{
    char pat[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD attr = GetFileAttributesA(path);

    if (attr == INVALID_FILE_ATTRIBUTES)
        return;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        const char *b = strrchr(path, '\\');

        if (!b)
            b = strrchr(path, '/');
        snprintf(full, sizeof(full), "%s", path);
        lx_resolve(full, sizeof(full));     /* an explicit file may be a WSL symlink */
        cand_add(full, b ? b + 1 : path);
        return;
    }
    snprintf(pat, sizeof(pat), "%s\\*", path);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        /* WSL symlinks are reparse points Windows cannot read: skip them */
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        if (!strstr(fd.cFileName, ".so"))
            continue;
        snprintf(full, sizeof(full), "%s\\%s", path, fd.cFileName);
        cand_add(full, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got;
    uint8_t *buf;

    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(h, NULL);
    buf = malloc(size ? size : 1);
    if (!buf || !ReadFile(h, buf, size, &got, NULL) || got != size) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *len = size;
    return buf;
}

/* DT_SONAME straight from the file (no mapping) */
static void read_soname(struct cand *c)
{
    size_t len;
    uint8_t *d = read_file(c->path, &len);
    Ehdr *eh = (Ehdr *)d;
    Phdr *ph;
    uint32_t strtab = 0, soname = (uint32_t)-1, i, dynoff = 0, dynsz = 0;

    snprintf(c->soname, sizeof(c->soname), "-");
    if (!d || len < sizeof(Ehdr) || memcmp(d, "\x7f" "ELF", 4) || d[4] != 1 || eh->e_machine != 3) {
        free(d);
        return;
    }
    ph = (Phdr *)(d + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) {
            dynoff = ph[i].p_offset;
            dynsz = ph[i].p_filesz;
        }
    snprintf(c->soname, sizeof(c->soname), "%s", c->base);
    for (i = 0; dynoff && i + 8 <= dynsz; i += 8) {
        Dyn *dy = (Dyn *)(d + dynoff + i);

        if (dy->d_tag == DT_STRTAB)
            strtab = dy->d_val;
        else if (dy->d_tag == DT_SONAME)
            soname = dy->d_val;
    }
    if (strtab && soname != (uint32_t)-1) {
        /* strtab is a vaddr: find its file offset */
        for (i = 0; i < eh->e_phnum; i++)
            if (ph[i].p_type == PT_LOAD && strtab >= ph[i].p_vaddr &&
                strtab < ph[i].p_vaddr + ph[i].p_filesz) {
                uint32_t o = strtab - ph[i].p_vaddr + ph[i].p_offset + soname;

                if (o < len)
                    snprintf(c->soname, sizeof(c->soname), "%s", (char *)d + o);
                break;
            }
    }
    free(d);
}

/*
 * A DT_NEEDED name to a file: the first candidate (search list order) whose
 * DT_SONAME is the name, looking only at files named like it (the name
 * itself, the name plus a version suffix, or the same stem: libgcc_s.so.1
 * is libgcc_s-4.3.2-20081105.so.1).
 */
static const char *find_lib(const char *name)
{
    char stem[128];
    char *p;
    int i;

    snprintf(stem, sizeof(stem), "%s", name);
    if ((p = strstr(stem, ".so")))
        *p = 0;
    for (i = 0; i < ncands; i++) {
        struct cand *c = &cands[i];

        if (strncmp(c->base, stem, strlen(stem)))
            continue;
        if (!c->soname[0])
            read_soname(c);
        if (!strcmp(c->soname, name))
            return c->path;
    }
    return NULL;
}

/* ---- mapping ---------------------------------------------------------------------- */

static void *low_reserve;          /* main's early hold on the executable's range */

void elf_reserve_low(void)
{
    low_reserve = VirtualAlloc((void *)0x08000000, 0x08000000, MEM_RESERVE, PAGE_NOACCESS);
}

/*
 * The thread pointer (%gs:0 on Linux) of a statically linked TLS user (Unity's
 * libstdc++ __cxa_get_globals: %gs:0 - 8): a TLS slot, fs:[0xE10 + 4 slot],
 * holding the end of this thread's block of the executable's TLS.
 */
static DWORD tp_slot = TLS_OUT_OF_INDEXES;

static uint32_t tp_disp(void)
{
    if (tp_slot == TLS_OUT_OF_INDEXES && ((tp_slot = TlsAlloc()) == TLS_OUT_OF_INDEXES || tp_slot >= 64))
        return 0;
    return 0xE10 + 4 * tp_slot;
}

/* rewrite the stack protector's %gs:0x14 reads to %fs:0x14, and %gs:0 to the thread-pointer slot */
static int fix_gs(uint8_t *p, size_t n)
{
    size_t i;
    int count = 0;

    for (i = 0; i + 7 <= n; i++) {
        uint32_t d;

        if (p[i] != 0x65)
            continue;
        if (p[i + 1] == 0xa1 && !memcmp(p + i + 2, "\x14\0\0\0", 4)) {
            p[i] = 0x64;
            count++;
        } else if ((p[i + 1] == 0x8b || p[i + 1] == 0x33) && (p[i + 2] & 0xc7) == 0x05 &&
                   !memcmp(p + i + 3, "\x14\0\0\0", 4)) {
            p[i] = 0x64;
            count++;
        } else if (p[i + 1] == 0xa1 && !memcmp(p + i + 2, "\0\0\0\0", 4) && (d = tp_disp())) {
            p[i] = 0x64;                   /* mov eax, gs:0 -> mov eax, fs:[slot] */
            memcpy(p + i + 2, &d, 4);
            count++;
        } else if (p[i + 1] == 0x8b && (p[i + 2] & 0xc7) == 0x05 && !memcmp(p + i + 3, "\0\0\0\0", 4) &&
                   (d = tp_disp())) {
            p[i] = 0x64;                   /* mov reg, gs:0 */
            memcpy(p + i + 3, &d, 4);
            count++;
        }
    }
    return count;
}

/* this thread's thread pointer: set on every thread before it runs ELF code */
void elf_thread_tp(void)
{
    struct elf_mod *m = exe_mod;
    uint32_t ti[2];
    uint8_t *block;

    if (tp_slot == TLS_OUT_OF_INDEXES || !m || !m->tls_id || TlsGetValue(tp_slot))
        return;
    ti[0] = m->tls_id;
    ti[1] = 0;
    if ((block = elf_tls_get_addr(ti)))
        TlsSetValue(tp_slot, block + ((m->tls_memsz + m->tls_align - 1) & ~(m->tls_align - 1)));
}

static uint32_t tls_next_id = 1;
static struct elf_mod *tls_mods[64];

static uintptr_t dynval(struct elf_mod *m, int32_t tag, int *found)
{
    Dyn *d;

    for (d = m->dyn; d && d->d_tag; d++)
        if (d->d_tag == tag) {
            if (found)
                *found = 1;
            return d->d_val;
        }
    if (found)
        *found = 0;
    return 0;
}

static void parse_dynamic(struct elf_mod *m)
{
    int f;
    uintptr_t v;
    Dyn *d;

    m->strtab = (const char *)(m->bias + dynval(m, DT_STRTAB, NULL));
    m->symtab = (Sym *)(m->bias + dynval(m, DT_SYMTAB, NULL));
    v = dynval(m, DT_HASH, &f);
    if (f) {
        m->hash = (uint32_t *)(m->bias + v);
        m->nsyms = m->hash[1];
    }
    v = dynval(m, DT_GNU_HASH, &f);
    if (f) {
        uint32_t *g = (uint32_t *)(m->bias + v), nb = g[0], symoff = g[1], bloom = g[2], i;
        uint32_t *buckets = g + 4 + bloom, *chain = buckets + nb, max = 0;

        m->gnuhash = g;
        if (!m->hash) {
            for (i = 0; i < nb; i++)
                if (buckets[i] > max)
                    max = buckets[i];
            if (max >= symoff)
                while (!(chain[max - symoff] & 1))
                    max++;
            m->nsyms = max + 1;
        }
    }
    v = dynval(m, DT_SONAME, &f);
    if (f)
        snprintf(m->name, sizeof(m->name), "%s", m->strtab + v);
    v = dynval(m, DT_VERSYM, &f);
    if (f)
        m->versym = (uint16_t *)(m->bias + v);
    /* version index -> name, from both the definitions and the needs */
    m->nver = 64;
    m->vername = calloc(m->nver, sizeof(char *));
    v = dynval(m, DT_VERDEF, &f);
    if (f) {
        uint8_t *p = (uint8_t *)(m->bias + v);

        for (;;) {
            uint16_t ndx = *(uint16_t *)(p + 4);
            uint32_t aux = *(uint32_t *)(p + 12), next = *(uint32_t *)(p + 16);

            if (ndx < m->nver)
                m->vername[ndx] = m->strtab + *(uint32_t *)(p + aux);
            if (!next)
                break;
            p += next;
        }
    }
    v = dynval(m, DT_VERNEED, &f);
    if (f) {
        uint8_t *p = (uint8_t *)(m->bias + v);

        for (;;) {
            uint16_t cnt = *(uint16_t *)(p + 2);
            uint32_t aux = *(uint32_t *)(p + 8), next = *(uint32_t *)(p + 12);
            uint8_t *a = p + aux;
            int i;

            for (i = 0; i < cnt; i++) {
                uint16_t other = *(uint16_t *)(a + 6);

                if (other < m->nver)
                    m->vername[other] = m->strtab + *(uint32_t *)(a + 8);
                if (!*(uint32_t *)(a + 12))
                    break;
                a += *(uint32_t *)(a + 12);
            }
            if (!next)
                break;
            p += next;
        }
    }
    m->nneeded = 0;
    for (d = m->dyn; d->d_tag; d++)
        if (d->d_tag == DT_NEEDED)
            m->nneeded++;
    m->needed = calloc(m->nneeded + 1, sizeof(*m->needed));
}

static struct elf_mod *map_elf(const char *path, int is_exe)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    Ehdr *eh = (Ehdr *)d;
    Phdr *ph;
    struct elf_mod *m;
    uint32_t lo = 0xffffffff, hi = 0, i;
    uint8_t *base;
    int gs = 0, by_section = 0;

    if (!d) {
        snprintf(dl_error, sizeof(dl_error), "%s: cannot read", path);
        return NULL;
    }
    if (len < sizeof(Ehdr) || memcmp(d, "\x7f" "ELF", 4) || d[4] != 1 || eh->e_machine != 3) {
        snprintf(dl_error, sizeof(dl_error), "%s: not an i386 ELF", path);
        free(d);
        return NULL;
    }
    ph = (Phdr *)(d + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < lo)
                lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > hi)
                hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    lo &= ~0xfffu;
    hi = (hi + 0xfff) & ~0xfffu;
    m = calloc(1, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", path);
    {
        const char *b = strrchr(path, '\\');

        if (!b)
            b = strrchr(path, '/');
        snprintf(m->name, sizeof(m->name), "%s", b ? b + 1 : path);
    }
    if (eh->e_type == 2) {          /* ET_EXEC: at its link address */
        uint32_t lo64 = lo & ~0xffffu;

        if (low_reserve) {
            VirtualFree(low_reserve, 0, MEM_RELEASE);
            low_reserve = NULL;
        }
        base = VirtualAlloc((void *)(uintptr_t)lo64, hi - lo64, MEM_RESERVE | MEM_COMMIT,
                            PAGE_EXECUTE_READWRITE);
        if (base != (uint8_t *)(uintptr_t)lo64) {
            snprintf(dl_error, sizeof(dl_error), "%s: cannot map at 0x%x-0x%x (error %lu)",
                     path, lo, hi, GetLastError());
            free(d);
            free(m);
            return NULL;
        }
        m->bias = 0;
        m->map = base;
        m->maplen = hi - lo64;
    } else {
        base = VirtualAlloc(NULL, hi - lo, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!base) {
            snprintf(dl_error, sizeof(dl_error), "%s: out of memory", path);
            free(d);
            free(m);
            return NULL;
        }
        m->bias = (uintptr_t)base - lo;
        m->map = base;
        m->maplen = hi - lo;
    }
    for (i = 0; i < eh->e_phnum; i++) {
        Phdr *p = &ph[i];

        if (p->p_type == PT_LOAD) {
            memcpy((void *)(m->bias + p->p_vaddr), d + p->p_offset, p->p_filesz);
        } else if (p->p_type == PT_DYNAMIC) {
            m->dyn = (Dyn *)(m->bias + p->p_vaddr);
        } else if (p->p_type == PT_PHDR) {
            m->phdr = (Phdr *)(m->bias + p->p_vaddr);
        } else if (p->p_type == PT_TLS) {
            m->tls_filesz = p->p_filesz;
            m->tls_memsz = p->p_memsz;
            m->tls_align = p->p_align ? p->p_align : 4;
            m->tls_image = (void *)(m->bias + p->p_vaddr);
            if (tls_next_id < 64) {
                m->tls_id = tls_next_id++;
                tls_mods[m->tls_id] = m;
            }
        }
    }
    /* %gs rewrites: in the code sections (SHF_EXECINSTR), else the executable segments; the
       segments also hold .gnu.hash, .dynsym and .dynstr, whose bytes may look like the patterns */
    if (eh->e_shoff && eh->e_shentsize == sizeof(Shdr) && eh->e_shnum &&
        (uint64_t)eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Shdr) <= len) {
        Shdr *sh = (Shdr *)(d + eh->e_shoff);

        for (i = 0; i < eh->e_shnum; i++)
            if (sh[i].sh_type == 1 && (sh[i].sh_flags & 4) && sh[i].sh_addr >= lo &&
                sh[i].sh_addr + sh[i].sh_size <= hi) {   /* PROGBITS, EXECINSTR */
                gs += fix_gs((uint8_t *)(m->bias + sh[i].sh_addr), sh[i].sh_size);
                by_section = 1;
            }
    }
    for (i = 0; !by_section && i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X))
            gs += fix_gs((uint8_t *)(m->bias + ph[i].p_vaddr), ph[i].p_filesz);
    m->phnum = eh->e_phnum;
    if (!m->phdr) {
        /* program headers inside the first segment, or a copy */
        if (eh->e_phoff >= 0 && lo + eh->e_phoff + eh->e_phnum * sizeof(Phdr) <= hi &&
            ph[0].p_offset == 0)
            m->phdr = (Phdr *)(m->bias + lo + eh->e_phoff);
        else {
            m->phdr = malloc(eh->e_phnum * sizeof(Phdr));
            memcpy(m->phdr, ph, eh->e_phnum * sizeof(Phdr));
        }
    }
    m->entry = eh->e_entry + m->bias;
    m->is_exe = is_exe;
    free(d);
    if (!m->dyn) {
        snprintf(dl_error, sizeof(dl_error), "%s: no dynamic section", path);
        return NULL;
    }
    parse_dynamic(m);
    if (lx_lddebug)
        LOG("load %s at %p (%u KiB), %d stack-protector reads moved to %%fs", m->name,
            (void *)m->map, (unsigned)(m->maplen >> 10), gs);
    if (!mods)
        mods = m;
    else
        mods_tail->next = m;
    mods_tail = m;
    dl_adds++;
    return m;
}

static struct elf_mod *native_mod(const char *name)
{
    struct elf_mod *m;

    for (m = mods; m; m = m->next)
        if (m->native && !strcmp(m->name, name))
            return m;
    m = calloc(1, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->native = 1;
    m->linked = m->inited = m->loaded_deps = 1;
    if (!mods)
        mods = m;
    else
        mods_tail->next = m;
    mods_tail = m;
    return m;
}

static struct elf_mod *find_loaded(const char *name)
{
    struct elf_mod *m;

    for (m = mods; m; m = m->next)
        if (!strcmp(m->name, name))
            return m;
    return NULL;
}

/* load name's file, or the native stand-in */
static struct elf_mod *load_by_name(const char *name)
{
    struct elf_mod *m = find_loaded(name);
    const char *path;

    if (m)
        return m;
    if (is_native_lib(name))
        return native_mod(name);
    path = find_lib(name);
    /* a path (MuziBox: ./lib/libfmodex.so): the candidate by its file's soname,
       the list's libs naming the file */
    if (!path && strrchr(name, '/'))
        path = find_lib(strrchr(name, '/') + 1);
    if (!path) {
        snprintf(dl_error, sizeof(dl_error), "%s: not found in the search list", name);
        return NULL;
    }
    m = map_elf(path, 0);
    if (m && strcmp(m->name, name))
        LOG("%s: file %s has soname %s", name, path, m->name);
    return m;
}

/* DT_NEEDED breadth first, appending to the global list */
static int load_deps(struct elf_mod *root)
{
    struct elf_mod *m;
    int errors = 0;

    for (m = root; m; m = m->next) {
        Dyn *d;
        int i = 0;

        if (m->native || m->loaded_deps)
            continue;
        m->loaded_deps = 1;
        for (d = m->dyn; d->d_tag; d++) {
            if (d->d_tag != DT_NEEDED)
                continue;
            m->needed[i] = load_by_name(m->strtab + d->d_val);
            if (!m->needed[i]) {
                LOG("cannot load %s (needed by %s): %s", m->strtab + d->d_val, m->name, dl_error);
                errors++;
                continue;
            }
            i++;
        }
        m->nneeded = i;
    }
    return errors;
}

/* ---- symbol lookup ---------------------------------------------------------------------- */

static int ver_ok(struct elf_mod *m, uint32_t i, const char *ver, int relaxed)
{
    uint16_t v, idx;

    if (!m->versym)
        return 1;
    v = m->versym[i];
    idx = v & 0x7fff;
    if (ver && !relaxed) {
        if (idx <= 1)
            return 1;
        return idx < m->nver && m->vername[idx] && !strcmp(m->vername[idx], ver);
    }
    return !(v & 0x8000);
}

static int sym_ok(struct elf_mod *m, uint32_t i, const char *name, const char *ver, int relaxed)
{
    Sym *s = &m->symtab[i];
    int bind = s->st_info >> 4, type = s->st_info & 15;

    if (s->st_shndx == 0 || bind == STB_LOCAL)
        return 0;
    if (type != 0 && type != STT_OBJECT && type != STT_FUNC && type != STT_TLS && type != 5)
        return 0;
    if (strcmp(m->strtab + s->st_name, name))
        return 0;
    return ver_ok(m, i, ver, relaxed);
}

static Sym *mod_find(struct elf_mod *m, const char *name, const char *ver, int relaxed)
{
    if (m->gnuhash) {
        uint32_t *g = m->gnuhash, nb = g[0], symoff = g[1], bloom = g[2];
        uint32_t *buckets = g + 4 + bloom, *chain = buckets + nb;
        uint32_t h = gnu_hash(name), i = buckets[h % nb];

        if (i < symoff)
            return NULL;
        for (;; i++) {
            uint32_t h2 = chain[i - symoff];

            if ((h | 1) == (h2 | 1) && sym_ok(m, i, name, ver, relaxed))
                return &m->symtab[i];
            if (h2 & 1)
                break;
        }
        return NULL;
    }
    if (m->hash) {
        uint32_t nb = m->hash[0], *bucket = m->hash + 2, *chain = bucket + nb, i;

        for (i = bucket[elf_hash(name) % nb]; i; i = chain[i])
            if (sym_ok(m, i, name, ver, relaxed))
                return &m->symtab[i];
    }
    return NULL;
}

/* the global scope; skip = the executable for R_386_COPY */
static void *scope_lookup(const char *name, const char *ver, struct elf_mod *skip,
                          struct elf_mod **def, Sym **defsym)
{
    int relaxed;

    for (relaxed = 0; relaxed < 2; relaxed++) {
        struct elf_mod *m;
        int native_done = 0;

        for (m = mods; m; m = m->next) {
            Sym *s;

            if (m == skip)
                continue;
            if (m->dll) {
                void *p = (void *)GetProcAddress(m->dll, name);

                if (p) {
                    *def = m;
                    *defsym = NULL;
                    return p;
                }
                continue;
            }
            if (m->native) {
                void *p;

                if (native_done)
                    continue;
                native_done = 1;
                if ((p = native_lookup(name))) {
                    *def = m;
                    *defsym = NULL;
                    return p;
                }
                continue;
            }
            if ((s = mod_find(m, name, ver, relaxed))) {
                *def = m;
                *defsym = s;
                if ((s->st_info & 15) == STT_TLS)
                    return (void *)(uintptr_t)s->st_value;
                return (void *)(m->bias + s->st_value);
            }
        }
        if (!native_done) {
            void *p = native_lookup(name);

            if (p) {
                *def = NULL;
                *defsym = NULL;
                return p;
            }
        }
    }
    return NULL;
}

/* ---- missing symbols ------------------------------------------------------------------- */

static struct missing { char name[96]; char who[64]; struct missing *next; } *missing;
static int nmissing;

static void __attribute__((used)) missing_hit(const char *name)
{
    static char seen[64][96];
    static int nseen;
    int i;

    for (i = 0; i < nseen; i++)
        if (!strcmp(seen[i], name))
            return;
    if (nseen < 64)
        snprintf(seen[nseen++], 96, "%s", name);
    LOG("called unresolved function %s (returning 0)", name);
}

static void *missing_stub(const char *name)
{
    uint8_t *p = stub_alloc(24);
    char *copy = strdup(name);
    int32_t rel;

    p[0] = 0x68;                        /* push name */
    memcpy(p + 1, &copy, 4);
    p[5] = 0xe8;                        /* call missing_hit */
    rel = (int32_t)((uint8_t *)missing_hit - (p + 10));
    memcpy(p + 6, &rel, 4);
    p[10] = 0x83; p[11] = 0xc4; p[12] = 0x04;   /* add esp, 4 */
    p[13] = 0x31; p[14] = 0xc0;                 /* xor eax, eax */
    p[15] = 0xc3;                               /* ret */
    return p;
}

static void note_missing(const char *name, struct elf_mod *who)
{
    struct missing *m;

    for (m = missing; m; m = m->next)
        if (!strcmp(m->name, name))
            return;
    m = calloc(1, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    snprintf(m->who, sizeof(m->who), "%s", who->name);
    m->next = missing;
    missing = m;
    nmissing++;
}

/* ---- relocation ---------------------------------------------------------------------------- */

static int reloc_errors;

/* symbol i of m, for a relocation: its address (TLS: its offset), and module */
static uint32_t resolve(struct elf_mod *m, uint32_t i, struct elf_mod **def, int copy)
{
    Sym *s = &m->symtab[i], *ds = NULL;
    const char *name = m->strtab + s->st_name, *ver = NULL;
    int bind = s->st_info >> 4, vis = s->st_other & 3;
    void *p;

    *def = m;
    if (s->st_shndx != 0 && (bind == STB_LOCAL || vis == 2 || vis == 3)) {
        *def = m;
        return (s->st_info & 15) == STT_TLS ? s->st_value : m->bias + s->st_value;
    }
    if (m->versym) {
        uint16_t idx = m->versym[i] & 0x7fff;

        if (idx >= 2 && idx < m->nver && s->st_shndx == 0)
            ver = m->vername[idx];
    }
    p = scope_lookup(name, ver, copy ? exe_mod : NULL, def, &ds);
    if (p || (*def && ds)) {
        if (lx_lddebug > 1)
            LOG("  %s: %s%s%s -> %p (%s)", m->name, name, ver ? "@" : "", ver ? ver : "", p,
                *def ? (*def)->name : "native");
        if (!*def)
            *def = m;
        return (uint32_t)(uintptr_t)p;
    }
    if (s->st_shndx != 0) {           /* only our own definition (not exported) */
        *def = m;
        return (s->st_info & 15) == STT_TLS ? s->st_value : m->bias + s->st_value;
    }
    *def = NULL;
    if (bind == STB_WEAK)
        return 0;
    note_missing(name, m);
    if ((s->st_info & 15) == STT_OBJECT)
        return 0;
    return (uint32_t)(uintptr_t)missing_stub(name);
}

static void reloc_one(struct elf_mod *m, uint32_t off, uint32_t info, int has_addend, int32_t addend)
{
    uint32_t *P = (uint32_t *)(m->bias + off);
    uint32_t type = info & 0xff, si = info >> 8, S = 0;
    int32_t A = has_addend ? addend : (int32_t)*P;
    struct elf_mod *def = m;

    if (type == R_386_NONE)
        return;
    if (si && type != R_386_RELATIVE)
        S = resolve(m, si, &def, type == R_386_COPY);
    switch (type) {
    case R_386_32:
        *P = S + A;
        break;
    case R_386_PC32:
        *P = S + A - (uint32_t)(uintptr_t)P;
        break;
    case R_386_GLOB_DAT:
    case R_386_JMP_SLOT:
        *P = S;
        break;
    case R_386_RELATIVE:
        *P = m->bias + A;
        break;
    case R_386_COPY:
        if (S)
            memcpy(P, (void *)(uintptr_t)S, m->symtab[si].st_size);
        else
            LOG("%s: copy relocation for %s unresolved", m->name, m->strtab + m->symtab[si].st_name);
        break;
    case R_386_TLS_DTPMOD32:
        *P = si ? (def ? def->tls_id : 0) : m->tls_id;
        break;
    case R_386_TLS_DTPOFF32:
        *P = (si ? S : 0) + A;
        break;
    default:
        LOG("%s: unsupported relocation type %u at 0x%x (%s)", m->name, type, off,
            si ? m->strtab + m->symtab[si].st_name : "-");
        reloc_errors++;
    }
}

static void relocate(struct elf_mod *m)
{
    uintptr_t rel = dynval(m, DT_REL, NULL), relsz = dynval(m, DT_RELSZ, NULL);
    uintptr_t rela = dynval(m, DT_RELA, NULL), relasz = dynval(m, DT_RELASZ, NULL);
    uintptr_t jmp = dynval(m, DT_JMPREL, NULL), jmpsz = dynval(m, DT_PLTRELSZ, NULL);
    int pltrela = dynval(m, DT_PLTREL, NULL) == DT_RELA;
    uint32_t i;

    for (i = 0; rel && i < relsz / sizeof(Rel); i++) {
        Rel *r = (Rel *)(m->bias + rel) + i;

        reloc_one(m, r->r_offset, r->r_info, 0, 0);
    }
    for (i = 0; rela && i < relasz / sizeof(Rela); i++) {
        Rela *r = (Rela *)(m->bias + rela) + i;

        reloc_one(m, r->r_offset, r->r_info, 1, r->r_addend);
    }
    if (pltrela) {
        for (i = 0; jmp && i < jmpsz / sizeof(Rela); i++) {
            Rela *r = (Rela *)(m->bias + jmp) + i;

            reloc_one(m, r->r_offset, r->r_info, 1, r->r_addend);
        }
    } else {
        for (i = 0; jmp && i < jmpsz / sizeof(Rel); i++) {
            Rel *r = (Rel *)(m->bias + jmp) + i;

            reloc_one(m, r->r_offset, r->r_info, 0, 0);
        }
    }
}

/* relocate everything not linked yet; returns the number of missing symbols */
static int link_new(void)
{
    struct elf_mod *m;

    /*
     * The executable last: its copy relocations take the libraries' data
     * (vtables...) as relocated, like ld.so.  Binding does not depend on the
     * order, only copied contents do.
     */
    for (m = mods; m; m = m->next)
        if (!m->native && !m->linked && !m->is_exe) {
            m->linked = 1;
            relocate(m);
        }
    for (m = mods; m; m = m->next)
        if (!m->native && !m->linked) {
            m->linked = 1;
            relocate(m);
        }
    return nmissing;
}

int elf_link_all(int check_only)
{
    struct missing *mi;
    struct elf_mod *m;
    int n;

    n = link_new();
    LOG("modules:");
    for (m = mods; m; m = m->next)
        LOG("  %-28s %s", m->name, m->dll ? m->path : m->native ? "(native)" : m->path);
    if (n) {
        LOG("%d unresolved symbol%s:", n, n > 1 ? "s" : "");
        for (mi = missing; mi; mi = mi->next)
            LOG("  %-40s (%s)", mi->name, mi->who);
    } else {
        LOG("all symbols resolved");
    }
    if (reloc_errors)
        LOG("%d unsupported relocations", reloc_errors);
    (void)check_only;
    return n + reloc_errors;
}

/* ---- initialisation ------------------------------------------------------------------------ */

static int g_argc;
static char **g_argv, **g_envp;

static void init_mod(struct elf_mod *m)
{
    uintptr_t init, arr, arrsz;
    int f, i;

    if (m->inited || m->native)
        return;
    m->inited = 1;
    for (i = 0; i < m->nneeded; i++)
        init_mod(m->needed[i]);
    if (m->is_exe)
        return;        /* __libc_start_main runs the executable's own */
    if (lx_lddebug)
        LOG("init %s", m->name);
    init = dynval(m, DT_INIT, &f);
    if (f)
        elf_call3((void *)(m->bias + init), g_argc, U32(g_argv), U32(g_envp));
    arr = dynval(m, DT_INIT_ARRAY, &f);
    arrsz = dynval(m, DT_INIT_ARRAYSZ, NULL);
    for (i = 0; f && i < (int)(arrsz / 4); i++) {
        uint32_t fn = ((uint32_t *)(m->bias + arr))[i];

        if (fn && fn != 0xffffffff)
            elf_call3((void *)(uintptr_t)fn, g_argc, U32(g_argv), U32(g_envp));
    }
}

void elf_run_inits(int argc, char **argv, char **envp)
{
    struct elf_mod *m;

    g_argc = argc;
    g_argv = argv;
    g_envp = envp;
    for (m = mods; m; m = m->next)
        init_mod(m);
}

/*
 * Preloaded native modules (the feature "preload"): DLLs whose exports stand
 * in for the library named soname, ahead of everything but the executable
 * in the lookup order, as LD_PRELOAD puts them.
 */
static struct { char soname[128]; HMODULE dll; } preloads[8];
static int npreloads;

void elf_preload(const char *soname, void *dll)
{
    if (npreloads < 8) {
        snprintf(preloads[npreloads].soname, sizeof(preloads[0].soname), "%s", soname);
        preloads[npreloads++].dll = dll;
    }
}

struct elf_mod *elf_load_exe(const char *path)
{
    int i;

    if (!dl_lock_init) {
        InitializeCriticalSection(&dl_lock);
        dl_lock_init = 1;
    }
    exe_mod = map_elf(path, 1);
    if (!exe_mod) {
        LOG("%s", dl_error);
        return NULL;
    }
    for (i = 0; i < npreloads; i++) {
        struct elf_mod *m = calloc(1, sizeof(*m));

        snprintf(m->name, sizeof(m->name), "%s", preloads[i].soname);
        GetModuleFileNameA(preloads[i].dll, m->path, sizeof(m->path));
        m->native = 1;
        m->dll = preloads[i].dll;
        m->linked = m->inited = m->loaded_deps = 1;
        mods_tail->next = m;
        mods_tail = m;
    }
    if (load_deps(exe_mod))
        return NULL;
    return exe_mod;
}

/* jump to _start with the stack the kernel would give it */
__asm__(
    ".globl _enter_elf\n"
    "_enter_elf:\n"
    "    movl 4(%esp), %eax\n"
    "    movl 8(%esp), %esp\n"
    "    xorl %edx, %edx\n"
    "    xorl %ebp, %ebp\n"
    "    jmp *%eax\n");
void enter_elf(uint32_t entry, uint32_t *sp) __attribute__((noreturn));

void elf_start(struct elf_mod *exe, int argc, char **argv, char **envp)
{
    int nenv = 0, i, n;
    uint32_t *sp;

    while (envp[nenv])
        nenv++;
    n = 1 + argc + 1 + nenv + 1 + 2;
    sp = __builtin_alloca(n * 4 + 64);
    sp = (uint32_t *)(((uintptr_t)sp + 15) & ~(uintptr_t)15);
    i = 0;
    sp[i++] = argc;
    for (n = 0; n < argc; n++)
        sp[i++] = U32(argv[n]);
    sp[i++] = 0;
    for (n = 0; n < nenv; n++)
        sp[i++] = U32(envp[n]);
    sp[i++] = 0;
    sp[i++] = 0;              /* AT_NULL */
    sp[i++] = 0;
    LOG("entering %s at 0x%x", exe->name, exe->entry);
    enter_elf(exe->entry, sp);
}

/* ---- calling ELF code ------------------------------------------------------------------------ */

__asm__(
    ".globl _elf_call\n"
    ".globl _elf_call64\n"
    "_elf_call:\n"
    "_elf_call64:\n"
    "    pushl %ebp\n"
    "    movl %esp, %ebp\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    movl 12(%ebp), %ecx\n"
    "    movl 16(%ebp), %esi\n"
    "    leal 0(,%ecx,4), %eax\n"
    "    subl %eax, %esp\n"
    "    andl $-16, %esp\n"
    "    movl %esp, %edi\n"
    "    cld\n"
    "    rep movsl\n"
    "    call *8(%ebp)\n"
    "    leal -8(%ebp), %esp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebp\n"
    "    ret\n");

/* ---- TLS ---------------------------------------------------------------------------------------- */

static __thread void **dtv;
static __thread uint32_t ndtv;

void *elf_tls_get_addr(uint32_t *ti)
{
    uint32_t id = ti[0];
    struct elf_mod *m;

    if (id >= ndtv) {
        dtv = realloc(dtv, 64 * sizeof(void *));
        memset(dtv + ndtv, 0, (64 - ndtv) * sizeof(void *));
        ndtv = 64;
    }
    if (!dtv[id]) {
        m = id < 64 ? tls_mods[id] : NULL;
        if (!m) {
            LOG("TLS access to unknown module %u", id);
            return NULL;
        }
        dtv[id] = _aligned_malloc(m->tls_memsz ? m->tls_memsz : 1, m->tls_align < 16 ? 16 : m->tls_align);
        memset(dtv[id], 0, m->tls_memsz);
        memcpy(dtv[id], m->tls_image, m->tls_filesz);
    }
    return (uint8_t *)dtv[id] + ti[1];
}

static __attribute__((regparm(1))) void *l____tls_get_addr(uint32_t *ti)
{
    return elf_tls_get_addr(ti);
}

static void *l___tls_get_addr(uint32_t *ti)
{
    return elf_tls_get_addr(ti);
}

/* ---- dl* ------------------------------------------------------------------------------------------- */

struct l_dl_phdr_info {
    uint32_t addr;
    const char *name;
    const Phdr *phdr;
    uint16_t phnum;
    uint16_t pad;
    unsigned long long adds, subs;
    uint32_t tls_modid;
    void *tls_data;
} PACKED;

int elf_iterate_phdr(void *cb, void *data)
{
    struct elf_mod *m;
    int r = 0;

    for (m = mods; m && !r; m = m->next) {
        struct l_dl_phdr_info info;

        if (m->native)
            continue;
        memset(&info, 0, sizeof(info));
        info.addr = m->bias;
        info.name = m->is_exe ? "" : m->path;
        info.phdr = m->phdr;
        info.phnum = m->phnum;
        info.adds = dl_adds;
        info.subs = 0;
        info.tls_modid = m->tls_id;
        r = elf_call3(cb, U32(&info), sizeof(info), U32(data));
    }
    return r;
}

static int l_dl_iterate_phdr(void *cb, void *data)
{
    return elf_iterate_phdr(cb, data);
}

void *elf_dlopen(const char *name, int flags)
{
    struct elf_mod *m, *first_new;
    char path[MAX_PATH];

    (void)flags;
    if (!name)
        return exe_mod;
    EnterCriticalSection(&dl_lock);
    first_new = mods_tail;
    if (strchr(name, '/')) {
        const char *b = strrchr(name, '/') + 1;

        if ((m = find_loaded(b)) || (is_native_lib(b) && (m = native_mod(b)))) {
            LeaveCriticalSection(&dl_lock);
            return m;
        }
        if (lx_path(name, path, sizeof(path)) || !(m = map_elf(path, 0))) {
            snprintf(dl_error, sizeof(dl_error), "%s: cannot open", name);
            LeaveCriticalSection(&dl_lock);
            return NULL;
        }
    } else {
        m = load_by_name(name);
    }
    if (!m) {
        LeaveCriticalSection(&dl_lock);
        LOG("dlopen %s: %s", name, dl_error);
        return NULL;
    }
    if (!m->native) {
        load_deps(first_new ? first_new : mods);
        link_new();
        core_lib_loaded(m, m->name);
        init_mod(m);
    }
    LeaveCriticalSection(&dl_lock);
    LOG("dlopen %s -> %s%s", name, m->name, m->native ? " (native)" : "");
    return m;
}

void *elf_dlsym(void *h, const char *name)
{
    struct elf_mod *m = h, *def;
    Sym *s;
    void *p = NULL;

    if (h && h != (void *)-1 && h != exe_mod) {
        if (m->dll)
            p = (void *)GetProcAddress(m->dll, name);
        else if (m->native)
            p = native_lookup(name);
        else if ((s = mod_find(m, name, NULL, 1)))
            p = (void *)(m->bias + s->st_value);
    }
    if (!p)
        p = scope_lookup(name, NULL, NULL, &def, &s);
    if (!p)
        snprintf(dl_error, sizeof(dl_error), "undefined symbol: %s", name);
    if (lx_lddebug)
        LOG("dlsym %s -> %p", name, p);
    return p;
}

int elf_dlclose(void *h)
{
    (void)h;
    return 0;
}

const char *elf_dlerror(void)
{
    static char out[512];

    if (!dl_error[0])
        return NULL;
    snprintf(out, sizeof(out), "%s", dl_error);
    dl_error[0] = 0;
    return out;
}

static void *l_dlopen(const char *n, int f) { return elf_dlopen(n, f); }
static void *l_dlsym(void *h, const char *n) { return elf_dlsym(h, n); }
static int l_dlclose(void *h) { return elf_dlclose(h); }
static const char *l_dlerror(void) { return elf_dlerror(); }

int elf_owns(uint32_t addr)
{
    struct elf_mod *m;

    for (m = mods; m; m = m->next)
        if (!m->native && addr >= (uintptr_t)m->map && addr < (uintptr_t)m->map + m->maplen)
            return 1;
    return 0;
}

int elf_describe(uint32_t addr, char *buf, size_t n)
{
    struct elf_mod *m;

    for (m = mods; m; m = m->next)
        if (!m->native && addr >= (uintptr_t)m->map && addr < (uintptr_t)m->map + m->maplen) {
            snprintf(buf, n, "%s+0x%x", m->name, (unsigned)(addr - m->bias));
            return 1;
        }
    {
        HMODULE h;
        char name[MAX_PATH];

        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)addr, &h) &&
            GetModuleFileNameA(h, name, sizeof(name))) {
            const char *b = strrchr(name, '\\');

            snprintf(buf, n, "%s+0x%x", b ? b + 1 : name, (unsigned)(addr - (uintptr_t)h));
            return 1;
        }
    }
    snprintf(buf, n, "0x%x", addr);
    return 0;
}

const struct nsym elf_syms[] = {
    N(___tls_get_addr), N(__tls_get_addr), N(dl_iterate_phdr),
    N(dlopen), N(dlsym), N(dlclose), N(dlerror),
    NA(__libc_dlopen_mode, l_dlopen), NA(__libc_dlsym, l_dlsym),
    { NULL, NULL },
};
