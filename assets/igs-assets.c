/*
 * igs-assets: list, dump, check and extract the IGS games' data formats
 * (libigsa, wiki Formats).
 *
 *   igs-assets formats
 *   igs-assets [options] list|dump|check FILE...
 *   igs-assets [options] extract FILE... [-o DIR]     (DIR/<file name>/...)
 *   igs-assets [options] scan DIR...                  (check every file a format claims)
 *
 *   -f FORMAT     force the format
 *   --mask HEX    the FPGA asset mask (32 hex digits)
 *   --game ID     take the mask from the software list entry ID; scan finds
 *                 the ID in the path (a cache or set folder named after it)
 *   --root DIR    the loader directory (hash/), default: next to or above the executable
 *   -v            scan: name every file checked
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "igsa.h"
#include "romset.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static const char *prog = "igs-assets";
static uint8_t opt_mask[16];
static int have_mask, have_sw, verbose;
static const char *opt_game;

static void usage(FILE *f)
{
    fprintf(f, "usage: %s formats\n"
               "       %s [-f FORMAT] [--mask HEX | --game ID] [--root DIR] list|dump|check FILE...\n"
               "       %s [...] extract FILE... [-o DIR]\n"
               "       %s [...] [-v] scan DIR...\n", prog, prog, prog, prog);
}

static int parse_hex(const char *s, uint8_t out[16])
{
    int i;

    if (strlen(s) != 32)
        return -1;
    for (i = 0; i < 16; i++) {
        unsigned v;

        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void find_root(char *root, size_t n, const char *argv0)
{
    char exe[1024] = "", t[1100], *p;
    struct stat st;
    int up;

#ifdef _WIN32
    GetModuleFileNameA(NULL, exe, sizeof(exe));
#else
    ssize_t l = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

    if (l > 0)
        exe[l] = 0;
    else
        snprintf(exe, sizeof(exe), "%s", argv0);
#endif
    for (p = exe; *p; p++)
        if (*p == '\\')
            *p = '/';
    for (up = 0; up < 3; up++) {
        if (!(p = strrchr(exe, '/')))
            break;
        *p = 0;
        snprintf(t, sizeof(t), "%s/hash", exe);
        if (!stat(t, &st)) {
            snprintf(root, n, "%s", exe);
            return;
        }
    }
    snprintf(root, n, ".");
}

static int load_sw(const char *root)
{
    if (have_sw)
        return have_sw > 0 ? 0 : -1;
    rs_config(root);
    rs_quiet = 1;
    have_sw = sw_load(rs_hashpath()) ? -1 : 1;
    return have_sw > 0 ? 0 : -1;
}

/* the mask of software list entry id: asset_mask */
static int sw_mask(const char *id, uint8_t out[16])
{
    const struct sw_info *sw = sw_find(id);
    const char *m;

    if (!sw || !(m = sw_feature(sw, "asset_mask", NULL)))
        return -1;
    return parse_hex(m, out);
}

/* scan: the set id a path is under (a component named after a software list entry) */
static int path_mask(const char *path, uint8_t out[16])
{
    char buf[1400], *s, *e;

    snprintf(buf, sizeof(buf), "%s", path);
    for (s = buf; *s; s++)
        if (*s == '\\')
            *s = '/';
    for (s = buf; s && *s; s = e ? e + 1 : NULL) {
        e = strchr(s, '/');
        if (e)
            *e = 0;
        if (*s && sw_find(s))
            return sw_mask(s, out);
        if (e)
            *e = '/';
    }
    return -1;
}

static const char *base_name(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');

    if (b > a)
        a = b;
    return a ? a + 1 : p;
}

/* ---- scan --------------------------------------------------------------------------------- */

struct tally { const igsa_format *f; long ok, bad, entries; };
#define MAXFMT 512                  /* more than igsa_formats[] holds */
static struct tally tal[MAXFMT + 1];
static long unclaimed, files;
static struct { char ext[16]; long n; } unk[256];

static void count_unclaimed(const char *path)
{
    const char *b = base_name(path), *e = strrchr(b, '.');
    char x[16];
    int i;

    snprintf(x, sizeof(x), "%s", e && e != b ? e : "(none)");
    for (i = 0; x[i]; i++)
        if (x[i] >= 'A' && x[i] <= 'Z')
            x[i] += 32;
    for (i = 0; i < 255 && unk[i].n && strcmp(unk[i].ext, x); i++)
        ;
    snprintf(unk[i].ext, sizeof(unk[i].ext), "%s", x);
    unk[i].n++;
}

static void scan_file(const char *path, const igsa_format *force)
{
    igsa_ctx c;
    const igsa_format *f;
    uint8_t m[16];
    int i;

    memset(&c, 0, sizeof(c));
    c.mode = IGSA_CHECK;
    if (have_mask)
        c.mask = opt_mask;
    else if (have_sw > 0 && !path_mask(path, m))
        c.mask = m;
    files++;
    if (verbose) {              /* the path first: a crash names its file */
        fprintf(stderr, "> %s\n", path);
        fflush(stderr);
    }
    f = igsa_open(&c, path, force);
    if (!f) {
        if (c.err[0])
            fprintf(stderr, "%s\n", c.err);
        unclaimed++;
        count_unclaimed(path);
        if (verbose)
            printf("--   %-8s %s\n", "", path);
        igsa_close(&c);
        return;
    }
    for (i = 0; i < MAXFMT && tal[i].f && tal[i].f != f; i++)
        ;
    tal[i].f = f;
    if (igsa_run(&c, f)) {
        tal[i].bad++;
        printf("FAIL %-8s %s: %s\n", f->name, path, c.err);
    } else {
        tal[i].ok++;
        tal[i].entries += c.entries;
        if (verbose)
            printf("ok   %-8s %s%s (%d entries)\n", f->name, path, c.decrypted ? " [decrypted]" : "", c.entries);
    }
    igsa_close(&c);
    fflush(stdout);
}

