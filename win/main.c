/*
 * IGS loader for Windows:
 *   igs-loader.exe [--check] <id>     run (--check: only load and link)
 *   igs-loader.exe --list             the games, and which rom sets are found
 *   igs-loader.exe --verify <id>      check a set against the list's hashes
 *
 * <id> is a software name from hash/<machine>.xml.  The rom set (<id>\,
 * <id>.7z or <id>.zip on the rompath; archives are extracted once into
 * %LOCALAPPDATA%\igs-loader\cache\<id>) is the cabinet's file tree: the
 * game's cwd is <set>\<DATA>, system libraries come from <set>\lib and
 * <set>\usr\lib, absolute paths resolve inside the set.  Sets up the same
 * IGS_* environment run.sh does, loads the machine plugin DLL and the
 * game's i386 ELF binary with its libraries, and starts it.  --check lists
 * every module and unresolved symbol.  See docs/IGS_LOADER.md, "Windows"
 * and "Rom sets".
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <float.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>

#include "lx.h"
#include "../romset/romset.h"

void log_init(void);
void sdl_init(void);
void sdl_shutdown(void);

static const struct sw_info *sw;

static const char *mget(const char *key, const char *def)
{
    return sw_feature(sw, key, def);
}

/* a cabinet path (or one relative to DATA) as a Windows path inside the set */
static void set_path(const char *set, const char *data, const char *p, char *out, size_t n)
{
    char *q;

    if (p[0] == '/')
        snprintf(out, n, "%s%s", set, p);
    else
        snprintf(out, n, "%s\\%s", data, p);
    for (q = out; *q; q++)
        if (*q == '/')
            *q = '\\';
}


static void setenv_kv(const char *k, const char *v)
{
    _putenv_s(k, v ? v : "");
}

