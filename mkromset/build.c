/*
 * mkr_build(): mkromset.py's main() and Builder.  The software list entry's
 * "build" feature is a ';'-separated list of steps that mirror the
 * cabinet's boot (docs/IGS_LOADER.md, "Building sets"):
 *
 *   root p2               the root fs (E2100/E2200 igs_rofs with the entry's
 *                         rofs_profile; E2000 plaintext ROFS)
 *   decrypt SRC DST SIZE  FPGA-cipher decrypt SRC to DST with asset_mask
 *   copy SRC DST          copy a file inside the tree
 *   assemble DST          E2000: rebuild the split executable at DST
 *   mount pN DIR          partition N replaces DIR's contents: SquashFS 4,
 *                         IGS_SHFS or ext2/3 (lost+found left out)
 *   unupx SRC DST         unpack the UPX-packed executable SRC to DST
 *   loop SRC DIR          the SquashFS image file SRC replaces DIR's contents;
 *                         the image is left out ('*' in SRC matches files, in
 *                         DIR it is the match's name without its extension)
 *   poweron SRC DST       E2200 launcher decrypt: skip a 4-byte key-id header,
 *                         decrypt with exe_mask, keep the trailer's size
 *
 * then absolute symlinks become relative, the list's files are checked and
 * the tree is packed.
 */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mkr.h"
#include "romset.h"

#include "7zCrc.h"
#include "Aes.h"
#include "Sha1.h"
#include "XzCrc64.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

/* ---- small helpers ---------------------------------------------------------------------- */

#ifdef _WIN32
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o;
    (void)p;
    (void)c;
    CrcGenerateTable();
    Crc64GenerateTable();
    AesGenTables();
    Sha1Prepare();
    return TRUE;
}

void mkr_once(void) { InitOnceExecuteOnce(&once, once_fn, NULL, NULL); }
#else
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void once_fn(void)
{
    CrcGenerateTable();
    Crc64GenerateTable();
    AesGenTables();
    Sha1Prepare();
}

void mkr_once(void) { pthread_once(&once, once_fn); }
#endif

static double now_s(void)
{
#ifdef _WIN32
    return GetTickCount64() / 1000.0;
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
#endif
}

void mkr_sha1_hex(const uint8_t d[20], char hex[41])
{
    int i;

    for (i = 0; i < 20; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
}

int mkr_sha1_file(struct mkr_job *j, FILE *f, char hex[41], uint64_t *size)
{
    CSha1 c;
    uint8_t d[20], *buf = malloc(1 << 20);
    size_t k;

    *size = 0;
    Sha1_Init(&c);
    while (f && (k = fread(buf, 1, 1 << 20, f)) > 0) {
        Sha1_Update(&c, buf, k);
        *size += k;
        if (mkr_progress(j, k)) {
            free(buf);
            return -1;
        }
    }
    free(buf);
    Sha1_Final(&c, d);
    mkr_sha1_hex(d, hex);
    return 0;
}

char *mkr_latin1(const uint8_t *s, size_t n)
{
    char *o = malloc(2 * n + 1), *p = o;
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] < 0x80) {
            *p++ = (char)s[i];
        } else {
            *p++ = (char)(0xC0 | s[i] >> 6);
            *p++ = (char)(0x80 | (s[i] & 63));
        }
    }
    *p = 0;
    return o;
}

int mkr_exists(const char *path)
{
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;

    return lstat(path, &st) == 0;
#endif
}

static int is_dir(const char *path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);

    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int mkr_mkdirs(const char *path)
{
    char p[MKR_PATH], *s;

    snprintf(p, sizeof(p), "%s", path);
    for (s = p + 1; *s; s++)
        if ((*s == '/' || *s == '\\') && s[-1] != ':') {
            char c = *s;

            *s = 0;
#ifdef _WIN32
            CreateDirectoryA(p, NULL);
#else
            mkdir(p, 0755);
#endif
            *s = c;
        }
#ifdef _WIN32
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
    return is_dir(p) ? 0 : -1;
}