static void scan_dir(const char *dir, const igsa_format *force)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[1400];
    struct stat st;

    if (!d) {
        scan_file(dir, force);
        return;
    }
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &st))
            continue;
        if (S_ISDIR(st.st_mode))
            scan_dir(p, force);
        else if (S_ISREG(st.st_mode))
            scan_file(p, force);
    }
    closedir(d);
}

/* ---- main --------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const igsa_format *force = NULL;
    const char *cmd = NULL, *outdir = ".", *root_opt = NULL;
    char root[1024];
    char **files_v = calloc((size_t)argc, sizeof(char *));
    int nf = 0, i, rc = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-f") && i + 1 < argc) {
            if (!(force = igsa_find(argv[++i]))) {
                fprintf(stderr, "%s: no format %s (see: %s formats)\n", prog, argv[i], prog);
                return 2;
            }
        } else if (!strcmp(a, "-o") && i + 1 < argc)
            outdir = argv[++i];
        else if (!strcmp(a, "--mask") && i + 1 < argc) {
            if (parse_hex(argv[++i], opt_mask)) {
                fprintf(stderr, "%s: --mask wants 32 hex digits\n", prog);
                return 2;
            }
            have_mask = 1;
        } else if (!strcmp(a, "--game") && i + 1 < argc)
            opt_game = argv[++i];
        else if (!strcmp(a, "--root") && i + 1 < argc)
            root_opt = argv[++i];
        else if (!strcmp(a, "-v"))
            verbose = 1;
        else if (!cmd)
            cmd = a;
        else
            files_v[nf++] = argv[i];
    }
    if (!cmd) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(cmd, "formats")) {
        for (i = 0; igsa_formats[i]; i++)
            printf("%-10s %s\n", igsa_formats[i]->name, igsa_formats[i]->desc);
        return 0;
    }
    if (root_opt)
        snprintf(root, sizeof(root), "%s", root_opt);
    else
        find_root(root, sizeof(root), argv[0]);
    if (opt_game) {
        if (load_sw(root) || sw_mask(opt_game, opt_mask)) {
            fprintf(stderr, "%s: no asset_mask for %s in %s\n", prog, opt_game, rs_hashpath());
            return 2;
        }
        have_mask = 1;
    }
    if (!nf) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(cmd, "scan")) {
        if (!have_mask)
            load_sw(root);      /* masks by set id; no lists is fine */
        for (i = 0; i < nf; i++)
            scan_dir(files_v[i], force);
        printf("\n%-10s %8s %6s %10s\n", "format", "files", "failed", "entries");
        for (i = 0; tal[i].f; i++) {
            printf("%-10s %8ld %6ld %10ld\n", tal[i].f->name, tal[i].ok + tal[i].bad, tal[i].bad, tal[i].entries);
            if (tal[i].bad)
                rc = 1;
        }
        printf("%ld files, %ld not a known format:", files, unclaimed);
        for (i = 0; i < 256 && unk[i].n; i++)
            printf(" %s %ld", unk[i].ext, unk[i].n);
        printf("\n");
        return rc;
    }
    for (i = 0; i < nf; i++) {
        igsa_ctx c;
        const igsa_format *f;
        char od[1400];

        memset(&c, 0, sizeof(c));
        if (!strcmp(cmd, "list"))
            c.mode = IGSA_LIST;
        else if (!strcmp(cmd, "dump"))
            c.mode = IGSA_DUMP;
        else if (!strcmp(cmd, "check"))
            c.mode = IGSA_CHECK;
        else if (!strcmp(cmd, "extract"))
            c.mode = IGSA_EXTRACT;
        else {
            usage(stderr);
            return 2;
        }
        c.out = stdout;
        if (have_mask)
            c.mask = opt_mask;
        else if (!load_sw(root)) {
            static uint8_t m[16];

            if (!path_mask(files_v[i], m))
                c.mask = m;
        }
        snprintf(od, sizeof(od), "%s/%s", outdir, base_name(files_v[i]));
        c.outdir = od;
        if (!(f = igsa_open(&c, files_v[i], force))) {
            fprintf(stderr, "%s: %s\n", prog, c.err[0] ? c.err : "not a known format");
            igsa_close(&c);
            rc = 1;
            continue;
        }
        if (nf > 1 && c.mode != IGSA_DUMP)
            printf("%s: %s%s\n", files_v[i], f->name, c.decrypted ? " (decrypted)" : "");
        if (igsa_run(&c, f)) {
            fprintf(stderr, "%s: %s: %s\n", prog, files_v[i], c.err);
            rc = 1;
        } else if (c.mode == IGSA_CHECK)
            printf("%s: %s ok, %d entries%s\n", files_v[i], f->name, c.entries, c.decrypted ? " (decrypted)" : "");
        igsa_close(&c);
    }
    free(files_v);
    return rc;
}
