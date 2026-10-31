/*
 * Hero of Robots: stand-in for the cabinet's libcasmcard.so (Castles
 * EZ100-style PC/SC wrapper) with one reader and one inserted Java card,
 * the game's credit card.  Built with SONAME libcasmcard.so and preloaded
 * (manifest PRELOAD), so the game's Cas* imports land here instead of on
 * pcscd.  docs/HeroOfRobots.md, Smart card.
 *
 * The game (SmartCard::Authentication) talks to the card applet with
 * RSA-wrapped APDUs:
 *   - it writes a 256-byte ciphertext of [r0 r1 t0 t1 t2 t3] (PKCS#1 type 2,
 *     the game's built-in 2048-bit public key) in two halves: INS 0x0F
 *     query, 0x0D debit; t = packed local time for a debit, 0 for a query;
 *   - it reads 256 bytes back in two halves and decrypts them with the key
 *     the ASIC gave it (card.h): query [credit.BE16 0 0 0 0 r0 r1 area],
 *     debit [credit.BE16 r0 r1 area t t].
 * The real card holds the private half of the game's key; this one does
 * not.  The challenge bytes r0, r1 and the padding all come from rand() on
 * the game's card thread, so rand() is interposed here and logged per
 * thread: the card rebuilds the plaintext from that log and checks it by
 * re-encrypting it with the game's public key (a real RSA check, no game
 * memory is read).  The reply is then genuinely encrypted with card.h's N.
 *
 * Environment: HERO_CARD_CREDIT (points on the card, default 100),
 * HERO_REGION (area byte, as the ASIC plugin's).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "card.h"

#define LOG(...) do { fputs("[igs] card: ", stderr); fprintf(stderr, __VA_ARGS__); \
                      fputc('\n', stderr); } while (0)

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

/* the Windows loader's preload: the library this one stands in for (its SONAME) */
EXPORT const char igs_soname[] = "libcasmcard.so";

/* ---- 2048-bit arithmetic (little-endian 32-bit limbs) ------------------------- */

#define LIMBS 64