int mkr_rmtree(const char *path)
{
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[MKR_PATH];
    DWORD a = GetFileAttributesA(path);

    if (a == INVALID_FILE_ATTRIBUTES)
        return 0;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileA(path))
            RemoveDirectoryA(path);
        return 0;
    }
    snprintf(pat, sizeof(pat), "%s\\*", path);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char c[MKR_PATH];

            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(c, sizeof(c), "%s\\%s", path, fd.cFileName);
            mkr_rmtree(c);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(path) ? 0 : -1;
#else
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (lstat(path, &st))
        return 0;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    chmod(path, 0700);
    if ((d = opendir(path))) {
        while ((e = readdir(d))) {
            char c[MKR_PATH];

            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(c, sizeof(c), "%s/%s", path, e->d_name);
            mkr_rmtree(c);
        }
        closedir(d);
    }
    return rmdir(path);
#endif
}

int mkr_rename(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static long long file_size(const char *path)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a))
        return -1;
    return (long long)a.nFileSizeHigh << 32 | a.nFileSizeLow;
#else
    struct stat st;

    return stat(path, &st) ? -1 : (long long)st.st_size;
#endif
}

/* ---- the job ---------------------------------------------------------------------------- */

int mkr_fail(struct mkr_job *j, const char *fmt, ...)
{
    va_list ap;

    if (!j->err[0]) {
        va_start(ap, fmt);
        vsnprintf(j->err, sizeof(j->err), fmt, ap);
        va_end(ap);
    }
    return -1;
}

void mkr_log(struct mkr_job *j, int level, const char *fmt, ...)
{
    char line[2048];
    va_list ap;

    if (!j->o->log)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    j->o->log(j->o->ctx, level, line);
}

int mkr_check(struct mkr_job *j)
{
    if (j->cancelled || (j->o->cancel && *j->o->cancel)) {
        if (!j->cancelled) {
            j->cancelled = 1;
            snprintf(j->err, sizeof(j->err), "cancelled");
        }
        return -1;
    }
    return 0;
}

void mkr_phase(struct mkr_job *j, const char *phase, uint64_t total)
{
    j->phase = phase;
    j->pdone = j->plast = 0;
    j->ptotal = total;
    if (j->o->progress)
        j->o->progress(j->o->ctx, phase, 0, total);
}

int mkr_progress(struct mkr_job *j, uint64_t add)
{
    j->pdone += add;
    if (j->pdone - j->plast < (4u << 20))
        return 0;
    j->plast = j->pdone;
    if (j->o->progress && j->phase)
        j->o->progress(j->o->ctx, j->phase, j->pdone, j->ptotal);
    return mkr_check(j);
}

const char *mkr_feature(const struct mkr_job *j, const char *name, const char *def)
{
    int i;

    /* softlist.py: a dict, so the last of the entry's sharedfeat/feature wins */
    for (i = j->nfeat - 1; i >= 0; i--)
        if (!strcmp(j->feat[i].k, name))
            return j->feat[i].v;
    return def;
}

int mkr_threads(const struct mkr_job *j)
{
    int n = j->o->threads;

    if (n <= 0) {
#ifdef _WIN32
        SYSTEM_INFO si;

        GetSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors;
#else
        n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        /* a 32-bit process: the -mx9 encoder needs about 700 MB per block thread */
        if (sizeof(void *) < 8 && n > 2)
            n = 2;
    }
    return n < 1 ? 1 : n > 64 ? 64 : n;
}

static char *sdup(const char *s) { return s ? strdup(s) : NULL; }

static void copy_entry(struct mkr_job *j, const struct sw_info *sw)
{
    int i;

    j->id = sdup(sw->name);
    j->machine = sdup(sw->list);
    j->description = sdup(sw->description);
    j->cloneof = sdup(sw->cloneof);
    j->disk = sdup(sw->disk);
    j->disk_sha1 = sdup(sw->disk_sha1);
    j->nfeat = sw->nfeat;
    j->feat = calloc(sw->nfeat + 1, sizeof(*j->feat));
    for (i = 0; i < sw->nfeat; i++) {
        j->feat[i].k = sdup(sw->feat[i].name);
        j->feat[i].v = sdup(sw->feat[i].value);
    }
    j->nrom = sw->nrom;
    j->rom = calloc(sw->nrom + 1, sizeof(*j->rom));
    for (i = 0; i < sw->nrom; i++) {
        j->rom[i].name = sdup(sw->rom[i].name);
        j->rom[i].sha1 = sdup(sw->rom[i].sha1);
        j->rom[i].size = sw->rom[i].size;
    }
}

