/*
 * E2000 machine plugin: the PLX PCI9030 card and the protection CPU's
 * transport, for E2000 games.
 *
 *   - /dev/plx/Pci9030 (driver node) and /dev/plx/Pci9030-0 (the card):
 *     the PLX SDK ioctls the games use (device find, version, location,
 *     BAR size/properties/map).  The other PLX chip nodes do not exist.
 *   - BAR0 (PLX local registers) and BAR2 (shared message RAM) are plain
 *     memory; BAR3 (16-bit mailbox registers) is an MMIO trap with separate
 *     read (chip -> game) and write (game -> chip) sides.
 *   - Writing 1 to BAR3+14 rings the doorbell: the chip answers at once.
 *     IGS036: it reads the game's message from BAR2 (key word RC4-decrypted
 *     with the game -> IGS036 stream, payload XOR key), asks the game plugin
 *     for the reply, writes it back the same way with its own stream, and
 *     sets its status registers to the game's command + 1.
 *     IGS027A: the game's mailbox byte is sequence | port; the chip answers
 *     the next sequence number | 0x40.  Of the two rings per packet, the
 *     first carries the packet (RC4-decrypted, checksummed), whose reply the
 *     chip writes back encrypted with its own stream.
 *     IGS036 message transport: the game's sub-messages (length RC4, bytes
 *     XOR the chip's last key) go to the game plugin, whose answer is the
 *     next message: key, length and three header words RC4 (one stream both
 *     ways), eight XOR the key, the sub-messages XOR the key; the status
 *     registers become the game's command + 1.  Speed Driver 2's layout
 *     moves the header and RC4s only the chip's words.
 *     IGS036 packet transport: the game's packet (checksum word RC4, bytes
 *     XOR its low byte) goes to the game plugin as sub-messages, whose answer
 *     is written back the same way with the chip's stream; the status
 *     registers become the game's sequence + 1 and the packet length.  A
 *     board reset (PLX ioctl) restarts the chip.
 *   - /dev/roio: BIOS byte reads (ioctl 0xFC, address ^ 0x1FB8408E, data
 *     ^ 0xB8) served from the strings the game plugin lists.
 *   - /dev/pccard0: must open; only used when the BIOS check fails.
 * Protocol: docs/IGS_E2000.md.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "e2000.h"

#define BAR0_SIZE 0x80
#define BAR2_SIZE 0x10000
#define BAR3_SIZE 0x100
#define BAR_PHYS(n) (0xE0000000u + (n) * 0x100000u)

/* PLX SDK ioctls (struct of 0x24 bytes: return code at +0) */
#define PLX_LOCATION   0xC0245100u   /* device location at +12 */
#define PLX_VERSION    0xC0245101u   /* +8 rev, +9 minor, +10 major */
#define PLX_RESET      0xC0245103u
#define PLX_FIND       0xC0245104u   /* +8 index / count (80001 = count), location at +12 */
#define PLX_BAR_PROPS  0xC0245105u   /* in: +4 bar; out: +8 phys address, +12 I/O flag */
#define PLX_BAR_SIZE   0xC0245106u   /* in: +4 bar; out: +8 size */
#define PLX_BAR_MAP    0xC024510Bu   /* binds the preceding mmap; status at +0 */
#define PLX_OK 512

#define ROIO_READ      0xFC
#define ROIO_ADDR_XOR  0x1FB8408Eu
#define ROIO_DATA_XOR  0xB8

#define BAR3_DOORBELL  14

enum { DEV_NONE, DEV_PLX_DRIVER, DEV_PLX_CARD, DEV_ROIO, DEV_PCCARD, DEV_TDC };

static const struct igs_core *core;
static const struct e2000_game *game;

#define LOG(...) core->log("e2000: " __VA_ARGS__)
#define VLOG(...) do { if (core->verbose) LOG(__VA_ARGS__); } while (0)

/* ---- RC4 (the games use OpenSSL's; key = the tag string with its NUL) ------- */

struct rc4 { uint8_t s[256], i, j; };

static void rc4_init_len(struct rc4 *r, const void *key, size_t n)
{
    const uint8_t *k = key;
    unsigned i, j = 0;

    for (i = 0; i < 256; i++)
        r->s[i] = i;
    for (i = 0; i < 256; i++) {
        uint8_t t = r->s[i];

        j = (j + t + k[i % n]) & 0xff;
        r->s[i] = r->s[j];
        r->s[j] = t;
    }
    r->i = r->j = 0;
}

