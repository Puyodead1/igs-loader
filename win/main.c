/*
 * IGS loader for Windows: igs-loader.exe [--check] <game | manifest.conf>
 *
 * Reads manifests/<game>.conf (KEY=VALUE, shell-style quotes), sets up the
 * same IGS_* environment run.sh does, loads the machine plugin DLL and the
 * game's i386 ELF binary with its libraries, and starts it.  --check only
 * loads and links, listing every module and unresolved symbol.
 * Linux paths in the manifest (/mnt/<d>/...) are mapped to <D>:\...
 * See docs/IGS_LOADER.md, "Windows".
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <float.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>

#include "lx.h"

void log_init(void);
void sdl_init(void);
void sdl_shutdown(void);

#define MAXKEYS 64
static struct { char key[32]; char val[1024]; } kv[MAXKEYS];
static int nkv;

static const char *mget(const char *key, const char *def)
{
    int i;

    for (i = 0; i < nkv; i++)
        if (!strcmp(kv[i].key, key))
            return kv[i].val;
    return def;
}

static int parse_manifest(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[2048];

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *eq, *v, *o;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || !(eq = strchr(p, '=')) || nkv == MAXKEYS)
            continue;
        *eq = 0;
        snprintf(kv[nkv].key, sizeof(kv[nkv].key), "%s", p);
        /* the value: shell words with '...' / "..." quoting, up to a comment */
        v = eq + 1;
        o = kv[nkv].val;
        while (*v && *v != '\n' && *v != '\r' && o < kv[nkv].val + sizeof(kv[nkv].val) - 1) {
            if (*v == '"' || *v == '\'') {
                char q = *v++;

                while (*v && *v != q && o < kv[nkv].val + sizeof(kv[nkv].val) - 1)
                    *o++ = *v++;
                if (*v)
                    v++;
            } else if (*v == ' ' || *v == '\t' || *v == '#') {
                break;
            } else {
                *o++ = *v++;
            }
        }
        *o = 0;
        nkv++;
    }
    fclose(f);
    return 0;
}

static void setenv_kv(const char *k, const char *v)
{
    _putenv_s(k, v ? v : "");
}

static int win_dir(const char *lin, char *out, size_t n)
{
    if (lx_path(lin, out, n))
        return -1;
    return GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES ? -1 : 0;
}

