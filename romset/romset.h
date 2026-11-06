/*
 * Rom sets: the software lists (hash/<machine>.xml), the rompath, and
 * folder / .7z / .zip sets extracted into a cache.  Portable C, shared by
 * the Windows loader (win/main.c) and the Linux helper (igs-romset.c).
 * See wiki Loader, "Rom sets".
 */
#ifndef IGS_ROMSET_H
#define IGS_ROMSET_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {                    /* the GUI (gui/) is C++ */
#endif

#define RS_PATH 1024

/* ---- software lists --------------------------------------------------------------------- */

struct sw_feature { char *name, *value; };
struct sw_rom { char *name, *sha1; long long size; };

/*
 * <info name="tunable" value="NAME type default text"/>: a game plugin's
 * environment variable.  type: int, hex, bool, str or enum:a|b|c; default
 * "-" = unset (the plugin's own).  A text starting "[Group] " puts it in a
 * group.
 */
struct sw_tunable { char *name, *type, *def, *group, *text; };

struct sw_info {
    char *list;                 /* the machine: e2100, e2000 */
    char *name, *cloneof;
    /*
     * <software supported="no">: the set builds and verifies but cannot run
     * (<info name="unsupported"> says why); NULL = runs
     */
    char *supported;
    char *description, *year, *publisher, *version;
    char *disk, *disk_sha1;
    int nfeat, nrom, ninfo, ntun;
    struct sw_feature *feat;
    struct sw_rom *rom;
    struct sw_feature *info;    /* every <info>, in order */
    struct sw_tunable *tun;
};

/* parse every *.xml in the hash dirs (';'-separated); 0 or -1 (message in rs_error) */
int sw_load(const char *hashpath);
void sw_free(void);             /* forget the lists (sw_load again to reload) */
int sw_count(void);
const struct sw_info *sw_at(int i);
const struct sw_info *sw_find(const char *name);
const char *sw_feature(const struct sw_info *sw, const char *name, const char *def);
const char *sw_info_value(const struct sw_info *sw, const char *name, const char *def);
/* 0 when the entry is supported="no" (the set cannot run) */
int sw_runnable(const struct sw_info *sw);

/* ---- configuration ---------------------------------------------------------------------- */

/*
 * The loader root (holds hash/ and igs-loader.ini).  Reads igs-loader.ini
 * there (IGS_INI names another file): rompath=, cachepath=, hashpath= before
 * the first [section]; IGS_ROMPATH / IGS_CACHE / IGS_HASHPATH override.
 */
void rs_config(const char *root);
const char *rs_root(void);
const char *rs_ini(void);
const char *rs_hashpath(void);
const char *rs_rompath(void);
const char *rs_cachepath(void);

/* the key=value lines of the ini's [<name>] section, in order; the count */
int rs_section(const char *name, void (*fn)(const char *key, const char *value, void *ctx), void *ctx);

/* ---- sets ------------------------------------------------------------------------------- */

enum { RS_NONE, RS_DIR, RS_7Z, RS_ZIP };

/* where the set is on the rompath (no extraction): its kind and path */
int rs_locate(const char *id, char *path, size_t n);

/*
 * The set's directory, ready to use: a folder set itself, or the cache dir
 * of an archive set (extracted when missing or when the archive changed).
 * 0 or -1 (message in rs_error).
 */
int rs_open(const char *id, char *dir, size_t n);

/* --list: every game, and which kind of set was found for it */
void rs_list(FILE *out);

/* check the list's <rom> entries in a set dir; prints a line each; 0 = all good */
int rs_verify(const struct sw_info *sw, const char *dir, FILE *out);

/* an archive set's extracted copy: <cachepath>/<id>; its size in bytes (-1: none); delete it */
void rs_cache_dir(const char *id, char *out, size_t n);
long long rs_cache_size(const char *id);
int rs_cache_reset(const char *id);
long long rs_tree_size(const char *path);   /* bytes in a tree (links not followed) */

extern char rs_error[512];
extern int rs_quiet;

/* ---- internals (archive readers) -------------------------------------------------------- */

struct rs_sink;
int rs_sink_dir(struct rs_sink *s, const char *rel, unsigned mode);
FILE *rs_sink_file(struct rs_sink *s, const char *rel);
void rs_sink_close(struct rs_sink *s, FILE *f, const char *rel, unsigned mode);
int rs_sink_link(struct rs_sink *s, const char *rel, const char *target);
/*
 * Archive readers call rs_sink_scan() with every entry name first: when all
 * of them sit under "<id>/", that folder is stripped.  rs_sink_name() then
 * gives the path to write, or NULL for an entry to skip (unsafe names).
 */
void rs_sink_scan(struct rs_sink *s, const char *name);
const char *rs_sink_name(struct rs_sink *s, const char *name);

int rs_extract_7z(const char *archive, struct rs_sink *s);
int rs_extract_zip(const char *archive, struct rs_sink *s);

uint32_t rs_crc32(uint32_t crc, const void *p, size_t n);   /* zip/7z CRC-32, crc starts at 0 */

void rs_sha1(const void *data, size_t n, uint8_t out[20]);
struct rs_sha1 { uint32_t h[5]; uint64_t len; uint8_t buf[64]; };
void rs_sha1_init(struct rs_sha1 *c);
void rs_sha1_update(struct rs_sha1 *c, const void *p, size_t n);
void rs_sha1_final(struct rs_sha1 *c, uint8_t out[20]);

#ifdef __cplusplus
}
#endif

#endif