static int hexval(int c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

static void bn_from_hex(uint32_t *x, const char *h)
{
    size_t n = strlen(h), i;

    memset(x, 0, LIMBS * 4);
    for (i = 0; i < n && i < LIMBS * 8; i++)
        x[i / 8] |= (uint32_t)hexval(h[n - 1 - i]) << (4 * (i % 8));
}

/* 256 big-endian bytes */
static void bn_from_bytes(uint32_t *x, const uint8_t *b)
{
    int i;

    memset(x, 0, LIMBS * 4);
    for (i = 0; i < 256; i++)
        x[i / 4] |= (uint32_t)b[255 - i] << (8 * (i % 4));
}

static void bn_to_bytes(const uint32_t *x, uint8_t *b)
{
    int i;

    for (i = 0; i < 256; i++)
        b[255 - i] = x[i / 4] >> (8 * (i % 4));
}

/* r = a * b mod m (schoolbook product, bitwise reduction) */
static void mulmod(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m)
{
    uint32_t t[2 * LIMBS], rem[LIMBS + 1];
    int i, j, bit;

    memset(t, 0, sizeof(t));
    for (i = 0; i < LIMBS; i++) {
        uint64_t carry = 0;

        for (j = 0; j < LIMBS; j++) {
            uint64_t v = (uint64_t)a[i] * b[j] + t[i + j] + carry;

            t[i + j] = (uint32_t)v;
            carry = v >> 32;
        }
        t[i + LIMBS] = (uint32_t)carry;
    }
    memset(rem, 0, sizeof(rem));
    for (bit = 2 * LIMBS * 32 - 1; bit >= 0; bit--) {
        int ge = 1;

        for (i = LIMBS; i > 0; i--)
            rem[i] = rem[i] << 1 | rem[i - 1] >> 31;
        rem[0] = rem[0] << 1 | (t[bit / 32] >> (bit % 32) & 1);
        if (!rem[LIMBS]) {
            for (i = LIMBS - 1; i >= 0; i--)
                if (rem[i] != m[i]) {
                    ge = rem[i] > m[i];
                    break;
                }
        }
        if (ge) {
            uint64_t borrow = 0;

            for (i = 0; i < LIMBS; i++) {
                uint64_t v = (uint64_t)rem[i] - m[i] - borrow;

                rem[i] = (uint32_t)v;
                borrow = v >> 63;
            }
            rem[LIMBS] -= (uint32_t)borrow;
        }
    }
    memcpy(r, rem, LIMBS * 4);
}

/* out = (in ^ e mod n), all 256-byte big-endian blocks */
static void rsa_public(uint8_t *out, const uint8_t *in, const char *n_hex, uint32_t e)
{
    uint32_t n[LIMBS], b[LIMBS], r[LIMBS];
    int bit = 31;

    bn_from_hex(n, n_hex);
    bn_from_bytes(b, in);
    while (bit > 0 && !(e >> bit & 1))
        bit--;
    memcpy(r, b, sizeof(r));
    while (--bit >= 0) {
        mulmod(r, r, r, n);
        if (e >> bit & 1)
            mulmod(r, r, b, n);
    }
    bn_to_bytes(r, out);
}

/* ---- rand() log --------------------------------------------------------------- */

#define RAND_LOG 2048
static __thread int rand_log[RAND_LOG];
static __thread int rand_n;

/* the game's rand(), recorded per thread; cleared after every APDU */
EXPORT int rand(void)
{
    static int (*real)(void);
    int v;

    /* dlvsym: the loader core interposes dlsym, and RTLD_NEXT is per caller */
    if (!real)
        *(void **)&real = dlvsym(RTLD_NEXT, "rand", "GLIBC_2.0");
    v = real();
    if (rand_n < RAND_LOG)
        rand_log[rand_n++] = v;
    return v;
}

/* our own padding bytes: the game's generator stays untouched */
static uint32_t prng(void)
{
    static uint32_t x;

    if (!x)
        x = (uint32_t)time(NULL) | 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* ---- the card ------------------------------------------------------------------ */

static struct {
    int credit, area, inited;
    uint8_t ct[256];             /* the game's challenge, written in halves */
    uint8_t resp[256];           /* our answer, read in halves */
} card;

static void card_init(void)
{
    const char *v;

    if (card.inited)
        return;
    card.inited = 1;
    v = getenv("HERO_CARD_CREDIT");
    card.credit = v && *v ? atoi(v) : 100;
    v = getenv("HERO_REGION");
    card.area = v && *v ? (int)strtol(v, NULL, 0) : HERO_REGION_DEFAULT;
    LOG("Java card in reader 0, %d points", card.credit);
}

/* Authentication's timestamp: sec, min << 6, hour << 12, mday << 17, mon << 22, year << 26 */
static uint32_t packed_time(time_t when)
{
    struct tm tm;

    localtime_r(&when, &tm);
    return ((uint32_t)(tm.tm_year - 100) << 26) | ((uint32_t)((tm.tm_mon + 1) & 0xf) << 22) |
           ((uint32_t)(tm.tm_mday & 0x1f) << 17) | ((uint32_t)(tm.tm_hour & 0x1f) << 12) |
           ((uint32_t)(tm.tm_min & 0x3f) << 6) | (uint32_t)(tm.tm_sec & 0x3f);
}

/*
 * _rsaPack's block from the rand() log at k: 00 02, 247 padding bytes in
 * chunks of 64 (rand() % 255 each, zeros drawn again after the chunk),
 * 00, the 6-byte message [log[k], log[k + 1], t].  0 if the log runs out.
 */
static int rebuild(uint8_t *m, int k, uint32_t t)
{
    int idx = k + 2, done = 0, j;

    m[0] = 0;
    m[1] = 2;
    while (done < 247) {
        int c = 247 - done < 64 ? 247 - done : 64;
        uint8_t *v = m + 2 + done;

        for (j = 0; j < c; j++) {
            if (idx >= rand_n)
                return 0;
            v[j] = rand_log[idx++] % 255;
        }
        for (j = 0; j < c; j++)
            while (!v[j]) {
                if (idx >= rand_n)
                    return 0;
                v[j] = rand_log[idx++] % 255;
            }
        done += c;
    }
    m[249] = 0;
    m[250] = (uint8_t)rand_log[k];
    m[251] = (uint8_t)rand_log[k + 1];
    memcpy(m + 252, &t, 4);
    return 1;
}

/* the whole challenge is in: recover it, then answer (debit or query) */
static void card_answer(int debit)
{
    uint8_t m[256], c[256], blk[256], msg[13] = { 0 };
    int k, dt, found = 0, i;
    uint32_t t = 0;
    time_t now = time(NULL);

    for (k = 0; !found && k + 2 + 247 <= rand_n && k < 8; k++)
        for (dt = 0; !found && dt < (debit ? 3 : 1); dt++) {
            t = debit ? packed_time(now - dt) : 0;
            if (!rebuild(m, k, t))
                break;
            rsa_public(c, m, GAME_KEY_N, 65537);
            found = !memcmp(c, card.ct, 256);
        }
    if (!found) {
        LOG("challenge not recovered (%d rand() calls logged)", rand_n);
        m[250] = rand_n > 0 ? (uint8_t)rand_log[0] : 0;
        m[251] = rand_n > 1 ? (uint8_t)rand_log[1] : 0;
        t = 0;
    }
    if (debit && card.credit > 0)
        card.credit--;
    msg[0] = card.credit >> 8;
    msg[1] = card.credit;
    if (debit) {
        msg[2] = m[250];
        msg[3] = m[251];
        msg[4] = card.area;
        memcpy(msg + 5, &t, 4);
        memcpy(msg + 9, &t, 4);
    } else {
        msg[6] = m[250];
        msg[7] = m[251];
        msg[8] = card.area;
    }
    LOG("%s: %d points left%s", debit ? "debit" : "query", card.credit,
        found ? "" : " (unverified challenge)");
    /* 00 02 PS 00 M, encrypted to the key the ASIC gave the game */
    blk[0] = 0;
    blk[1] = 2;
    for (i = 2; i < 256 - 1 - (int)sizeof(msg); i++)
        while (!(blk[i] = prng() >> 8))
            ;
    blk[i++] = 0;
    memcpy(blk + i, msg, sizeof(msg));
    rsa_public(card.resp, blk, CARD_KEY_N, CARD_KEY_E);
}

/* one APDU; returns the response length (data + SW1 SW2) */
static uint32_t card_apdu(const uint8_t *a, uint32_t n, uint8_t *r, uint32_t max)
{
    static const uint8_t select[] = { 0x00, 0xA4, 0x04, 0x00, 0x0A };
    int half;

    if (max < 2)
        return 0;
    r[0] = 0x6D;                                        /* unknown instruction */
    r[1] = 0x00;
    if (n >= 4 && a[0] == 0x80 && a[1] == 0xCA) {       /* GET DATA (CPLC): unused */
        r[0] = 0x90;
        return 2;
    }
    if (n >= 5 && !memcmp(a, select, sizeof(select))) { /* SELECT the applet */
        r[0] = 0x90;
        return 2;
    }
    if (n < 5 || a[0] != 0x00 || (a[1] != 0x0F && a[1] != 0x0D))
        return 2;
    half = a[3] & 0x80 ? 128 : 0;
    if (n >= 5 + 128 && a[4] == 0x80) {                 /* write a challenge half */
        memcpy(card.ct + half, a + 5, 128);
        if (half)
            card_answer(a[1] == 0x0D);
        r[0] = 0x90;
        return 2;
    }
    if (n == 5 && a[4] == 0x80 && max >= 130) {         /* read an answer half */
        memcpy(r, card.resp + half, 128);
        r[128] = 0x90;
        r[129] = 0x00;
        return 130;
    }
    return 2;
}

/* ---- the Cas* API (pcsclite-style; LONG/DWORD are 32-bit) ----------------------- */

#define READER "IGS loader card reader 0"
#define HCARD 0x5ca4d

EXPORT long CasEstablishContext(unsigned long scope, const void *r1, const void *r2, long *ctx)
{
    (void)scope; (void)r1; (void)r2;
    card_init();
    if (ctx)
        *ctx = 1;
    return 0;
}

EXPORT long CasReleaseContext(long ctx)
{
    (void)ctx;
    return 0;
}

/* one reader: a multi-string "name\0\0" */
EXPORT long CasListReaders(long ctx, const char *groups, char *readers, unsigned long *len)
{
    (void)ctx; (void)groups;
    if (readers) {
        memcpy(readers, READER, sizeof(READER));
        readers[sizeof(READER)] = 0;
    }
    if (len)
        *len = sizeof(READER) + 1;
    return 0;
}

EXPORT long CasConnect(long ctx, const char *reader, unsigned long share,
                       unsigned long protocols, long *card_h, unsigned long *proto)
{
    (void)ctx; (void)reader; (void)share; (void)protocols;
    card_init();
    if (card_h)
        *card_h = HCARD;
    if (proto)
        *proto = 1;                                    /* T=0 */
    return 0;
}

EXPORT long CasDisconnect(long card_h, unsigned long disposition)
{
    (void)card_h; (void)disposition;
    return 0;
}

/* a Java card's ATR (not 24 bytes long: that would be the SLE4442 path) */
EXPORT long CasStatus(long card_h, char *name, unsigned long *name_len, unsigned long *state,
                      unsigned long *proto, unsigned char *atr, unsigned long *atr_len)
{
    static const uint8_t java_atr[] = {
        0x3B, 0x6A, 0x00, 0x00, 0x80, 0x65, 0xA2, 0x01, 0x01, 0x01, 0x3D, 0x72, 0xD6, 0x43,
    };

    (void)card_h;
    if (name && name_len && *name_len >= sizeof(READER))
        memcpy(name, READER, sizeof(READER));
    if (name_len)
        *name_len = sizeof(READER);
    if (state)
        *state = 6;                                    /* SCARD_SPECIFIC */
    if (proto)
        *proto = 1;
    if (atr && atr_len && *atr_len >= sizeof(java_atr))
        memcpy(atr, java_atr, sizeof(java_atr));
    if (atr_len)
        *atr_len = sizeof(java_atr);
    return 0;
}

EXPORT long CasTransmit(long card_h, const void *send_pci, const unsigned char *sbuf,
                        unsigned long slen, void *recv_pci, unsigned char *rbuf,
                        unsigned long *rlen)
{
    (void)card_h; (void)send_pci; (void)recv_pci;
    if (!rbuf || !rlen)
        return 0x80100004;                             /* SCARD_E_INVALID_PARAMETER */
    *rlen = card_apdu(sbuf, slen, rbuf, *rlen);
    /* the next challenge's rand() calls start here (not between its halves) */
    if (!(slen >= 5 + 128 && sbuf[0] == 0x00 && (sbuf[1] == 0x0F || sbuf[1] == 0x0D) &&
          !(sbuf[3] & 0x80)))
        rand_n = 0;
    return 0;
}

EXPORT long CasGetRdrType(long card_h, unsigned char *buf, unsigned long *len)
{
    (void)card_h; (void)buf;
    if (len)
        *len = 0;
    return 0;
}

/* SLE4442 memory cards: never inserted */
EXPORT int SLE4442_Read_Main_Memory(void) { return -1; }
EXPORT int SLE4442_Update_Main_Memory(void) { return -1; }
EXPORT int SLE4442_Read_Protection_Memory(void) { return -1; }
EXPORT int SLE4442_Write_Protection_Memory(void) { return -1; }
EXPORT int SLE4442_Update_Security_Memory(void) { return -1; }
EXPORT int SLE4442_Compare_Verification_Data(void) { return -1; }