static void rc4_init(struct rc4 *r, const char *key)
{
    rc4_init_len(r, key, strlen(key) + 1);
}

static void rc4_crypt(struct rc4 *r, uint8_t *b, size_t n)
{
    size_t k;

    for (k = 0; k < n; k++) {
        uint8_t t;

        r->i++;
        r->j += r->s[r->i];
        t = r->s[r->i];
        r->s[r->i] = r->s[r->j];
        r->s[r->j] = t;
        b[k] ^= r->s[(uint8_t)(r->s[r->i] + r->s[r->j])];
    }
}

static uint32_t rc4_word(struct rc4 *r, uint32_t v)
{
    rc4_crypt(r, (uint8_t *)&v, 4);
    return v;
}

/* ---- state -------------------------------------------------------------------- */

static struct {
    pthread_mutex_t lock;
    int fds[8], kinds[8], nfds;
    int pending_bar;                     /* BAR named by the last size/props query */
    uint8_t *bar0, *bar2, *bar3_map;
    uint16_t bar3_in[8];                 /* IGS036 -> game */
    uint16_t bar3_out[8];                /* game -> IGS036 */
    struct rc4 to_game, from_game;
    uint32_t exchanges;
    int mbox_written, pkt_phase;         /* IGS027A: mailbox byte since the last ring; ring 1 or 2 */
    uint32_t msg_key;                    /* IGS036 message: the key of the chip's last message */
    uint8_t rekey[64];                   /* IGS036 message: a re-key waiting for the message's end */
    int rekey_len;
} s ={ .lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP };  /* the plugin's own fopen comes back */

static int fd_kind(int fd)
{
    int i;

    for (i = 0; i < s.nfds; i++)
        if (s.fds[i] == fd)
            return s.kinds[i];
    return DEV_NONE;
}

/* ---- IGS036 ------------------------------------------------------------------- */

static uint32_t bar2_ld(uint32_t w) { uint32_t v; memcpy(&v, s.bar2 + 4 * w, 4); return v; }
static void bar2_st(uint32_t w, uint32_t v) { memcpy(s.bar2 + 4 * w, &v, 4); }

static void msg_send(const uint8_t *in, uint32_t nin, uint8_t status);
static void sd2_send(const uint8_t *in, uint32_t nin, uint8_t status, uint32_t word, uint32_t req);
static void pkt36_send(const uint8_t *in, uint32_t nin, uint8_t seq);

static void igs036_reset(void)
{
    if (game->transport == E2000_IGS027A || game->transport == E2000_IGS036_PKT) {
        rc4_init_len(&s.to_game, game->pkt_key_to_game, game->pkt_key_len);
        rc4_init_len(&s.from_game, game->pkt_key_from_game, game->pkt_key_len);
    } else if (game->transport == E2000_IGS036_MSG) {
        rc4_init_len(&s.to_game, game->msg_key, game->msg_key_len);
    } else {
        rc4_init(&s.to_game, game->key_to_game);
        rc4_init(&s.from_game, game->key_from_game);
    }
    memset(s.bar3_in, 0, sizeof(s.bar3_in));
    s.exchanges = 0;
    s.mbox_written = s.pkt_phase = 0;
    s.rekey_len = 0;
    if (game->reset)
        game->reset();
    /* the chip's power-on message, there before the game reads the status */
    if (game->transport == E2000_IGS036_MSG && game->msg_layout == E2000_MSG_SD2)
        sd2_send(NULL, 0, 0x80, 0, 0);
    else if (game->transport == E2000_IGS036_MSG)
        msg_send(NULL, 0, 0x80);
    else if (game->transport == E2000_IGS036_PKT)
        pkt36_send(NULL, 0, 1);            /* the game first looks for sequence 1 */
}

static void api_rekey(int to_game, const void *key, int len)
{
    if (game->transport == E2000_IGS036_MSG) {
        s.rekey_len = len < (int)sizeof(s.rekey) ? len : (int)sizeof(s.rekey);
        memcpy(s.rekey, key, s.rekey_len);
        return;
    }
    rc4_init_len(to_game ? &s.to_game : &s.from_game, key, len);
}

/* ---- IGS036 message transport (Speed Rider) --------------------------------------- */

#define MSG_KEY     8                      /* BAR2 byte offsets of the header */
#define MSG_LEN     12
#define MSG_NEXT    16                     /* the next message's offset (always 0 here) */
#define MSG_HDR     20                     /* E2000_MSG_HDR words */
#define MSG_SEQ     64                     /* the status this message answers */
#define MSG_DATA    128

