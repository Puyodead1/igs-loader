/*
 * igs-mkromset: build a loader rom set from an original IGS disk image, the
 * C port of romtool/mkromset.py with the same options and output.
 *
 *   igs-mkromset <id> <image.chd|image.img> [-o <dir>] [--format 7z|zip|dir]
 *                [--mx <0-9>] [--no-verify]
 *   igs-mkromset --list
 *
 * Also --root DIR (the loader directory: hash/, igs-loader.ini; default:
 * where the executable is, or its parent with hash/, or two up for
 * build\win\) and --threads N (compression threads, default: the CPUs).
 * The software lists come from hash/ (IGS_HASHPATH, hashpath=), the default
 * output from rompath= in igs-loader.ini (IGS_INI names another file).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mkromset.h"
#include "romset.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static const char *prog = "igs-mkromset";
static volatile int cancel;

static void on_signal(int sig)
{
    (void)sig;
    cancel = 1;
}

static void usage(FILE *f)
{
    fprintf(f, "usage: %s [-h] [-o OUTPUT] [--format {7z,zip,dir}] [--mx MX] [--no-verify] [--list]\n"
               "       %*s [--root DIR] [--threads N] [id] [image]\n", prog, (int)strlen(prog), "");
}

static int arg_error(const char *fmt, const char *a)
{
    usage(stderr);
    fprintf(stderr, "%s: error: ", prog);
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    return 2;
}

static void on_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (level == MKR_ERROR) {
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
        return;
    }
    printf(level == MKR_TOOL ? "%s\n" : "[mkromset] %s\n", line);
    fflush(stdout);
}

static int exists_dir(const char *p)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);

    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    return access(p, F_OK) == 0;
#endif
}

/* the loader root: next to the executable when hash/ is there, else one up (build/), else two (build\win\) */
static void find_root(char *root, size_t n)
{
    char exe[1024] = "", t[1100], *p;
    int i;

#ifdef _WIN32
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    for (p = exe; *p; p++)
        if (*p == '\\')
            *p = '/';
#else
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

    exe[k > 0 ? k : 0] = 0;
#endif
    if ((p = strrchr(exe, '/')))
        *p = 0;
    for (i = 0; i < 3; i++) {
        snprintf(t, sizeof(t), "%s/hash", exe);
        if (exists_dir(t))
            break;
        if (!(p = strrchr(exe, '/')))
            break;
        *p = 0;
    }
    if (i == 3 || !exe[0]) {
#ifdef _WIN32
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        if ((p = strrchr(exe, '\\')))
            *p = 0;
#else
        k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        exe[k > 0 ? k : 0] = 0;
        if ((p = strrchr(exe, '/')))
            *p = 0;
#endif
    }
    snprintf(root, n, "%s", exe);
}

static int by_name(const void *a, const void *b)
{
    return strcmp((*(const struct sw_info *const *)a)->name, (*(const struct sw_info *const *)b)->name);
}

static void list(void)
{
    int i, n = sw_count();
    const struct sw_info **v = malloc((n + 1) * sizeof(*v));

    for (i = 0; i < n; i++)
        v[i] = sw_at(i);
    qsort(v, n, sizeof(*v), by_name);
    for (i = 0; i < n; i++) {
        char clone[160] = "";

        if (v[i]->cloneof)
            snprintf(clone, sizeof(clone), "  (clone of %s)", v[i]->cloneof);
        printf("%-24s %-6s %-30s %s%s\n", v[i]->name, v[i]->list, v[i]->description, v[i]->disk ? v[i]->disk : "None",
               clone);
    }
    free(v);
}

/* "--opt value" or "--opt=value"; long options may be abbreviated (argparse) */
static int opt(const char *a, const char *name, int minlen, char **argv, int *i, int argc, const char **val)
{
    size_t l = strcspn(a, "=");

    if (l < (size_t)minlen || strncmp(a, name, l) || l > strlen(name))
        return 0;
    if (a[l] == '=') {
        *val = a + l + 1;
        return 1;
    }
    if (*i + 1 >= argc) {
        *val = NULL;
        return 1;
    }
    *val = argv[++*i];
    return 1;
}

