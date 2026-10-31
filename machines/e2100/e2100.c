/*
 * E2100 machine plugin: the FPGA card behind /dev/pccard0 and the protection
 * ASIC's transport, for every E2100 game.
 *
 *   - BAR0 (512 KiB SRAM / message buffer) is plain shared memory.
 *   - BAR1 (2 KiB header registers + mailbox) is an MMIO trap: the mailbox
 *     has separate read and write sides.
 *   - ioctls: reset/notify, the event counter, SRAM DMA, the asset cipher.
 *   - The ASIC boot handshake and the runtime transport: batch framing, the
 *     older register table (Scratch) or plain request/response (Hero of
 *     Robots), as the game plugin asks.  Everything above the transport
 *     (cipher, messages, game logic) is the game plugin's ($IGS_GAME, see
 *     e2100.h).
 *   - Serial ports the game plugin puts a peripheral on (read/write/termios).
 * Protocol: docs/IGS_E2100_PROTECTION.md.  This started as a port of the QEMU
 * model (hw/misc/igs_e2100_fpga.c on the igs branch).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "e2100.h"

#define BAR0_SIZE 0x80000
#define BAR1_SIZE 0x800

/* vendor driver ioctls (kernel FPGA_iIoctl) */
#define IOC_ASIC_RESET  0x6600
#define IOC_NOTIFY      0x6601
#define IOC_SEL_BAR     0x40046602u
#define IOC_GET_EVENTS  0x80046603u
#define IOC_CLR_EVENTS  0x40046604u
#define IOC_SRAM_READ   0x80106605u
#define IOC_SRAM_WRITE  0x40106606u
#define IOC_KEY         0x4024660Fu
#define IOC_CRYPT_A     0xC0106610u
#define IOC_CRYPT_B     0xC0106611u

#define MBOX_BASE  0x100
#define MBOX_WORDS 8

#define ASIC_CMD_HELLO    0xF0F0F0F0u
#define ASIC_CMD_DONE     0x90909090u
#define HOST_REPLY_RETRY  0x0A0A0A0Au
#define HOST_REPLY_DONE   0x0C0C0C0Cu
#define ASIC_MAX_WORDS    (BAR0_SIZE / 4)
#define ASIC_RUNTIME_PERIOD_MS 16
#define ASIC_REPLY_DELAY_MS    2

enum {
    ASIC_IDLE, ASIC_HELLO_SENT, ASIC_WAIT_KEYS, ASIC_HS_SENT,
    ASIC_DONE_SENT, ASIC_RUNTIME_WAIT, ASIC_RUNTIME,
};

/* DMA descriptor passed by the game: {user buffer, bar, length, SRAM offset} */
struct dma_desc { uint8_t *buf; uint32_t bar, len, sram; };
/* crypto ioctl: {encrypt flag, source desc, destination desc, 16-byte key} */
struct crypt_req { uint32_t encrypt; struct dma_desc *src, *dst; uint8_t *key; };

static const struct igs_core *core;
static const struct e2100_game *game;

#define LOG(...) core->log("e2100: " __VA_ARGS__)

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int fd;                       /* the game's /dev/pccard0 */
    int sel_bar;
    uint8_t *bar0;
    uint8_t *bar1_map;            /* trapped view handed to the game */
    uint8_t bar1[BAR1_SIZE];      /* header registers */
    uint32_t mbox_out[MBOX_WORDS], mbox_in[MBOX_WORDS];
    volatile int32_t events;      /* the driver's 0x40 event counter */

    /* pending ASIC action (the QEMU model's one-shot timer) */
    int timer_armed;
    struct timespec deadline;
    int pending_event, pending_mbox;
    uint32_t pending_in[2];

    uint32_t asic_state, session_key, running_sum;
    struct e2100_channel ch;

    /* register-table transport: BAR1 0x000..0x0FF read and write sides */
    int regtable;
    uint32_t reg_rx[E2100_REG_WORDS], reg_tx[E2100_REG_WORDS];

    /* request/response transport: the reply waiting for its event */
    int reqresp, rr_pending;
    uint32_t rr_len;
    double rr_time;
    uint8_t rr_reply[E2100_REQRESP_MAX];
} s = { .lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1 };