static uint32_t bar2_ldb(uint32_t o) { uint32_t v; memcpy(&v, s.bar2 + o, 4); return v; }
static void bar2_stb(uint32_t o, uint32_t v) { memcpy(s.bar2 + o, &v, 4); }

/*
 * Write the chip's next message.  The game decrypts key, next offset, the
 * three RC4 header words and the length in that order, and its reply's
 * length after that, all from one RC4 stream.
 */
static void msg_send(const uint8_t *in, uint32_t nin, uint8_t status)
{
    static uint8_t out[E2000_SUB_MAX + 4];
    uint32_t hdr[E2000_MSG_HDR] = { 0 }, n, i, key;

    memset(out, 0, sizeof(out));
    n = game->message(in, nin, hdr, out, E2000_SUB_MAX);
    if (n > E2000_SUB_MAX)
        n = E2000_SUB_MAX;
    n++;                                   /* the ending 0 */
    key = (uint32_t)rand() % 100000 + 1;
    bar2_stb(MSG_KEY, rc4_word(&s.to_game, key));
    bar2_stb(MSG_NEXT, rc4_word(&s.to_game, 0));
    for (i = 0; i < E2000_MSG_HDR_RC4; i++)
        bar2_stb(MSG_HDR + 4 * i, rc4_word(&s.to_game, hdr[i]));
    bar2_stb(MSG_LEN, rc4_word(&s.to_game, n));
    for (; i < E2000_MSG_HDR; i++)
        bar2_stb(MSG_HDR + 4 * i, hdr[i] ^ key);
    bar2_stb(MSG_SEQ, status);
    for (i = 0; i < n; i += 4) {
        uint32_t w;

        memcpy(&w, out + i, 4);
        bar2_stb(MSG_DATA + i, w ^ key);
    }
    s.msg_key = key;
    if (s.rekey_len) {
        rc4_init_len(&s.to_game, s.rekey, s.rekey_len);
        s.rekey_len = 0;
    }
    for (i = 0; i < 8; i++)
        s.bar3_in[i] = status;
}

/* the game rang: read its reply (sub-messages XOR our last key), answer with the next message */
static void msg_doorbell(void)
{
    static uint8_t in[E2000_SUB_MAX + 4];
    uint8_t cmd = s.bar3_out[0] & 0xff;
    uint32_t n = rc4_word(&s.to_game, bar2_ldb(MSG_LEN)), i;

    if (n > E2000_SUB_MAX) {
        LOG("IGS036 message: bad length %u", n);
        n = 0;
    }
    for (i = 0; i < n; i += 4) {
        uint32_t w = bar2_ldb(MSG_DATA + i) ^ s.msg_key;

        memcpy(in + i, &w, 4);
    }
    if (s.exchanges < 3)
        LOG("IGS036 message %u: cmd 0x%02x, %u bytes in", s.exchanges, cmd, n);
    msg_send(in, n, (uint8_t)((cmd + 1) | 0x80));
    s.exchanges++;
}

/*
 * Speed Driver 2's layout (pci/c/pci.cpp): the chip's key at +20, the
 * sub-message length at +24, steer/gas/brake floats at +48/+52/+56, the
 * main state at +60 and a word at +72, all through the RC4 stream (the game
 * decrypts +20, +48, +52, +56, +60, +72, +24 in that order); plain words at
 * +28 and +68; the status this message answers at +0x3000.  The game's
 * answer is not RC4'd: length at +24, a word at +60, both XOR the chip's
 * key, a request word at +64 in clear.
 */
#define SD2_KEY     20
#define SD2_LEN     24
#define SD2_W28     28
#define SD2_FLOATS  48
#define SD2_STATE   60
#define SD2_REQ     64
#define SD2_W68     68
#define SD2_W72     72
#define SD2_SEQ     0x3000

