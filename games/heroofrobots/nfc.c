/*
 * Hero of Robots: the NFC reader (nfc.h), part of the preloaded card library
 * (#included by libcasmcard.c).  docs/HeroOfRobots.md, NFC.
 *
 * A thread polls the reader every 250 ms, keeps a copy of the tag on it and
 * does the queued writes, so neither the game loop nor the card thread ever
 * waits on the reader.  Readers:
 *   - PC/SC (winscard.dll, or libpcsclite.so.1 loaded at run time): the
 *     first reader whose name contains HERO_NFC_READER (default "ACR122");
 *     Type 2 tags through the reader's pseudo-APDUs FF CA (UID), FF B0
 *     (read 16 bytes), FF D6 (write one page);
 *   - IGS_NFC_FAKE=<dir>: <dir>/tag.bin is the tag on the reader, a
 *     540-byte NTAG215 image (pages 0..134); no file, no tag.
 * Writes only ever touch the user pages 4..17, and only on a tag that is
 * blank or already the loader's; the capability container (page 3) must
 * say NDEF (E1).
 *
 * Environment: HERO_NFC (features: "1"/"all", or a list of scan, dispense,
 * points), HERO_NFC_READER, HERO_NFC_WAIT (seconds a dispensed card waits
 * for a tag, default 30), IGS_NFC_FAKE.
 */
#include <ctype.h>
#include <pthread.h>

#include "nfc.h"

#define NLOG(...) do { fputs("[igs] nfc: ", stderr); fprintf(stderr, __VA_ARGS__); \
                       fputc('\n', stderr); } while (0)

#define REC_TYPE "igs-loader:hero"
#define PAYLOAD 32
#define FIRST_PAGE 4
#define AREA 64                     /* bytes read from page 4 to classify a tag */
#define TAG_BYTES 540               /* NTAG215: 135 pages */

/* ---- readers ---------------------------------------------------------------------- */

struct reader {
    const char *name;
    int (*open)(void);                              /* 0: ready */
    int (*poll)(uint8_t *uid, int *uid_len);        /* 1 tag, 0 none */
    int (*read16)(int page, uint8_t *b);            /* 0: ok */
    int (*write4)(int page, const uint8_t *b);      /* 0: ok */
};

/* -- PC/SC: LONG/DWORD/handles are 32-bit on both i386 ABIs */
#ifdef _WIN32
#define PCSC __stdcall
#define PCSC_LIB "winscard.dll"
#define PCSC_A "A"
#else
#define PCSC
#define PCSC_LIB "libpcsclite.so.1"
#define PCSC_A ""
#endif

struct io_req { unsigned long proto, len; };

static struct {
    void *lib;
    long (PCSC *establish)(unsigned long, const void *, const void *, unsigned long *);
    long (PCSC *list)(unsigned long, const char *, char *, unsigned long *);
    long (PCSC *connect)(unsigned long, const char *, unsigned long, unsigned long,
                         unsigned long *, unsigned long *);
    long (PCSC *disconnect)(unsigned long, unsigned long);
    long (PCSC *status)(unsigned long, char *, unsigned long *, unsigned long *,
                        unsigned long *, uint8_t *, unsigned long *);
    long (PCSC *transmit)(unsigned long, const struct io_req *, const uint8_t *, unsigned long,
                          struct io_req *, uint8_t *, unsigned long *);
    unsigned long ctx, card, proto;
    int have_ctx, connected;
    char reader[256];
    double next_list;
} pc;

static double now_s(void)
{
    struct timespec t;

    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int ci_contains(const char *s, const char *sub)
{
    size_t n = strlen(sub), i;

    for (; *s; s++) {
        for (i = 0; i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)sub[i]); i++)
            ;
        if (i == n)
            return 1;
    }
    return !n;
}

static int pcsc_open(void)
{
    if (!(pc.lib = dlopen(PCSC_LIB, RTLD_NOW))) {
        NLOG("no PC/SC (%s not found): no reader", PCSC_LIB);
        return -1;
    }
    *(void **)&pc.establish = dlsym(pc.lib, "SCardEstablishContext");
    *(void **)&pc.list = dlsym(pc.lib, "SCardListReaders" PCSC_A);
    *(void **)&pc.connect = dlsym(pc.lib, "SCardConnect" PCSC_A);
    *(void **)&pc.disconnect = dlsym(pc.lib, "SCardDisconnect");
    *(void **)&pc.status = dlsym(pc.lib, "SCardStatus" PCSC_A);
    *(void **)&pc.transmit = dlsym(pc.lib, "SCardTransmit");
    if (!pc.establish || !pc.list || !pc.connect || !pc.disconnect || !pc.status || !pc.transmit) {
        NLOG("%s lacks the SCard calls: no reader", PCSC_LIB);
        return -1;
    }
    return 0;
}

