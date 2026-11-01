/*
 * Hero of Robots: the NFC reader (nfc.h), part of the preloaded card library
 * (#included by libcasmcard.c).  docs/HeroOfRobots.md, NFC.
 *
 * A thread polls the reader every 250 ms, keeps a copy of the tag on it and
 * does the queued writes, so neither the game loop nor the card thread ever
 * waits on the reader.  The readers and the tag format are in nfc_tag.c
 * (shared with the card tool, hero-nfc.c).
 * Writes only ever touch the user pages 4..17, and only on a tag that is
 * blank or already the loader's; the capability container (page 3) must
 * say NDEF (E1).
 *
 * Environment: HERO_NFC (features: "1"/"all", or a list of scan, dispense,
 * points), HERO_NFC_READER, HERO_NFC_WAIT (seconds a dispensed card waits
 * for a tag, default 30), IGS_NFC_FAKE.
 */
#include <pthread.h>

#include "nfc_tag.c"

/* ---- the reader thread ------------------------------------------------------------ */

#define QUEUE 8

static struct {
    const struct reader *rd;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    struct hero_nfc_tag tag;            /* the copy handed out */
    uint32_t count;                     /* the tag's write count */
    int user_pages;                     /* from the capability container */
    char queue[QUEUE][13];              /* dispensed cards waiting for a tag */
    double queued_at[QUEUE];
    int queued;
    int write_points;                   /* the copy's points need writing */
    double wait;
} nf = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER, .tag.points = -1 };

/* read and classify the tag just placed (outside the lock) */
static int examine(struct hero_nfc_tag *t, uint32_t *count, int *pages)
{
    return tag_examine(nf.rd, t, count, pages);
}

/* write the copy's code and points to the tag, then read it back */
static int commit(const char *code, int points, uint32_t count)
{
    return tag_commit(nf.rd, nf.user_pages, code, points, count);
}

static void *reader_thread(void *arg)
{
    uint8_t uid[10];
    int uid_len, present_before = 0;

    (void)arg;
    if (nf.rd->open()) {
        NLOG("reader thread stopped");
        return NULL;
    }
    for (;;) {
        struct timespec ts;
        struct hero_nfc_tag t;
        double now = now_s();
        int present = nf.rd->poll(uid, &uid_len), i;

        pthread_mutex_lock(&nf.lock);
        if (present && (!present_before || uid_len != nf.tag.uid_len ||
                        memcmp(uid, nf.tag.uid, uid_len))) {
            uint32_t count = 0;
            int pages = 0;
            char hex[24];

            for (i = 0; i < uid_len; i++)
                sprintf(hex + 2 * i, "%02X", uid[i]);
            pthread_mutex_unlock(&nf.lock);
            memset(&t, 0, sizeof(t));
            t.state = examine(&t, &count, &pages);
            pthread_mutex_lock(&nf.lock);
            t.serial = nf.tag.serial + 1;
            memcpy(t.uid, uid, uid_len);
            t.uid_len = uid_len;
            nf.tag = t;
            nf.count = count;
            nf.user_pages = pages;
            nf.write_points = 0;
            if (t.state == TAG_LOADER)
                NLOG("tag %s: the loader's, card %s, points %d", hex, t.code[0] ? t.code : "none",
                     t.points);
            else
                NLOG("tag %s: %s", hex, t.state == TAG_BLANK ? "blank" : "not the loader's (left alone)");
        } else if (!present && present_before) {
            NLOG("tag removed");
            nf.tag.state = TAG_NONE;
            nf.write_points = 0;
        }
        present_before = present;

        /* dispensed cards: the oldest goes on a blank or card-less loader tag */
        while (nf.queued && now - nf.queued_at[0] > nf.wait) {
            NLOG("card %s not written: no blank tag within %.0f s", nf.queue[0], nf.wait);
            memmove(nf.queue, nf.queue + 1, sizeof(nf.queue[0]) * --nf.queued);
            memmove(nf.queued_at, nf.queued_at + 1, sizeof(double) * nf.queued);
        }
        t = nf.tag;
        if (nf.queued && (t.state == TAG_BLANK || (t.state == TAG_LOADER && !t.code[0]))) {
            char code[13];
            int points = t.state == TAG_LOADER ? t.points : -1;

            memcpy(code, nf.queue[0], sizeof(code));
            pthread_mutex_unlock(&nf.lock);
            i = commit(code, points, nf.count + 1);
            pthread_mutex_lock(&nf.lock);
            if (!i && nf.tag.serial == t.serial) {
                memmove(nf.queue, nf.queue + 1, sizeof(nf.queue[0]) * --nf.queued);
                memmove(nf.queued_at, nf.queued_at + 1, sizeof(double) * nf.queued);
                nf.tag.state = TAG_LOADER;
                memcpy(nf.tag.code, code, sizeof(code));
                nf.count++;
                NLOG("card %s written to the tag", code);
            } else if (i) {
                NLOG("writing card %s failed; place the tag again", code);
                nf.tag.state = TAG_FOREIGN;     /* untouched until it is placed again */
            }
        }
        /* points */
        t = nf.tag;
        if (nf.write_points && t.state == TAG_LOADER) {
            nf.write_points = 0;
            pthread_mutex_unlock(&nf.lock);
            i = commit(t.code, t.points, nf.count + 1);
            pthread_mutex_lock(&nf.lock);
            if (i) {
                NLOG("writing %d points failed; place the tag again", t.points);
                nf.tag.state = TAG_FOREIGN;
            } else {
                nf.count++;
                NLOG("%d points written to the tag", t.points);
            }
        }
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        if (!nf.queued && !nf.write_points)
            pthread_cond_timedwait(&nf.wake, &nf.lock, &ts);
        pthread_mutex_unlock(&nf.lock);
    }
    return NULL;
}