static void free_entry(struct mkr_job *j)
{
    int i;

    free(j->id);
    free(j->machine);
    free(j->description);
    free(j->cloneof);
    free(j->disk);
    free(j->disk_sha1);
    for (i = 0; i < j->nfeat; i++) {
        free(j->feat[i].k);
        free(j->feat[i].v);
    }
    free(j->feat);
    for (i = 0; i < j->nrom; i++) {
        free(j->rom[i].name);
        free(j->rom[i].sha1);
    }
    free(j->rom);
}

/* ---- the steps -------------------------------------------------------------------------- */

static int part(struct mkr_job *j, const char *name, uint64_t *off, uint64_t *size)
{
    int n;

    if (strlen(name) != 2 || name[0] != 'p' || !isdigit((unsigned char)name[1]) || !j->phave[name[1] - '0'])
        return mkr_fail(j, "bad partition '%s' in build step", name);
    n = name[1] - '0';
    if (!j->pcount[n])
        return mkr_fail(j, "partition %s is empty", name);
    *off = j->pstart[n] * 512;
    *size = j->pcount[n] * 512;
    return 0;
}

/* a file of the tree, links followed: its contents */
static int load(struct mkr_job *j, const char *cab, uint8_t **data, size_t *n)
{
    struct mkr_node *f = vfs_lookup(j, cab, 1);

    if (!f)
        return mkr_fail(j, "%s: no such file in the set", cab);
    if (f->type != N_FILE)
        return mkr_fail(j, "%s: not a file", cab);
    return vfs_load(j, f, data, n);
}

static int hex_mask(struct mkr_job *j, const char *feature, uint8_t mask[16])
{
    const char *s = mkr_feature(j, feature, NULL);
    int i = 0;

    if (!s)
        return mkr_fail(j, "the entry has no %s", feature);
    while (*s && i < 32) {
        int v;

        if (*s == ' ') {
            s++;
            continue;
        }
        v = isdigit((unsigned char)*s) ? *s - '0' : isxdigit((unsigned char)*s) ? (tolower((unsigned char)*s) - 'a' + 10)
                                                                                    : -1;
        if (v < 0)
            break;
        if (i & 1)
            mask[i / 2] |= (uint8_t)v;
        else
            mask[i / 2] = (uint8_t)(v << 4);
        i++;
        s++;
    }
    while (*s == ' ')
        s++;
    if (i != 32 || *s)
        return mkr_fail(j, "%s is not 16 hex bytes", feature);
    return 0;
}

static int step_root(struct mkr_job *j, char **a)
{
    uint64_t off, size;

    if (part(j, a[0], &off, &size))
        return -1;
    if (!strcmp(j->machine, "e2000"))
        return fs_rofs24(j, off, size, j->root);
    return fs_rofs(j, off, size, j->root, mkr_feature(j, "rofs_profile", NULL));
}

static int step_decrypt(struct mkr_job *j, char **a)
{
    uint8_t mask[16], *d;
    size_t n;
    char *e;
    long long size = strtoll(a[2], &e, 10);
    int rc;

    if (e == a[2] || *e)
        return mkr_fail(j, "decrypt: bad size %s", a[2]);
    if (hex_mask(j, "asset_mask", mask) || load(j, a[0], &d, &n))
        return -1;
    memset(d + n, 0, 16);
    n = (n + 15) & ~(size_t)15;
    fpga_decrypt(mask, d, n);
    /* [:size] */
    if (size < 0)
        size = (long long)n + size < 0 ? 0 : (long long)n + size;
    if ((size_t)size > n)
        size = (long long)n;
    rc = vfs_save(j, a[1], d, (size_t)size);
    free(d);
    return rc;
}

static int step_copy(struct mkr_job *j, char **a)
{
    uint8_t *d;
    size_t n;
    int rc;

    if (load(j, a[0], &d, &n))
        return -1;
    rc = vfs_save(j, a[1], d, n);
    free(d);
    return rc;
}

