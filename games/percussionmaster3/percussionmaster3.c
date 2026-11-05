/*
 * Percussion Master 3 (E2200): its IGS036 firmware's side of the
 * request/response transport, rebuilt.  wiki Percussion Master 3,
 * wiki E2200.  The executable (pm3) keeps its dynamic symbols; names
 * below are its own.
 *
 *   - Framing (Igs036P2A::SendData / ReadData): BAR1 word 0 = total bytes;
 *     at BAR0 + off: [sum][r][data], sum's low byte = r + the data bytes.
 *     Replies: [sum][echo][next off][data], echo = the game's last sum byte,
 *     sum = echo + the data bytes; the low nibble of next off is where both
 *     sides put the next frame (always 0 here).  At most 800 bytes.
 *   - Channel: RC4 over the data (rc4_CypherKey / rc4_CypherData), one S-box
 *     carried over, i and j restart per message.  The game's first message
 *     (type 0, 8 random bytes) is encrypted after KSA("PM3   IGSA36"), then
 *     the S-box is keyed with those 8 bytes.
 *   - Messages [type u32][len u32][data], game -> IGS036: 0 key, 1 the 36
 *     state objects, 2 state status (4 bytes: 1 running, 2 done) or a
 *     state's end (36 bytes: id and its results), 3 I/O board output
 *     (lamps, meters), 4 chart key request, 6 chart cut request (the chart's
 *     file name), 8 / 10 / 11 sensor thresholds wanted / all set / one set,
 *     12 and 19 start-up, 16 network data, 17 coin record request.
 *   - IGS036 -> game (Game::iIgs036_RecvCmdPrcess): 2 the current state
 *     object, 3 the I/O board (keys, drum sensors), 5 chart keys, 7 a chart's
 *     cut 16 bytes, 9 sensor thresholds, 13 version, 15 the network data, 18
 *     coin record.
 *   - The state machine runs on the pointer we send: the game calls Init,
 *     Process and End on it; after an End we pick the next state.  That
 *     choice was the chip's (not dumped) and is rebuilt here.
 *   - Network data (messages 14 / 15 / 16): the store id, FPGA-encrypted;
 *     kept in ../rewrite/igs036.nv.  Without a message 15 the game never
 *     connects (wiki Percussion Master 3 Online).
 *
 * Invented: the state flow; versions, thresholds and the coin record are
 * plausible values; thresholds are not kept between runs; the FPGA mask of
 * the network-data key.  The charts' keys and cut bytes are recovered from
 * the dump (wiki Keys), not the chip's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2100/e2100.h"

static const struct e2100_api *api;
#define LOG(...) api->core->log("pm3: " __VA_ARGS__)
#define VLOG(...) do { if (api->core->verbose) LOG(__VA_ARGS__); } while (0)

#define REPLY_MAX 800                     /* Igs036P2A::ReadData's buffer */
#define NV_FILE "../rewrite/igs036.nv"    /* cwd /PM3/exe; p4, next to SettingInfo.dat */
#define NV_MAGIC 0x334d5031u              /* "1PM3" */

/* states: the index in the game's message-1 table (GameProcessStateList) */
enum {
    S_LOGO = 1, S_COINPAGE = 3, S_LOGIN = 4, S_SIGN = 6, S_SELTYPE = 7, S_SELMODE = 8,
    S_SELEV = 10, S_SELSKILL = 11, S_SELLEVEL = 13, S_SELSONG = 14, S_STAGE = 15,
    S_RESULT = 17, S_FINAL = 19, S_HOUSE = 20, S_PHOTO = 21, S_RANK = 23, S_THANK = 25,
    S_SELROLE = 26, S_MATCH = 28, S_DRAGON = 30, S_SETTING = 31, S_INFOMATCH = 32,
    S_NETRANK = 33, S_DRAGON2 = 35,
};

static struct {
    uint8_t sbox[256];
    uint8_t echo;                         /* the game's last sum byte */
    uint32_t table[36];
    int have_table, cur;
    int demo;                             /* the coin page timed out: a demo song */
    uint32_t keys;                        /* key-in bits (IOBoard word 0) */
    uint16_t sensors;                     /* drum sensors hit, bit = sensor + 6 * player */
    int level;                            /* the hit's sensor level (PM3_VELOCITY) */
    uint32_t fix[12][4];                  /* thresholds: max, min, default, current */
    int coins, coin_t;
    int want_keys, want_cut, want_coinrec, want_fix, want_net, booted;
    uint8_t cut[16];
    int help, shift;
    double t0;
    int after_board;                      /* where InfoMatch / InfoDragon lead back to */
    int online;                           /* the coin page saw the game server (Coinpage +19716) */
    struct { uint32_t magic; uint8_t net[16]; } nv;
    /* PM3_SCRIPT */
    uint32_t frames;
    uint32_t script_keys;
    uint16_t script_sensors;
} g;