/* find the reader: the context and the list, at most every 5 s */
static int pcsc_find(void)
{
    const char *want = getenv("HERO_NFC_READER");
    static int said;
    char names[4096], *n;
    unsigned long len = sizeof(names);
    long r;

    if (pc.reader[0])
        return 1;
    if (now_s() < pc.next_list)
        return 0;
    pc.next_list = now_s() + 5;
    if (!want || !*want)
        want = "ACR122";
    if (!pc.have_ctx) {
        if ((r = pc.establish(2 /* SCARD_SCOPE_SYSTEM */, NULL, NULL, &pc.ctx))) {
            if (!said++)
                NLOG("PC/SC unavailable (%08lx): no reader", (unsigned long)r);
            return 0;
        }
        pc.have_ctx = 1;
    }
    if ((r = pc.list(pc.ctx, NULL, names, &len)))
        len = 0;
    for (n = names; len && *n; n += strlen(n) + 1)
        if (ci_contains(n, want)) {
            snprintf(pc.reader, sizeof(pc.reader), "%s", n);
            NLOG("reader \"%s\"", pc.reader);
            said = 0;
            return 1;
        }
    if (!said++)
        NLOG("no reader matching \"%s\"", want);
    return 0;
}

static int apdu(const uint8_t *a, unsigned long n, uint8_t *r, unsigned long *rn)
{
    struct io_req req = { pc.proto, sizeof(struct io_req) };

    if (pc.transmit(pc.card, &req, a, n, NULL, r, rn) || *rn < 2 ||
        r[*rn - 2] != 0x90 || r[*rn - 1] != 0x00)
        return -1;
    *rn -= 2;
    return 0;
}

static void pcsc_drop(void)
{
    if (pc.connected)
        pc.disconnect(pc.card, 0 /* SCARD_LEAVE_CARD */);
    pc.connected = 0;
}

static int pcsc_poll(uint8_t *uid, int *uid_len)
{
    static const uint8_t get_uid[] = { 0xFF, 0xCA, 0x00, 0x00, 0x00 };
    uint8_t r[64], atr[40];
    unsigned long rn = sizeof(r), nl = 0, st, pr, al = sizeof(atr);

    if (!pcsc_find())
        return 0;
    if (pc.connected && pc.status(pc.card, NULL, &nl, &st, &pr, atr, &al))
        pcsc_drop();
    if (!pc.connected) {
        long e = pc.connect(pc.ctx, pc.reader, 2 /* SCARD_SHARE_SHARED */, 3 /* T0 | T1 */,
                            &pc.card, &pc.proto);

        if (e == (long)0x80100009 || e == (long)0x80100017) {   /* unknown / unavailable reader */
            NLOG("reader \"%s\" gone", pc.reader);
            pc.reader[0] = 0;
        }
        if (e)
            return 0;
        pc.connected = 1;
    }
    if (apdu(get_uid, sizeof(get_uid), r, &rn) || rn > 10) {
        pcsc_drop();
        return 0;
    }
    memcpy(uid, r, rn);
    *uid_len = rn;
    return 1;
}

static int pcsc_read16(int page, uint8_t *b)
{
    uint8_t a[5] = { 0xFF, 0xB0, 0x00, (uint8_t)page, 16 }, r[32];
    unsigned long rn = sizeof(r);

    if (apdu(a, sizeof(a), r, &rn) || rn < 16)
        return -1;
    memcpy(b, r, 16);
    return 0;
}

static int pcsc_write4(int page, const uint8_t *b)
{
    uint8_t a[9] = { 0xFF, 0xD6, 0x00, (uint8_t)page, 4 }, r[8];
    unsigned long rn = sizeof(r);

    memcpy(a + 5, b, 4);
    return apdu(a, sizeof(a), r, &rn);
}

static const struct reader pcsc_reader = { "PC/SC", pcsc_open, pcsc_poll, pcsc_read16, pcsc_write4 };

/* -- a file per tag (IGS_NFC_FAKE) */
static char fake_path[1024];

static int fake_open(void)
{
    snprintf(fake_path, sizeof(fake_path), "%s/tag.bin", getenv("IGS_NFC_FAKE"));
    NLOG("fake reader: %s", fake_path);
    return 0;
}

static int fake_load(uint8_t *img)
{
    FILE *f = fopen(fake_path, "rb");
    size_t n;

    if (!f)
        return -1;
    n = fread(img, 1, TAG_BYTES, f);
    fclose(f);
    return n == TAG_BYTES ? 0 : -1;
}

static int fake_poll(uint8_t *uid, int *uid_len)
{
    uint8_t img[TAG_BYTES];

    if (fake_load(img))
        return 0;
    memcpy(uid, img, 3);                /* UID0-2, BCC0, UID3-6 */
    memcpy(uid + 3, img + 4, 4);
    *uid_len = 7;
    return 1;
}

static int fake_read16(int page, uint8_t *b)
{
    uint8_t img[TAG_BYTES];

    if (fake_load(img) || page < 0 || page * 4 + 16 > TAG_BYTES)
        return -1;
    memcpy(b, img + page * 4, 16);
    return 0;
}

static int fake_write4(int page, const uint8_t *b)
{
    uint8_t img[TAG_BYTES];
    FILE *f;

    if (fake_load(img) || page < FIRST_PAGE || page > 129)
        return -1;
    memcpy(img + page * 4, b, 4);
    if (!(f = fopen(fake_path, "r+b")))
        return -1;
    fseek(f, page * 4, SEEK_SET);
    fwrite(b, 1, 4, f);
    return fclose(f) ? -1 : 0;
}