static int step_unupx(struct mkr_job *j, char **a)
{
    uint8_t *d, *o;
    size_t n, on;
    int rc;

    if (load(j, a[0], &d, &n))
        return -1;
    rc = upx_unpack(j, d, n, &o, &on);
    free(d);
    if (rc)
        return -1;
    rc = vfs_save(j, a[1], o, on);
    free(o);
    return rc;
}

static int step_poweron(struct mkr_job *j, char **a)
{
    uint8_t mask[16], *d;
    size_t n;
    uint32_t size;
    int rc;

    if (hex_mask(j, "exe_mask", mask) || load(j, a[0], &d, &n))
        return -1;
    /* skip the key-id word */
    if (n >= 4) {
        memmove(d, d + 4, n - 4);
        n -= 4;
    } else {
        n = 0;
    }
    memset(d + n, 0, 16);
    n = (n + 15) & ~(size_t)15;
    fpga_decrypt(mask, d, n);
    /* trailer: random word, plain size, key id */
    if (n < 12) {
        free(d);
        return mkr_fail(j, "%s: bad decrypt (wrong exe_mask?)", a[0]);
    }
    size = rd32(d + n - 8);
    if (size > n - 12 || memcmp(d, "\x7f" "ELF", 4)) {
        free(d);
        return mkr_fail(j, "%s: bad decrypt (wrong exe_mask?)", a[0]);
    }
    rc = vfs_save(j, a[1], d, size);
    free(d);
    return rc;
}

static int step_assemble(struct mkr_job *j, char **a)
{
    uint8_t *o;
    size_t on;
    int rc;

    if (e2000_assemble(j, &o, &on))
        return -1;
    rc = vfs_save(j, a[0], o, on);
    free(o);
    return rc;
}

/* posixpath.splitext()[0] of a name */
static void strip_ext(const char *n, char *out, size_t size)
{
    const char *dot = strrchr(n, '.'), *c;

    snprintf(out, size, "%s", n);
    if (!dot)
        return;
    for (c = n; c < dot; c++)
        if (*c != '.') {
            out[dot - n] = 0;
            return;
        }
}

