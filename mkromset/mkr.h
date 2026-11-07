/*
 * mkromset internals: the build job, the set tree, disk images and the
 * filesystem readers.  Not part of the API (mkromset.h).
 *
 * The set is built as a tree in memory (struct mkr_node): directories,
 * symlinks and modes live there, file contents in files of the stage,
 * <output>/.<id>.stage: on Linux at their own path under t/ when they can be
 * (the folder set is then t/ itself, no file moved), else numbered files
 * ("blobs") under b/ (always on Windows: NTFS names and case).  That keeps the
 * result independent of the host filesystem (Windows has no symlinks or modes)
 * and the build steps' "replace this directory" cheap.  The output is written
 * from the tree: an archive, or a folder.
 */
#ifndef IGS_MKR_H
#define IGS_MKR_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "mkromset.h"

#define MKR_PATH 4096

/* ---- the set tree ----------------------------------------------------------------------- */

enum { N_DIR = 1, N_FILE, N_LINK };

struct mkr_node {
    char *name;                 /* "" for the root */
    int type;
    unsigned mode;              /* permission bits, 07777 */
    struct mkr_node *parent;
    struct mkr_node **kids;     /* directory: unsorted */
    int nkids, capkids;
    uint64_t size;              /* file */
    uint32_t blob;              /* file: 0 = empty */
    char *target;               /* link */
    int inplace;                /* file: written at its own path under the stage's t/ */
    int ondisk;                 /* directory: made under t/ */
    struct mkr_node *hnext;     /* name hash chain */
};

/* ---- disk images ------------------------------------------------------------------------ */

struct mkr_chd;

struct mkr_img {
    void *fh;                   /* raw: a HANDLE (Windows) or an fd cast */
    struct mkr_chd *chd;
    uint64_t size;
    uint64_t skip;              /* the dump lacks the disk's first skip bytes (read as zeros) */
};

/* ---- the job ---------------------------------------------------------------------------- */

struct mkr_kv { char *k, *v; };
struct mkr_rom { char *name, *sha1; long long size; };

struct mkr_job {
    const struct mkr_options *o;
    /* the software list entry, copied */
    char *id, *machine, *description, *cloneof, *disk, *disk_sha1;
    struct mkr_kv *feat;
    int nfeat;
    struct mkr_rom *rom;
    int nrom;
    char outdir[MKR_PATH], stage[MKR_PATH], blobdir[MKR_PATH], tree[MKR_PATH];
    uint32_t nblob;
    struct mkr_node *root;
    struct mkr_node **hash;     /* (parent, name) -> node */
    size_t hsize, hcount;
    struct mkr_img img;
    uint64_t pstart[65], pcount[65];    /* partitions in sectors, p1..p64 */
    int phave[65];
    char err[512];
    int cancelled;
    const char *phase;
    uint64_t pdone, ptotal, plast;
};

int mkr_fail(struct mkr_job *j, const char *fmt, ...);
void mkr_log(struct mkr_job *j, int level, const char *fmt, ...);
/* -1 once the caller asked to cancel */
int mkr_check(struct mkr_job *j);
/* progress of the current phase; also checks the cancel flag every few MB */
int mkr_progress(struct mkr_job *j, uint64_t add);
void mkr_phase(struct mkr_job *j, const char *phase, uint64_t total);
const char *mkr_feature(const struct mkr_job *j, const char *name, const char *def);
int mkr_threads(const struct mkr_job *j);

/* ---- the tree (vfs.c) ------------------------------------------------------------------- */

int vfs_init(struct mkr_job *j);
void vfs_free(struct mkr_job *j);
struct mkr_node *vfs_child(struct mkr_job *j, struct mkr_node *dir, const char *name);
struct mkr_node *vfs_new(struct mkr_job *j, struct mkr_node *dir, const char *name, int type, unsigned mode);
void vfs_remove(struct mkr_job *j, struct mkr_node *n);         /* and everything under it */
void vfs_clear(struct mkr_job *j, struct mkr_node *dir);        /* a directory's contents */
/* the set path of a node, "/a/b" ("/" for the root) */
void vfs_path(const struct mkr_node *n, char *out, size_t size);
/* a sorted copy of a directory's children (free it) */
struct mkr_node **vfs_sorted(struct mkr_node *dir);

/*
 * Path lookup with the host's rules inside the set: symlinks followed in
 * every component but the last (and in the last with follow); absolute
 * targets start at the set root, ".." stops there.  NULL when missing.
 */
struct mkr_node *vfs_lookup(struct mkr_job *j, const char *path, int follow);
struct mkr_node *vfs_follow(struct mkr_job *j, struct mkr_node *n);    /* NULL: dangling/loop */
/* os.makedirs(path, exist_ok=True): missing directories made with mode 0755; the directory */
struct mkr_node *vfs_mkdirs(struct mkr_job *j, const char *path);
/* split a path into its parent directory (made when missing) and last name */
struct mkr_node *vfs_parent(struct mkr_job *j, const char *path, char *name, size_t n, int make);