static void sd2_send(const uint8_t *in, uint32_t nin, uint8_t status, uint32_t word, uint32_t req)
{
    static uint8_t out[E2000_SUB_MAX + 4];
    uint32_t hdr[E2000_MSG_HDR] = { 0 }, n, i, key;

    memset(out, 0, sizeof(out));
    hdr[E2000_SD2_IN_WORD] = word;
    hdr[E2000_SD2_IN_REQ] = req;
    n = game->message(in, nin, hdr, out, E2000_SUB_MAX);
    if (n > E2000_SUB_MAX)
        n = E2000_SUB_MAX;
    n++;                                   /* the ending 0 */
    key = (uint32_t)rand() % 100000 + 1;
    bar2_stb(SD2_KEY, rc4_word(&s.to_game, key));
    for (i = 0; i < 3; i++)
        bar2_stb(SD2_FLOATS + 4 * i, rc4_word(&s.to_game, hdr[E2000_SD2_STEER + i]));
    bar2_stb(SD2_STATE, rc4_word(&s.to_game, hdr[E2000_SD2_STATE]));
    bar2_stb(SD2_W72, rc4_word(&s.to_game, hdr[E2000_SD2_W72]));
    bar2_stb(SD2_LEN, rc4_word(&s.to_game, n));
    bar2_stb(SD2_W28, hdr[E2000_SD2_W28]);
    bar2_stb(SD2_W68, hdr[E2000_SD2_W68]);
    bar2_stb(SD2_REQ, 0);
    bar2_stb(SD2_SEQ, status);
    for (i = 0; i < n; i += 4) {
        uint32_t w;

        memcpy(&w, out + i, 4);
        bar2_stb(MSG_DATA + i, w ^ key);
    }
    s.msg_key = key;
    for (i = 0; i < 8; i++)
        s.bar3_in[i] = status;
}

static void sd2_doorbell(void)
{
    static uint8_t in[E2000_SUB_MAX + 4];
    uint8_t cmd = s.bar3_out[0] & 0xff;
    uint32_t n = bar2_ldb(SD2_LEN) ^ s.msg_key, i;

    if (n > E2000_SUB_MAX) {
        LOG("IGS036 message: bad length %u", n);
        n = 0;
    }
    for (i = 0; i < n; i += 4) {
        uint32_t w = bar2_ldb(MSG_DATA + i) ^ s.msg_key;

        memcpy(in + i, &w, 4);
    }
    if (s.exchanges < 3)
        LOG("IGS036 message %u: cmd 0x%02x, %u bytes in", s.exchanges, cmd, n);
    sd2_send(in, n, (uint8_t)((cmd + 1) | 0x80), bar2_ldb(SD2_STATE) ^ s.msg_key, bar2_ldb(SD2_REQ));
    s.exchanges++;
}

/* ---- IGS036 packet transport (Panda Family) ----------------------------------------- */

/*
 * A packet in BAR2: a checksum word (the 32-bit sum of bytes 4..n-1 before
 * masking), then sub-messages [type, length (16-bit), data]; bytes 4..n-1
 * are XOR the checksum's low byte, and the checksum word goes through the
 * sender's RC4 stream.  The status (BAR3 registers 0..3, low bytes) is the
 * sequence byte | n << 8; the game rings with sequence + 1 and its own n,
 * the chip answers with that + 1.
 */
static uint32_t pkt36_sum(const uint8_t *b, uint32_t n)
{
    uint32_t sum = 0, i;

    for (i = 4; i < n; i++)
        sum += b[i];
    return sum;
}

static void pkt36_status(uint32_t v)
{
    int k;

    for (k = 0; k < 8; k++)
        s.bar3_in[k] = k < 4 ? (v >> (8 * k)) & 0xff : 0;
}

static void pkt36_send(const uint8_t *in, uint32_t nin, uint8_t seq)
{
    static uint8_t b[E2000_PKT36_MAX];
    uint32_t n, sum, i;

    memset(b, 0, sizeof(b));
    n = game->message(in, nin, NULL, b + 4, sizeof(b) - 12);
    if (n > sizeof(b) - 12)
        n = sizeof(b) - 12;
    /* an empty sub-message ends the list (the game stops one byte short), then pad to words */
    n = (4 + n + 3 + 3) & ~3u;
    sum = pkt36_sum(b, n);
    for (i = 4; i < n; i++)
        b[i] ^= sum & 0xff;
    memcpy(b, &sum, 4);
    rc4_crypt(&s.to_game, b, 4);
    memcpy(s.bar2, b, n);
    pkt36_status(seq | n << 8);
}

