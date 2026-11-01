/*
 * Hero of Robots m100fa (E2100) game plugin: its IGS036 firmware's side of
 * the request/response transport, and the two serial peripherals.
 * docs/HeroOfRobots.md.
 *
 *   - Channel: modified RC4 over each whole message, both directions, one
 *     S-box carried over (i and j restart at 0 per message).  The game's
 *     first sub-message 0 (start-up: KSA "LF KO IGSA36" first) or 5 (once
 *     per attract cycle) re-keys with its payload after the game message.
 *   - Framing: [r][w][w bytes of nibble table] then [type][len][data]...,
 *     then a dword whose low byte is the running byte sum of everything
 *     before it, carried across both directions.  Our replies use an
 *     all-zero 16-byte header (the game then starts at byte 16).
 *   - Messages in: 0 key, 1 state table, 2 state status, 3 next state,
 *     4 IO-board output, 5 key, 6 RSA key request, 7-13/16-18 settings,
 *     bookkeeping, version, QR rank (word 0: 0 read, 1 save + data).
 *   - Messages out: 0 asset key, 2 state pointer, 4 inputs, 6 RSA key
 *     parts, 7-13/16-18 the stored blocks.
 *   - The state machine runs on the pointer we echo: table[current], with
 *     current = 26 (copyright) at boot and every state's End reporting the
 *     next one (message 3).
 *   - QR cards: the game forwards the reader's text in message 14 and the
 *     ASIC answers with the card's number (message 14, a dword; the game
 *     prints it as 9 digits).  With HERO_NFC (nfc.h), a loader tag's card
 *     code is "scanned" as that text, and a dispensed card is written to a
 *     blank tag.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"
#include "card.h"
#include "nfc.h"

static const struct e2100_api *api;
#define LOG(...) api->core->log("hero: " __VA_ARGS__)
#define VLOG(...) do { if (api->core->verbose) LOG(__VA_ARGS__); } while (0)

/* input bits (keys = r1 | r2 << 4 | r3 << 8 of message 4) */
enum {
    IN_P1_X = 0x1, IN_P1_Z = 0x2, IN_P1_C = 0x4,
    IN_P2_M = 0x10, IN_P2_N = 0x20, IN_P2_B = 0x40,
    IN_DOWN = 0x200, IN_UP = 0x400, IN_TEST = 0x800, IN_SERVICE = 0x1000,
    IN_COIN = 0x2000,
};

static struct {
    uint8_t sbox[256];
    uint8_t sum;                      /* running byte checksum, both ways */
    uint32_t table[32];               /* message 1: state objects by id */
    int have_table, current;
    int booted;                       /* message 0 seen: the asset key goes out */
    uint32_t want;                    /* bitmask of message types to answer */
    uint32_t want_rsa;                /* RSA key parts asked for */
    uint32_t keys;                    /* held inputs */
    int coins, coin_sent;             /* coin presses still to deliver */
    int help;
    uint8_t region;
    double t0;                        /* ASIC reset, for the log */
    char qr_code[13];                 /* the NFC card code the QR port last sent */
    unsigned qr_serial;               /* ...from this tag (once per placing) */
    uint32_t qr_value;                /* message 14's answer */
} g;

/* ---- channel cipher (Encrypt::RC4_CypherKey / RC4_CypherData) ---------------- */

static void rc4_key(const uint8_t *k, uint32_t n)
{
    uint32_t i, j = 0;
    uint8_t t;

    if (n - 1 > 255)
        return;
    for (i = 0; i < 256; i++)
        g.sbox[i] = i;
    for (i = 0; i < 256; i++) {
        j = (j + k[i % n] + g.sbox[i]) & 255;
        t = g.sbox[j];
        g.sbox[j] = g.sbox[i];
        g.sbox[i] = t;
    }
}

static void rc4_crypt(uint8_t *p, uint32_t n)
{
    uint32_t i = 0, j = 0, k;
    uint8_t t;

    for (k = 0; k < n; k++) {
        i = (i + 1) & 255;
        j = (j + g.sbox[i]) & 255;
        t = g.sbox[i];
        g.sbox[i] = g.sbox[j];
        g.sbox[j] = t;
        p[k] ^= g.sbox[(g.sbox[i] + t) & 255];
    }
}

static const uint8_t boot_key[12] = "LF KO IGSA36";

/* the trailer's low byte: running sum + every byte before it */
static int sum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t sum = g.sum;
    uint32_t i;

    if (len < 4)
        return 0;
    for (i = 0; i < len - 4; i++)
        sum += p[i];
    return sum == p[len - 4];
}