/* serial ports with a peripheral (the game plugin's e2100_serial table) */
#define SERIAL_MAX 8
#define SERIAL_BUF 4096
static struct serial {
    const struct e2100_serial *dev;
    int fd;
    uint32_t len;                 /* answer bytes not read yet */
    uint8_t buf[SERIAL_BUF];
} ports[SERIAL_MAX];
static int nports;

/* ---- FPGA asset cipher (docs/IGS_SOUND_CRYPTO.md) ------------------------- */

/* the mask follows the ASIC's runtime key: per game, $IGS_ASSET_MASK */
static uint8_t cipher_mask[16];
static const uint8_t cipher_perm[16] = {
    14, 10, 8, 1, 0, 12, 9, 4, 3, 6, 11, 7, 2, 13, 5, 15,
};
static const uint16_t cipher_flip = 0xb4d2;

static void cipher_mask_from_env(void)
{
    const char *hex = core->env("IGS_ASSET_MASK", "eea8543c6a16835d0900d7739f6bf77f");
    int i;

    for (i = 0; i < 16; i++) {
        unsigned v;

        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            LOG("bad IGS_ASSET_MASK %s", hex);
            return;
        }
        cipher_mask[i] = v;
    }
}

static uint64_t ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void st64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint64_t rol64(uint64_t v, int n) { return (v << n) | (v >> (64 - n)); }
static uint64_t ror64(uint64_t v, int n) { return (v >> n) | (v << (64 - n)); }

static void cipher_decrypt_block(uint8_t *blk)
{
    uint8_t d[16], u[16];
    int i;

    for (i = 0; i < 16; i++)
        d[i] = blk[i] ^ cipher_mask[i];
    st64(u, ror64(ld64(d), 1));
    st64(u + 8, ror64(ld64(d + 8), 3));
    for (i = 0; i < 16; i++)
        if (cipher_flip & (1u << i))
            u[i] ^= 0x80;
    for (i = 0; i < 16; i++)
        blk[i] = u[cipher_perm[i]];
}

static void cipher_encrypt_block(uint8_t *blk)
{
    uint8_t u[16];
    int i;

    for (i = 0; i < 16; i++)
        u[cipher_perm[i]] = blk[i];
    for (i = 0; i < 16; i++)
        if (cipher_flip & (1u << i))
            u[i] ^= 0x80;
    st64(u, rol64(ld64(u), 1));
    st64(u + 8, rol64(ld64(u + 8), 3));
    for (i = 0; i < 16; i++)
        blk[i] = u[i] ^ cipher_mask[i];
}

/* ---- runtime channel framing ------------------------------------------------ */

static uint32_t bar0_ld(uint32_t w) { uint32_t v; memcpy(&v, s.bar0 + 4 * w, 4); return v; }
static void bar0_st(uint32_t w, uint32_t v) { memcpy(s.bar0 + 4 * w, &v, 4); }
static uint32_t bar1_reg(uint32_t i) { uint32_t v; memcpy(&v, s.bar1 + 4 * (i & 0x3f), 4); return v; }
static void bar1_set_reg(uint32_t i, uint32_t v) { memcpy(s.bar1 + 4 * (i & 0x3f), &v, 4); }

static void batch_add(struct e2100_batch *b, uint32_t type, const uint32_t *w, uint32_t len)
{
    if (b->nhdrs >= 16 || b->npayload + len > E2100_BATCH_MAX)
        return;
    memcpy(b->payload + b->npayload, w, len * 4);
    b->npayload += len;
    b->hdrs[b->nhdrs++] = (len << 7) | type;
}