static void pkt36_doorbell(void)
{
    static uint8_t b[E2000_PKT36_MAX];
    uint32_t st = (s.bar3_out[0] & 0xff) | (s.bar3_out[1] & 0xff) << 8 |
                  (s.bar3_out[2] & 0xff) << 16 | (uint32_t)(s.bar3_out[3] & 0xff) << 24;
    uint32_t n = st >> 8, sum, i;

    if (n < 4 || n > sizeof(b)) {
        LOG("IGS036 packet: bad length %u", n);
        n = 4;
    }
    memcpy(b, s.bar2, n);
    rc4_crypt(&s.from_game, b, 4);
    memcpy(&sum, b, 4);
    for (i = 4; i < n; i++)
        b[i] ^= sum & 0xff;
    if (pkt36_sum(b, n) != sum)
        LOG("IGS036 packet %u: checksum 0x%08x, expected 0x%08x", s.exchanges, pkt36_sum(b, n), sum);
    if (s.exchanges < 3)
        LOG("IGS036 packet %u: seq 0x%02x, %u bytes in", s.exchanges, st & 0xff, n);
    pkt36_send(b + 4, n - 4, (uint8_t)(st + 1));
    s.exchanges++;
}

/* ---- IGS027A ------------------------------------------------------------------ */

#define PKT_CRYPT_OFF (E2000_PKT_PR * 4)
#define PKT_CRYPT_LEN 160

static uint32_t pkt_sum(const uint32_t *p)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < PKT_CRYPT_LEN / 4; i++)
        sum += p[E2000_PKT_PR + i];
    return sum;
}

/*
 * The game rang.  The start-up ring comes without a mailbox byte and is
 * answered 0x80.  Every packet then takes two rings: the first with the
 * packet in BAR2, the second to collect the reply; each is answered with the
 * next sequence number (1..15) | 0x40.
 */
static void igs027_doorbell(void)
{
    static uint32_t pkt[E2000_PKT_WORDS];
    uint8_t w = s.bar3_out[0] & 0xff;
    int port = w & 0xf0, seq = w & 15;
    uint32_t len, sum;

    if (!s.mbox_written) {
        s.bar3_in[0] = 0x80;
        s.pkt_phase = 0;
        VLOG("IGS027A start-up ring");
        return;
    }
    s.mbox_written = 0;
    if (!s.pkt_phase) {
        memcpy(pkt, s.bar2, sizeof(pkt));
        if (port != 0x50) {
            rc4_crypt(&s.from_game, (uint8_t *)pkt + PKT_CRYPT_OFF, PKT_CRYPT_LEN);
            if ((sum = pkt_sum(pkt)) != pkt[E2000_PKT_SUM])
                LOG("IGS027A packet checksum 0x%08x, expected 0x%08x (type %u)", sum,
                    pkt[E2000_PKT_SUM], pkt[E2000_PKT_PR]);
        }
        if (s.exchanges < 3)
            LOG("packet %u: port 0x%x, type %u.%u", s.exchanges, port, pkt[E2000_PKT_PR],
                pkt[E2000_PKT_PR + 1]);
        len = game->packet(port, pkt);
        if (len > (E2000_PKT_WORDS - E2000_PKT_DATA) * 4)
            len = (E2000_PKT_WORDS - E2000_PKT_DATA) * 4;
        pkt[0] = 0;                        /* no debug text */
        pkt[E2000_PKT_LEN] = len;
        pkt[E2000_PKT_SUM] = pkt_sum(pkt);
        rc4_crypt(&s.to_game, (uint8_t *)pkt + PKT_CRYPT_OFF, PKT_CRYPT_LEN);
        memcpy(s.bar2, pkt, len > 0x63 ? len + E2000_PKT_DATA * 4 : PKT_CRYPT_OFF + PKT_CRYPT_LEN);
        s.exchanges++;
    }
    s.pkt_phase ^= 1;
    s.bar3_in[0] = (seq % 15 + 1) | 0x40;
}

/* the game rang the doorbell: answer its message */
static void igs036_doorbell(void)
{
    uint32_t cmd = (s.bar3_out[0] & 0xff) | (s.bar3_out[1] & 0xff) << 8 |
                   (s.bar3_out[2] & 0xff) << 16 | (uint32_t)(s.bar3_out[3] & 0xff) << 24;
    static uint32_t req[E2000_MSG_MAX], reply[E2000_MSG_MAX];
    uint32_t n = 0, key, i;
    int k;

    memset(reply, 0, sizeof(reply));

    /* command 0 is the start-up ring: no message in BAR2 yet */
    if (cmd) {
        n = bar2_ld(0);
        if (n < 2 || n > E2000_MSG_MAX) {
            LOG("bad message length %u (cmd %u)", n, cmd);
            n = 0;
        } else {
            req[0] = n;
            req[1] = key = rc4_word(&s.from_game, bar2_ld(1));
            for (i = 2; i < n; i++)
                req[i] = bar2_ld(i) ^ key;
        }
    }
    reply[0] = game->exchange(n ? req : NULL, n, reply);
    if (reply[0] < 2 || reply[0] > E2000_MSG_MAX)
        reply[0] = 2;
    key = (uint32_t)rand() % 100000;
    bar2_st(0, reply[0]);
    bar2_st(1, rc4_word(&s.to_game, key));
    for (i = 2; i < reply[0]; i++)
        bar2_st(i, reply[i] ^ key);
    /* status: the game's command + 1, as bytes in the low halves */
    for (k = 0; k < 4; k++)
        s.bar3_in[k] = ((cmd + 1) >> (8 * k)) & 0xff;
    for (k = 4; k < 8; k++)
        s.bar3_in[k] = 0;
    if (core->verbose || s.exchanges < 3)
        LOG("exchange %u: cmd %u, %u words in, %u out", s.exchanges, cmd, n, reply[0]);
    s.exchanges++;
}

