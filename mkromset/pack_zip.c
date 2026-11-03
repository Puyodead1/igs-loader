/*
 * .zip sets with zlib's minizip, entry for entry what mkromset.py's
 * pack_zip() wrote with Python's zipfile: os.walk order (each directory's
 * subdirectories, links to directories included, then its other entries,
 * both sorted), directories as "name/" (mode 0755), files deflated at the
 * --mx level (0: zlib's default) with their unix mode, symlinks stored with
 * the target as data (Info-ZIP style, S_IFLNK | 0777); zip64 where needed.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mkr.h"

#include "zip.h"

struct zw {
    struct mkr_job *j;
    zipFile z;
    uLong dos;
    int level;
    uint8_t *buf;
};

static int is_dirish(struct mkr_job *j, struct mkr_node *n)
{
    struct mkr_node *t = vfs_follow(j, n);

    return t && t->type == N_DIR;
}

static int ascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80)
            return 0;
    return 1;
}

static int entry(struct zw *w, struct mkr_node *n, const char *rel)
{
    struct mkr_job *j = w->j;
    zip_fileinfo fi;
    char name[MKR_PATH];
    int method = Z_DEFLATED, big = 0, e;
    uint32_t mode;

    memset(&fi, 0, sizeof(fi));
    fi.dosDate = w->dos;
    if (n->type == N_LINK) {
        mode = 0120777;
        method = 0;
        snprintf(name, sizeof(name), "%s", rel);
    } else if (n->type == N_DIR) {
        mode = 040755;
        method = 0;
        snprintf(name, sizeof(name), "%s/", rel);
    } else {
        mode = 0100000 | n->mode;
        big = n->size >= 0xF0000000ull;
        snprintf(name, sizeof(name), "%s", rel);
    }
    fi.external_fa = (uLong)mode << 16 | (n->type == N_DIR ? 0x10 : 0);
    e = zipOpenNewFileInZip4_64(w->z, name, &fi, NULL, 0, NULL, 0, NULL, method, method ? w->level : 0, 0,
                                -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY, NULL, 0, 3 << 8 | (big ? 45 : 20),
                                ascii(name) ? 0 : 0x800, big);
    if (e != ZIP_OK)
        return mkr_fail(j, "zip: cannot add %s", rel);
    if (n->type == N_LINK) {
        e = zipWriteInFileInZip(w->z, n->target, (unsigned)strlen(n->target));
    } else if (n->type == N_FILE && n->blob) {
        FILE *f = vfs_ropen(j, n);
        size_t k;

        if (!f)
            return -1;
        while (e == ZIP_OK && (k = fread(w->buf, 1, 1 << 20, f)) > 0) {
            e = zipWriteInFileInZip(w->z, w->buf, (unsigned)k);
            if (mkr_progress(j, k)) {
                fclose(f);
                return -1;
            }
        }
        fclose(f);
    }
    if (e != ZIP_OK || zipCloseFileInZip(w->z) != ZIP_OK)
        return mkr_fail(j, "zip: write error at %s (disk full?)", rel);
    return 0;
}

static int walk(struct zw *w, struct mkr_node *d, const char *prefix)
{
    struct mkr_node **k = vfs_sorted(d);
    char rel[MKR_PATH];
    int i, pass, rc = 0;

    /* sorted(dirs) + sorted(files) */
    for (pass = 0; pass < 2 && !rc; pass++)
        for (i = 0; i < d->nkids && !rc; i++)
            if (is_dirish(w->j, k[i]) == !pass) {
                snprintf(rel, sizeof(rel), "%s%s%s", prefix, prefix[0] ? "/" : "", k[i]->name);
                rc = entry(w, k[i], rel);
            }
    /* then os.walk goes into the real directories, in order */
    for (i = 0; i < d->nkids && !rc; i++)
        if (k[i]->type == N_DIR) {
            snprintf(rel, sizeof(rel), "%s%s%s", prefix, prefix[0] ? "/" : "", k[i]->name);
            rc = walk(w, k[i], rel);
        }
    free(k);
    return rc;
}

static uint64_t tree_bytes(const struct mkr_node *d)
{
    uint64_t t = 0;
    int i;

    for (i = 0; i < d->nkids; i++)
        t += d->kids[i]->type == N_DIR ? tree_bytes(d->kids[i]) : d->kids[i]->size;
    return t;
}

int pack_zip(struct mkr_job *j, const char *path)
{
    struct zw w;
    time_t now = time(NULL);
    struct tm tmv, *t = &tmv;
    int rc;

#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    memset(&w, 0, sizeof(w));
    w.j = j;
    w.level = j->o->mx > 0 ? (j->o->mx > 9 ? 9 : j->o->mx) : Z_DEFAULT_COMPRESSION;
    w.dos = (uLong)((t->tm_year - 80) << 25 | (t->tm_mon + 1) << 21 | t->tm_mday << 16 | t->tm_hour << 11 |
                    t->tm_min << 5 | t->tm_sec / 2);
    if (!(w.z = zipOpen64(path, APPEND_STATUS_CREATE)))
        return mkr_fail(j, "cannot create %s", path);
    w.buf = malloc(1 << 20);
    mkr_phase(j, "pack", tree_bytes(j->root));
    rc = walk(&w, j->root, "");
    if (zipClose(w.z, NULL) != ZIP_OK && !rc)
        rc = mkr_fail(j, "zip: write error in %s (disk full?)", path);
    free(w.buf);
    return rc;
}