/* game -> ASIC: header (payload << 7 | n) in BAR1[tx_idx], body in BAR0 */
static void receive_batch(void)
{
    static uint32_t words[ASIC_MAX_WORDS];
    uint32_t hdr = bar1_reg(s.ch.tx_idx);
    uint32_t nmsg = hdr & 0x7f, payload = hdr >> 7, total = nmsg + payload + 1;
    uint32_t sum = 0, pos = 0, i;

    if (nmsg == 0 && payload == 0)
        return;
    if (total > ASIC_MAX_WORDS) {
        LOG("rx batch too large hdr=0x%08x", hdr);
        return;
    }
    for (i = 0; i < total; i++)
        words[i] = bar0_ld(i);
    game->decrypt_from_game(words, total);
    for (i = 1; i < total; i++)
        sum += words[i];
    if (words[0] != s.running_sum + sum)
        LOG("rx checksum mismatch: got 0x%08x want 0x%08x", words[0], s.running_sum + sum);
    s.running_sum = words[0];
    for (i = 0; i < nmsg; i++) {
        uint32_t mh = words[1 + payload + i], len = mh >> 7;

        if (pos + len > payload) {
            LOG("rx message %u overruns payload", i);
            break;
        }
        game->message(&s.ch, mh & 0x7f, len, &words[1 + pos]);
        pos += len;
    }
    if (game->batch_done)
        game->batch_done(&s.ch);
}

/* ASIC -> game: header in BAR1[rx_idx], [checksum, payload, headers] in BAR0 */
static void send_batch(const struct e2100_batch *b)
{
    static uint32_t w[1 + E2100_BATCH_MAX + 16];
    uint32_t n = 1 + b->npayload + b->nhdrs, sum = 0, i;

    bar1_set_reg(s.ch.rx_idx, (b->npayload << 7) | b->nhdrs);
    if (b->npayload == 0 && b->nhdrs == 0)
        return;
    memcpy(w + 1, b->payload, b->npayload * 4);
    memcpy(w + 1 + b->npayload, b->hdrs, b->nhdrs * 4);
    for (i = 1; i < n; i++)
        sum += w[i];
    s.running_sum += sum;
    w[0] = s.running_sum;
    game->encrypt_to_game(w, n);
    for (i = 0; i < n; i++)
        bar0_st(i, w[i]);
}

/* ---- register-table transport ------------------------------------------------ */

#define REG_BLOCKS 0x40000         /* reply blocks: clear of the game's, from 0 */

static void reg_put(struct e2100_regs *r, uint32_t word, uint32_t value)
{
    if (word >= 1 && word <= 25)
        r->w[0] |= 1u << word;
    else if (word >= 33 && word <= 52)
        r->w[32] |= 1u << (word - 32);
    else
        return;
    r->w[word] = value;
}

static void reg_block(struct e2100_regs *r, uint32_t word, const void *data, uint32_t len)
{
    uint32_t off = (r->block_top + 3) & ~3u;

    if (off + len > BAR0_SIZE) {
        LOG("reply block %u does not fit", word);
        return;
    }
    memcpy(s.bar0 + off, data, len);
    r->block_top = off + len;
    if (word <= 30)
        r->w[31] |= 1u << word;
    reg_put(r, word, off);
}

static uint32_t reg_sum(const uint32_t *w, uint32_t from, uint32_t to)
{
    uint32_t sum = 0, i;

    for (i = from; i <= to; i++)
        sum += w[i];
    return sum & 0xff;
}

/* game -> ASIC: the table the game wrote, then rang */
static void reg_receive(void)
{
    uint32_t w[E2100_REG_WORDS], i;

    memcpy(w, s.reg_tx, sizeof(w));
    if (w[26] != reg_sum(w, 1, 25) || w[53] != reg_sum(w, 33, 52))
        LOG("rx table checksum mismatch: %02x/%02x want %02x/%02x",
            w[26], w[53], reg_sum(w, 1, 25), reg_sum(w, 33, 52));
    for (i = 1; i <= 52; i++) {
        uint32_t mask = i < 32 ? w[0] >> i : w[32] >> (i - 32);
        int block = i <= 30 && (w[31] >> i & 1) && w[i] < BAR0_SIZE;

        if (i == 26 || i == 31 || i == 32 || !(mask & 1))
            continue;
        game->reg_cmd(i, w[i], block ? s.bar0 + w[i] : NULL, block ? BAR0_SIZE - w[i] : 0);
    }
}

