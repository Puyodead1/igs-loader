/*
 * igs-romset: the Linux side's rom-set helper (run.sh calls it).
 *
 *   igs-romset [--root DIR] --list           ids, descriptions, clones, sets found
 *   igs-romset [--root DIR] --env <id>       resolve (and extract) the set, print
 *                                            shell-safe KEY='value' lines to eval
 *   igs-romset [--root DIR] --verify <id>    check the set against the list's hashes
 *   igs-romset [--root DIR] --path <id>      print the set directory
 *
 * --root is the loader directory (hash/, igs-loader.ini); default: the
 * parent of the directory holding this executable.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "romset.h"

static void shq(const char *k, const char *v)
{
    printf("%s='", k);
    for (; v && *v; v++)
        if (*v == '\'')
            fputs("'\\''", stdout);
        else
            putchar(*v);
    puts("'");
}


int main(int argc, char **argv)
{
    char root[RS_PATH] = "", dir[RS_PATH];
    const char *cmd = NULL, *id = NULL;
    const struct sw_info *sw;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            snprintf(root, sizeof(root), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-q"))
            rs_quiet = 1;
        else if (argv[i][0] == '-')
            cmd = argv[i];
        else
            id = argv[i];
    }
    if (!root[0]) {
        ssize_t n = readlink("/proc/self/exe", root, sizeof(root) - 1);
        char *p;

        root[n > 0 ? n : 0] = 0;
        for (i = 0; i < 2 && (p = strrchr(root, '/')); i++)
            *p = 0;
    }
    rs_config(root);
    if (sw_load(rs_hashpath())) {
        fprintf(stderr, "igs-romset: %s\n", rs_error);
        return 1;
    }
    if (cmd && (!strcmp(cmd, "--list") || !strcmp(cmd, "-l"))) {
        rs_list(stdout);
        return 0;
    }
    if (!cmd || !id) {
        fprintf(stderr, "usage: igs-romset [--root DIR] --list | --env <id> | --verify <id> | --path <id>\n");
        return 2;
    }
    if (!(sw = sw_find(id))) {
        fprintf(stderr, "igs-romset: unknown game %s (--list shows the ids)\n", id);
        return 1;
    }
    if (rs_open(id, dir, sizeof(dir))) {
        fprintf(stderr, "igs-romset: %s\n", rs_error);
        return 1;
    }
    if (!strcmp(cmd, "--path")) {
        puts(dir);
        return 0;
    }
    if (!strcmp(cmd, "--verify")) {
        int bad = rs_verify(sw, dir, stdout);

        printf("%s: %s\n", id, bad ? "BAD" : sw->nrom ? "good" : "no hashes listed");
        return bad ? 1 : 0;
    }
    if (!strcmp(cmd, "--env")) {
        shq("ID", sw->name);
        shq("NAME", sw->description);
        shq("MACHINE", sw->list);
        shq("PARENT", sw->cloneof);
        shq("SET", dir);
        for (i = 0; i < sw->nfeat; i++) {
            char k[64];
            size_t j;

            for (j = 0; sw->feat[i].name[j] && j < sizeof(k) - 1; j++)
                k[j] = isalnum((unsigned char)sw->feat[i].name[j]) ? (char)toupper((unsigned char)sw->feat[i].name[j]) : '_';
            k[j] = 0;
            /* never let a list entry clobber the shell's own variables */
            if (!strcmp(k, "PATH") || !strcmp(k, "HOME") || !strcmp(k, "IFS") || !strcmp(k, "SHELL")
                || !strncmp(k, "LD_", 3) || isdigit((unsigned char)k[0]))
                continue;
            shq(k, sw->feat[i].value);
        }
        return 0;
    }
    fprintf(stderr, "igs-romset: unknown option %s\n", cmd);
    return 2;
}