/* ---- channel ------------------------------------------------------------------ */

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

static const uint8_t boot_key[12] = "PM3   IGSA36";

static uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* the messages fill the data exactly */
static int well_formed(const uint8_t *p, uint32_t n)
{
    uint32_t off = 0;

    while (off + 8 <= n) {
        uint32_t len = ld32(p + off + 4);

        if (ld32(p + off) > 64 || len > n - off - 8)
            return 0;
        off += 8 + len;
    }
    return off == n;
}

/* ---- chart keys ------------------------------------------------------------------ */

/*
 * The charts (media/script_AES, .enccut) are FPGA-cipher encrypted
 * (AES::iAESDecrypt): a key-id word, the data, a trailer {random, size, key
 * id}.  The key comes from the chip's table (message 5, 10 x 32 bytes) and
 * each file's first 16 bytes are cut off: the chip sends them (message 7)
 * for the chart the game asks for (message 6).  The table here is
 * PowerOn's (/PM3.bin/PowerOn decrypts pm3 with its key 5, whose mask is
 * also chart key 5's); the masks were recovered from the charts themselves
 * (wiki Keys).  SongList.enc uses a key built into pm3.
 */
static const uint8_t chart_key[10][16] = {
    { 0x4e, 0x67, 0xd5, 0x98, 0xe7, 0x3e, 0x6a, 0x6f, 0x67, 0x1d, 0x87, 0xcd, 0xa7, 0x13, 0xb2, 0x7c },
    { 0x33, 0x2d, 0x47, 0x3d, 0x27, 0x11, 0x02, 0x72, 0xee, 0xf7, 0xa5, 0x18, 0xe5, 0x1f, 0x4a, 0x62 },
    { 0x66, 0x11, 0x8f, 0xad, 0x67, 0x03, 0x12, 0x8c, 0x5a, 0x2d, 0xff, 0xb2, 0x5c, 0x72, 0x1e, 0x5e },
    { 0x3e, 0x07, 0x55, 0x48, 0x07, 0xae, 0x5a, 0x1f, 0x3a, 0x9d, 0x3e, 0x22, 0x78, 0x91, 0x2e, 0x0e },
    { 0x3a, 0x8d, 0x7e, 0x52, 0x77, 0xa2, 0xce, 0x3e, 0x9e, 0x68, 0x75, 0x98, 0xe6, 0xee, 0xca, 0x2f },
    { 0x45, 0x57, 0x85, 0x93, 0x07, 0x5e, 0x67, 0x5f, 0xc4, 0x7d, 0xf9, 0x32, 0x79, 0x02, 0x9e, 0x36 },
    { 0xe7, 0x4d, 0x67, 0xce, 0xd7, 0x1b, 0xb3, 0x6c, 0x43, 0x62, 0x85, 0x38, 0xef, 0x3b, 0x2a, 0x66 },
    { 0x61, 0x8f, 0x87, 0x54, 0x65, 0x44, 0x75, 0x3d, 0x57, 0x5a, 0x87, 0xfd, 0x37, 0x13, 0x42, 0x74 },
    { 0x62, 0xed, 0x67, 0xfd, 0xaf, 0x63, 0x22, 0xbc, 0xc2, 0xed, 0xf6, 0x3e, 0xcc, 0x62, 0x43, 0xae },
    { 0x61, 0x60, 0x3e, 0x59, 0x81, 0x94, 0x75, 0xd3, 0xc0, 0x0d, 0x7e, 0x92, 0x76, 0x99, 0x47, 0xce },
};
static const uint8_t chart_mask[10][16] = {
    { 0x32, 0x58, 0x21, 0x7a, 0x79, 0xe8, 0x82, 0x61, 0xf1, 0x39, 0xe5, 0xad, 0xc7, 0x3d, 0x61, 0x1a },
    { 0xd8, 0x82, 0x84, 0x5f, 0xd9, 0xc6, 0x84, 0xab, 0x15, 0x8f, 0xf1, 0x28, 0x1d, 0xf4, 0x4a, 0x03 },
    { 0x89, 0x0f, 0xe5, 0x18, 0x07, 0x3e, 0xa7, 0x28, 0x35, 0x32, 0x72, 0x0c, 0x56, 0x96, 0xda, 0x58 },
    { 0x16, 0x1d, 0xe4, 0xf0, 0xb2, 0x10, 0xbc, 0x08, 0xd6, 0x22, 0x75, 0x28, 0xd6, 0xff, 0xa9, 0x52 },
    { 0x95, 0x80, 0x66, 0xea, 0x68, 0x8c, 0xe5, 0xd9, 0xee, 0xce, 0x58, 0x2a, 0x5f, 0xe9, 0x5a, 0x51 },
    { 0x3a, 0x0e, 0x2d, 0x44, 0xe5, 0x78, 0xa0, 0xa8, 0x13, 0x70, 0xdd, 0x14, 0x7c, 0xbf, 0x2e, 0x19 },
    { 0x18, 0x03, 0xa7, 0x3f, 0xa9, 0xfe, 0xe3, 0xe0, 0x28, 0x2d, 0xf9, 0x20, 0x9d, 0x24, 0x93, 0x92 },
    { 0x9a, 0xda, 0xb8, 0x2c, 0x2d, 0xa8, 0xa0, 0xea, 0x30, 0xbf, 0x41, 0xa3, 0x66, 0x3c, 0x63, 0x7a },
    { 0x98, 0xcf, 0xef, 0x1a, 0xc9, 0xb6, 0x89, 0xa4, 0x52, 0xec, 0xe2, 0x2a, 0x53, 0x94, 0x5a, 0x55 },
    { 0xcf, 0x97, 0xd6, 0xc8, 0x27, 0x69, 0x98, 0x47, 0x90, 0xa0, 0xe1, 0x70, 0x72, 0x9b, 0x1b, 0x31 },
};
/* pm3's own key (0x98A2A80) for SongList.enc */
static const uint8_t list_key[16] = {
    0x3e, 0x07, 0xd2, 0x97, 0x57, 0x8e, 0x4a, 0x7f, 0x47, 0x1d, 0x83, 0xc8, 0x67, 0x93, 0x32, 0x6c,
};
static const uint8_t list_mask[16] = {
    0x16, 0x18, 0xa1, 0xda, 0x69, 0xc8, 0x20, 0x69, 0xa5, 0x3b, 0xc9, 0x2c, 0xc6, 0x78, 0x25, 0x52,
};