/* ASIC -> game: a fresh table with the sums */
static void reg_send(void)
{
    static struct e2100_regs r;

    memset(&r, 0, sizeof(r));
    r.block_top = REG_BLOCKS;
    game->reg_build(&r);
    r.w[26] = reg_sum(r.w, 1, 25);
    r.w[53] = reg_sum(r.w, 33, 52);
    memcpy(s.reg_rx, r.w, sizeof(r.w));
}

/* ---- request/response transport ------------------------------------------------ */

/* game -> ASIC: byte length in BAR1 word 0, the bytes from BAR0 0 */
static void rr_receive(void)
{
    uint32_t len = bar1_reg(0);

    if (len > BAR0_SIZE) {
        LOG("rx message too large (%u bytes)", len);
        len = 0;
    }
    s.rr_time = core->now();
    s.rr_len = game->exchange(s.bar0, len, s.rr_reply, E2100_REQRESP_MAX);
    if (s.rr_len > E2100_REQRESP_MAX)
        s.rr_len = 0;
    s.rr_pending = 1;
}

/* ASIC -> game: the same layout, at the event */
static void rr_send(void)
{
    double late;

    if (!s.rr_pending)
        return;
    s.rr_pending = 0;
    /* the games hang themselves when a reply takes seconds (Hero: 3 s) */
    late = core->now() - s.rr_time;
    if (late > 0.5)
        LOG("reply %.0f ms after the request", late * 1000);
    bar1_set_reg(0, s.rr_len);
    memcpy(s.bar0, s.rr_reply, s.rr_len);
}

/* ---- ASIC handshake and timer ------------------------------------------------ */

/* one pending action, like timer_mod(): re-posting replaces it */
static void asic_post(int event, int mbox, uint32_t w0, uint32_t w1, int delay_ms)
{
    clock_gettime(CLOCK_MONOTONIC, &s.deadline);
    s.deadline.tv_nsec += (long)delay_ms * 1000000;
    while (s.deadline.tv_nsec >= 1000000000) {
        s.deadline.tv_nsec -= 1000000000;
        s.deadline.tv_sec++;
    }
    s.pending_event = event;
    s.pending_mbox = mbox;
    s.pending_in[0] = w0;
    s.pending_in[1] = w1;
    s.timer_armed = 1;
    pthread_cond_signal(&s.cond);
}

static void asic_fire(void)
{
    if (s.asic_state == ASIC_RUNTIME && s.regtable) {
        reg_send();
    } else if (s.asic_state == ASIC_RUNTIME && s.reqresp) {
        rr_send();
    } else if (s.asic_state == ASIC_RUNTIME) {
        static struct e2100_batch b;

        b.npayload = b.nhdrs = 0;
        game->build(&s.ch, &b);
        send_batch(&b);
    }
    if (s.pending_mbox) {
        s.mbox_in[0] = s.pending_in[0];
        s.mbox_in[1] = s.pending_in[1];
    }
    /* exactly one event per host action: the game dispatches on count == 1 */
    if (s.pending_event)
        __sync_fetch_and_add(&s.events, 1);
}

static void asic_reset(void)
{
    LOG("ASIC reset");
    s.timer_armed = 0;
    s.asic_state = ASIC_HELLO_SENT;
    s.session_key = 0;
    s.running_sum = 0;
    s.rr_pending = 0;
    s.ch.rx_idx = s.ch.tx_idx = 0;
    game->reset(&s.ch);
    /* ASIC boot hello; the game echoes it without checking the content */
    asic_post(1, 1, 0x1ec2e100, 0, 50);
}