/* blobs: a file's contents being written */
struct mkr_w {
    FILE *f;
    uint32_t blob;
    uint64_t size;
    int inplace;
    char path[MKR_PATH];
};
/* a file's contents to come; dir/name: where it goes (the file is written there when it can be) */
int vfs_wopen(struct mkr_job *j, struct mkr_w *w, struct mkr_node *dir, const char *name);
int vfs_write(struct mkr_job *j, struct mkr_w *w, const void *p, size_t n);
int vfs_wzero(struct mkr_job *j, struct mkr_w *w, uint64_t n);  /* a hole */
void vfs_wabort(struct mkr_job *j, struct mkr_w *w);
/*
 * Close w and put it at dir/name with mode (the old entry there replaced: a
 * file or link, or the file a link points to, as open(path, "wb") does);
 * NULL on error.
 */
struct mkr_node *vfs_put(struct mkr_job *j, struct mkr_node *dir, const char *name, struct mkr_w *w, unsigned mode);
/* a symlink at dir/name, the old entry removed */
struct mkr_node *vfs_link(struct mkr_job *j, struct mkr_node *dir, const char *name, const char *target);
/* a directory at dir/name: kept if there is one (or a link to one), else made with mode */
struct mkr_node *vfs_dir(struct mkr_job *j, struct mkr_node *dir, const char *name, unsigned mode);

void vfs_file_path(const struct mkr_job *j, const struct mkr_node *f, char *out, size_t n);
FILE *vfs_ropen(struct mkr_job *j, const struct mkr_node *f);  /* NULL for an empty file too */
int vfs_load(struct mkr_job *j, const struct mkr_node *f, uint8_t **data, size_t *n);
int vfs_save(struct mkr_job *j, const char *path, const uint8_t *data, size_t n);   /* mkromset.py write() */

/* finish(): drop lost+found, absolute links -> relative; the count fixed */
int vfs_finish(struct mkr_job *j);
/* the folder set: the tree with its blobs moved into dir (Windows: links become copies) */
int vfs_materialise(struct mkr_job *j, const char *dir);

/* ---- images (image.c) ------------------------------------------------------------------- */

int img_open(struct mkr_job *j, struct mkr_img *m, const char *path);
int img_open_node(struct mkr_job *j, struct mkr_img *m, const struct mkr_node *f);
void img_close(struct mkr_img *m);
int img_read(struct mkr_job *j, struct mkr_img *m, uint64_t off, void *buf, size_t n);
/* the CHD header's data SHA-1 (v5), "" when none; 0 or -1 */
int img_chd_sha1(struct mkr_job *j, const char *path, char hex[41]);
int img_partitions(struct mkr_job *j);
int img_hash(struct mkr_job *j, const char *path, char hex[41]);

/* ---- filesystems ------------------------------------------------------------------------ */

/* E2100/E2200 igs_rofs: the partition at off/size into dir (profile NULL: try all) */
int fs_rofs(struct mkr_job *j, uint64_t off, uint64_t size, struct mkr_node *dir, const char *profile);
/* E2000 plaintext ROFS */
int fs_rofs24(struct mkr_job *j, uint64_t off, uint64_t size, struct mkr_node *dir);
/* E3000 plaintext Game Image v2 (Speed Driver 3), keys by profile */
int fs_gimg(struct mkr_job *j, uint64_t off, uint64_t size, struct mkr_node *dir, const char *profile);
int fs_gimg_known(const char *profile);
/* E2000 IGS_SHFS */
int fs_shfs(struct mkr_job *j, struct mkr_img *m, uint64_t off, struct mkr_node *dir);
/* SquashFS 4 (unsquashfs semantics: force = -f) */
int fs_squashfs(struct mkr_job *j, struct mkr_img *m, uint64_t off, struct mkr_node *dir, int force);
/* ext2/3/4 (debugfs rdump of every top-level entry but lost+found) */
int fs_ext2(struct mkr_job *j, struct mkr_img *m, uint64_t off, struct mkr_node *dir);

#define SHFS_MAGIC 0xD4AA2682u
#define SQFS_MAGIC 0x73717368u

/* ---- executables (exe.c) ---------------------------------------------------------------- */

void fpga_decrypt(const uint8_t mask[16], uint8_t *data, size_t n);   /* n a multiple of 16 */
int upx_unpack(struct mkr_job *j, const uint8_t *in, size_t n, uint8_t **out, size_t *outn);
int e2000_assemble(struct mkr_job *j, uint8_t **out, size_t *outn);

/* ---- packing ---------------------------------------------------------------------------- */

int pack_7z(struct mkr_job *j, const char *out);
int pack_zip(struct mkr_job *j, const char *out);

/* ---- helpers (build.c) ------------------------------------------------------------------ */

void mkr_once(void);            /* the SDK's tables */
void mkr_sha1_hex(const uint8_t d[20], char hex[41]);
int mkr_sha1_file(struct mkr_job *j, FILE *f, char hex[41], uint64_t *size);
/* latin-1 bytes -> UTF-8 (Python's decode('latin-1') then a filesystem path) */
char *mkr_latin1(const uint8_t *s, size_t n);
int mkr_rmtree(const char *path);
int mkr_mkdirs(const char *path);
int mkr_exists(const char *path);
int mkr_rename(const char *from, const char *to);

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

#endif