/*
 * The key of the network data (pm3 0x989E1E0; NetManager::pubResetNetDataWithEncrypt,
 * iRecvNetDataFrom36WithDecrypt).  The real FPGA's mask for it is unknown (no
 * cabinet's message 16 was dumped); both directions run on this emulated FPGA,
 * so any mask round-trips.  All zero: the blob is then the cipher's bare
 * shuffle, which this plugin undoes to read and clear the MAC.
 */
static const uint8_t net_key[16] = {
    0x4e, 0x27, 0xd1, 0x99, 0x47, 0x3e, 0x7a, 0x0f, 0x57, 0x3d, 0x82, 0xc8, 0x37, 0x43, 0x62, 0x9c,
};
static const uint8_t net_mask[16];

static const uint8_t *asset_mask(const uint8_t *k)
{
    int i;

    if (!memcmp(k, net_key, 16))
        return net_mask;
    if (!memcmp(k, list_key, 16))
        return list_mask;
    for (i = 0; i < 10; i++)
        if (!memcmp(k, chart_key[i], 16))
            return chart_mask[i];
    return NULL;
}

/* the FPGA cipher (machines/e2100, wiki Sound Crypto) */
static const uint8_t perm[16] = { 14, 10, 8, 1, 0, 12, 9, 4, 3, 6, 11, 7, 2, 13, 5, 15 };

static void cipher_encrypt(uint8_t *blk, const uint8_t *mask)
{
    uint8_t u[16];
    uint64_t lo, hi;
    int i;

    for (i = 0; i < 16; i++)
        u[perm[i]] = blk[i];
    for (i = 0; i < 16; i++)
        if (0xb4d2 >> i & 1)
            u[i] ^= 0x80;
    memcpy(&lo, u, 8);
    memcpy(&hi, u + 8, 8);
    lo = lo << 1 | lo >> 63;
    hi = hi << 3 | hi >> 61;
    memcpy(u, &lo, 8);
    memcpy(u + 8, &hi, 8);
    for (i = 0; i < 16; i++)
        blk[i] = u[i] ^ mask[i];
}

static void cipher_decrypt(uint8_t *blk, const uint8_t *mask)
{
    uint8_t d[16];
    uint64_t lo, hi;
    int i;

    for (i = 0; i < 16; i++)
        d[i] = blk[i] ^ mask[i];
    memcpy(&lo, d, 8);
    memcpy(&hi, d + 8, 8);
    lo = lo >> 1 | lo << 63;
    hi = hi >> 3 | hi << 61;
    memcpy(d, &lo, 8);
    memcpy(d + 8, &hi, 8);
    for (i = 0; i < 16; i++)
        if (0xb4d2 >> i & 1)
            d[i] ^= 0x80;
    for (i = 0; i < 16; i++)
        blk[i] = d[perm[i]];
}

/*
 * Message 6 carries the chart's file name (STRUCT_SONG_INFO +144, 32
 * bytes, e.g. "p001_easy").  The cut bytes: the key-id word (bits 4, 10,
 * 17, 29), then the first 12 bytes of block 0, whose plaintext is every
 * chart's first line, "#------ BASE DATA ------" (its last 4 cipher bytes,
 * still in the file, check it).
 */