int main(int argc, char **argv)
{
    char exedir[MAX_PATH], root[MAX_PATH], data[MAX_PATH], rootfs[MAX_PATH], buf[MAX_PATH * 2];
    char exe[MAX_PATH], *p, **envp, *eargv[2];
    const char *game = NULL, *v, *bin;
    int check = 0, listing = 0, verify = 0, i, n, missing;
    struct elf_mod *em;
    const struct nsym *tables[9];

    /* first of all: keep the executable's link address range free */
    elf_reserve_low();
    log_init();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetProcessDPIAware();
    timeBeginPeriod(1);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check"))
            check = 1;
        else if (!strcmp(argv[i], "--list") || !strcmp(argv[i], "-l"))
            listing = 1;
        else if (!strcmp(argv[i], "--verify"))
            verify = 1;
        else if (argv[i][0] != '-')
            game = argv[i];
    }
    GetModuleFileNameA(NULL, exedir, sizeof(exedir));
    if ((p = strrchr(exedir, '\\')))
        *p = 0;
    /* the loader root (hash\, igs-loader.ini): next to the exe, or two up (build\win\) */
    snprintf(root, sizeof(root), "%s\\hash", exedir);
    if (GetFileAttributesA(root) != INVALID_FILE_ATTRIBUTES) {
        snprintf(root, sizeof(root), "%s", exedir);
    } else {
        snprintf(buf, sizeof(buf), "%s\\..\\..", exedir);
        if (!GetFullPathNameA(buf, sizeof(root), root, NULL))
            snprintf(root, sizeof(root), "%s", exedir);
    }
    rs_config(root);
    if (sw_load(rs_hashpath())) {
        fprintf(stderr, "%s\n", rs_error);
        return 1;
    }
    if (listing) {
        rs_list(stdout);
        return 0;
    }
    if (!game) {
        fprintf(stderr, "usage: igs-loader [--check] <id> | --list | --verify <id>\n");
        return 2;
    }
    if (!(sw = sw_find(game))) {
        fprintf(stderr, "unknown game %s (igs-loader --list shows the ids)\n", game);
        return 1;
    }
    if (rs_open(game, rootfs, sizeof(rootfs))) {
        fprintf(stderr, "%s\n", rs_error);
        return 1;
    }
    if (verify) {
        n = rs_verify(sw, rootfs, stdout);
        printf("%s: %s\n", game, n ? "BAD" : sw->nrom ? "good" : "no hashes listed");
        return n ? 1 : 0;
    }
    lx_verbose = getenv("IGS_VERBOSE") != NULL;
    lx_lddebug = (int)lx_env_long("IGS_LDDEBUG", 0);

    /* the environment run.sh sets up */
    snprintf(buf, sizeof(buf), "%s\\%s.dll", exedir, sw->list);
    setenv_kv("IGS_MACHINE", buf);
    snprintf(buf, sizeof(buf), "%s\\%s.dll", exedir, mget("plugin", ""));
    setenv_kv("IGS_GAME", buf);
    setenv_kv("IGS_ASSET_MASK", mget("asset_mask", ""));
    setenv_kv("IGS_VARIANT", mget("variant", ""));
    setenv_kv("IGS_NAME", sw->description);
    /* SDL built into the game (E2000): its drivers, as run.sh sets them */
    if ((v = mget("video_driver", NULL)) && !getenv("SDL_VIDEODRIVER"))
        setenv_kv("SDL_VIDEODRIVER", v);
    if ((v = mget("audio_driver", NULL)) && !getenv("SDL_AUDIODRIVER"))
        setenv_kv("SDL_AUDIODRIVER", v);
    /* env: per-game NAME=value words */
    if ((v = mget("env", NULL))) {
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
    x11_init();
    tables[0] = libc_syms;
    tables[1] = sys_syms;
    tables[2] = thread_syms;
    tables[3] = elf_syms;
    tables[4] = sdl_syms;
    tables[5] = gl_syms;
    tables[6] = x11_syms;
    tables[7] = NULL;
    elf_set_native(tables);

    /*
     * preload: libraries of the loader's own (build/<name>.so on Linux) as
     * DLLs next to the exe; each exports igs_soname, the library it stands in for
     */
    if ((v = mget("preload", NULL))) {
        char pre[512], *save = NULL, *w;

        snprintf(pre, sizeof(pre), "%s", v);
        for (w = strtok_r(pre, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
            HMODULE h;
            const char *soname;

            snprintf(buf, sizeof(buf), "%s\\%s", exedir, w);
            if ((p = strstr(buf, ".so")) && !p[3])
                *p = 0;
            strncat(buf, ".dll", sizeof(buf) - strlen(buf) - 1);
            h = LoadLibraryA(buf);
            soname = h ? (const char *)(void *)GetProcAddress(h, "igs_soname") : NULL;
            if (!soname) {
                LOG("preload %s: cannot load %s (error %lu)", w, buf, GetLastError());
                return 1;
            }
            elf_preload(soname, h);
            LOG("preload %s as %s", buf, soname);
        }
    }

    /* the set is the cabinet's root fs; DATA (the game's cwd) is inside it */
    lx_set_dirs("", "");
    set_path(rootfs, rootfs, mget("data", "/"), data, sizeof(data));
    lx_resolve(data, sizeof(data));
    if (GetFileAttributesA(data) == INVALID_FILE_ATTRIBUTES) {
        LOG("DATA %s not found in the set %s", mget("data", "(unset)"), rootfs);
        return 1;
    }
    lx_set_dirs(data, rootfs);
    SetCurrentDirectoryA(data);
    LOG("%s: set %s, DATA %s", sw->description, rootfs, data);

    /* libraries: LIBS (the game's own: cabinet paths or relative to DATA), DATA's lib dirs, the root fs */
    if ((v = mget("libs", NULL))) {
        char libs[1024], *save = NULL, *w;

        snprintf(libs, sizeof(libs), "%s", v);
        for (w = strtok_r(libs, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
            set_path(rootfs, data, w, buf, sizeof(buf));
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
        /* the cabinets' LD_LIBRARY_PATH (/etc/X11/IGS): Panda Family's SDL_mixer and GLU */
        snprintf(buf, sizeof(buf), "%s\\usr\\X11R6\\lib", rootfs);
        elf_add_search(buf);
    }

    bin = mget("binary", "");
    set_path(rootfs, data, bin, exe, sizeof(exe));
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
    snprintf(buf, sizeof(buf), "%s%s", bin[0] == '/' ? "" : "./", bin);
    eargv[0] = l_strdup(buf);
    eargv[1] = NULL;

    _control87(_PC_64, _MCW_PC);           /* x87 extended precision, as on Linux */
    elf_run_inits(1, eargv, envp);
    elf_start(em, 1, eargv, envp);
}
