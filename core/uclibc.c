/*
 * uClibc 0.9.26 on glibc: what an E1000 executable needs beyond the names
 * the two share.  Built as build/uclibc/libc.so.0 (this file), libm.so.0 and
 * libdl.so.0 (-DSHIM_ONLY: empty, they only pull in libm.so.6 / libdl.so.2);
 * run.sh puts build/uclibc on LD_LIBRARY_PATH and starts the executable with
 * the host's ld-linux.so.2 (its PT_INTERP is /lib/ld-uClibc.so.0).
 * libpthread.so.0 is glibc's own name.  The structures the games pass
 * (struct dirent, tm, termios, sigaction, timeb) have the same i386 layouts
 * in both libraries.
 */
#ifndef SHIM_ONLY
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * uClibc's _start: __uClibc_start_main(argc, argv, envp, _init, _fini).  The
 * library calls the executable's main through its dynamic symbol table.
 */
void __uClibc_start_main(int argc, char **argv, char **envp, void (*init)(void), void (*fini)(void))
{
    int (*main_fn)(int, char **, char **);

    *(void **)&main_fn = dlsym(RTLD_DEFAULT, "main");
    if (!main_fn) {
        fprintf(stderr, "[igs] uclibc: the executable exports no main\n");
        exit(127);
    }
    if (fini)
        atexit(fini);
    if (init)
        init();
    exit(main_fn(argc, argv, envp));
}

/* the getc / putc macros */
int __fgetc_unlocked(FILE *f)
{
    return fgetc_unlocked(f);
}

int __fputc_unlocked(int c, FILE *f)
{
    return fputc_unlocked(c, f);
}
#endif
