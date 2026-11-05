/*
 * igs-mkromset: build a loader rom set from an original IGS disk image (raw
 * .img or MAME .chd), the C port of romtool/mkromset.py.  One call builds
 * one set; the CLI (igs-mkromset.c) and the GUI run it in-process.
 *
 * Threads: mkr_build() keeps all its state in the call (no globals besides
 * once-initialised tables) and may run on a worker thread.  It reads the
 * software list entry (romset/softlist.c) at its start, so the lists must
 * not be reloaded (sw_free / sw_load) until it returns; give the entry in
 * mkr_options.sw to avoid even the lookup.  Several builds may run at once
 * when they write to different outputs.
 *
 * See wiki Loader, "Building sets".
 */
#ifndef IGS_MKROMSET_H
#define IGS_MKROMSET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sw_info;

enum mkr_format { MKR_7Z, MKR_ZIP, MKR_DIR };

/* log levels: INFO and WARN are mkromset.py's "[mkromset] ..." lines, TOOL the
   extractors' own summary lines (printed without the prefix), ERROR the reason
   a build stopped */
enum mkr_level { MKR_INFO, MKR_TOOL, MKR_WARN, MKR_ERROR };

/* mkr_build results */
enum { MKR_OK = 0, MKR_FAILED = -1, MKR_CANCELLED = -2 };

struct mkr_options {
    const char *id;             /* software name from hash/<machine>.xml */
    const struct sw_info *sw;   /* its entry; NULL: sw_find(id) in the loaded lists */
    const char *image;          /* .img (raw disk) or .chd */
    const char *output;         /* output directory; NULL: mkr_default_output() */
    int format;                 /* enum mkr_format (default MKR_7Z) */
    int mx;                     /* compression level 0-9 (default 9): 7z -mx, zip deflate level */
    int no_verify;              /* skip hashing a .img against the list's disk SHA-1 */
    int threads;                /* compression threads; 0 = from the CPU count */

    /* called on the building thread, or while packing a .7z on the encoder's threads (never two at
       once); may be NULL */
    void (*log)(void *ctx, int level, const char *line);
    /* phase ("hash", the step name, "verify", "pack"); total 0 = unknown */
    void (*progress)(void *ctx, const char *phase, uint64_t done, uint64_t total);
    void *ctx;
    /* set non-zero from any thread to stop the build (checked every few MB) */
    volatile int *cancel;
};

void mkr_options_init(struct mkr_options *o);

/*
 * Build the set: <output>/<id>.7z, <id>.zip or the folder <id>/.  The
 * staging tree <output>/.<id>.stage and any partial archive are removed
 * whatever the result.  MKR_OK, MKR_FAILED (reason in err and logged) or
 * MKR_CANCELLED.
 */
int mkr_build(const struct mkr_options *o, char *err, size_t errn);

/*
 * mkromset.py's default output: the first rompath= entry of the ini that
 * romset/ rs_config() found (relative entries under the loader root), else
 * <root>/roms.  Reads romset's configuration: call it where rs_config runs.
 */
void mkr_default_output(char *out, size_t n);

const char *mkr_format_name(int format);   /* "7z", "zip", "dir" */
int mkr_format_parse(const char *s);       /* -1 for an unknown name */

#ifdef __cplusplus
}
#endif

#endif