static uint32_t bar3_read(void *opaque, uint32_t off, int size)
{
    uint32_t v = 0;
    int b;

    (void)opaque;
    pthread_mutex_lock(&s.lock);
    for (b = 0; b < size; b += 2)
        if ((off + b) / 2 < 8)
            v |= (uint32_t)s.bar3_in[(off + b) / 2] << (8 * b);
    pthread_mutex_unlock(&s.lock);
    return size == 1 ? v & 0xff : v;
}

static void bar3_write(void *opaque, uint32_t off, int size, uint32_t v)
{
    int b;

    (void)opaque;
    pthread_mutex_lock(&s.lock);
    for (b = 0; b < size; b += 2) {
        uint32_t r = (off + b) / 2;

        if (r >= 8)
            continue;
        s.bar3_out[r] = v >> (8 * b);
        if (r == 0)
            s.mbox_written = 1;
        if (r * 2 == BAR3_DOORBELL && (s.bar3_out[r] & 1)) {
            if (game->transport == E2000_IGS027A)
                igs027_doorbell();
            else if (game->transport == E2000_IGS036_MSG && game->msg_layout == E2000_MSG_SD2)
                sd2_doorbell();
            else if (game->transport == E2000_IGS036_MSG)
                msg_doorbell();
            else if (game->transport == E2000_IGS036_PKT)
                pkt36_doorbell();
            else
                igs036_doorbell();
        }
    }
    pthread_mutex_unlock(&s.lock);
}

/* ---- PLX SDK -------------------------------------------------------------------- */

static void plx_location(uint8_t *loc)
{
    uint16_t dev = 0x9030, ven = 0x10B5;

    memset(loc, 0, 18);
    loc[0] = 1;                            /* bus */
    loc[1] = 9;                            /* slot */
    memcpy(loc + 2, &dev, 2);
    memcpy(loc + 4, &ven, 2);
    strcpy((char *)loc + 6, "Pci9030-0");
}

static uint32_t bar_size(int bar)
{
    switch (bar) {
    case 0: return BAR0_SIZE;
    case 2: return BAR2_SIZE;
    case 3: return BAR3_SIZE;
    default: return 0;
    }
}

static int plx_ioctl(unsigned long req, uint8_t *a)
{
    uint32_t v, status = PLX_OK;
    uint16_t bar;

    switch (req) {
    case PLX_FIND:
        memcpy(&v, a + 8, 4);
        if (v != 80001)
            plx_location(a + 12);          /* index 0 is the only card */
        v = 1;
        memcpy(a + 8, &v, 4);
        break;
    case PLX_LOCATION:
        plx_location(a + 12);
        break;
    case PLX_VERSION:
        a[8] = 0;
        a[9] = 3;
        a[10] = 4;                         /* SDK 4.3, as the games require */
        break;
    case PLX_BAR_SIZE:
        memcpy(&bar, a + 4, 2);
        s.pending_bar = bar;
        v = bar_size(bar);
        memcpy(a + 8, &v, 4);
        break;
    case PLX_BAR_PROPS:
        memcpy(&bar, a + 4, 2);
        s.pending_bar = bar;
        v = bar_size(bar) ? BAR_PHYS(bar) : 0;
        memcpy(a + 8, &v, 4);
        a[12] = 0;                         /* memory, not I/O */
        break;
    case PLX_RESET:
        /* PlxPciBoardReset: Panda Family resets the IGS036, then starts its RC4 streams over */
        if (game->transport == E2000_IGS036_PKT && s.bar2)
            igs036_reset();
        break;
    case PLX_BAR_MAP:
        break;
    default:
        LOG("PLX ioctl 0x%lx", req);
        break;
    }
    memcpy(a, &status, 4);
    return 0;
}