static const struct reader fake_reader = { "fake", fake_open, fake_poll, fake_read16, fake_write4 };

/* ---- the tag format --------------------------------------------------------------- */

static uint32_t crc32(const uint8_t *p, int n)
{
    uint32_t c = 0xFFFFFFFF;
    int k;

    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

/* 03 L [D4 0F 20 "igs-loader:hero" payload] FE, from page 4 */
static int build_area(uint8_t *out, const char *code, int points, uint32_t count)
{
    uint8_t *p = out, *pl;
    uint32_t c;
    int tl = sizeof(REC_TYPE) - 1;

    memset(out, 0, AREA);
    *p++ = 0x03;
    *p++ = 3 + tl + PAYLOAD;
    *p++ = 0xD4;                        /* MB ME SR, TNF 4 (external) */
    *p++ = tl;
    *p++ = PAYLOAD;
    memcpy(p, REC_TYPE, tl);
    p += tl;
    pl = p;
    memcpy(pl, "HERO", 4);
    pl[4] = 1;
    pl[5] = (code[0] ? 1 : 0) | (points >= 0 ? 2 : 0);
    strncpy((char *)pl + 8, code, 12);
    if (points >= 0) {
        pl[20] = points >> 8;
        pl[21] = points;
    }
    memcpy(pl + 24, &count, 4);
    c = crc32(pl, 28);
    memcpy(pl + 28, &c, 4);
    p += PAYLOAD;
    *p++ = 0xFE;
    return p - out;
}

/* the first 64 user bytes -> TAG_BLANK / TAG_LOADER (+ code, points, count) / TAG_FOREIGN */
static int parse_area(const uint8_t *a, struct hero_nfc_tag *t, uint32_t *count)
{
    int i = 0, tl = sizeof(REC_TYPE) - 1;
    const uint8_t *pl;
    uint32_t c;

    t->code[0] = 0;
    t->points = -1;
    *count = 0;
    /* NULL, lock control and memory control TLVs come before the NDEF one */
    while (i < AREA && (a[i] == 0x00 || a[i] == 0x01 || a[i] == 0x02)) {
        if (a[i] == 0x00) {
            i++;
            continue;
        }
        if (i + 1 >= AREA)
            return TAG_FOREIGN;
        i += 2 + a[i + 1];
    }
    if (i >= AREA || a[i] == 0xFE)
        return TAG_BLANK;
    if (a[i] != 0x03 || i + 1 >= AREA)
        return TAG_FOREIGN;
    if (a[i + 1] == 0x00)
        return TAG_BLANK;                   /* empty NDEF message */
    if (i + 5 + tl > AREA || a[i + 2] != 0xD4 || a[i + 3] != tl ||
        memcmp(a + i + 5, REC_TYPE, tl))
        return TAG_FOREIGN;
    /* the loader's record: damaged contents read as an empty loader tag */
    pl = a + i + 5 + tl;
    if (pl + PAYLOAD > a + AREA || a[i + 4] != PAYLOAD || memcmp(pl, "HERO", 4) || pl[4] != 1)
        return TAG_LOADER;
    memcpy(&c, pl + 28, 4);
    if (c != crc32(pl, 28))
        return TAG_LOADER;
    if (pl[5] & 1) {
        memcpy(t->code, pl + 8, 12);
        t->code[12] = 0;
    }
    if (pl[5] & 2)
        t->points = pl[20] << 8 | pl[21];
    memcpy(count, pl + 24, 4);
    return TAG_LOADER;
}

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
    uint8_t hdr[16], a[AREA];
    int i;

    if (nf.rd->read16(0, hdr))
        return TAG_FOREIGN;
    /* capability container: E1, version, size / 8, access */
    if (hdr[12] != 0xE1 || hdr[15] != 0x00) {
        *pages = 0;
        return TAG_FOREIGN;
    }
    *pages = hdr[14] * 2;
    if (*pages < AREA / 4)
        return TAG_FOREIGN;
    for (i = 0; i < AREA; i += 16)
        if (nf.rd->read16(FIRST_PAGE + i / 4, a + i))
            return TAG_FOREIGN;
    return parse_area(a, t, count);
}

/* write the copy's code and points to the tag, then read it back */
static int commit(const char *code, int points, uint32_t count)
{
    uint8_t a[AREA], back[AREA];
    int n = build_area(a, code, points, count), i;

    if (nf.user_pages < (n + 3) / 4)
        return -1;
    for (i = 0; i < n; i += 4)
        if (nf.rd->write4(FIRST_PAGE + i / 4, a + i))
            return -1;
    for (i = 0; i < AREA; i += 16)
        if (nf.rd->read16(FIRST_PAGE + i / 4, back + i))
            return -1;
    return memcmp(a, back, (n + 3) & ~3) ? -1 : 0;
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

static struct hero_nfc nfc_api = {
    .version = HERO_NFC_VERSION,
    .get = nfc_get,
    .dispense = nfc_dispense,
    .set_points = nfc_set_points,
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