static void asic_notify(void)
{
    uint32_t k = s.session_key, cmd = s.mbox_out[0];

    switch (s.asic_state) {
    case ASIC_IDLE:
        break;
    case ASIC_HELLO_SENT:
        s.asic_state = ASIC_WAIT_KEYS;
        asic_post(1, 0, 0, 0, ASIC_REPLY_DELAY_MS);
        break;
    case ASIC_WAIT_KEYS:
        /* the host wrote (r ^ 0x08080808, r) and four session keys at 0x108 */
        s.session_key = k = s.mbox_out[2];
        s.asic_state = ASIC_HS_SENT;
        asic_post(1, 1, ASIC_CMD_HELLO ^ k, (uint32_t)rand() ^ k, ASIC_REPLY_DELAY_MS);
        break;
    case ASIC_HS_SENT:
        if ((cmd ^ k) != HOST_REPLY_RETRY)
            LOG("unexpected handshake reply 0x%08x", cmd ^ k);
        /* skip the firmware upload: report done straight away */
        s.asic_state = ASIC_DONE_SENT;
        asic_post(1, 1, ASIC_CMD_DONE ^ k, (uint32_t)rand() ^ k, ASIC_REPLY_DELAY_MS);
        break;
    case ASIC_DONE_SENT:
        if ((cmd ^ k) != HOST_REPLY_DONE)
            LOG("unexpected done reply 0x%08x", cmd ^ k);
        LOG("handshake complete");
        s.asic_state = ASIC_RUNTIME_WAIT;
        asic_post(1, 0, 0, 0, ASIC_REPLY_DELAY_MS);
        break;
    case ASIC_RUNTIME_WAIT:
        s.asic_state = ASIC_RUNTIME;
        LOG("runtime channel up (%s)", game->name);
        /* fall through: the game's first runtime batch */
    case ASIC_RUNTIME:
        if (s.reqresp) {
            /* one reply per message, never unsolicited */
            rr_receive();
            asic_post(1, 0, 0, 0, ASIC_REPLY_DELAY_MS);
            break;
        }
        if (s.regtable)
            reg_receive();
        else
            receive_batch();
        asic_post(1, 0, 0, 0, ASIC_RUNTIME_PERIOD_MS);
        break;
    }
}

static void *asic_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s.lock);
    for (;;) {
        while (!s.timer_armed)
            pthread_cond_wait(&s.cond, &s.lock);
        if (pthread_cond_timedwait(&s.cond, &s.lock, &s.deadline) == ETIMEDOUT &&
            s.timer_armed) {
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > s.deadline.tv_sec ||
                (now.tv_sec == s.deadline.tv_sec && now.tv_nsec >= s.deadline.tv_nsec)) {
                s.timer_armed = 0;
                asic_fire();
            }
        }
    }
    return NULL;
}

/* ---- BAR1: header registers + mailbox (separate read and write sides) ------ */

static int is_mbox(uint32_t off, int size)
{
    return size == 4 && !(off & 3) && off >= MBOX_BASE && off < MBOX_BASE + 4 * MBOX_WORDS;
}

static uint32_t bar1_read(void *opaque, uint32_t off, int size)
{
    uint32_t v = 0;

    (void)opaque;
    pthread_mutex_lock(&s.lock);
    if (s.regtable && size == 4 && !(off & 3) && off < 4 * E2100_REG_WORDS)
        v = s.reg_rx[off / 4];
    else if (is_mbox(off, size))
        v = s.mbox_in[(off - MBOX_BASE) / 4];
    else if (off + size <= BAR1_SIZE)
        memcpy(&v, s.bar1 + off, size);
    pthread_mutex_unlock(&s.lock);
    return v;
}

static void bar1_write(void *opaque, uint32_t off, int size, uint32_t v)
{
    (void)opaque;
    pthread_mutex_lock(&s.lock);
    if (s.regtable && size == 4 && !(off & 3) && off < 4 * E2100_REG_WORDS)
        s.reg_tx[off / 4] = v;
    else if (is_mbox(off, size))
        s.mbox_out[(off - MBOX_BASE) / 4] = v;
    else if (off + size <= BAR1_SIZE)
        memcpy(s.bar1 + off, &v, size);
    pthread_mutex_unlock(&s.lock);
}

/* ---- serial ports ---------------------------------------------------------------- */

static const struct e2100_serial *serial_dev(const char *path)
{
    const struct e2100_serial *d;

    for (d = game->serial; d && d->path; d++)
        if (!strcmp(d->path, path))
            return d;
    return NULL;
}