static void replace_all(const char *s, const char *with, char *out, size_t size)
{
    size_t o = 0, wl = strlen(with);

    for (; *s && o + 1 < size; s++) {
        if (*s == '*' && o + wl < size) {
            memcpy(out + o, with, wl);
            o += wl;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
}

static int step_loop(struct mkr_job *j, char **a)
{
    const char *src = a[0], *cab = a[1], *base = strrchr(src, '/');
    struct mkr_node *img, *dn, *parent, *e;
    struct mkr_img m;
    char name[1024];
    int rc;

    base = base ? base + 1 : src;
    if (strchr(base, '*')) {
        char dir[MKR_PATH], pre[256], post[256];
        const char *star = strchr(base, '*'), *star2;
        struct mkr_node *d, **k;
        int i, n;

        snprintf(dir, sizeof(dir), "%.*s", (int)(base - src - (base > src ? 1 : 0)), src);
        snprintf(pre, sizeof(pre), "%.*s", (int)(star - base), base);
        star2 = strchr(star + 1, '*');
        snprintf(post, sizeof(post), "%.*s", (int)(star2 ? star2 - star - 1 : (long)strlen(star + 1)), star + 1);
        d = vfs_lookup(j, dir[0] ? dir : "/", 1);
        if (!d || d->type != N_DIR)
            return mkr_fail(j, "%s: no such directory", dir);
        /* sorted(os.listdir(d)) first: the loop below changes the directory */
        k = vfs_sorted(d);
        n = d->nkids;
        {
            char **names = malloc((n + 1) * sizeof(*names));

            for (i = 0; i < n; i++)
                names[i] = strdup(k[i]->name);
            free(k);
            rc = 0;
            for (i = 0; i < n; i++) {
                size_t l = strlen(names[i]), pl = strlen(pre), sl = strlen(post);

                if (!rc && l >= pl + sl && !strncmp(names[i], pre, pl) && !strcmp(names[i] + l - sl, post)) {
                    char s2[MKR_PATH], c2[MKR_PATH], stem[1024], *args[2];

                    snprintf(s2, sizeof(s2), "%s/%s", dir, names[i]);
                    strip_ext(names[i], stem, sizeof(stem));
                    replace_all(cab, stem, c2, sizeof(c2));
                    args[0] = s2;
                    args[1] = c2;
                    rc = step_loop(j, args);
                }
                free(names[i]);
            }
            free(names);
        }
        return rc;
    }
    if (!(img = vfs_lookup(j, src, 0)))
        return mkr_fail(j, "%s: no such file in the set", src);
    /* the directory: removed when there is one */
    if ((e = vfs_lookup(j, cab, 0))) {
        struct mkr_node *t = vfs_follow(j, e);

        if (t && t->type == N_DIR) {
            if (e->type == N_LINK)
                return mkr_fail(j, "%s: cannot replace a symlink to a directory", cab);
            vfs_remove(j, e);
        }
    }
    if (!(parent = vfs_parent(j, cab, name, sizeof(name), 1)))
        return -1;
    if (vfs_child(j, parent, name))
        return mkr_fail(j, "%s: exists and is not a directory", cab);
    dn = vfs_new(j, parent, name, N_DIR, 0700);
    {
        struct mkr_node *f = vfs_follow(j, img);

        if (!f || f->type != N_FILE)
            return mkr_fail(j, "%s: not a file", src);
        if (img_open_node(j, &m, f))
            return -1;
    }
    rc = fs_squashfs(j, &m, 0, dn, 0);
    img_close(&m);
    if (rc)
        return -1;
    /* the image is left out of the set */
    vfs_remove(j, img);
    return 0;
}

static int step_mount(struct mkr_job *j, char **a)
{
    uint64_t off, size;
    struct mkr_node *e, *d;
    uint8_t magic[4] = { 0 }, extm[2] = { 0 };
    long long got;

    if (part(j, a[0], &off, &size))
        return -1;
    /* the mount hides whatever the root fs has there */
    e = vfs_lookup(j, a[1], 0);
    if (e) {
        struct mkr_node *t = vfs_follow(j, e);

        if (!t || t->type != N_DIR)
            vfs_remove(j, e);
        else
            vfs_clear(j, t);
    }
    if (!(d = vfs_mkdirs(j, a[1])))
        return -1;
    got = img_read(j, &j->img, off, magic, 4);
    if (!got)
        img_read(j, &j->img, off + 1080, extm, 2);
    j->err[0] = 0;
    if (!memcmp(magic, "hsqs", 4))
        return fs_squashfs(j, &j->img, off, d, 1);
    if (rd32(magic) == SHFS_MAGIC)
        return fs_shfs(j, &j->img, off, d);
    if (extm[0] == 0x53 && extm[1] == 0xEF)
        return fs_ext2(j, &j->img, off, d);
    return mkr_fail(j, "%s: unknown filesystem (magic %02x%02x%02x%02x)", a[0], magic[0], magic[1], magic[2], magic[3]);
}

static const struct {
    const char *op;
    int nargs;
    int (*fn)(struct mkr_job *j, char **a);
} steps[] = {
    { "root", 1, step_root },       { "decrypt", 3, step_decrypt }, { "copy", 2, step_copy },
    { "unupx", 2, step_unupx },     { "poweron", 2, step_poweron }, { "loop", 2, step_loop },
    { "assemble", 1, step_assemble }, { "mount", 2, step_mount },
};

static int run_steps(struct mkr_job *j, const char *recipe)
{
    char *buf = strdup(recipe), *save = NULL, *s;
    int rc = 0;

    for (s = strtok_r(buf, ";", &save); s && !rc; s = strtok_r(NULL, ";", &save)) {
        char *w[8], *save2 = NULL, *t, args[1024] = "";
        int n = 0, i, k;
        double t0 = now_s();

        for (t = strtok_r(s, " \t\r\n", &save2); t && n < 8; t = strtok_r(NULL, " \t\r\n", &save2))
            w[n++] = t;
        if (!n)
            continue;
        for (k = 1; k < n; k++) {
            if (k > 1)
                strncat(args, " ", sizeof(args) - strlen(args) - 1);
            strncat(args, w[k], sizeof(args) - strlen(args) - 1);
        }
        for (i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++)
            if (!strcmp(steps[i].op, w[0]))
                break;
        if (i == (int)(sizeof(steps) / sizeof(steps[0]))) {
            rc = mkr_fail(j, "unknown build step '%s'", w[0]);
            break;
        }
        if (n - 1 != steps[i].nargs) {
            rc = mkr_fail(j, "build step %s takes %d arguments: %s", w[0], steps[i].nargs, args);
            break;
        }
        mkr_phase(j, w[0], 0);
        rc = steps[i].fn(j, w + 1);
        if (!rc)
            rc = mkr_check(j);
        if (!rc)
            mkr_log(j, MKR_INFO, "%-8s %s  (%.0f s)", w[0], args, now_s() - t0);
    }
    free(buf);
    return rc;
}

/* ---- verify ----------------------------------------------------------------------------- */

/* mkromset.py resolve(): a cabinet path inside the set, following the set's own links */
static struct mkr_node *resolve(struct mkr_job *j, const char *cab)
{
    struct mkr_node *n = vfs_lookup(j, cab, 1);

    return n;
}

static int verify(struct mkr_job *j, int *ok)
{
    const char *data = mkr_feature(j, "data", "/"), *binary = mkr_feature(j, "binary", NULL);
    int i, n = j->nrom + (binary ? 1 : 0);

    *ok = 1;
    mkr_phase(j, "verify", 0);
    for (i = 0; i < n; i++) {
        char name[MKR_PATH], hex[41];
        const char *sha1 = i < j->nrom ? j->rom[i].sha1 : NULL;
        long long size = i < j->nrom ? j->rom[i].size : -1;
        struct mkr_node *f;
        uint64_t got = 0;
        FILE *fp;
        int good;

        if (i < j->nrom) {
            snprintf(name, sizeof(name), "%s", j->rom[i].name);
        } else if (binary[0] == '/') {
            snprintf(name, sizeof(name), "%s", binary);
        } else {
            size_t l = strlen(data);

            while (l && data[l - 1] == '/')
                l--;
            snprintf(name, sizeof(name), "%.*s/%s", (int)l, data, binary);
        }
        f = resolve(j, name);
        if (!f || f->type != N_FILE) {
            mkr_log(j, MKR_INFO, "MISSING %s", name);
            *ok = 0;
            continue;
        }
        fp = vfs_ropen(j, f);
        if (f->blob && !fp)
            return -1;
        if (mkr_sha1_file(j, fp, hex, &got)) {
            if (fp)
                fclose(fp);
            return -1;
        }
        if (fp)
            fclose(fp);
        good = (!sha1 || sha1[0] == '@' || !strcmp(sha1, hex)) && (size < 0 || (uint64_t)size == got);
        mkr_log(j, MKR_INFO, "%-6s %s  %llu  %s", good ? "ok" : "BAD", name, (unsigned long long)got, hex);
        if (!good)
            *ok = 0;
    }
    return 0;
}

/* ---- options ---------------------------------------------------------------------------- */

void mkr_options_init(struct mkr_options *o)
{
    memset(o, 0, sizeof(*o));
    o->format = MKR_7Z;
    o->mx = 9;
}

const char *mkr_format_name(int format)
{
    return format == MKR_ZIP ? "zip" : format == MKR_DIR ? "dir" : "7z";
}

int mkr_format_parse(const char *s)
{
    return !strcmp(s, "7z") ? MKR_7Z : !strcmp(s, "zip") ? MKR_ZIP : !strcmp(s, "dir") ? MKR_DIR : -1;
}

/* mkromset.py host_path(): an ini path (T:\x or /mnt/t/x) in this host's form */
static void host_path(const char *in, char *out, size_t n)
{
    char p[MKR_PATH], *c;
    size_t l;

    while (*in == ' ' || *in == '\t')
        in++;
    snprintf(p, sizeof(p), "%s", in);
    for (l = strlen(p); l && (p[l - 1] == ' ' || p[l - 1] == '\t' || p[l - 1] == '\r' || p[l - 1] == '\n'); l--)
        p[l - 1] = 0;
#ifdef _WIN32
    if (!strncmp(p, "/mnt/", 5) && strlen(p) > 6 && p[6] == '/') {
        snprintf(out, n, "%c:%s", toupper((unsigned char)p[5]), p + 6);
        for (c = out; *c; c++)
            if (*c == '/')
                *c = '\\';
        return;
    }
#else
    if (strlen(p) > 2 && p[1] == ':' && isalpha((unsigned char)p[0])) {
        snprintf(out, n, "/mnt/%c%s", tolower((unsigned char)p[0]), p + 2);
        for (c = out; *c; c++)
            if (*c == '\\')
                *c = '/';
        return;
    }
#endif
    (void)c;
    snprintf(out, n, "%s", p);
}

void mkr_default_output(char *out, size_t n)
{
    char line[MKR_PATH * 2];
    FILE *f = fopen(rs_ini(), "r");

    /* ini_value('rompath'): the first rompath= before the first [section] */
    while (f && fgets(line, sizeof(line), f)) {
        char *k = line, *eq, *e, *save = NULL;

        while (isspace((unsigned char)*k))
            k++;
        if (*k == '[')
            break;
        if (*k == '#' || *k == ';' || !(eq = strchr(k, '=')))
            continue;
        for (e = eq; e > k && isspace((unsigned char)e[-1]); e--)
            ;
        if (e - k != 7 || strncasecmp(k, "rompath", 7))
            continue;
        fclose(f);
        f = NULL;
        for (k = strtok_r(eq + 1, ";", &save); k; k = strtok_r(NULL, ";", &save)) {
            char h[MKR_PATH];

            host_path(k, h, sizeof(h));
            if (!h[0])
                continue;
#ifdef _WIN32
            if (!(h[0] && h[1] == ':') && h[0] != '\\' && h[0] != '/')
#else
            if (h[0] != '/')
#endif
                snprintf(out, n, "%s" SEP "%s", rs_root(), h);
            else
                snprintf(out, n, "%s", h);
            return;
        }
        break;
    }
    if (f)
        fclose(f);
    snprintf(out, n, "%s" SEP "roms", rs_root());
}

/* ---- the build -------------------------------------------------------------------------- */

static int build(struct mkr_job *j)
{
    const struct mkr_options *o = j->o;
    const char *recipe = mkr_feature(j, "build", NULL);
    char out[MKR_PATH], part[MKR_PATH + 8];
    double t0 = now_s();
    int ok, is_chd;
    size_t l = strlen(o->image);

    if (!recipe)
        return mkr_fail(j, "%s has no build recipe", j->id);
    if (mkr_mkdirs(j->outdir))
        return mkr_fail(j, "cannot create %s", j->outdir);
    if (mkr_exists(j->stage))
        mkr_rmtree(j->stage);
    if (mkr_mkdirs(j->blobdir) || mkr_mkdirs(j->tree))
        return mkr_fail(j, "cannot create %s", j->stage);
    vfs_init(j);
    is_chd = l >= 4 && !strcmp(o->image + l - 4, ".chd");
    if (!is_chd && l >= 4 && o->image[l - 4] == '.' && (o->image[l - 3] | 32) == 'c' && (o->image[l - 2] | 32) == 'h'
        && (o->image[l - 1] | 32) == 'd')
        is_chd = 1;
    if (is_chd) {
        char got[41];

        if (img_chd_sha1(j, o->image, got))
            return -1;
        if (got[0] && j->disk_sha1 && strcmp(got, j->disk_sha1))
            mkr_log(j, MKR_WARN, "WARNING: %s data SHA-1 %s, the list has %s (%s)", o->image, got, j->disk_sha1,
                    j->disk ? j->disk : "None");
        mkr_log(j, MKR_INFO, "reading the CHD in place: %s", o->image);
    } else if (!o->no_verify && j->disk_sha1) {
        char got[41];

        mkr_log(j, MKR_INFO, "hashing %s", o->image);
        if (img_hash(j, o->image, got))
            return -1;
        if (strcmp(got, j->disk_sha1))
            mkr_log(j, MKR_WARN, "WARNING: %s SHA-1 %s, the list has %s (%s)", o->image, got, j->disk_sha1,
                    j->disk ? j->disk : "None");
        else
            mkr_log(j, MKR_INFO, "image matches %s", j->disk ? j->disk : "None");
    }
    if (img_open(j, &j->img, o->image) || img_partitions(j))
        return -1;
    if (run_steps(j, recipe))
        return -1;
    mkr_log(j, MKR_INFO, "%d absolute symlinks made relative", vfs_finish(j));
    img_close(&j->img);
    if (verify(j, &ok))
        return -1;
    if (!ok)
        mkr_log(j, MKR_WARN, "WARNING: the set does not match the list");
    if (mkr_check(j))
        return -1;
    if (o->format == MKR_DIR) {
        const char *tree = j->tree;

        snprintf(out, sizeof(out), "%s" SEP "%s", j->outdir, j->id);
        mkr_phase(j, "pack", 0);
        if (vfs_materialise(j, tree))
            return -1;
        if (mkr_exists(out))
            mkr_rmtree(out);
#ifdef _WIN32
        if (!MoveFileA(tree, out))
#else
        if (rename(tree, out))
#endif
            return mkr_fail(j, "cannot rename %s to %s", tree, out);
        mkr_log(j, MKR_INFO, "set -> %s", out);
        return 0;
    }
    snprintf(out, sizeof(out), "%s" SEP "%s.%s", j->outdir, j->id, mkr_format_name(o->format));
    snprintf(part, sizeof(part), "%s.part", out);
    mkr_log(j, MKR_INFO, "packing %s", out);
    remove(part);
    if ((o->format == MKR_ZIP ? pack_zip(j, part) : pack_7z(j, part)) || mkr_check(j)) {
        remove(part);
        return -1;
    }
    if (mkr_rename(part, out)) {
        remove(part);
        return mkr_fail(j, "cannot rename %s to %s", part, out);
    }
    mkr_log(j, MKR_INFO, "set -> %s, %.1f MB, %.0f s", out, file_size(out) / 1e6, now_s() - t0);
    return 0;
}

int mkr_build(const struct mkr_options *o, char *err, size_t errn)
{
    struct mkr_job *j = calloc(1, sizeof(*j));
    const struct sw_info *sw = o->sw;
    int rc;

    mkr_once();
    j->o = o;
    if (!o->id || !o->image) {
        snprintf(j->err, sizeof(j->err), "need an id and an image");
    } else if (!sw && !(sw = sw_find(o->id))) {
        snprintf(j->err, sizeof(j->err), "unknown game '%s' (--list shows the ids)", o->id);
    } else if (o->format != MKR_7Z && o->format != MKR_ZIP && o->format != MKR_DIR) {
        snprintf(j->err, sizeof(j->err), "bad format %d", o->format);
    } else {
        copy_entry(j, sw);
        if (o->output && o->output[0])
            snprintf(j->outdir, sizeof(j->outdir), "%s", o->output);
        else
            mkr_default_output(j->outdir, sizeof(j->outdir));
        snprintf(j->stage, sizeof(j->stage), "%s" SEP ".%s.stage", j->outdir, j->id);
        snprintf(j->blobdir, sizeof(j->blobdir), "%s" SEP "b", j->stage);
        snprintf(j->tree, sizeof(j->tree), "%s" SEP "t", j->stage);
    }
    rc = j->err[0] ? -1 : build(j);
    img_close(&j->img);
    vfs_free(j);
    if (j->stage[0] && mkr_exists(j->stage))
        mkr_rmtree(j->stage);
    if (rc) {
        if (!j->err[0])
            snprintf(j->err, sizeof(j->err), "failed");
        if (!j->cancelled)
            mkr_log(j, MKR_ERROR, "%s", j->err);
    }
    if (err && errn)
        snprintf(err, errn, "%s", rc ? j->err : "");
    rc = rc ? (j->cancelled ? MKR_CANCELLED : MKR_FAILED) : MKR_OK;
    free_entry(j);
    free(j);
    return rc;
}