static void make_cut(const uint8_t *info)
{
    static const uint8_t head[16] = "#------ BASE DAT";
    char name[33], path[96];
    uint8_t *f = NULL, blk[16];
    long n = 0;
    FILE *fp;
    int i, kid = -1;
    uint32_t w;

    memset(g.cut, 0, sizeof(g.cut));
    memcpy(name, info, 32);
    name[32] = 0;
    snprintf(path, sizeof(path), "../media/script_AES/%s.enccut", name);
    if ((fp = fopen(path, "rb"))) {
        fseek(fp, 0, SEEK_END);
        n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        f = malloc(n > 0 ? n : 1);
        if (!f || fread(f, 1, n, fp) != (size_t)n)
            n = 0;
        fclose(fp);
    }
    /* the key: the one whose trailer names itself */
    for (i = 0; i < 10 && n >= 20 && (n - 4) % 16 == 0; i++) {
        memcpy(blk, f + n - 16, 16);
        cipher_decrypt(blk, chart_mask[i]);
        if (ld32(blk + 12) == (uint32_t)i && ld32(blk + 8) < (uint32_t)n) {
            kid = i;
            break;
        }
    }
    if (kid < 0) {
        LOG("chart %s: no key fits", path);
        free(f);
        return;
    }
    memcpy(blk, head, 16);
    cipher_encrypt(blk, chart_mask[kid]);
    if (memcmp(blk + 12, f, 4))
        LOG("chart %s: the first line differs, its first 16 bytes are guessed", name);
    w = (rand() & ~0x20020410u) | (kid & 1) << 4 | (kid >> 1 & 1) << 10 | (kid >> 2 & 1) << 17 |
        (kid >> 3 & 1) << 29;
    st32(g.cut, w);
    memcpy(g.cut + 4, blk, 12);
    VLOG("chart %s, key %d", name, kid);
    free(f);
}

/* ---- network data (messages 14, 15, 16) -------------------------------------------- */

/*
 * Message 16 is {u32 store id, MAC[6], 0} encrypted with net_key; message 15
 * gives it back (word 0 zero: not registered, the game registers again).  A
 * non-zero MAC that differs from eth0's stops the game's network for good
 * (NetManager::NetProcess state 2), and eth0 differs between the loaders and
 * between WSL boots, so the MAC is cleared before it is kept: zero is never
 * compared.
 */
static void nv_load(void)
{
    FILE *f = fopen(NV_FILE, "rb");

    if (!f || fread(&g.nv, sizeof(g.nv), 1, f) != 1 || g.nv.magic != NV_MAGIC)
        memset(&g.nv, 0, sizeof(g.nv));
    if (f)
        fclose(f);
}

static void nv_save(void)
{
    FILE *f = fopen(NV_FILE, "wb");

    if (!f) {
        LOG("cannot write %s", NV_FILE);
        return;
    }
    g.nv.magic = NV_MAGIC;
    fwrite(&g.nv, sizeof(g.nv), 1, f);
    fclose(f);
}

static void net_keep(const uint8_t *d)
{
    uint8_t b[16];

    memcpy(b, d, 16);
    cipher_decrypt(b, net_mask);
    memset(b + 4, 0, 12);                 /* the MAC and the pad */
    LOG("network data kept: store id %u", ld32(b));
    cipher_encrypt(b, net_mask);
    memcpy(g.nv.net, b, 16);
    nv_save();
}

/* ---- the state flow (the chip's, rebuilt) ---------------------------------------- */

/*
 * Each state's End sends [status, id, six results, a flag word]; values the
 * game wants hidden are padded with random multiples (x + 15 * r, x + 2 * r,
 * x + 3 * r), so the result is the remainder.  Bit 0 of the flag word: the
 * test button was pressed (to the operator menu).  The results the routing
 * reads:
 *   Coinpage   [0] 1 a player joined, 0 timed out (the demo song)
 *   SelMode    [0] the mode (0 standard, 1 special, 2 event: guessed)
 *   Stage      (none used)
 *   Resultp    [1] % 15 Resultp::CheckNext: 1, 2 another round, 4 a
 *              holiday or contest board, 5 a dragon board, 0 / 3 the end
 *   FinalResult [0..2] & 1 the holiday or contest / dragon / dragon 2 boards
 *   House      [1] House::CheckNext: 0 photo, 2 / 6 one / both players
 *              continue, 5 online with a guest, 7 done; after player 1's
 *              house with player 2 in the game: 1 player 2's house, 3 / 4
 *              a board first
 *   Rank       [0] % 3: 1 after a game, 0 in the attract; [2] & 1 online
 *              (NetManager::NetGetStatus > 6)
 *   Login      [0], [1] player 1 / 2 finished a new ID's passwords
 *              (Login::Confirm): sign-up; [4] 1: the dragon 2 board for a
 *              level 10+ member (BoardInfo +1040, cleared by InfoDragon 35)
 *   Sign       [4] % 16 Sign::CheckNext: 1 the other player signs up too, 3
 *              the dragon 2 board (as Login's), else done
 *   netrank    [0] 1 after a game, 0 in the attract
 *   Match      [0] & 1 online, [3] % 3: 1 in a game (20 s, timer), 0 in
 *              the attract (10 s)
 *   Coinpage   [1] online (GameData +19716, GameInfo::CheckConnect)
 * Online, after the local ranking comes the net ranking (netrank), then in
 * the attract the event boards (Match); in a game the boards come after the
 * login, before the play type.
 */
