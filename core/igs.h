/*
 * IGS loader: the interface between the loader core and machine plugins.
 *
 * Layers (docs/IGS_LOADER.md):
 *   core     LD_PRELOAD library: libc/SDL interposition, device routing,
 *            MMIO traps, frame pacing, audio and the overlay renderer.
 *   machine  the emulated board (e.g. e2100: the FPGA card and the protection
 *            ASIC's transport), loaded from $IGS_MACHINE.
 *   game     what one game's ASIC firmware does, loaded by the machine
 *            (its own interface, e.g. machines/e2100/e2100.h).
 *
 * Machines and games are plain C: no libc interposition and no SDL or GL
 * calls, only the services below.  That keeps them portable to a future
 * non-Linux core.
 */
#ifndef IGS_H
#define IGS_H

#include <stddef.h>
#include <stdint.h>

#define IGS_API_VERSION 1

/* services the core provides to machines (and through them to games) */
struct igs_core {
    int version;

    void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    int verbose;                                   /* $IGS_VERBOSE */
    /* configuration: environment variable or default */
    const char *(*env)(const char *name, const char *def);
    long (*env_long)(const char *name, long def);
    double (*now)(void);                           /* monotonic seconds */

    /* a file descriptor to hand to the game for an emulated device */
    int (*placeholder_fd)(void);

    /*
     * MMIO: a PROT_NONE mapping whose game accesses (8/16/32-bit mov,
     * movzx, mov imm) are trapped and turned into these callbacks.
     */
    void *(*mmio_map)(size_t len, void *opaque,
                      uint32_t (*read)(void *opaque, uint32_t off, int size),
                      void (*write)(void *opaque, uint32_t off, int size, uint32_t v));

    /* overlay drawing; valid only inside igs_machine.draw */
    void (*fill)(int x, int y, int w, int h, float r, float g, float b, float a);
    void (*text)(int x, int y, const char *s, float r, float g, float b);
    int font_w, font_h;
};

/* what a machine plugin provides */
struct igs_machine {
    int version;
    const char *name;

    /* device files: claim a path, then handle the fds it returned */
    int (*handles)(const char *path);
    int (*open)(const char *path);                 /* fd or -1 */
    int (*ioctl)(int fd, unsigned long req, void *arg);
    void *(*mmap)(int fd, size_t len);             /* (void *)-1 on failure */
    void (*close)(int fd);

    /* input (SDL 1.2 key symbols) and the overlay */
    void (*key)(int sym, int down);
    void (*draw)(int width, int height);           /* main thread, before swap */

    /* optional: the game opened a data file with fopen (path as given) */
    void (*file_opened)(const char *path);

    /*
     * optional: read/write on the machine's fds (serial ports); return the
     * byte count, or -1 with errno set.  Without them the fds read and
     * write as /dev/null.  termios calls (tcsetattr, tcgetattr, tcflush)
     * arrive at ioctl as TCSETS+n, TCGETS and TCFLSH.
     */
    long (*read)(int fd, void *buf, size_t n);
    long (*write)(int fd, const void *buf, size_t n);

    /* optional: a short status line for the window title, e.g. "credit 3" */
    void (*status)(char *buf, size_t n);

    /*
     * optional: the contents of a file the cabinet's system provides and
     * the host does not (e.g. a 2.4 kernel's /proc/meminfo), or NULL.  name
     * is the path given to fopen, or "|command" for popen (IGS_NO_SYSTEM).
     * Valid until the next call.
     */
    const char *(*vfile)(const char *name);
};

/* exported by a machine .so */
#define IGS_MACHINE_ENTRY "igs_machine_entry"
typedef const struct igs_machine *(*igs_machine_entry_fn)(const struct igs_core *core);

#endif
