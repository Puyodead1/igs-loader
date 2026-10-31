/*
 * Rom sets: configuration (igs-loader.ini, IGS_* overrides), the rompath
 * lookup, the extraction cache and the extraction target ("sink").
 *
 * Per id the rompath entries are tried in order, each for <id>/ (a folder
 * set, used in place), <id>.7z and <id>.zip.  An archive set is extracted
 * once into <cache>/<id>; a stamp (archive path, size, mtime) re-extracts it
 * when the archive changes.  The cache is writable: the game's settings and
 * bookkeeping live there until the next re-extraction.
 *
 * Symlinks stored in archives are recreated on Linux.  Windows has no
 * usable equivalent here, so they are materialised as copies of their
 * targets (followed inside the set; directory links copied as trees, links
 * into themselves skipped).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "romset.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/stat.h>
#define SEP '\\'
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define SEP '/'
#endif

char rs_error[512];
int rs_quiet;

static char cfg_root[RS_PATH], cfg_rompath[RS_PATH * 4], cfg_cache[RS_PATH], cfg_hash[RS_PATH * 4];

/* ---- paths ------------------------------------------------------------------------------ */

/* a path from the ini or the environment in this host's form (T:\x <-> /mnt/t/x) */
static void host_form(const char *in, char *out, size_t n)
{
    char *p;

#ifdef _WIN32
    if (!strncmp(in, "/mnt/", 5) && in[5] && (in[6] == '/' || !in[6]))
        snprintf(out, n, "%c:%s", in[5] & ~32, in[6] ? in + 6 : "\\");
    else
        snprintf(out, n, "%s", in);
    for (p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
#else
    if (((in[0] | 32) >= 'a' && (in[0] | 32) <= 'z') && in[1] == ':' && (in[2] == '\\' || in[2] == '/' || !in[2]))
        snprintf(out, n, "/mnt/%c%s", in[0] | 32, in + 2);
    else
        snprintf(out, n, "%s", in);
    for (p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
#endif
    /* no trailing separator */
    for (p = out + strlen(out); p > out + 1 && (p[-1] == '/' || p[-1] == '\\') && p[-2] != ':'; p--)
        p[-1] = 0;
}

static int is_abs(const char *p)
{
#ifdef _WIN32
    return (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/';
#else
    return p[0] == '/';
#endif
}

/* a ';'-separated list (':' too on Linux) in host form, relative entries under the root */
static void path_list(const char *in, char *out, size_t n)
{
    char buf[RS_PATH * 4], *save = NULL, *e;

    snprintf(buf, sizeof(buf), "%s", in);
    out[0] = 0;
#ifndef _WIN32
    /* ':' separates too, but not after a drive letter (T:\...) */
    for (e = buf; *e; e++)
        if (*e == ':' && !(e > buf && (e == buf + 1 || e[-2] == ';' || e[-2] == ':') && (e[1] == '\\' || e[1] == '/')))
            *e = ';';
#endif
    for (e = strtok_r(buf, ";", &save); e; e = strtok_r(NULL, ";", &save)) {
        char h[RS_PATH], full[RS_PATH];

        while (*e == ' ' || *e == '\t')
            e++;
        if (!*e)
            continue;
        host_form(e, h, sizeof(h));
        if (is_abs(h))
            snprintf(full, sizeof(full), "%s", h);
        else
            snprintf(full, sizeof(full), "%s%c%s", cfg_root, SEP, h);
        if (out[0])
            strncat(out, ";", n - strlen(out) - 1);
        strncat(out, full, n - strlen(out) - 1);
    }
}

static int exists(const char *p)
{
#ifdef _WIN32
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;

    return lstat(p, &st) == 0;
#endif
}

static int is_dir(const char *p)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);

    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;

    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int make_dir(const char *p)
{
#ifdef _WIN32
    return CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

static int mkdirs(const char *path)
{
    char p[RS_PATH], *s;

    snprintf(p, sizeof(p), "%s", path);
    for (s = p + 1; *s; s++)
        if ((*s == '/' || *s == '\\') && s[-1] != ':') {
            char c = *s;

            *s = 0;
            if (!is_dir(p) && make_dir(p))
                return -1;
            *s = c;
        }
    return is_dir(p) ? 0 : make_dir(p);
}

/* remove a tree (the cache's own files only) */
static void rmtree(const char *path)
{
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[RS_PATH];
    DWORD a = GetFileAttributesA(path);

    if (a == INVALID_FILE_ATTRIBUTES)
        return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileA(path))
            RemoveDirectoryA(path);
        return;
    }
    snprintf(pat, sizeof(pat), "%s\\*", path);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char c[RS_PATH];

            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(c, sizeof(c), "%s\\%s", path, fd.cFileName);
            rmtree(c);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
#else
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (lstat(path, &st))
        return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    chmod(path, 0700);
    if ((d = opendir(path))) {
        while ((e = readdir(d))) {
            char c[RS_PATH];

            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(c, sizeof(c), "%s/%s", path, e->d_name);
            rmtree(c);
        }
        closedir(d);
    }
    rmdir(path);
#endif
}

static int stamp_of(const char *path, long long *size, long long *mtime)
{
#ifdef _WIN32
    struct _stati64 st;

    if (_stati64(path, &st))
        return -1;
#else
    struct stat st;

    if (stat(path, &st))
        return -1;
#endif
    *size = (long long)st.st_size;
    *mtime = (long long)st.st_mtime;
    return 0;
}

/* ---- configuration ---------------------------------------------------------------------- */

static void default_cache(char *out, size_t n)
{
    const char *v;

#ifdef _WIN32
    if ((v = getenv("LOCALAPPDATA")) && *v)
        snprintf(out, n, "%s\\igs-loader\\cache", v);
    else
        snprintf(out, n, "%s\\cache", cfg_root);
#else
    if ((v = getenv("XDG_CACHE_HOME")) && *v)
        snprintf(out, n, "%s/igs-loader", v);
    else if ((v = getenv("HOME")) && *v)
        snprintf(out, n, "%s/.cache/igs-loader", v);
    else
        snprintf(out, n, "%s/cache", cfg_root);
#endif
}

void rs_config(const char *root)
{
    char ini[RS_PATH], line[RS_PATH * 4], rompath[RS_PATH * 4] = "roms", hash[RS_PATH * 4] = "hash", cache[RS_PATH] = "";
    const char *v;
    FILE *f;

    host_form(root, cfg_root, sizeof(cfg_root));
    snprintf(ini, sizeof(ini), "%s%cigs-loader.ini", cfg_root, SEP);
    if ((f = fopen(ini, "r"))) {
        while (fgets(line, sizeof(line), f)) {
            char *k = line, *eq, *e;

            while (*k == ' ' || *k == '\t')
                k++;
            if (*k == '#' || *k == ';' || *k == '[' || !(eq = strchr(k, '=')))
                continue;
            *eq = 0;
            for (e = eq; e > k && (e[-1] == ' ' || e[-1] == '\t'); e--)
                e[-1] = 0;
            for (v = eq + 1; *v == ' ' || *v == '\t'; v++)
                ;
            for (e = (char *)v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--)
                e[-1] = 0;
            if (!strcmp(k, "rompath"))
                snprintf(rompath, sizeof(rompath), "%s", v);
            else if (!strcmp(k, "cachepath"))
                snprintf(cache, sizeof(cache), "%s", v);
            else if (!strcmp(k, "hashpath"))
                snprintf(hash, sizeof(hash), "%s", v);
        }
        fclose(f);
    }
    if ((v = getenv("IGS_ROMPATH")) && *v)
        snprintf(rompath, sizeof(rompath), "%s", v);
    if ((v = getenv("IGS_HASHPATH")) && *v)
        snprintf(hash, sizeof(hash), "%s", v);
    if ((v = getenv("IGS_CACHE")) && *v)
        snprintf(cache, sizeof(cache), "%s", v);
    path_list(rompath, cfg_rompath, sizeof(cfg_rompath));
    path_list(hash, cfg_hash, sizeof(cfg_hash));
    if (cache[0]) {
        char *semi;

        path_list(cache, cfg_cache, sizeof(cfg_cache));
        if ((semi = strchr(cfg_cache, ';')))
            *semi = 0;
    } else {
        default_cache(cfg_cache, sizeof(cfg_cache));
    }
    /*
     * one subfolder per platform: the stamps hold host paths, and Windows
     * extracts symlinks as copies, so the two loaders cannot share a set
     */
    v = cfg_cache + strlen(cfg_cache);
#ifdef _WIN32
    snprintf((char *)v, sizeof(cfg_cache) - (v - cfg_cache), "\\windows");
#else
    snprintf((char *)v, sizeof(cfg_cache) - (v - cfg_cache), "/linux");
#endif
}

const char *rs_hashpath(void) { return cfg_hash; }
const char *rs_rompath(void) { return cfg_rompath; }
const char *rs_cachepath(void) { return cfg_cache; }

/* ---- lookup ----------------------------------------------------------------------------- */

int rs_locate(const char *id, char *path, size_t n)
{
    char buf[RS_PATH * 4], *save = NULL, *d;

    snprintf(buf, sizeof(buf), "%s", cfg_rompath);
    for (d = strtok_r(buf, ";", &save); d; d = strtok_r(NULL, ";", &save)) {
        snprintf(path, n, "%s%c%s", d, SEP, id);
        if (is_dir(path))
            return RS_DIR;
        snprintf(path, n, "%s%c%s.7z", d, SEP, id);
        if (exists(path))
            return RS_7Z;
        snprintf(path, n, "%s%c%s.zip", d, SEP, id);
        if (exists(path))
            return RS_ZIP;
    }
    path[0] = 0;
    return RS_NONE;
}

/* ---- the sink --------------------------------------------------------------------------- */

struct rs_link { char *path, *target; };

struct rs_sink {
    char root[RS_PATH];
    char id[128];
    int strip, scanned;
    char cur[RS_PATH];
    struct rs_link *links;
    int nlinks, caplinks;
    long long files;
};

void rs_sink_scan(struct rs_sink *s, const char *name)
{
    size_t l = strlen(s->id);

    while (name[0] == '.' && name[1] == '/')
        name += 2;
    if (!s->scanned)
        s->strip = 1;
    s->scanned = 1;
    if (strncmp(name, s->id, l) || (name[l] && name[l] != '/'))
        s->strip = 0;
}

const char *rs_sink_name(struct rs_sink *s, const char *name)
{
    char *c, *p;

    while (name[0] == '.' && name[1] == '/')
        name += 2;
    while (*name == '/')
        name++;
    if (s->strip) {
        name += strlen(s->id);
        while (*name == '/')
            name++;
    }
    snprintf(s->cur, sizeof(s->cur), "%s", name);
    for (p = s->cur + strlen(s->cur); p > s->cur && p[-1] == '/'; p--)
        p[-1] = 0;
    if (!s->cur[0])
        return NULL;
    /* nothing may leave the set */
    for (c = s->cur; c; c = strchr(c, '/') ? strchr(c, '/') + 1 : NULL)
        if ((c[0] == '.' && c[1] == '.' && (c[2] == '/' || !c[2])) || c[0] == '/' || strchr(c, ':') == c + 1)
            return NULL;
    return s->cur;
}

static void full(const struct rs_sink *s, const char *rel, char *out, size_t n)
{
    char *p;

    snprintf(out, n, "%s%c%s", s->root, SEP, rel);
#ifdef _WIN32
    for (p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
#else
    (void)p;
#endif
}

static int parent_dirs(const char *path)
{
    char d[RS_PATH], *p;

    snprintf(d, sizeof(d), "%s", path);
    p = strrchr(d, SEP);
    if (!p)
        return 0;
    *p = 0;
    return mkdirs(d);
}

int rs_sink_dir(struct rs_sink *s, const char *rel, unsigned mode)
{
    char p[RS_PATH];

    full(s, rel, p, sizeof(p));
    if (mkdirs(p)) {
        snprintf(rs_error, sizeof(rs_error), "cannot create %s", p);
        return -1;
    }
#ifndef _WIN32
    chmod(p, ((mode ? mode : 0755) | 0700) & ~022);
#else
    (void)mode;
#endif
    return 0;
}

FILE *rs_sink_file(struct rs_sink *s, const char *rel)
{
    char p[RS_PATH];
    FILE *f;

    full(s, rel, p, sizeof(p));
    parent_dirs(p);
#ifdef _WIN32
    SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
#endif
    remove(p);
    if (!(f = fopen(p, "wb"))) {
        snprintf(rs_error, sizeof(rs_error), "cannot create %s", p);
        return NULL;
    }
    setvbuf(f, NULL, _IOFBF, 1 << 16);
    s->files++;
    return f;
}

void rs_sink_close(struct rs_sink *s, FILE *f, const char *rel, unsigned mode)
{
    char p[RS_PATH];

    fclose(f);
    full(s, rel, p, sizeof(p));
#ifndef _WIN32
    /* without a stored unix mode, assume executable: the game binaries must run */
    chmod(p, mode ? ((mode | 0600) & ~022) : 0755);
#else
    (void)mode;
    (void)p;
#endif
}

int rs_sink_link(struct rs_sink *s, const char *rel, const char *target)
{
#ifdef _WIN32
    if (s->nlinks == s->caplinks) {
        s->caplinks = s->caplinks ? s->caplinks * 2 : 256;
        s->links = realloc(s->links, s->caplinks * sizeof(*s->links));
    }
    s->links[s->nlinks].path = strdup(rel);
    s->links[s->nlinks].target = strdup(target);
    s->nlinks++;
    return 0;
#else
    char p[RS_PATH], t[RS_PATH * 2] = "";
    const char *c;

    /* an absolute target means the cabinet's root: the set's root here */
    if (target[0] == '/') {
        for (c = strchr(rel, '/'); c; c = strchr(c + 1, '/'))
            strcat(t, "../");
        while (*target == '/')
            target++;
        strncat(t, *target ? target : ".", sizeof(t) - strlen(t) - 1);
        target = t;
    }
    full(s, rel, p, sizeof(p));
    parent_dirs(p);
    unlink(p);
    if (symlink(target, p)) {
        snprintf(rs_error, sizeof(rs_error), "cannot create symlink %s", p);
        return -1;
    }
    s->nlinks++;
    return 0;
#endif
}

#ifdef _WIN32
/* ---- Windows: links become copies ------------------------------------------------------- */

static int link_cmp(const void *a, const void *b)
{
    return strcmp(((const struct rs_link *)a)->path, ((const struct rs_link *)b)->path);
}

static const struct rs_link *find_link(const struct rs_sink *s, const char *rel)
{
    struct rs_link key;

    key.path = (char *)rel;
    return bsearch(&key, s->links, s->nlinks, sizeof(*s->links), link_cmp);
}

/* join and normalise a set-relative path ('/'-separated, no leading '/', '..' clamped at the root) */
static void norm(const char *base, const char *rel, char *out, size_t n)
{
    char tmp[RS_PATH], *save = NULL, *c;

    if (rel[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", rel);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", base, rel);
    out[0] = 0;
    for (c = strtok_r(tmp, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
        if (!strcmp(c, "."))
            continue;
        if (!strcmp(c, "..")) {
            char *sl = strrchr(out, '/');

            if (sl)
                *sl = 0;
            else
                out[0] = 0;
            continue;
        }
        if (out[0])
            strncat(out, "/", n - strlen(out) - 1);
        strncat(out, c, n - strlen(out) - 1);
    }
}

/* follow the set's links in a set-relative path; -1 for a loop */
static int resolve(const struct rs_sink *s, const char *in, char *out, size_t n)
{
    char cur[RS_PATH], pre[RS_PATH];
    int hops;

    snprintf(cur, sizeof(cur), "%s", in);
    for (hops = 0; hops < 40; hops++) {
        const char *c = cur;
        int found = 0;

        pre[0] = 0;
        while (*c) {
            const char *e = strchr(c, '/');
            size_t l = e ? (size_t)(e - c) : strlen(c);
            const struct rs_link *lk;

            {
                size_t pl = strlen(pre);

                if (pl && pl < sizeof(pre) - 1)
                    pre[pl++] = '/';
                if (l > sizeof(pre) - 1 - pl)
                    l = sizeof(pre) - 1 - pl;
                memcpy(pre + pl, c, l);
                pre[pl + l] = 0;
            }
            if ((lk = find_link(s, pre))) {
                char dir[RS_PATH], joined[RS_PATH], *sl;

                snprintf(dir, sizeof(dir), "%s", pre);
                sl = strrchr(dir, '/');
                if (sl)
                    *sl = 0;
                else
                    dir[0] = 0;
                norm(dir, lk->target, joined, sizeof(joined));
                if (e) {
                    strncat(joined, "/", sizeof(joined) - strlen(joined) - 1);
                    strncat(joined, e + 1, sizeof(joined) - strlen(joined) - 1);
                }
                snprintf(cur, sizeof(cur), "%s", joined);
                found = 1;
                break;
            }
            c = e ? e + 1 : c + l;
        }
        if (!found) {
            snprintf(out, n, "%s", cur);
            return 0;
        }
    }
    return -1;
}

static long long nmat;

static void copy_any(const struct rs_sink *s, const char *src, const char *dst, int depth);

static void copy_tree(const struct rs_sink *s, const char *src, const char *dst, int depth)
{
    char ps[RS_PATH], pd[RS_PATH], pat[RS_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t l = strlen(src);
    int i;

    /* a directory link into itself: stop */
    if (depth > 8 || (!strncmp(dst, src, l) && (dst[l] == '/' || !src[0])))
        return;
    full(s, dst, pd, sizeof(pd));
    mkdirs(pd);
    full(s, src, ps, sizeof(ps));
    snprintf(pat, sizeof(pat), "%s\\*", ps);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char cs[RS_PATH], cd[RS_PATH];

            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(cs, sizeof(cs), "%s/%s", src, fd.cFileName);
            if (find_link(s, cs))
                continue;               /* handled below */
            snprintf(cd, sizeof(cd), "%s/%s", dst, fd.cFileName);
            copy_any(s, cs, cd, depth);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* the links in that directory */
    for (i = 0; i < s->nlinks; i++) {
        const char *p = s->links[i].path, *sl = strrchr(p, '/');
        char cd[RS_PATH], r[RS_PATH];

        if ((sl ? (size_t)(sl - p) : 0) != l || strncmp(p, src, l) || (l && !sl))
            continue;
        if (resolve(s, p, r, sizeof(r)))
            continue;
        snprintf(cd, sizeof(cd), "%s/%s", dst, sl ? sl + 1 : p);
        copy_any(s, r, cd, depth + 1);
    }
}

static void copy_any(const struct rs_sink *s, const char *src, const char *dst, int depth)
{
    char ps[RS_PATH], pd[RS_PATH];
    DWORD a;

    full(s, src, ps, sizeof(ps));
    a = GetFileAttributesA(ps);
    if (a == INVALID_FILE_ATTRIBUTES)
        return;                         /* dangling */
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        copy_tree(s, src, dst, depth);
        return;
    }
    full(s, dst, pd, sizeof(pd));
    parent_dirs(pd);
    if (CopyFileA(ps, pd, FALSE))
        nmat++;
}

static void materialise(struct rs_sink *s)
{
    int i, dangling = 0;

    qsort(s->links, s->nlinks, sizeof(*s->links), link_cmp);
    nmat = 0;
    for (i = 0; i < s->nlinks; i++) {
        char r[RS_PATH], p[RS_PATH];

        if (resolve(s, s->links[i].path, r, sizeof(r))) {
            dangling++;
            continue;
        }
        full(s, r, p, sizeof(p));
        if (!exists(p)) {
            dangling++;
            continue;
        }
        copy_any(s, r, s->links[i].path, 0);
    }
    if (!rs_quiet)
        fprintf(stderr, "igs-loader: %d symlinks materialised as copies (%d dangling or looping)\n",
                s->nlinks - dangling, dangling);
    for (i = 0; i < s->nlinks; i++) {
        free(s->links[i].path);
        free(s->links[i].target);
    }
    free(s->links);
    s->links = NULL;
    s->nlinks = 0;
}
#endif

/* ---- the cache -------------------------------------------------------------------------- */

static void make_stamp(const char *archive, char *out, size_t n)
{
    long long size = 0, mtime = 0;

    stamp_of(archive, &size, &mtime);
    snprintf(out, n, "archive=%s\nsize=%lld\nmtime=%lld\n", archive, size, mtime);
}

int rs_open(const char *id, char *dir, size_t n)
{
    char arc[RS_PATH], stamp[RS_PATH * 2], have[RS_PATH * 2], sp[RS_PATH], part[RS_PATH];
    struct rs_sink *s;
    int kind = rs_locate(id, arc, sizeof(arc)), rc;
    size_t got;
    FILE *f;
    time_t t0;

    if (kind == RS_NONE) {
        snprintf(rs_error, sizeof(rs_error), "no rom set %s (%s/, .7z or .zip) on the rompath %s",
                 id, id, cfg_rompath[0] ? cfg_rompath : "(empty)");
        return -1;
    }
    if (kind == RS_DIR) {
        snprintf(dir, n, "%s", arc);
        return 0;
    }
    snprintf(dir, n, "%s%c%s", cfg_cache, SEP, id);
    snprintf(sp, sizeof(sp), "%s%c.igs-romset", dir, SEP);
    make_stamp(arc, stamp, sizeof(stamp));
    if ((f = fopen(sp, "rb"))) {
        got = fread(have, 1, sizeof(have) - 1, f);
        have[got] = 0;
        fclose(f);
        if (!strcmp(have, stamp))
            return 0;
    }
    if (mkdirs(cfg_cache)) {
        snprintf(rs_error, sizeof(rs_error), "cannot create the cache %s", cfg_cache);
        return -1;
    }
    snprintf(part, sizeof(part), "%s.part", dir);
    rmtree(part);
    if (mkdirs(part)) {
        snprintf(rs_error, sizeof(rs_error), "cannot create %s", part);
        return -1;
    }
    if (!rs_quiet)
        fprintf(stderr, "igs-loader: extracting %s to %s ...\n", arc, dir);
    t0 = time(NULL);
    s = calloc(1, sizeof(*s));
    snprintf(s->root, sizeof(s->root), "%s", part);
    snprintf(s->id, sizeof(s->id), "%s", id);
    rs_error[0] = 0;
    rc = kind == RS_7Z ? rs_extract_7z(arc, s) : rs_extract_zip(arc, s);
    if (!rc && !rs_quiet)
        fprintf(stderr, "igs-loader: %lld files, %d symlinks, %ld s\n", s->files, s->nlinks, (long)(time(NULL) - t0));
#ifdef _WIN32
    if (!rc)
        materialise(s);
#endif
    free(s->links);
    free(s);
    if (rc) {
        rmtree(part);
        return -1;
    }
    snprintf(sp, sizeof(sp), "%s%c.igs-romset", part, SEP);
    if (!(f = fopen(sp, "wb")) || fputs(stamp, f) < 0) {
        if (f)
            fclose(f);
        snprintf(rs_error, sizeof(rs_error), "cannot write %s", sp);
        return -1;
    }
    fclose(f);
    rmtree(dir);
#ifdef _WIN32
    if (!MoveFileA(part, dir)) {
#else
    if (rename(part, dir)) {
#endif
        snprintf(rs_error, sizeof(rs_error), "cannot rename %s to %s", part, dir);
        return -1;
    }
    return 0;
}

/* ---- verification ----------------------------------------------------------------------- */

int rs_verify(const struct sw_info *sw, const char *dir, FILE *out)
{
    int i, bad = 0;

    for (i = 0; i < sw->nrom; i++) {
        const struct sw_rom *r = &sw->rom[i];
        char p[RS_PATH], hex[41], *q;
        uint8_t buf[1 << 16], d[20];
        struct rs_sha1 c;
        long long size = 0;
        size_t got;
        FILE *f;
        int k;

        snprintf(p, sizeof(p), "%s%s%s", dir, r->name[0] == '/' ? "" : "/", r->name);
        for (q = p; *q; q++)
            if (*q == '/' || *q == '\\')
                *q = SEP;
        if (!(f = fopen(p, "rb"))) {
            fprintf(out, "MISSING  %s\n", r->name);
            bad++;
            continue;
        }
        rs_sha1_init(&c);
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            rs_sha1_update(&c, buf, got);
            size += (long long)got;
        }
        fclose(f);
        rs_sha1_final(&c, d);
        for (k = 0; k < 20; k++)
            snprintf(hex + 2 * k, 3, "%02x", d[k]);
        if ((r->sha1 && strcmp(r->sha1, hex)) || (r->size >= 0 && r->size != size)) {
            fprintf(out, "BAD      %s  %lld %s (expected %lld %s)\n", r->name, size, hex, r->size,
                    r->sha1 ? r->sha1 : "-");
            bad++;
        } else {
            fprintf(out, "ok       %s  %lld %s\n", r->name, size, hex);
        }
    }
    return bad;
}