#define AFTER_LOGIN (g.online ? S_MATCH : S_SELTYPE)
static int next_state(int id, const uint32_t *w)
{
    if ((w[8] & 1) && id != S_SETTING)
        return S_SETTING;
    switch (id) {
    case S_LOGO:     return S_COINPAGE;
    case S_COINPAGE:
        g.demo = !w[2];
        g.online = w[3] & 1;
        return w[2] ? S_LOGIN : S_STAGE;
    case S_LOGIN:
        if (w[2] == 1 || w[3] == 1)
            return S_SIGN;
        if (w[6] == 1) {
            g.after_board = AFTER_LOGIN;
            return S_DRAGON2;
        }
        return AFTER_LOGIN;
    case S_SIGN:
        switch (w[6] % 16) {
        case 1:  return S_SIGN;
        case 3:  g.after_board = AFTER_LOGIN; return S_DRAGON2;
        default: return AFTER_LOGIN;
        }
    case S_SELTYPE:  return S_SELMODE;
    case S_SELMODE:
        return w[2] == 1 ? S_SELSKILL : w[2] == 2 ? S_SELEV : S_SELLEVEL;
    case S_SELSKILL:
    case S_SELEV:
    case S_SELLEVEL: return S_SELSONG;
    case S_SELSONG:  return S_STAGE;
    case S_STAGE:    return g.demo ? S_RANK : S_RESULT;
    case S_RESULT:
        g.after_board = S_FINAL;
        switch (w[3] % 15) {
        case 1: case 2: return S_SELSONG;
        case 4:         return S_INFOMATCH;
        case 5:         return S_DRAGON;
        default:        return S_FINAL;
        }
    case S_FINAL:    return S_HOUSE;
    case S_HOUSE:
        /* 1, 3, 4 come only from player 1's house with player 2 in the game:
           player 2's house is next (after the board) */
        g.after_board = S_HOUSE;
        switch (w[3]) {
        case 0:            return S_PHOTO;
        case 1:            return S_HOUSE;
        case 2: case 6:    return S_SELMODE;
        case 3:            return S_INFOMATCH;
        case 4:            return S_DRAGON;
        default:           return S_RANK;
        }
    case S_PHOTO:    return S_RANK;
    case S_RANK:
        if (w[4] & 1)
            return S_NETRANK;
        return w[2] % 3 == 1 ? S_THANK : S_LOGO;
    case S_NETRANK:  return w[2] == 1 ? S_THANK : S_MATCH;
    case S_MATCH:    return w[5] % 3 == 1 ? S_SELTYPE : S_LOGO;
    case S_INFOMATCH:
    case S_DRAGON:
    case S_DRAGON2:  return g.after_board ? g.after_board : S_FINAL;
    default:         return S_LOGO;
    }
}
static void message(uint32_t type, const uint8_t *d, uint32_t n)
{
    uint32_t w[9];

    switch (type) {
    case 1:                                  /* the state objects */
        if (n >= sizeof(g.table)) {
            memcpy(g.table, d, sizeof(g.table));
            g.have_table = 1;
            LOG("state table received");
        }
        break;
    case 2:                                  /* status, or a state's end */
        if (n == 36) {
            memcpy(w, d, sizeof(w));
            int next = next_state(w[1], w);

            if (g.have_table && next > 0 && next < 36 && !g.table[next]) {
                LOG("state %d has no object; logo instead", next);
                next = S_LOGO;
            }

            LOG("%.1f s: state %u ends (%u %u %u %u %u %u), next %d", api->core->now() - g.t0,
                w[1], w[2], w[3], w[4], w[5], w[6], w[7], next);
            g.cur = next;
        }
        break;
    case 4:                                  /* chart keys wanted */
        g.want_keys = 1;
        break;
    case 6:                                  /* a chart's cut bytes wanted */
        if (n >= 32) {
            make_cut(d);
            g.want_cut = 1;
        }
        break;
    case 8:                                  /* the sensor test asks for the thresholds */
        g.want_fix = 1;
        break;
    case 10:                                 /* all thresholds, from the sensor test */
        if (n >= sizeof(g.fix))
            memcpy(g.fix, d, sizeof(g.fix));
        break;
    case 11:                                 /* one threshold: sensor, max, min, default, current */
        if (n >= 20 && ld32(d) < 12)
            memcpy(g.fix[ld32(d)], d + 4, 16);
        break;
    case 14:                                 /* the network data wanted (Game::iIGS_Init) */
        g.want_net = 1;
        break;
    case 16:                                 /* network data to keep (after registration) */
        if (n >= 16)
            net_keep(d);
        break;
    case 17:                                 /* coin record wanted */
        g.want_coinrec = 1;
        break;
    default:
        VLOG("message %u (%u bytes)", type, n);
        break;
    }
}