static struct serial *serial_port(int fd)
{
    int i;

    for (i = 0; fd >= 0 && i < nports; i++)
        if (ports[i].fd == fd)
            return &ports[i];
    return NULL;
}

static int serial_open(const struct e2100_serial *d)
{
    int i;

    for (i = 0; i < nports; i++)
        if (ports[i].dev == d) {
            ports[i].len = 0;
            return ports[i].fd;  /* reopened: the same port */
        }
    if (nports == SERIAL_MAX)
        return -1;
    ports[nports].dev = d;
    ports[nports].fd = core->placeholder_fd();
    ports[nports].len = 0;
    LOG("%s: serial port (fd %d)", d->path, ports[nports].fd);
    return ports[nports++].fd;
}

/* termios: accepted and ignored; TCFLSH drops the pending answer */
static int serial_ioctl(struct serial *p, unsigned long req, void *arg)
{
    if (req == 0x540B)                       /* TCFLSH */
        p->len = 0;
    else if (req == 0x5401 && arg)           /* TCGETS: a raw port */
        memset(arg, 0, 36);
    return 0;
}

static long dev_read(int fd, void *buf, size_t n)
{
    struct serial *p;
    long r;

    pthread_mutex_lock(&s.lock);
    p = serial_port(fd);
    if (!p || !p->len) {
        pthread_mutex_unlock(&s.lock);
        errno = EAGAIN;
        return -1;
    }
    r = n < p->len ? (long)n : (long)p->len;
    memcpy(buf, p->buf, r);
    memmove(p->buf, p->buf + r, p->len - r);
    p->len -= r;
    pthread_mutex_unlock(&s.lock);
    return r;
}

static long dev_write(int fd, const void *buf, size_t n)
{
    struct serial *p;

    pthread_mutex_lock(&s.lock);
    p = serial_port(fd);
    if (p)
        p->len += p->dev->write(buf, n, p->buf + p->len, SERIAL_BUF - p->len);
    pthread_mutex_unlock(&s.lock);
    return n;
}

/* ---- the device file ----------------------------------------------------------- */

static int dev_handles(const char *path)
{
    return !strcmp(path, "/dev/pccard0") || serial_dev(path);
}

static int dev_open(const char *path)
{
    const struct e2100_serial *d = serial_dev(path);
    pthread_condattr_t ca;
    pthread_t t;
    int fd;

    if (d) {
        pthread_mutex_lock(&s.lock);
        fd = serial_open(d);
        pthread_mutex_unlock(&s.lock);
        return fd;
    }
    if (s.fd >= 0)
        return s.fd;
    s.fd = core->placeholder_fd();
    cipher_mask_from_env();
    s.bar0 = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    s.bar1_map = core->mmio_map(BAR1_SIZE, NULL, bar1_read, bar1_write);
    /* deadlines are CLOCK_MONOTONIC */
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&s.cond, &ca);
    pthread_create(&t, NULL, asic_thread, NULL);
    /* power-on event; the game's polling thread clears the counter first */
    s.events = 1;
    LOG("pccard0 emulated (fd %d)", s.fd);
    return s.fd;
}

static int dma_check(const struct dma_desc *d)
{
    return d && d->buf && d->sram < BAR0_SIZE && d->len <= BAR0_SIZE - d->sram;
}

