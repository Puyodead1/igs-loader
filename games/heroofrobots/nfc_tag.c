/*
 * Hero of Robots: NFC tag readers and the loader's tag format (nfc.h),
 * shared by the game's reader thread (nfc.c) and the card tool
 * (hero-nfc.c), so both agree byte for byte.  #included; the includer
 * provides dlopen/dlsym (and may define NLOG first).
 *
 * Readers:
 *   - PC/SC (winscard.dll, or libpcsclite.so.1 loaded at run time): the
 *     first reader whose name contains nfc_reader_want / HERO_NFC_READER
 *     (default "ACR122"); Type 2 tags through the reader's pseudo-APDUs
 *     FF CA (UID), FF B0 (read 16 bytes), FF D6 (write one page);
 *   - fake: nfc_fake_dir / IGS_NFC_FAKE=<dir>: <dir>/tag.bin is the tag on
 *     the reader, a 540-byte NTAG215 image (pages 0..134); no file, no tag.
 *     Pages 4..129 are writable; page 3 (capability container) is OTP: a
 *     write ORs its bits in.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nfc.h"

#ifndef NLOG
#define NLOG(...) do { fputs("[igs] nfc: ", stderr); fprintf(stderr, __VA_ARGS__); \
                       fputc('\n', stderr); } while (0)
#endif

#define REC_TYPE "igs-loader:hero"
#define PAYLOAD 32
#define FIRST_PAGE 4
#define AREA 64                     /* bytes read from page 4 to classify a tag */
#define TAG_BYTES 540               /* NTAG215: 135 pages */
#define LAST_USER_PAGE 129          /* NTAG215 */

/* overrides of HERO_NFC_READER / IGS_NFC_FAKE (the card tool's options) */
static const char *nfc_reader_want, *nfc_fake_dir;

/* ---- readers ---------------------------------------------------------------------- */

struct reader {
    const char *name;
    int (*open)(void);                              /* 0: ready */
    int (*poll)(uint8_t *uid, int *uid_len);        /* 1 tag, 0 none */
    int (*read16)(int page, uint8_t *b);            /* 0: ok */
    int (*write4)(int page, const uint8_t *b);      /* 0: ok */
};

/*
 * -- PC/SC: LONG/DWORD/handles are 32-bit on both i386 ABIs and unsigned
 * long on LP64 pcsclite (not on Win64: build i686 there)
 */
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

static const char *pcsc_want(void)
{
    const char *want = nfc_reader_want ? nfc_reader_want : getenv("HERO_NFC_READER");

    return want && *want ? want : "ACR122";
}

/* the reader names (a NUL-separated list) into names; 0: ok */
static int pcsc_names(char *names, unsigned long len)
{
    static int said;
    long r;

    if (!pc.have_ctx) {
        if ((r = pc.establish(2 /* SCARD_SCOPE_SYSTEM */, NULL, NULL, &pc.ctx))) {
            if (!said++)
                NLOG("PC/SC unavailable (%08lx): no reader", (unsigned long)r);
            return -1;
        }
        pc.have_ctx = 1;
    }
    if (pc.list(pc.ctx, NULL, names, &len) || len < 2)
        names[0] = names[1] = 0;
    return 0;
}

/* find the reader: the context and the list, at most every 5 s */
static int pcsc_find(void)
{
    const char *want = pcsc_want();
    static int said;
    char names[4096], *n;

    if (pc.reader[0])
        return 1;
    if (now_s() < pc.next_list)
        return 0;
    pc.next_list = now_s() + 5;
    if (pcsc_names(names, sizeof(names)))
        return 0;
    for (n = names; *n; n += strlen(n) + 1)
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

/* -- a file per tag */
static char fake_path[1024];

static int fake_open(void)
{
    snprintf(fake_path, sizeof(fake_path), "%s/tag.bin",
             nfc_fake_dir ? nfc_fake_dir : getenv("IGS_NFC_FAKE"));
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
    uint8_t img[TAG_BYTES], w[4];
    FILE *f;
    int i;

    if (fake_load(img) || page < 3 || page > LAST_USER_PAGE)
        return -1;
    for (i = 0; i < 4; i++)
        w[i] = page == 3 ? img[12 + i] | b[i] : b[i];       /* the CC is one-time programmable */
    if (!(f = fopen(fake_path, "r+b")))
        return -1;
    fseek(f, page * 4, SEEK_SET);
    fwrite(w, 1, 4, f);
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

/* read and classify the tag on the reader; *pages: user pages from the CC (0: no NDEF CC) */
static int tag_examine(const struct reader *rd, struct hero_nfc_tag *t, uint32_t *count, int *pages)
{
    uint8_t hdr[16], a[AREA];
    int i;

    *pages = 0;
    if (rd->read16(0, hdr))
        return TAG_FOREIGN;
    /* capability container: E1, version, size / 8, access */
    if (hdr[12] != 0xE1 || hdr[15] != 0x00)
        return TAG_FOREIGN;
    *pages = hdr[14] * 2;
    if (*pages < AREA / 4)
        return TAG_FOREIGN;
    for (i = 0; i < AREA; i += 16)
        if (rd->read16(FIRST_PAGE + i / 4, a + i))
            return TAG_FOREIGN;
    return parse_area(a, t, count);
}

/* write a code (and points, -1 none) as the loader's record, then read it back; 0: ok */
static int tag_commit(const struct reader *rd, int user_pages, const char *code, int points,
                      uint32_t count)
{
    uint8_t a[AREA], back[AREA];
    int n = build_area(a, code, points, count), i;

    if (user_pages < (n + 3) / 4)
        return -1;
    for (i = 0; i < n; i += 4)
        if (rd->write4(FIRST_PAGE + i / 4, a + i))
            return -1;
    for (i = 0; i < AREA; i += 16)
        if (rd->read16(FIRST_PAGE + i / 4, back + i))
            return -1;
    return memcmp(a, back, (n + 3) & ~3) ? -1 : 0;
}