/* ---- replies ------------------------------------------------------------------------ */

static uint32_t put(uint8_t *r, uint32_t pos, uint32_t type, const void *data, uint32_t n)
{
    if (pos + 8 + n > REPLY_MAX)
        return pos;
    st32(r + pos, type);
    st32(r + pos + 4, n);
    memcpy(r + pos + 8, data, n);
    return pos + 8 + n;
}

/* message 3: IOBoard's 20 bytes: key-in word, 12 sensor levels, the pressed
   bitmap, a byte sum */
static uint32_t put_io(uint8_t *r, uint32_t pos)
{
    uint8_t io[20] = { 0 };
    uint32_t keys = g.keys | g.script_keys;
    uint16_t sensors = g.sensors | g.script_sensors;
    int i;

    /* coin: held 4 frames, released 4 (GameKey counts a press from frame 2) */
    if (g.coin_t > 0) {
        if (g.coin_t-- > 4)
            keys |= 1u << 26;
    } else if (g.coins) {
        g.coins--;
        g.coin_t = 8;
        keys |= 1u << 26;
    }
    st32(io, keys);
    /* a sensor counts as hit when its level reaches its threshold */
    for (i = 0; i < 12; i++) {
        if (!(sensors >> i & 1))
            continue;
        io[5 + i] = g.level;
        if ((uint32_t)g.level >= g.fix[i][3])
            io[17 + i / 8] |= 1 << (i & 7);
    }
    for (i = 0; i < 19; i++)
        io[19] += io[i];
    return put(r, pos, 3, io, sizeof(io));
}

/* keypad digit -> key-in bit, player 1 (0..9, then the two extra keys) and player 2 */
static const uint8_t pad_bit[2][12] = {
    { 6, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 7 },
    { 10, 16, 17, 18, 19, 20, 21, 22, 23, 8, 9, 11 },
};

/*
 * PM3_SCRIPT="sec:key[+],..." presses keys on a timetable from start-up, for
 * testing.  Keys: coin, service, test; player 1 b1 r1 (little drums 1, 2), d1
 * (the surface, both halves), m1 (both rims), k0..k9 kstar khash (keypad);
 * player 2 b2 r2 d2 m2, j0..j9 jstar jhash.  A press lasts 0.2 s, or with a
 * trailing + until the next entry.  PM3_SCRIPT_CLOCK=frames counts the game's
 * frames (60 a second) instead of wall time.
 */
static int script_key(const char *name, uint32_t *keys, uint16_t *sensors)
{
    static const struct { const char *name; uint32_t keys; uint16_t sensors; } named[] = {
        { "service", 1u << 25, 0 }, { "test", 1u << 24, 0 },
        { "b1", 0, 1 << 0 }, { "r1", 0, 1 << 1 }, { "d1", 0, 3 << 3 }, { "m1", 0, 1 << 2 | 1 << 5 },
        { "b2", 0, 1 << 6 }, { "r2", 0, 1 << 7 }, { "d2", 0, 3 << 9 }, { "m2", 0, 1 << 8 | 1 << 11 },
    };
    unsigned i;
    int p, k;

    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (!strcmp(name, named[i].name)) {
            *keys = named[i].keys;
            *sensors = named[i].sensors;
            return 1;
        }
    if ((name[0] != 'k' && name[0] != 'j') || !name[1])
        return 0;
    p = name[0] == 'j';
    if (!strcmp(name + 1, "star"))
        k = 10;
    else if (!strcmp(name + 1, "hash"))
        k = 11;
    else if (name[1] >= '0' && name[1] <= '9' && !name[2])
        k = name[1] - '0';
    else
        return 0;
    *keys = 1u << pad_bit[p][k];
    *sensors = 0;
    return 1;
}