/* ---- the ASIC's stored blocks (settings, bookkeeping, version, rank) --------- */

/*
 * Reply = the saved data (save_len bytes, the save message after its word
 * 0 = 1) followed by whatever the game expects after it (defaults).
 * Settings start valid with key word 0 (plaintext): see each Recv check.
 */
static struct block {
    uint32_t type, len, save_len;
    uint8_t data[1612];
} blocks[] = {
    /* GameSetting: 12 bytes + key, 12 defaults + 4.  b0 2..4, b1 1..5,
       b3 coin value, b4 coins per credit, b5 credits per game, b7 free play */
    { 7, 32, 16, { 2, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                   2, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0 } },
    /* SoundSetting: 8 bytes + key, 12 defaults (b0/b4 < 24, b1/b5 <= 50,
       the rest <= 32) */
    { 8, 24, 12, { 0, 30, 16, 16, 0, 30, 16, 16, 0, 0, 0, 0,
                   0, 30, 16, 16, 0, 30, 16, 16 } },
    /* ContestSet: c0..c5 + key, 8 defaults (c3 1..5) */
    { 9, 16, 8, { 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1 } },
    /* Password: 4 digits + key, 16 more */
    { 10, 24, 8, { 1, 2, 3, 4 } },
    { 11, 1612, 1612, { 0 } },        /* bookkeeping */
    { 12, 24, 24, { 0 } },            /* key-in/out counts */
    { 13, 4, 4, { 0 } },              /* cards bought */
    { 17, 32, 32, { 0 } },            /* QR rank string (invalid: the game resets it) */
    { 18, 28, 28, { 0 } },            /* GameLock (zero passes its checksum) */
};

static struct block *block(uint32_t type)
{
    unsigned i;

    for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
        if (blocks[i].type == type)
            return &blocks[i];
    return NULL;
}

/* ---- messages ----------------------------------------------------------------- */

static uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void message(uint32_t type, const uint8_t *d, uint32_t n)
{
    struct block *b;
    uint32_t w0 = n >= 4 ? ld32(d) : 0;

    switch (type) {
    case 0:                              /* boot key: answer with the asset key */
        g.booted = 1;
        break;
    case 1:                              /* state table */
        if (n >= sizeof(g.table)) {
            memcpy(g.table, d, sizeof(g.table));
            g.have_table = 1;
            LOG("state table received, state %d", g.current);
        }
        break;
    case 2:                              /* state status: 1 staying, 2 done */
    case 4:                              /* IO-board output: lamps, meters */
    case 5:                              /* new key (handled by the caller) */
        break;
    case 3:                              /* next state */
        if (w0 < 32) {
            LOG("%.1f s: state %d -> %u", api->core->now() - g.t0, g.current, w0);
            g.current = w0;
        }
        break;
    case 6:                              /* RSA key part w0 */
        g.want_rsa |= 1u << (w0 & 3);
        break;
    case 14:                             /* the QR reader's text: decode it */
        /* only the NFC codes we sent; others (the reader's "**TIMEOUT**") go unanswered */
        if (g.qr_code[0] && n > strlen(g.qr_code) && !memcmp(d, g.qr_code, strlen(g.qr_code) + 1)) {
            g.qr_value = strtoul(g.qr_code, NULL, 10);
            g.qr_code[0] = 0;
            g.want |= 1u << 14;
            LOG("card %09u scanned", g.qr_value);
        }
        break;
    case 16:                             /* version */
        g.want |= 1u << 16;
        break;
    default:
        if (!(b = block(type)))
            break;
        if (w0 == 0) {
            g.want |= 1u << type;
        } else if (n > 4) {
            n -= 4;
            memcpy(b->data, d + 4, n < b->save_len ? n : b->save_len);
            LOG("block %u saved (%u bytes)", type, n);
        }
        break;
    }
}

static uint32_t put(uint8_t *r, uint32_t pos, uint32_t max, uint32_t type,
                    const void *data, uint32_t n)
{
    uint32_t w[2] = { type, n };

    if (pos + 8 + n + 4 > max)
        return pos;
    memcpy(r + pos, w, 8);
    memcpy(r + pos + 8, data, n);
    return pos + 8 + n;                  /* n is a multiple of 4 */
}

/* message 4: 12 bytes, r[11] = sum r[0..10], keys in the nibbles of r1..r3 */
static uint32_t put_inputs(uint8_t *r, uint32_t pos, uint32_t max)
{
    uint32_t keys = g.keys;
    uint8_t io[12] = { 0x29 };
    int i;

    /* the game counts a coin on every message with the bit: one-message pulses */
    if (g.coin_sent) {
        g.coin_sent = 0;
    } else if (g.coins) {
        g.coins--;
        g.coin_sent = 1;
        keys |= IN_COIN;
    }
    io[1] = keys & 0xf;
    io[2] = (keys >> 4) & 0xf;
    io[3] = keys >> 8;
    for (i = 0; i < 11; i++)
        io[11] += io[i];
    return put(r, pos, max, 4, io, sizeof(io));
}