/* ---- the API ------------------------------------------------------------------------ */

static void nfc_get(struct hero_nfc_tag *t)
{
    pthread_mutex_lock(&nf.lock);
    *t = nf.tag;
    pthread_mutex_unlock(&nf.lock);
}

static void nfc_dispense(const char *code)
{
    pthread_mutex_lock(&nf.lock);
    if (nf.queued == QUEUE) {
        NLOG("card %s not written: %d cards already wait for tags", code, QUEUE);
    } else {
        snprintf(nf.queue[nf.queued], sizeof(nf.queue[0]), "%s", code);
        nf.queued_at[nf.queued++] = now_s();
        NLOG("card %s dispensed: place a blank tag", code);
        pthread_cond_signal(&nf.wake);
    }
    pthread_mutex_unlock(&nf.lock);
}

static int nfc_set_points(unsigned serial, int points)
{
    int ok;

    pthread_mutex_lock(&nf.lock);
    ok = nf.tag.serial == serial && (nf.tag.state == TAG_BLANK || nf.tag.state == TAG_LOADER);
    if (ok) {
        if (nf.tag.state == TAG_BLANK)
            nf.tag.code[0] = 0;
        nf.tag.state = TAG_LOADER;
        nf.tag.points = points < 0 ? 0 : points > 0xFFFF ? 0xFFFF : points;
        nf.write_points = 1;
        pthread_cond_signal(&nf.wake);
    }
    pthread_mutex_unlock(&nf.lock);
    return ok;
}

static int nfc_pending(void)
{
    int n;

    pthread_mutex_lock(&nf.lock);
    n = nf.queued;
    pthread_mutex_unlock(&nf.lock);
    return n;
}

static struct hero_nfc nfc_api = {
    .version = HERO_NFC_VERSION,
    .get = nfc_get,
    .dispense = nfc_dispense,
    .set_points = nfc_set_points,
    .pending = nfc_pending,
};

/* HERO_NFC: "1" / "all", or a list of scan, dispense, points */
static unsigned nfc_features(void)
{
    const char *v = getenv("HERO_NFC");
    unsigned f = 0;

    if (!v || !*v || !strcmp(v, "0"))
        return 0;
    if (!strcmp(v, "1") || ci_contains(v, "all"))
        return NFC_SCAN | NFC_DISPENSE | NFC_POINTS;
    if (ci_contains(v, "scan"))
        f |= NFC_SCAN;
    if (ci_contains(v, "dispense"))
        f |= NFC_DISPENSE;
    if (ci_contains(v, "points"))
        f |= NFC_POINTS;
    return f;
}

/* NULL unless HERO_NFC is set; the first call starts the reader thread */
EXPORT const struct hero_nfc *hero_nfc_api(void)
{
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    static int started;
    const struct hero_nfc *r;
    pthread_t th;

    pthread_mutex_lock(&once);
    if (!started) {
        const char *v = getenv("HERO_NFC_WAIT");

        started = 1;
        nfc_api.features = nfc_features();
        if (nfc_api.features) {
            const char *fake = getenv("IGS_NFC_FAKE");

            nf.wait = v && *v ? atof(v) : 30;
            nf.rd = fake && *fake ? &fake_reader : &pcsc_reader;
            NLOG("on:%s%s%s; %s reader", nfc_api.features & NFC_SCAN ? " scan" : "",
                 nfc_api.features & NFC_DISPENSE ? " dispense" : "",
                 nfc_api.features & NFC_POINTS ? " points" : "", nf.rd->name);
            if (pthread_create(&th, NULL, reader_thread, NULL)) {
                NLOG("no reader thread");
                nfc_api.features = 0;
            } else {
                pthread_detach(th);
            }
        }
    }
    r = nfc_api.features ? &nfc_api : NULL;
    pthread_mutex_unlock(&once);
    return r;
}