int main(int argc, char **argv)
{
    struct mkr_options o;
    char root[1024] = "", err[512];
    const char *id = NULL, *image = NULL, *v;
    int i, listing = 0, rc;

    mkr_options_init(&o);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            printf("\nBuild an IGS loader rom set from a CHD or raw disk image.\n\n"
                   "positional arguments:\n  id\n  image\n\noptions:\n"
                   "  -h, --help            show this help message and exit\n"
                   "  -o OUTPUT, --output OUTPUT\n"
                   "                        output directory (default: rompath from igs-loader.ini, or roms/)\n"
                   "  --format {7z,zip,dir}\n"
                   "  --mx MX               compression level (default 9)\n"
                   "  --no-verify           skip the image SHA-1 check\n"
                   "  --list                list the known ids and their disk images\n"
                   "  --root DIR            the loader directory (hash/, igs-loader.ini)\n"
                   "  --threads N           compression threads (default: the CPU count)\n");
            return 0;
        }
        if (!strcmp(a, "-o") || (!strncmp(a, "-o", 2) && a[2] != '-')) {
            if (a[2])
                o.output = a + 2 + (a[2] == '=');
            else if (i + 1 < argc)
                o.output = argv[++i];
            else
                return arg_error("argument -o/--output: expected one argument%s", "");
        } else if (!strncmp(a, "--", 2) && a[2]) {
            if (opt(a, "--output", 4, argv, &i, argc, &v)) {
                if (!v)
                    return arg_error("argument -o/--output: expected one argument%s", "");
                o.output = v;
            } else if (opt(a, "--format", 4, argv, &i, argc, &v)) {
                if (!v)
                    return arg_error("argument --format: expected one argument%s", "");
                if ((o.format = mkr_format_parse(v)) < 0)
                    return arg_error("argument --format: invalid choice: '%s' (choose from '7z', 'zip', 'dir')", v);
            } else if (opt(a, "--mx", 3, argv, &i, argc, &v)) {
                char *e;

                if (!v)
                    return arg_error("argument --mx: expected one argument%s", "");
                o.mx = (int)strtol(v, &e, 10);
                if (e == v || *e)
                    return arg_error("argument --mx: invalid int value: '%s'", v);
            } else if (opt(a, "--root", 4, argv, &i, argc, &v)) {
                if (!v)
                    return arg_error("argument --root: expected one argument%s", "");
                snprintf(root, sizeof(root), "%s", v);
            } else if (opt(a, "--threads", 4, argv, &i, argc, &v)) {
                if (!v)
                    return arg_error("argument --threads: expected one argument%s", "");
                o.threads = atoi(v);
            } else if (!strcmp(a, "--no-verify") || (strlen(a) >= 4 && !strncmp("--no-verify", a, strlen(a)))) {
                o.no_verify = 1;
            } else if (!strcmp(a, "--list") || (strlen(a) >= 4 && !strncmp("--list", a, strlen(a)))) {
                listing = 1;
            } else {
                return arg_error("unrecognized arguments: %s", a);
            }
        } else if (a[0] == '-' && a[1]) {
            return arg_error("unrecognized arguments: %s", a);
        } else if (!id) {
            id = a;
        } else if (!image) {
            image = a;
        } else {
            return arg_error("unrecognized arguments: %s", a);
        }
    }
    if (!root[0])
        find_root(root, sizeof(root));
    rs_config(root);
    if (sw_load(rs_hashpath())) {
        fprintf(stderr, "%s\n", rs_error);
        return 1;
    }
    if (listing || !id) {
        list();
        return 0;
    }
    if (!image)
        return arg_error("need an image%s", "");
    if (!(o.sw = sw_find(id))) {
        int n = sw_count(), k;
        const struct sw_info **s = malloc((n + 1) * sizeof(*s));

        for (k = 0; k < n; k++)
            s[k] = sw_at(k);
        qsort(s, n, sizeof(*s), by_name);
        fprintf(stderr, "unknown game '%s'; known: ", id);
        for (k = 0; k < n; k++)
            fprintf(stderr, "%s%s", k ? ", " : "", s[k]->name);
        fputc('\n', stderr);
        free(s);
        return 1;
    }
    o.id = id;
    o.image = image;
    o.log = on_log;
    o.cancel = &cancel;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    rc = mkr_build(&o, err, sizeof(err));
    if (rc == MKR_CANCELLED) {
        fprintf(stderr, "%s: cancelled\n", prog);
        return 130;
    }
    return rc == MKR_OK ? 0 : 1;
}