static uint32_t put_rsa(uint8_t *r, uint32_t pos, uint32_t max, uint32_t part)
{
    static const char *const hex[4] = { CARD_KEY_P, CARD_KEY_Q, CARD_KEY_PINV, CARD_KEY_D };
    uint8_t buf[4 + 512];
    uint32_t n = part == 3 ? 512 : 256;

    memcpy(buf, &part, 4);
    memcpy(buf + 4, hex[part], n);
    return put(r, pos, max, 6, buf, 4 + n);
}

static uint32_t build(uint8_t *r, uint32_t max)
{
    uint32_t pos = 16, i;
    uint8_t sum;

    memset(r, 0, 16);
    if (g.booted) {
        /*
         * Flag 1: the ASIC's NVRAM is blank, settings start from its
         * defaults (read once, at the first Game::Process).  The 32-byte AES
         * key is not used (the FPGA cipher mask is known).  A non-zero flag
         * also clears the game's reply watchdog (Game+0xA210: more than 3 s
         * between request and reply and the game spins forever), so the
         * message goes with every reply: a desktop can stall that long.
         */
        uint32_t key[9] = { 1, 0x4f524548, 0x4f52464f, 0x544f42 };

        pos = put(r, pos, max, 0, key, sizeof(key));
    }
    if (g.have_table)
        pos = put(r, pos, max, 2, &g.table[g.current], 4);
    pos = put_inputs(r, pos, max);
    for (i = 0; i < 4; i++)
        if (g.want_rsa & 1u << i)
            pos = put_rsa(r, pos, max, i);
    if (g.want & 1u << 16) {
        /* words 0-1 the ASIC version, word 2 low byte the region */
        uint32_t v[13] = { 0x100, 0x100, g.region };

        pos = put(r, pos, max, 16, v, sizeof(v));
    }
    if (g.want & 1u << 14)
        pos = put(r, pos, max, 14, &g.qr_value, 4);
    for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
        if (g.want & 1u << blocks[i].type)
            pos = put(r, pos, max, blocks[i].type, blocks[i].data, blocks[i].len);
    g.want = g.want_rsa = 0;

    sum = g.sum;
    for (i = 0; i < pos; i++)
        sum += r[i];
    g.sum = sum;
    memset(r + pos, 0, 4);
    r[pos] = sum;
    pos += 4;
    rc4_crypt(r, pos);
    return pos;
}

/* one game message in, one reply out */
static uint32_t exchange(const uint8_t *req, uint32_t len, uint8_t *reply, uint32_t max)
{
    static uint8_t p[E2100_REQRESP_MAX], s0[256];
    static double last;
    const uint8_t *newkey = NULL;
    uint32_t off, newlen = 0, first = 1;
    double now = api->core->now();

    /* the game exchanges once per frame: a long gap is a stall on its side */
    if (last && now - last > 2)
        LOG("no message for %.1f s", now - last);
    last = now;
    if (len > sizeof(p))
        len = sizeof(p);
    if (len) {
        /* current S-box; failing that, a message starting with type 0 was
           encrypted after KSA("LF KO IGSA36") */
        memcpy(s0, g.sbox, sizeof(s0));
        memcpy(p, req, len);
        rc4_crypt(p, len);
        if (!sum_ok(p, len)) {
            rc4_key(boot_key, sizeof(boot_key));
            memcpy(p, req, len);
            rc4_crypt(p, len);
            if (!sum_ok(p, len)) {
                LOG("rx checksum mismatch (%u bytes)", len);
                memcpy(g.sbox, s0, sizeof(s0));
                len = 0;
            }
        }
    }
    if (len >= 12) {
        g.sum = p[len - 4];
        off = 8 + ((ld32(p + 4) + 3) & ~3u);
        while (off + 8 <= len - 4) {
            uint32_t type = ld32(p + off), n = ld32(p + off + 4);

            if (n > len - 4 - off - 8)
                break;
            if (first && (type == 0 || type == 5)) {
                newkey = p + off + 8;
                newlen = n;
            }
            first = 0;
            VLOG("message %u (%u bytes)", type, n);
            message(type, p + off + 8, n);
            off += 8 + ((n + 3) & ~3u);
        }
        if (newkey) {
            VLOG("re-key (%u bytes)", newlen);
            rc4_key(newkey, newlen);
        }
    }
    return build(reply, max);
}