static void script(void)
{
    static const char *p;
    static double t0, release_at;
    static int held, frames;
    const struct igs_core *c = api->core;
    uint32_t keys;
    uint16_t sensors;
    double now, at;
    char name[16];
    int n = 0, hold;

    if (!p) {
        p = c->env("PM3_SCRIPT", "");
        frames = !strcmp(c->env("PM3_SCRIPT_CLOCK", ""), "frames");
        t0 = c->now();
    }
    now = frames ? g.frames / 60.0 : c->now() - t0;
    if (held && release_at && now >= release_at) {
        g.script_keys = 0;
        g.script_sensors = 0;
        held = 0;
    }
    if (!*p || sscanf(p, "%lf:%15[a-z0-9]%n", &at, name, &n) != 2 || now < at)
        return;
    hold = p[n] == '+';
    p += n + hold;
    if (*p == ',')
        p++;
    g.script_keys = 0;
    g.script_sensors = 0;
    held = 0;
    if (!strcmp(name, "coin")) {
        g.coins++;
    } else if (script_key(name, &keys, &sensors)) {
        g.script_keys = keys;
        g.script_sensors = sensors;
        held = 1;
        release_at = hold ? 0 : now + 0.2;
    } else {
        LOG("script: unknown key %s", name);
        return;
    }
    LOG("script %.1f s %s%s", now, name, hold ? " (held)" : "");
}

static uint32_t build(uint8_t *r)
{
    uint32_t pos = 0;

    g.frames++;
    script();

    /* the objects these fill exist once the game has sent its state table */
    if (g.have_table && !g.booted) {
        /*
         * version (PM3Version::vRecvInfoFrom36): word 0 the FPGA's (bits 24..29
         * major, 17..23 minor, bit 30 set = not burnt), word 1 the IGS036's
         * fuses (bits 0, 1 set = not burnt), word 2 the IGS036's version
         */
        uint32_t ver[3] = { 1u << 24, 0, 100 };
        pos = put(r, pos, 13, ver, sizeof(ver));
        g.want_fix = 1;
        g.booted = 1;
    }
    if (!g.have_table)
        return pos;
    pos = put(r, pos, 2, &g.table[g.cur], 4);
    pos = put_io(r, pos);
    if (g.want_keys) {
        uint8_t k[320] = { 0 };
        int i;

        for (i = 0; i < 10; i++)
            memcpy(k + 32 * i, chart_key[i], 16);
        pos = put(r, pos, 5, k, sizeof(k));
        g.want_keys = 0;
    }
    if (g.want_cut) {
        pos = put(r, pos, 7, g.cut, sizeof(g.cut));
        g.want_cut = 0;
    }
    if (g.want_fix) {                        /* sensor thresholds (IOBoard + 32) */
        pos = put(r, pos, 9, g.fix, sizeof(g.fix));
        g.want_fix = 0;
    }
    if (g.want_coinrec) {
        uint8_t rec[172] = { 0 };

        pos = put(r, pos, 18, rec, sizeof(rec));
        g.want_coinrec = 0;
    }
    if (g.want_net) {                        /* as message 16 left it; zero: not registered */
        pos = put(r, pos, 15, g.nv.net, 16);
        g.want_net = 0;
    }
    return pos;
}

static uint32_t exchange(const uint8_t *req, uint32_t len, uint8_t *reply, uint32_t max)
{
    static uint8_t p[E2100_REQRESP_MAX], s0[256];
    uint32_t n = 0, off, rn, i;
    uint8_t sum;

    if (len >= 8 && len <= sizeof(p)) {
        n = len - 8;
        g.echo = req[0];
        memcpy(s0, g.sbox, sizeof(s0));
        memcpy(p, req + 8, n);
        rc4_crypt(p, n);
        if (!well_formed(p, n)) {
            rc4_key(boot_key, sizeof(boot_key));
            memcpy(p, req + 8, n);
            rc4_crypt(p, n);
            if (!well_formed(p, n)) {
                LOG("rx message garbled (%u bytes)", n);
                memcpy(g.sbox, s0, sizeof(s0));
                n = 0;
            }
        }
    }
    for (off = 0; off + 8 <= n; ) {
        uint32_t type = ld32(p + off), mlen = ld32(p + off + 4);

        message(type, p + off + 8, mlen);
        if (type == 0 && off == 0)
            rc4_key(p + 8, mlen);
        off += 8 + mlen;
    }
    if (max < REPLY_MAX)
        return 0;
    rn = build(reply + 12);
    rc4_crypt(reply + 12, rn);
    sum = g.echo;
    for (i = 0; i < rn; i++)
        sum += reply[12 + i];
    memset(reply, 0, 12);
    reply[0] = sum;
    reply[4] = g.echo;
    return 12 + rn;
}

/* ---- input and overlay -------------------------------------------------------- */

/*
 * SDL 1.2 key symbols.  Drums: the game's own developer keyboard (GameKey's
 * OIS table, sensor order of IOBoard): per player a left and a right edge,
 * the big drum's two halves and the small drum's two halves.
 */