static int dev_ioctl(int fd, unsigned long req, void *arg)
{
    struct serial *p;
    int ret = 0;

    pthread_mutex_lock(&s.lock);
    if ((p = serial_port(fd))) {
        ret = serial_ioctl(p, req, arg);
        pthread_mutex_unlock(&s.lock);
        return ret;
    }
    switch (req) {
    case IOC_ASIC_RESET:
        asic_reset();
        break;
    case IOC_NOTIFY:
        asic_notify();
        break;
    case IOC_SEL_BAR:
        s.sel_bar = *(int *)arg;
        break;
    case IOC_GET_EVENTS:
        *(int32_t *)arg = s.events;
        break;
    case IOC_CLR_EVENTS:
        s.events = 0;
        break;
    case IOC_SRAM_WRITE: {
        struct dma_desc *d = arg;

        if (!dma_check(d)) { ret = -1; break; }
        memcpy(s.bar0 + d->sram, d->buf, d->len);
        break;
    }
    case IOC_SRAM_READ: {
        struct dma_desc *d = arg;

        if (!dma_check(d)) { ret = -1; break; }
        memcpy(d->buf, s.bar0 + d->sram, d->len);
        break;
    }
    case IOC_CRYPT_A:
    case IOC_CRYPT_B: {
        /* DMA to SRAM with the cipher applied, then DMA back */
        struct crypt_req *c = arg;
        uint32_t i;

        if (!c || !dma_check(c->src) || !dma_check(c->dst)) { ret = -1; break; }
        memcpy(s.bar0 + c->src->sram, c->src->buf, c->src->len);
        for (i = 0; i + 16 <= c->src->len; i += 16) {
            if (c->encrypt)
                cipher_encrypt_block(s.bar0 + c->src->sram + i);
            else
                cipher_decrypt_block(s.bar0 + c->src->sram + i);
        }
        memcpy(c->dst->buf, s.bar0 + c->dst->sram, c->dst->len);
        break;
    }
    case IOC_KEY:
    case 0x6607: case 0x6608: case 0x6609: case 0x660A:
    case 0x660B: case 0x660C: case 0x660D: case 0x660E:
        break;  /* cipher key/mode writes: the mask comes from the manifest */
    default:
        LOG("unknown ioctl 0x%lx", req);
        ret = -1;
        errno = EINVAL;
    }
    pthread_mutex_unlock(&s.lock);
    return ret;
}

static void *dev_mmap(int fd, size_t len)
{
    (void)fd;
    LOG("map BAR%d (%zu bytes)", s.sel_bar, len);
    if (s.sel_bar == 0 && len <= BAR0_SIZE)
        return s.bar0;
    if (s.sel_bar == 1 && len <= BAR1_SIZE)
        return s.bar1_map;
    errno = EINVAL;
    return MAP_FAILED;
}

static void dev_close(int fd)
{
    (void)fd;  /* the card and the ports stay alive */
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

static void machine_status(char *buf, size_t n)
{
    if (!game->status)
        return;
    pthread_mutex_lock(&s.lock);
    game->status(buf, n);
    pthread_mutex_unlock(&s.lock);
}

/* ---- plugin entry --------------------------------------------------------------- */

static const struct igs_machine machine = {
    .version = IGS_API_VERSION,
    .name = "e2100",
    .handles = dev_handles,
    .open = dev_open,
    .ioctl = dev_ioctl,
    .mmap = dev_mmap,
    .close = dev_close,
    .key = machine_key,
    .draw = machine_draw,
    .read = dev_read,
    .write = dev_write,
    .status = machine_status,
};

static struct e2100_api api = {
    .version = E2100_API_VERSION,
    .batch_add = batch_add,
    .reg_put = reg_put,
    .reg_block = reg_block,
};

__attribute__((visibility("default")))
const struct igs_machine *igs_machine_entry(const struct igs_core *c)
{
    const char *path = getenv("IGS_GAME");
    e2100_game_entry_fn entry;
    void *h;

    core = c;
    api.core = c;
    h = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    entry = h ? (e2100_game_entry_fn)dlsym(h, E2100_GAME_ENTRY) : NULL;
    game = entry ? entry(&api) : NULL;
    if (!game || game->version != E2100_API_VERSION) {
        LOG("cannot load game plugin %s: %s", path ? path : "(IGS_GAME unset)",
            h ? "bad plugin" : dlerror());
        return NULL;
    }
    s.regtable = game->transport == E2100_TRANSPORT_REGTABLE;
    s.reqresp = game->transport == E2100_TRANSPORT_REQRESP;
    LOG("game plugin %s%s", game->name, s.regtable ? " (register-table transport)" :
        s.reqresp ? " (request/response transport)" : "");
    return &machine;
}