/* ---- serial peripherals --------------------------------------------------------- */

/* ---- NFC tags (HERO_NFC, the reader in the card library) ------------------------- */

static const struct hero_nfc *nfc(void)
{
    static const struct hero_nfc *n;
    static int tried;
    hero_nfc_api_fn fn;

    if (!tried) {                       /* racing threads both look it up: harmless */
#ifdef _WIN32
        *(void **)&fn = GetProcAddress(GetModuleHandleA("libcasmcard-hero.dll"), HERO_NFC_SYMBOL);
#else
        *(void **)&fn = dlsym(RTLD_DEFAULT, HERO_NFC_SYMBOL);
#endif
        n = fn ? fn() : NULL;
        if (n && n->version != HERO_NFC_VERSION)
            n = NULL;
        tried = 1;
    }
    return n;
}

/*
 * A dispensed card: the cabinet's dispenser holds printed cards and the game
 * never learns which one came out, so the loader picks one, HERO_NFC_CARD
 * or a random code from the game's card list (ROBOT, POWER and SKILL rows
 * with a 9-digit QR code).
 */
static int pick_card(char *code)
{
    static char (*codes)[10];
    static int ncodes, loaded;
    static uint32_t x;
    const char *v = getenv("HERO_NFC_CARD");

    if (v && *v) {
        snprintf(code, 13, "%s", v);
        return 1;
    }
    if (!loaded) {
        FILE *f = fopen("../media/localization/Card/QRCardRes.csv", "r");
        char line[512];

        loaded = 1;
        while (f && fgets(line, sizeof(line), f)) {
            char *c1 = strchr(line, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
            int i;

            if (!c2 || (strncmp(line, "ROBOT,", 6) && strncmp(line, "POWER,", 6) &&
                        strncmp(line, "SKILL,", 6)))
                continue;
            for (i = 1; i <= 9 && c2[i] >= '0' && c2[i] <= '9'; i++)
                ;
            if (i != 10 || c2[10] != ',')
                continue;
            if (!(ncodes & 255))
                codes = realloc(codes, (ncodes + 256) * sizeof(*codes));
            memcpy(codes[ncodes], c2 + 1, 9);
            codes[ncodes++][9] = 0;
        }
        if (f)
            fclose(f);
        LOG("%d card codes in QRCardRes.csv", ncodes);
        x = (uint32_t)(api->core->now() * 1000) | 1;    /* not rand(): the card's challenge uses it */
    }
    if (!ncodes)
        return 0;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    memcpy(code, codes[x % ncodes], 10);
    return 1;
}

/*
 * COM1 QR code reader: acknowledges every command with 06.  Trigger on
 * (1B 31): with an NFC loader tag just placed, the acknowledgement carries
 * its card code as the reader's text, 06 <code> CR LF.
 */
static uint32_t qr_write(const uint8_t *p, uint32_t n, uint8_t *reply, uint32_t max)
{
    const struct hero_nfc *nf = nfc();
    struct hero_nfc_tag t;
    uint32_t len;

    if (!n || !max)
        return 0;
    reply[0] = 0x06;
    if (n == 2 && p[0] == 0x1B && p[1] == 0x31 && nf && nf->features & NFC_SCAN) {
        nf->get(&t);
        if (t.state == TAG_LOADER && t.code[0] && t.serial != g.qr_serial &&
            (len = strlen(t.code)) + 3 <= max) {
            g.qr_serial = t.serial;
            memcpy(g.qr_code, t.code, sizeof(g.qr_code));
            memcpy(reply + 1, t.code, len);
            memcpy(reply + 1 + len, "\r\n", 2);
            LOG("QR reader: card %s from the NFC tag", t.code);
            return len + 3;
        }
    }
    return 1;
}

/*
 * COM2 card dispenser (CardDispenser): status 01 37 -> 23 (ready), sensor
 * 01 38 -> 30 (cards in stock), dispense 01 42 and reset 01 40 -> 50 (done).
 * After a dispense the game polls the status every 500 ms and leaves the
 * "cards obtained" screen only once it reads 16 (CheckStatus 7, taken as
 * "card taken", guess) or the 6 group: the next two polls answer 16.
 */
static uint32_t dispenser_write(const uint8_t *p, uint32_t n, uint8_t *reply, uint32_t max)
{
    static int taken;

    if (n < 2 || max < 2)
        return 0;
    reply[0] = p[0];
    switch (p[1]) {
    case 0x37:
        reply[1] = taken ? 0x16 : 0x23;
        if (taken)
            taken--;
        break;
    case 0x38: reply[1] = 0x30; break;
    case 0x42: {
        const struct hero_nfc *nf = nfc();
        char code[13];

        reply[1] = 0x50;
        taken = 2;
        LOG("card dispensed");
        if (nf && nf->features & NFC_DISPENSE && pick_card(code))
            nf->dispense(code);
        break;
    }
    default:
        VLOG("dispenser command %02x %02x", p[0], p[1]);
        reply[1] = 0x50;
        break;
    }
    return 2;
}

static const struct e2100_serial serial[] = {
    { "/dev/ttyS0", qr_write },
    { "/dev/ttyS1", dispenser_write },
    { NULL, NULL },
};

/* ---- input and overlay ---------------------------------------------------------- */

/*
 * SDL 1.2 key symbols.  The game's own X11 keyboard is off; everything
 * comes from the IO board.  X/Z/C are player 1's buttons (X confirms),
 * M/N/B player 2's.
 */
static const struct { int sym; uint32_t bit; } keymap[] = {
    { 'x', IN_P1_X }, { ' ', IN_P1_X }, { 13, IN_P1_X }, { 306, IN_P1_X },
    { 'z', IN_P1_Z }, { 'c', IN_P1_C },
    { 'm', IN_P2_M }, { 'n', IN_P2_N }, { 'b', IN_P2_B },
    { 273, IN_UP }, { 274, IN_DOWN },
    { 283, IN_TEST },                         /* F2 */
    { '9', IN_SERVICE },
};

static void key(int sym, int down)
{
    unsigned i;

    if (sym == '5') {
        if (down)
            g.coins++;
        return;
    }
    if (sym == 282) {                         /* F1 */
        if (down)
            g.help = !g.help;
        return;
    }
    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        if (keymap[i].sym != sym)
            continue;
        if (down)
            g.keys |= keymap[i].bit;
        else
            g.keys &= ~keymap[i].bit;
    }
}