static const struct { int sym; int sensors; } drums[] = {
    { 'a', 1 << 0 }, { 'f', 1 << 1 }, { 'w', 1 << 2 }, { 's', 1 << 3 }, { 'd', 1 << 4 }, { 'e', 1 << 5 },
    { ' ', 3 << 3 },                          /* Space: both halves of the big drum */
    { 'j', 1 << 6 }, { ';', 1 << 7 }, { 'i', 1 << 8 }, { 'k', 1 << 9 }, { 'l', 1 << 10 }, { 'o', 1 << 11 },
    { 13, 3 << 9 },                           /* Enter: player 2's big drum */
};

static void key(int sym, int down)
{
    unsigned i;
    int k = -1;

    if (sym == 303 || sym == 304) {           /* Shift: the numeric keypad is player 2's */
        g.shift = down;
        return;
    }
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
    if (sym == 283 || sym == '9') {           /* F2 test, 9 service */
        uint32_t bit = sym == 283 ? 1u << 24 : 1u << 25;

        g.keys = down ? g.keys | bit : g.keys & ~bit;
        return;
    }
    if (sym >= 256 && sym <= 265)             /* keypad 0..9 */
        k = sym - 256;
    else if (sym == 268)                      /* keypad * */
        k = 10;
    else if (sym == 267)                      /* keypad / */
        k = 11;
    if (k >= 0) {
        uint32_t bit = 1u << pad_bit[g.shift][k];

        g.keys = down ? g.keys | bit : g.keys & ~bit;
        return;
    }
    for (i = 0; i < sizeof(drums) / sizeof(drums[0]); i++)
        if (drums[i].sym == sym)
            g.sensors = down ? g.sensors | drums[i].sensors : g.sensors & ~drums[i].sensors;
}
static void draw(int w, int h)
{
    static const char *lines[] = {
        "5 coin  9 service  F2 test",
        "P1 A F S D W E Space  keypad",
        "P2 J ; K L I O Enter  F1 hide",
    };
    const struct igs_core *c = api->core;
    int i, n = 3, x, y;

    if (!g.help)
        return;
    /* bottom right: the screens' timers are at the top left */
    x = w - 34 * c->font_w;
    y = 4;
    c->fill(x, y, 34 * c->font_w, n * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < n; i++)
        c->text(x + 6, y + (n - 1 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

static void status(char *buf, size_t n)
{
    static const char *const names[36] = {
        [S_LOGO] = "logo", [S_COINPAGE] = "coin page", [S_LOGIN] = "login", [S_SIGN] = "sign up",
        [S_SELTYPE] = "select type", [S_SELMODE] = "select mode", [S_SELEV] = "select event",
        [S_SELSKILL] = "select skill", [S_SELLEVEL] = "select level", [S_SELSONG] = "select song",
        [S_STAGE] = "song", [S_RESULT] = "result", [S_FINAL] = "final result", [S_HOUSE] = "house",
        [S_PHOTO] = "photo", [S_RANK] = "ranking", [S_THANK] = "thanks", [S_SELROLE] = "select role",
        [S_MATCH] = "match", [S_DRAGON] = "dragon board", [S_SETTING] = "operator menu",
        [S_INFOMATCH] = "match board", [S_NETRANK] = "net ranking", [S_DRAGON2] = "dragon board",
    };

    if (g.cur == S_STAGE && g.demo)
        snprintf(buf, n, "demo");
    else if (g.cur >= 0 && g.cur < 36 && names[g.cur])
        snprintf(buf, n, "%s", names[g.cur]);
    else
        snprintf(buf, n, "state %d", g.cur);
}

static void reset(struct e2100_channel *ch)
{
    const char *v;
    int i;

    (void)ch;
    rc4_key(boot_key, sizeof(boot_key));
    nv_load();
    g.cur = S_LOGO;
    g.have_table = g.booted = 0;
    g.help = 1;
    g.t0 = api->core->now();
    v = getenv("PM3_VELOCITY");
    g.level = v && *v ? atoi(v) : 160;
    if (g.level < 1 || g.level > 255)
        g.level = 160;
    for (i = 0; i < 12; i++) {
        g.fix[i][0] = 255;
        g.fix[i][1] = 1;
        g.fix[i][2] = g.fix[i][3] = 40;
    }
}

static const struct e2100_game game = {
    .version = E2100_API_VERSION,
    .name = "Percussion Master 3",
    .reset = reset,
    .key = key,
    .draw = draw,
    .transport = E2100_TRANSPORT_REQRESP,
    .exchange = exchange,
    .status = status,
    .asset_mask = asset_mask,
};

__attribute__((visibility("default")))
const struct e2100_game *e2100_game_entry(const struct e2100_api *a)
{
    api = a;
    return &game;
}