int main(int argc, char **argv)
{
    char exedir[MAX_PATH], manifest[MAX_PATH], data[MAX_PATH], rootfs[MAX_PATH], buf[MAX_PATH * 2];
    char exe[MAX_PATH], *p, **envp, *eargv[2];
    const char *game = NULL, *v;
    int check = 0, i, n, missing;
    struct elf_mod *em;
    const struct nsym *tables[8];

    /* first of all: keep the executable's link address range free */
    elf_reserve_low();
    log_init();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetProcessDPIAware();
    timeBeginPeriod(1);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check"))
            check = 1;
        else if (argv[i][0] != '-')
            game = argv[i];
    }
    if (!game) {
        fprintf(stderr, "usage: igs-loader [--check] <game | manifest.conf>\n");
        return 2;
    }
    GetModuleFileNameA(NULL, exedir, sizeof(exedir));
    if ((p = strrchr(exedir, '\\')))
        *p = 0;
    n = (int)strlen(game);
    if (n > 5 && !strcmp(game + n - 5, ".conf"))
        snprintf(manifest, sizeof(manifest), "%s", game);
    else if ((v = getenv("IGS_MANIFESTS")))
        snprintf(manifest, sizeof(manifest), "%s\\%s.conf", v, game);
    else
        snprintf(manifest, sizeof(manifest), "%s\\..\\..\\manifests\\%s.conf", exedir, game);
    if (parse_manifest(manifest)) {
        fprintf(stderr, "no manifest %s\n", manifest);
        return 1;
    }
    lx_verbose = getenv("IGS_VERBOSE") != NULL;
    lx_lddebug = (int)lx_env_long("IGS_LDDEBUG", 0);

    /* the environment run.sh sets up */
    snprintf(buf, sizeof(buf), "%s\\%s.dll", exedir, mget("MACHINE", "e2100"));
    setenv_kv("IGS_MACHINE", buf);
    snprintf(buf, sizeof(buf), "%s\\%s.dll", exedir, mget("PLUGIN", ""));
    setenv_kv("IGS_GAME", buf);
    setenv_kv("IGS_ASSET_MASK", mget("ASSET_MASK", ""));
    setenv_kv("IGS_VARIANT", mget("VARIANT", ""));
    setenv_kv("IGS_NAME", mget("NAME", game));
    /* ENV: per-game NAME=value words */
    if ((v = mget("ENV", NULL))) {
        char envs[1024], *save = NULL, *w;

        snprintf(envs, sizeof(envs), "%s", v);
        for (w = strtok_r(envs, " ", &save); w; w = strtok_r(NULL, " ", &save))
            _putenv(w);
    }

    libc_init();
    sys_init();
    thread_init();
    sdl_init();
    gl_init();
    tables[0] = libc_syms;
    tables[1] = sys_syms;
    tables[2] = thread_syms;
    tables[3] = elf_syms;
    tables[4] = sdl_syms;
    tables[5] = gl_syms;
    tables[6] = NULL;
    elf_set_native(tables);

    /* DATA (the game's cwd) and the cabinet's root fs copy (ROOTFS, default $WORK/output) */
    lx_set_dirs("", "");
    if (win_dir(mget("DATA", ""), data, sizeof(data))) {
        LOG("DATA %s not found", mget("DATA", "(unset)"));
        return 1;
    }
    v = mget("ROOTFS", NULL);
    if (!v) {
        snprintf(buf, sizeof(buf), "%s/output", mget("WORK", mget("DATA", "")));
        v = buf;
    }
    if (win_dir(v, rootfs, sizeof(rootfs))) {
        LOG("ROOTFS %s not found: system libraries come from DATA only", v);
        rootfs[0] = 0;
    }
    lx_set_dirs(data, rootfs);
    SetCurrentDirectoryA(data);
    LOG("%s: DATA %s, root fs %s", mget("NAME", game), data, rootfs[0] ? rootfs : "-");

    /* libraries: LIBS (the game's own, relative to DATA), DATA's lib dirs, the root fs */
    if ((v = mget("LIBS", NULL))) {
        char libs[1024], *save = NULL, *w;

        snprintf(libs, sizeof(libs), "%s", v);
        for (w = strtok_r(libs, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
            snprintf(buf, sizeof(buf), "%s\\%s", data, w);
            for (p = buf; *p; p++)
                if (*p == '/')
                    *p = '\\';
            elf_add_search(buf);
        }
    }
    snprintf(buf, sizeof(buf), "%s\\lib", data);
    elf_add_search(buf);
    snprintf(buf, sizeof(buf), "%s\\libs", data);
    elf_add_search(buf);
    elf_add_search(data);
    if (rootfs[0]) {
        snprintf(buf, sizeof(buf), "%s\\lib", rootfs);
        elf_add_search(buf);
        snprintf(buf, sizeof(buf), "%s\\usr\\lib", rootfs);
        elf_add_search(buf);
    }

    snprintf(exe, sizeof(exe), "%s\\%s", data, mget("BINARY", ""));
    lx_resolve(exe, sizeof(exe));
    em = elf_load_exe(exe);
    if (!em)
        return 1;
    core_install_veh();
    if (!check)
        core_init();
    missing = elf_link_all(check);
    if (check)
        return missing ? 1 : 0;
    if (missing && !lx_env("IGS_MISSING_OK", NULL)) {
        LOG("refusing to start with unresolved symbols (IGS_MISSING_OK=1 to try anyway)");
        return 1;
    }

    /* argv / environ for the game */
    for (n = 0; _environ[n]; n++)
        ;
    envp = l_malloc((n + 1) * sizeof(char *));
    for (i = 0; i < n; i++)
        envp[i] = l_strdup(_environ[i]);
    envp[n] = NULL;
    lx_environ = envp;
    snprintf(buf, sizeof(buf), "./%s", mget("BINARY", "game"));
    eargv[0] = l_strdup(buf);
    eargv[1] = NULL;

    _control87(_PC_64, _MCW_PC);           /* x87 extended precision, as on Linux */
    elf_run_inits(1, eargv, envp);
    elf_start(em, 1, eargv, envp);
}