static void draw(int w, int h)
{
    static const char *lines[] = {
        "5 coin  X/Space start",
        "Z X C  Up/Down",
        "F2 test  9 service",
        "F1 hide this",
    };
    const struct igs_core *c = api->core;
    const struct hero_nfc *nf = nfc();
    struct hero_nfc_tag t;
    char tag[40];
    int i, y, n = 4;

    (void)w;
    if (!g.help)
        return;
    if (nf) {                                 /* a fifth line: the NFC tag */
        nf->get(&t);
        if (t.state == TAG_LOADER && t.points >= 0)
            snprintf(tag, sizeof(tag), "NFC %s %d pts", t.code[0] ? t.code : "-", t.points);
        else if (t.state == TAG_LOADER)
            snprintf(tag, sizeof(tag), "NFC %s", t.code[0] ? t.code : "-");
        else
            snprintf(tag, sizeof(tag), "NFC %s", t.state == TAG_BLANK ? "blank tag" :
                     t.state == TAG_FOREIGN ? "other tag" : "no tag");
        n = 5;
    }
    y = h - 4 - n * c->font_h;
    c->fill(0, y - 4, 22 * c->font_w, n * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < 4; i++)
        c->text(6, y + (n - 1 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
    if (n == 5)
        c->text(6, y + 6, tag, 1, 1, 0.6f);
}

/* the state the game is in; names only for the ids seen in the logs */
static void status(char *buf, size_t n)
{
    static const struct { int id; const char *name; } names[] = {
        { 26, "copyright" }, { 21, "warning" }, { 17, "IGS logo" }, { 1, "insert coin" },
        { 11, "card dispense" },
    };
    unsigned i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].id == g.current) {
            snprintf(buf, n, "%s", names[i].name);
            return;
        }
    snprintf(buf, n, "state %d", g.current);
}

/* ---- plugin entry ---------------------------------------------------------------- */

static void reset(struct e2100_channel *ch)
{
    const char *v = getenv("HERO_REGION");

    (void)ch;
    rc4_key(boot_key, sizeof(boot_key));
    g.sum = 0;
    g.current = 26;
    g.have_table = g.booted = 0;
    g.want = g.want_rsa = 0;
    g.region = v && *v ? (uint8_t)strtol(v, NULL, 0) : HERO_REGION_DEFAULT;
    g.help = 1;
    g.t0 = api->core->now();
}

static const struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Hero of Robots",
    .reset = reset,
    .key = key,
    .draw = draw,
    .transport = E2100_TRANSPORT_REQRESP,
    .exchange = exchange,
    .serial = serial,
    .status = status,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    api = a;
    return &game;
}