/* ---- /dev/roio ------------------------------------------------------------------ */

static int roio_byte(uint32_t addr)
{
    const struct e2000_bios *b;

    for (b = game->bios; b && b->text; b++)
        if (addr >= b->addr && addr < b->addr + strlen(b->text))
            return (uint8_t)b->text[addr - b->addr];
    return 0xff;
}

/* ---- /dev/tdcr ------------------------------------------------------------------ */

/*
 * The We Dance kernel's file cipher ("cpu/tdcr", major 214): ioctl 1 takes
 * {mode, key[256], key length at +0x104}, mode 1 only; each read() then
 * transforms the caller's buffer in place with one continuous stream.  It
 * is RC4 with a changed key schedule: S holds 32-bit words, and each swap
 * stores the old S[i] + 3 at S[j], with j += key + S[i] + 0x14.
 */
static struct { uint32_t s[256]; uint8_t i, j; int ready; } tdc;

static void tdc_key(const uint8_t *key, uint32_t n)
{
    uint32_t i, j = 0, k = 0;

    for (i = 0; i < 256; i++)
        tdc.s[i] = i;
    for (i = 0; i < 256 && n; i++) {
        uint32_t t = tdc.s[i] + 3;

        j = (j + key[k] + t + 0x11) & 0xff;
        k = k + 1 == n ? 0 : k + 1;
        tdc.s[i] = tdc.s[j];
        tdc.s[j] = t;
    }
    tdc.i = tdc.j = 0;
    tdc.ready = 1;
}

static void tdc_crypt(uint8_t *b, size_t n)
{
    size_t k;

    for (k = 0; k < n; k++) {
        uint32_t x, y;

        tdc.i++;
        x = tdc.s[tdc.i];
        tdc.j += x;
        y = tdc.s[tdc.j];
        tdc.s[tdc.i] = y;
        tdc.s[tdc.j] = x;
        b[k] ^= tdc.s[(x + y) & 0xff];
    }
}

/* ---- device files --------------------------------------------------------------- */

static int dev_kind(const char *path)
{
    if (!strcmp(path, "/dev/plx/Pci9030"))
        return DEV_PLX_DRIVER;
    if (!strcmp(path, "/dev/plx/Pci9030-0"))
        return DEV_PLX_CARD;
    if (!strcmp(path, "/dev/roio"))
        return DEV_ROIO;
    if (!strcmp(path, "/dev/pccard0"))
        return DEV_PCCARD;
    if (!strcmp(path, "/dev/tdcr"))
        return DEV_TDC;
    return DEV_NONE;
}

static int dev_handles(const char *path)
{
    /* every PLX node: the absent chips must fail to open */
    return !strncmp(path, "/dev/plx/", 9) || dev_kind(path) != DEV_NONE;
}

