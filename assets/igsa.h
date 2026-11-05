/*
 * libigsa: the IGS games' own data formats (wiki Formats).  Each format is a
 * module (fmt_*.c) with a probe and a parser; the parser runs in one of four
 * modes:
 *
 *   IGSA_CHECK    parse and decode everything, write nothing
 *   IGSA_LIST     one line per entry (name, kind, size) on c->out
 *   IGSA_DUMP     the structure as JSON on c->out
 *   IGSA_EXTRACT  the entries as standard files (PNG, WAV, JSON, raw) under c->outdir
 *
 * E2100 assets may be FPGA-encrypted: igsa_open() decrypts with c->mask when
 * the raw bytes match no format.
 */
#ifndef IGSA_H
#define IGSA_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { IGSA_CHECK, IGSA_LIST, IGSA_DUMP, IGSA_EXTRACT };

typedef struct igsa_json {
    FILE *f;
    int depth, first[64], key;      /* first[d]: no member written yet at depth d */
} igsa_json;

typedef struct igsa_ctx {
    const char *path;               /* the file (for messages, and formats that read siblings) */
    const uint8_t *d;               /* its bytes (decrypted when c->decrypted) */
    size_t n;
    int mode;
    FILE *out;                      /* LIST / DUMP output */
    const char *outdir;             /* EXTRACT: entries go to <outdir>/<name> */
    const uint8_t *mask;            /* the set's FPGA asset mask, or NULL */
    int decrypted;
    int entries;                    /* entries handled (counted by igsa_emit_*) */
    igsa_json j;
    char err[512];
} igsa_ctx;

typedef struct igsa_format {
    const char *name;               /* "act", "tsrg", ... */
    const char *desc;
    /* 0 = not this format; higher = surer.  path may be NULL */
    int (*probe)(const uint8_t *d, size_t n, const char *path);
    /* 0 or -1 (igsa_fail) */
    int (*parse)(igsa_ctx *c);
} igsa_format;

/* set while igsa_open probes decrypted bytes (formats that exist only encrypted) */
extern int igsa_probe_decrypted;

/* every format, NULL-terminated */
extern const igsa_format *const igsa_formats[];
const igsa_format *igsa_find(const char *name);

/*
 * Load a file into c and pick its format (forced when fmt != NULL), trying
 * the mask when the plain bytes match nothing.  Returns the format or NULL
 * (no match: c->err empty; read error: c->err set).  igsa_close frees.
 */
const igsa_format *igsa_open(igsa_ctx *c, const char *path, const igsa_format *fmt);
/* the same on a buffer the caller owns (an entry of a container) */
const igsa_format *igsa_probe_buf(const uint8_t *d, size_t n, const char *path);
int igsa_run(igsa_ctx *c, const igsa_format *f);
void igsa_close(igsa_ctx *c);

/* the FPGA asset cipher (mkromset/exe.c); n a multiple of 16 */
void fpga_decrypt(const uint8_t mask[16], uint8_t *data, size_t n);

/* ---- helpers for the modules ------------------------------------------------------------- */

int igsa_fail(igsa_ctx *c, const char *fmt, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 2, 3)))   /* __USE_MINGW_ANSI_STDIO */
#elif defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static inline uint16_t igsa_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t igsa_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t igsa_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t igsa_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline float igsa_lef(const uint8_t *p) { union { uint32_t u; float f; } v; v.u = igsa_le32(p); return v.f; }

/* a NUL-padded fixed-size string field, copied into out (size n+1 or more) */
void igsa_str(char *out, const uint8_t *p, size_t n);

/* JSON on c->out (DUMP); keys NULL inside arrays */
void igsa_jobj(igsa_ctx *c, const char *key);
void igsa_jarr(igsa_ctx *c, const char *key);
void igsa_jend(igsa_ctx *c);                         /* closes the innermost object or array */
void igsa_jint(igsa_ctx *c, const char *key, long long v);
void igsa_juint(igsa_ctx *c, const char *key, unsigned long long v);
void igsa_jhex(igsa_ctx *c, const char *key, unsigned long long v);  /* "0x..." string */
void igsa_jnum(igsa_ctx *c, const char *key, double v);
void igsa_jstr(igsa_ctx *c, const char *key, const char *s);         /* bytes >= 0x80 as \u00xx unless valid UTF-8 */
void igsa_jstrn(igsa_ctx *c, const char *key, const char *s, size_t n);
void igsa_jbool(igsa_ctx *c, const char *key, int v);
void igsa_jbytes(igsa_ctx *c, const char *key, const uint8_t *p, size_t n);  /* hex string */
/* GBK (CP936) text: converted to UTF-8 where the host can (Windows: always; POSIX: iconv) */
void igsa_jgbk(igsa_ctx *c, const char *key, const char *s, size_t n);
void igsa_jbig5(igsa_ctx *c, const char *key, const char *s, size_t n);   /* Big5 (CP950) */

/*
 * An entry.  LIST prints "<name>  <kind>  <info>"; EXTRACT writes the file
 * under outdir (directories made); CHECK and DUMP only count it.
 */
int igsa_emit_raw(igsa_ctx *c, const char *name, const void *p, size_t n);
/* 8-bit RGBA, rows top-down, stride w*4 */
int igsa_emit_png(igsa_ctx *c, const char *name, int w, int h, const uint8_t *rgba);
/* PCM: bits 8 (unsigned) or 16 (signed LE) */
int igsa_emit_wav(igsa_ctx *c, const char *name, int rate, int ch, int bits, const void *pcm, size_t n);
/* a JSON document written by fn(c, arg) through the igsa_j* calls */
int igsa_emit_json(igsa_ctx *c, const char *name, void (*fn)(igsa_ctx *c, void *arg), void *arg);

/* zlib inflate: 0 or -1; *out malloc'd */
int igsa_inflate(const uint8_t *in, size_t n, uint8_t **out, size_t *outn, size_t hint);

/* the engines' LZSS (lzss.c): 0 or -1; *out malloc'd */
int igsa_lzss(const uint8_t *in, size_t n, uint8_t **out, size_t *outn);

/* an embedded DDS file (DXT1/3/5 or uncompressed 16/24/32-bit): its first level as a PNG entry */
int igsa_emit_dds(igsa_ctx *c, const char *name, const uint8_t *d, size_t n);
/* an embedded PCX (8-bit paletted or 24-bit, RLE) as a PNG entry */
int igsa_emit_pcx(igsa_ctx *c, const char *name, const uint8_t *d, size_t n);
/* an embedded TGA (types 1, 2, 3, 9, 10, 11) as a PNG entry */
int igsa_emit_tga(igsa_ctx *c, const char *name, const uint8_t *d, size_t n);

/* pixel conversions to RGBA8 */
void igsa_rgb565(uint8_t *dst, const uint8_t *src, size_t px);     /* LE words */
void igsa_argb1555(uint8_t *dst, const uint8_t *src, size_t px);   /* LE words; bit 15 alpha */
void igsa_argb4444(uint8_t *dst, const uint8_t *src, size_t px);
void igsa_bgra8(uint8_t *dst, const uint8_t *src, size_t px);
void igsa_bgr8(uint8_t *dst, const uint8_t *src, size_t px);
/* DXT1/3/5 blocks to RGBA (w, h the image size; blocks of 4x4) */
int igsa_dxt(uint8_t *dst, const uint8_t *src, size_t n, int w, int h, int dxt);

/* a file name made safe for the host (path separators kept) */
void igsa_safe_name(char *s);

#ifdef __cplusplus
}
#endif
#endif
