/*
 * IGS_NO_SYSTEM: the shell commands a game uses to fill its RAM disk (We
 * Dance Online 2: "cp ./sound/voice/ /tmp/ -rf", "rm /tmp/x.ogg -rf") run
 * in-process; nothing else does.  Shared by core/core.c and win/libc_sys.c.
 *
 * tmpcmd_parse() splits a command of the form
 *     cp [-opts] SRC... DST [-opts]      rm [-opts] PATH... [-opts]
 * into its words without the options.  It accepts only these two commands
 * with plain words (no quotes, pipes, redirections or variables), every
 * cp source relative to the game's directory or under /tmp, without "..",
 * and every cp destination and rm path under /tmp.  Returns the word count
 * (argv[0] is "cp" or "rm"), or 0 for anything else.
 */
#ifndef IGS_TMPCMD_H
#define IGS_TMPCMD_H

#include <string.h>

#define TMPCMD_MAX 16

static int tmpcmd_in_tmp(const char *p)
{
    return !strncmp(p, "/tmp", 4) && (p[4] == '/' || !p[4]);
}

static int tmpcmd_parse(const char *cmd, char *buf, size_t n, char **argv)
{
    char *p, *w;
    int argc = 0, i;

    if (!cmd || strlen(cmd) >= n || strpbrk(cmd, "|;&<>$`'\"\\()"))
        return 0;
    strcpy(buf, cmd);
    for (p = buf; (w = strtok(p, " \t")); p = NULL) {
        if (w[0] == '-')
            continue;
        if (argc == TMPCMD_MAX)
            return 0;
        argv[argc++] = w;
    }
    if (argc < 2 || (strcmp(argv[0], "cp") && strcmp(argv[0], "rm")))
        return 0;
    if (!strcmp(argv[0], "cp") && (argc < 3 || !tmpcmd_in_tmp(argv[argc - 1])))
        return 0;
    for (i = 1; i < argc; i++) {
        if (strstr(argv[i], ".."))
            return 0;
        if ((argv[0][0] == 'r' || i == argc - 1) && !tmpcmd_in_tmp(argv[i]))
            return 0;
        if (argv[i][0] == '/' && !tmpcmd_in_tmp(argv[i]))
            return 0;
    }
    return argc;
}

#endif