static int dev_open(const char *path)
{
    int kind = dev_kind(path), fd;

    if (kind == DEV_NONE) {
        errno = ENOENT;
        return -1;
    }
    pthread_mutex_lock(&s.lock);
    if (kind == DEV_PLX_CARD && !s.bar2) {
        s.bar0 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        s.bar2 = mmap(NULL, BAR2_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        s.bar3_map = core->mmio_map(BAR3_SIZE, NULL, bar3_read, bar3_write);
        igs036_reset();
        LOG("PCI9030 card emulated");
    }
    fd = core->placeholder_fd();
    if (fd >= 0 && s.nfds < 8) {
        s.fds[s.nfds] = fd;
        s.kinds[s.nfds++] = kind;
    }
    pthread_mutex_unlock(&s.lock);
    VLOG("open %s -> %d", path, fd);
    return fd;
}

static int dev_ioctl(int fd, unsigned long req, void *arg)
{
    int kind, ret = 0;

    pthread_mutex_lock(&s.lock);
    kind = fd_kind(fd);
    switch (kind) {
    case DEV_PLX_DRIVER:
    case DEV_PLX_CARD:
        ret = arg ? plx_ioctl(req, arg) : -1;
        break;
    case DEV_ROIO:
        if (req == ROIO_READ)
            ret = roio_byte((uint32_t)(uintptr_t)arg ^ ROIO_ADDR_XOR) ^ ROIO_DATA_XOR;
        break;
    case DEV_TDC: {                    /* 1: {mode, key[256], key length} */
        const uint8_t *a = arg;
        uint32_t mode, n;

        if (req != 1 || !a) {
            errno = EINVAL;
            ret = -1;
            break;
        }
        memcpy(&mode, a, 4);
        memcpy(&n, a + 0x104, 4);
        if (mode != 1 || n > 256) {
            errno = EINVAL;
            ret = -1;
            break;
        }
        tdc_key(a + 4, n);
        break;
    }
    default:
        break;   /* pccard0: decoys and an unused challenge */
    }
    pthread_mutex_unlock(&s.lock);
    return ret;
}

static void *dev_mmap(int fd, size_t len)
{
    void *p = MAP_FAILED;

    pthread_mutex_lock(&s.lock);
    if (fd_kind(fd) == DEV_PLX_CARD && len <= bar_size(s.pending_bar) + 0xfff) {
        if (s.pending_bar == 0)
            p = s.bar0;
        else if (s.pending_bar == 2)
            p = s.bar2;
        else if (s.pending_bar == 3)
            p = s.bar3_map;
    }
    pthread_mutex_unlock(&s.lock);
    LOG("map BAR%d (%zu bytes) -> %p", s.pending_bar, len, p);
    if (p == MAP_FAILED)
        errno = EINVAL;
    return p;
}

static void dev_close(int fd)
{
    (void)fd;  /* the card stays alive */
}

/* reads: /dev/tdcr transforms the buffer; the other devices read as /dev/null */
static long dev_read(int fd, void *buf, size_t n)
{
    long ret = 0;

    pthread_mutex_lock(&s.lock);
    if (fd_kind(fd) == DEV_TDC) {
        if (tdc.ready) {
            tdc_crypt(buf, n);
            ret = (long)n;
        } else {
            errno = EINVAL;
            ret = -1;
        }
    }
    pthread_mutex_unlock(&s.lock);
    return ret;
}

static long dev_write(int fd, const void *buf, size_t n)
{
    (void)fd;
    (void)buf;
    return (long)n;
}

static void machine_key(int sym, int down)
{
    if (!game->key)
        return;
    pthread_mutex_lock(&s.lock);
    game->key(sym, down);
    pthread_mutex_unlock(&s.lock);
}

static void machine_draw(int w, int h)
{
    if (!game->draw)
        return;
    pthread_mutex_lock(&s.lock);
    game->draw(w, h);
    pthread_mutex_unlock(&s.lock);
}

static void machine_file_opened(const char *path)
{
    if (!game->file_opened)
        return;
    pthread_mutex_lock(&s.lock);
    game->file_opened(path);
    pthread_mutex_unlock(&s.lock);
}

static void machine_status(char *buf, size_t n)
{
    if (!game->status)
        return;
    pthread_mutex_lock(&s.lock);
    game->status(buf, n);
    pthread_mutex_unlock(&s.lock);
}

static const char *machine_vfile(const char *name)
{
    const char *r;

    if (!game->vfile)
        return NULL;
    pthread_mutex_lock(&s.lock);
    r = game->vfile(name);
    pthread_mutex_unlock(&s.lock);
    return r;
}

/* ---- plugin entry --------------------------------------------------------------- */

static const struct igs_machine machine = {
    .version = IGS_API_VERSION,
    .name = "e2000",
    .handles = dev_handles,
    .open = dev_open,
    .ioctl = dev_ioctl,
    .mmap = dev_mmap,
    .close = dev_close,
    .read = dev_read,
    .write = dev_write,
    .key = machine_key,
    .draw = machine_draw,
    .file_opened = machine_file_opened,
    .status = machine_status,
    .vfile = machine_vfile,
};

static struct e2000_api api = {
    .version = E2000_API_VERSION,
    .rekey = api_rekey,
};

__attribute__((visibility("default")))
const struct igs_machine *igs_machine_entry(const struct igs_core *c)
{
    const char *path = getenv("IGS_GAME");
    e2000_game_entry_fn entry;
    void *h;

    core = c;
    api.core = c;
    h = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    entry = h ? (e2000_game_entry_fn)dlsym(h, E2000_GAME_ENTRY) : NULL;
    game = entry ? entry(&api) : NULL;
    if (!game || game->version != E2000_API_VERSION) {
        LOG("cannot load game plugin %s: %s", path ? path : "(IGS_GAME unset)",
            h ? "bad plugin" : dlerror());
        return NULL;
    }
    LOG("game plugin %s", game->name);
    return &machine;
}
