/*
 * Pirate Revenge (E2000): what its IGS036 firmware does, as far as the game
 * needs.  docs/PirateRevenge.md.  The executable keeps its symbols; the names
 * below are the game's.
 *
 * The transport and message layout are Gold Captain's (same engine,
 * app/c/shareram_operator.c).  Every exchange the game sends 8 words:
 *   [2] motor (IOCardMotorValue), [3]/[7] lamps, [4] IOCardMsgTo27,
 *   [5] command (0 = none, up to 22), [6] its argument;
 * and takes back:
 *   [5] key bits (IOCardKeyValue), [6] VR (the rudder, 0..512), [9]
 *   IOCardMsgFrom27 (ticket SSR faults), [12]/[13] coin 1/2 counters,
 *   [14] tickets still to pay, [15] ack (the command answered), [16] its
 *   result, [17..] bulk results.
 *
 * Emulated: 2/3 add to a coin counter, 6..10 the sprite-group store, 13/14
 * the settings (NVRAM), 4/20/21/22 the ticket dispenser.  Reconstructed,
 * since the IGS036's internal ROM is not dumped: 5 stage WarZones (from the
 * stage file), 15 the weapon table (values made up), 19 the game flow.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2000/e2000.h"

#define REPLY_WORDS 17
#define GROUPS      128

#define NV_WORDS    754           /* SaveData: operator settings and bookkeeping */
#define NV_FILE     "../../fec_data/igs036.nv"   /* next to the game's gameset.bin */

enum {
    CMD_COIN1_ADD = 2, CMD_COIN2_ADD = 3, CMD_TICKETS_ADD = 4, CMD_WARZONE = 5,
    CMD_GROUP_ALLOC = 6, CMD_GROUP_FREE = 7, CMD_GET = 8, CMD_SET = 9, CMD_ADD = 10,
    CMD_NV_STORE = 13, CMD_NV_LOAD = 14, CMD_WEAPONS = 15, CMD_FUSES = 16, CMD_VERSION = 17,
    CMD_FLOW = 19, CMD_TICKETS_OWED = 20, CMD_TICKETS_OUT = 21, CMD_TICKETS_RESET = 22,
};

#define TICKET_EXCHANGES 12       /* exchanges per ticket paid out (about 0.2 s) */

/* rudder travel per exchange (about 60 a second): PR_STEER overrides the turn rate */
#define VR_TURN   3
#define VR_RETURN 8

/*
 * Coin counters (IOCardCoin_IsCoin1In): the game takes the difference from
 * the last value it saw, as a signed byte, and ignores decreases.  A
 * counter past COIN_WRAP is reset to 0 once no coin has come for
 * COIN_IDLE exchanges, so it never wraps through a coin.
 */
#define COIN_WRAP 100
#define COIN_IDLE 120

/*
 * Game flow (command 19): the IGS036 names the next scene (GameStatus) for
 * an event.  Reconstructed from SuperShipGame's scenes, not dumped:
 *   scenes  0 GameTestSelect, 1 GameStart, 2 GameSelectShip, 3 GameNextStage,
 *           4 GameSelectMode, 5 GameFight, 6 GameRace, 7 GameFightBoss,
 *           9 GameStaff, 10 GameResult, 11 GameOver, 12 leave the game.
 *   events  0 continue declined (GameContinue), 1 fight over, 2 game start
 *           (SuperShipGame), 3 title done, 4/5/6 mode picked with a SOD type
 *           0/2/3 stage (type 1 runs GameRace itself), 7 game over done,
 *           8 boss beaten, 9 result done, 10 staff stage over, 11 ship chosen,
 *           12 last stage done or quota missed (GameNextStage), 13 next
 *           stage, 14 test switch.
 * GameNextStage advances the stage counter (starts at -1, 2 stages per
 * chapter) and sends 12 or 13.  Scene 0 (hidden test menu) is never chosen.
 */
static const int8_t flow_next[] = {
    [0] = 11, [1] = 3, [2] = 1, [3] = 2, [4] = 5, [5] = 7, [6] = 9, [7] = 12, [8] = 10,
    [9] = 3, [10] = 3, [11] = 3, [12] = 11, [13] = 4, [14] = 12,
};

/*
 * Weapon table (command 15, WeaponInit -> WeaponDataDefault): 168 types x 8
 * words, the fields as in docs/IGS_E2000.md (Weapon record).  Types 0..46
 * are the player's, 47..162 enemy and boss shots, 163..167 the pick-up
 * specials.  The values are not the IGS036's: chosen to play sensibly.
 */
#define WEAPON_TYPES 168

static void weapon_record(uint32_t type, uint32_t *w)
{
    static const uint32_t player[8] = { 40, 40, 4000, 12, 40, 0, 40, 0 };
    static const uint32_t enemy[8] = { 8, 20, 4000, 90, 20, 0, 60, 300 };
    static const uint32_t special[8] = { 200, 40, 4000, 30, 999, 0, 80, 0 };

    memcpy(w, type < 47 ? player : type < 163 ? enemy : special, 8 * sizeof(*w));
}

#define WARZONE_MAX 30            /* 10 zones x 3 fields */

/* SDL 1.2 key symbols (the core translates X11 key events to these) */
enum {
    K_SPACE = 32, K_5 = 53, K_7 = 55, K_9 = 57, K_x = 120, K_z = 122,
    K_UP = 273, K_RIGHT = 275, K_LEFT = 276,
    K_F1 = 282, K_F2 = 283, K_LCTRL = 306,
};

/* key bits in reply word 5 (CRKey_Init: CRKey[i] tests bit i) */
enum {
    B_RIGHT = 1 << 0,    /* ControlerRight*: right button (game's own key m) */
    B_LEFT = 1 << 1,     /* ControlerLeft*: left button (n) */
    B_BOMB = 1 << 2,     /* ControlerBomb*: bomb (space) */
    B_COIN1 = 1 << 3,    /* coin switches: the counters are words 12/13 */
    B_COIN2 = 1 << 4,
    B_TEST = 1 << 5,     /* CRKey_IsTestTouch */
    B_SERVICE = 1 << 6,  /* CRKey_IsServicePush */
    B_TICKET = 1 << 7,   /* CRKey_IsTicketOut */
};

static const struct e2000_api *api;

static struct {
    uint32_t buttons;
    int left, right;
    int vr;                       /* 0..512, 256 = centre */
    int steer_rate;               /* VR units per exchange while a key is held */
    int32_t coins[2];             /* the counters the game reads (words 12/13) */
    int coin_idle;
    int32_t tickets_owed;         /* the ticket dispenser: still to pay out (word 14) */
    int dispensing, dispense_wait;
    int help;
    int32_t table[GROUPS][3];     /* the IGS036's sprite-group fields */
    uint32_t nv[NV_WORDS];        /* its NVRAM */
    int nv_valid;
    int32_t warzone[WARZONE_MAX]; /* the loading stage's WarZone values */
} g;

/*
 * The stage files the game loads (./sod/<name>, sodReader_LoadFile) keep
 * their WarZone section ("Sod37_WarZone": 3 words per zone); the IGS036
 * answers the same values one by one (command 5).  sod/release36/ has the
 * section zeroed, as Gold Captain's does: if the game ever loads from
 * there, the parent copy is read instead.
 */
static void warzone_load(const char *path)
{
    static const char tag[] = "                  Sod37_WarZone";  /* 32-byte field */
    const char *rel = strstr(path, "/release36/");
    char src[256], buf[8];
    FILE *f;
    long n = 0;
    int c, i;

    memset(g.warzone, 0xff, sizeof(g.warzone));   /* -1: no zone (sodReader_IsWarZoneEmpty) */
    if (rel)
        snprintf(src, sizeof(src), "%.*s/%s", (int)(rel - path), path, rel + 11);
    else
        snprintf(src, sizeof(src), "%s", path);
    if (!(f = fopen(src, "rb")))
        return;
    i = 0;
    while ((c = fgetc(f)) != EOF) {
        i = c == tag[i] ? i + 1 : c == tag[0] ? 1 : 0;
        if (i == (int)sizeof(tag) - 1)
            break;
    }
    /* NUL, a 6-byte decimal length (NUL-padded), the data */
    if (c != EOF && fgetc(f) != EOF && fread(buf, 1, 6, f) == 6) {
        buf[6] = 0;
        n = strtol(buf, NULL, 10);
        if (n > (long)sizeof(g.warzone))
            n = sizeof(g.warzone);
        n = fread(g.warzone, 1, n, f);
    }
    fclose(f);
    api->core->log("piraterevenge: %s: WarZone from %s (%ld bytes): %d %d %d", path, src, n,
                   g.warzone[0], g.warzone[1], g.warzone[2]);
}

static void pr_file_opened(const char *path)
{
    static int busy;   /* on Linux our own fopen comes back here (the core interposes it) */

    if (busy || !strstr(path, ".sod"))
        return;
    busy = 1;
    warzone_load(path);
    busy = 0;
}

static void pr_script(void);

/*
 * The rudder calibration (SaveData words 742..744, GameRudder_Init): the raw
 * rudder at both locks and centred; raw = 100 - VR / 5.12.  Factory
 * settings (SettingDataInit) leave all three 0, which divides by zero in
 * ControlerValue, so fill in the rudder's own range until an operator
 * calibrates in the test menu.
 */
#define NV_VR 742

static void nv_calibrate(void)
{
    if (!g.nv[NV_VR] && !g.nv[NV_VR + 1] && !g.nv[NV_VR + 2]) {
        g.nv[NV_VR] = 100;
        g.nv[NV_VR + 1] = 0;
        g.nv[NV_VR + 2] = 50;
    }
}

static void nv_load(void)
{
    FILE *f = fopen(NV_FILE, "rb");

    g.nv_valid = f && fread(g.nv, 4, NV_WORDS, f) == NV_WORDS;
    if (f)
        fclose(f);
    /* also when blank: SettingDataInit keeps these words of the blank reply */
    nv_calibrate();
}

static void nv_save(void)
{
    FILE *f = fopen(NV_FILE, "wb");

    if (!f) {
        api->core->log("piraterevenge: cannot write %s", NV_FILE);
        return;
    }
    fwrite(g.nv, 4, NV_WORDS, f);
    fclose(f);
}

/* one command; its result goes to reply[16], bulk data from reply[17] */
static uint32_t pr_command(uint32_t cmd, uint32_t arg, const uint32_t *req, uint32_t n,
                           uint32_t *reply, uint32_t *words)
{
    uint32_t grp = arg & 0x7f, field = (arg >> 8) & 7, i;
    int32_t value = (int32_t)arg >> 11;

    if (field > 2)
        field = 2;
    switch (cmd) {
    case CMD_COIN1_ADD:   /* IOCardCoin_Init zeroes the counters with these */
        g.coins[0] += (int32_t)arg;
        return 0;
    case CMD_COIN2_ADD:
        g.coins[1] += (int32_t)arg;
        return 0;
    case CMD_TICKETS_ADD:   /* IOCardOther_AddTicketOut; negative clears */
        g.tickets_owed = (int32_t)arg < 0 ? 0 : g.tickets_owed + (int32_t)arg;
        return 0;
    case CMD_TICKETS_OWED:  /* IOCardOther_GetResumeTicket */
        return g.tickets_owed;
    case CMD_TICKETS_OUT:   /* IOCardOther_TicketOut */
        g.dispensing = 1;
        return 0;
    case CMD_TICKETS_RESET: /* IOCardOther_InitSSR */
        g.dispensing = 0;
        return 0;
    case CMD_WARZONE:
        /* sodReader_MergeBitFieldForIGS036: zone << 11 | field << 8 | type, players, area, difficulty */
        i = (arg >> 11) * 3 + field;
        return i < WARZONE_MAX ? (uint32_t)g.warzone[i] : 0;
    case CMD_FLOW:
        if (arg < sizeof(flow_next) && flow_next[arg]) {
            api->core->log("piraterevenge: flow event %u -> scene %d", arg, flow_next[arg]);
            return flow_next[arg];
        }
        api->core->log("piraterevenge: flow event %u unknown, leaving the game", arg);
        return 12;
    case CMD_GROUP_ALLOC:   /* ActionGroupUse */
        for (i = 0; i < GROUPS; i++)
            if (!g.table[i][1])
                return i;
        return GROUPS - 1;
    case CMD_GROUP_FREE:
        memset(g.table[grp], 0, sizeof(g.table[grp]));
        return 0;
    case CMD_GET:
        return g.table[grp][field];
    case CMD_SET:
        g.table[grp][field] = value;
        return 0;
    case CMD_ADD:
        g.table[grp][field] += value;
        return 0;
    case CMD_NV_STORE:
        /* ShareramOperator_OSDToShareram: SaveData after the 8-word header, arg = its sum */
        if (n >= 8 + NV_WORDS) {
            memcpy(g.nv, req + 8, sizeof(g.nv));
            nv_calibrate();
            g.nv_valid = 1;
            nv_save();
        }
        return 0;
    case CMD_NV_LOAD:
        /* ShareramOperator_ShareramToOSD: result 1 = blank, the game runs SettingDataInit */
        memcpy(reply + 17, g.nv, sizeof(g.nv));
        *words = 17 + NV_WORDS;
        return !g.nv_valid;
    case CMD_WEAPONS:
        for (i = 0; i < WEAPON_TYPES; i++)
            weapon_record(i, reply + 17 + 8 * i);
        *words = 17 + WEAPON_TYPES * 8;
        return 0;
    case CMD_VERSION: {
        /* Main_GetIGS036InternalRomVersion: words 17..23, the last 8 bytes shown as "36IntRom:" */
        char v[28] = { 0 };

        memcpy(v, "IGS036", 6);
        memcpy(v + 10, "emulated", 8);
        memcpy(v + 20, "HLE", 3);
        memcpy(reply + 17, v, sizeof(v));
        *words = 24;
        return 0;
    }
    default:                /* 11 ActionGroupBackupStatus, 16 fuses/version (0) */
        return 0;
    }
}

static const struct e2000_bios bios[] = {
    { 0xFE0C1, "L4S5MG3" },
    { 0xFEC84, "6A6IXE19C-00" },
    { 0xFECDF, "I6A6IXE19" },
    { 0, NULL },
};

static void pr_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.vr = 256;
    g.steer_rate = getenv("PR_STEER") ? atoi(getenv("PR_STEER")) : VR_TURN;
    if (g.steer_rate < 1)
        g.steer_rate = VR_TURN;
    g.help = 1;
    nv_load();
}

static void coin_insert(int i)
{
    g.coins[i]++;
    g.coin_idle = 0;
}

static uint32_t pr_exchange(const uint32_t *req, uint32_t n, uint32_t *reply)
{
    uint32_t words = REPLY_WORDS;
    int target, step, i;

    pr_script();
    /* VR 512 is full left lock (raw 100 - VR / 5.12, see nv_calibrate) */
    target = g.left && !g.right ? 512 : g.right && !g.left ? 0 : 256;
    step = target == 256 ? VR_RETURN : g.steer_rate;
    g.vr += target > g.vr ? (target - g.vr < step ? target - g.vr : step)
          : target < g.vr ? -(g.vr - target < step ? g.vr - target : step) : 0;

    if (req && n > 6 && req[5]) {
        /* 16 comes every frame while the version shows (Main_ShowVersion) */
        if (api->core->verbose || (req[5] != CMD_GET && req[5] != CMD_SET && req[5] != CMD_ADD &&
                                   req[5] != CMD_FUSES))
            api->core->log("piraterevenge: command %u (0x%x)", req[5], req[6]);
        reply[16] = pr_command(req[5], req[6], req, n, reply, &words);
    }
    if (++g.coin_idle >= COIN_IDLE)
        for (i = 0; i < 2; i++)
            if (g.coins[i] >= COIN_WRAP || g.coins[i] < 0)
                g.coins[i] = 0;
    reply[5] = g.buttons;
    reply[6] = g.vr;
    reply[9] = 0;
    reply[12] = g.coins[0] & 0xffff;
    reply[13] = g.coins[1] & 0xffff;
    if (g.dispensing && g.tickets_owed > 0 && ++g.dispense_wait >= TICKET_EXCHANGES) {
        g.dispense_wait = 0;
        g.tickets_owed--;
        api->core->log("piraterevenge: ticket out, %d to go", g.tickets_owed);
    }
    if (!g.tickets_owed)
        g.dispensing = 0;
    reply[14] = g.tickets_owed;
    reply[15] = req && n > 5 ? req[5] : 0;
    return words;
}

/*
 * Keys.  The game's own SDL sees the keyboard too, and its debug keys
 * (DebugCheckKey) are live in this build: 6 enters the test menu, Esc
 * quits, Enter turns the I/O card off, t, c, p, b, u, v, 0, 1, 2, 8, Tab
 * and ` do other things.  None of them is used here.
 */
static void pr_key(int sym, int down)
{
    uint32_t bit = 0;

    switch (sym) {
    case K_LEFT: g.left = down; return;
    case K_RIGHT: g.right = down; return;
    case K_SPACE: case K_LCTRL: case K_UP: bit = B_BOMB; break;
    case K_z: bit = B_LEFT; break;
    case K_x: bit = B_RIGHT; break;
    case K_F2: bit = B_TEST; break;
    case K_9: bit = B_SERVICE; break;
    case K_F1:
        if (down)
            g.help = !g.help;
        return;
    case K_5: if (down) coin_insert(0); return;
    case K_7: if (down) coin_insert(1); return;
    default: return;
    }
    if (down)
        g.buttons |= bit;
    else
        g.buttons &= ~bit;
}

static void pr_draw(int w, int h)
{
    static const char *lines[] = {
        "5/7 coin  Space bomb",
        "Left/Right rudder  Z/X buttons",
        "F2 test  9 service",
        "F1 hide this",
    };
    const struct igs_core *c = api->core;
    int i, y;

    (void)w;
    if (!g.help)
        return;
    y = h - 4 - 4 * c->font_h;
    c->fill(0, y - 4, 31 * c->font_w, 4 * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < 4; i++)
        c->text(6, y + (3 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

/*
 * PR_SCRIPT="sec:key[+],..." presses a key at a time from start-up (key:
 * coin, coin2, bomb, left, right, l, r, test, service).  A press lasts 0.2 s,
 * or with a trailing + until the next entry.  Runs from the exchange.
 */
static void pr_script(void)
{
    static const struct { const char *name; int sym; } keys[] = {
        { "coin", K_5 }, { "coin2", K_7 }, { "bomb", K_SPACE }, { "left", K_LEFT },
        { "right", K_RIGHT }, { "l", K_z }, { "r", K_x }, { "test", K_F2 }, { "service", K_9 },
    };
    static const char *p;
    static double t0, release_at;
    static int held;
    const struct igs_core *c = api->core;
    double now, at;
    char name[16];
    int n = 0, hold, i;

    if (!p) {
        p = c->env("PR_SCRIPT", "");
        t0 = c->now();
    }
    now = c->now() - t0;
    if (held && release_at && now >= release_at) {
        pr_key(held, 0);
        held = 0;
    }
    if (!*p || sscanf(p, "%lf:%15[a-z0-9]%n", &at, name, &n) != 2 || now < at)
        return;
    hold = p[n] == '+';
    p += n + hold;
    if (*p == ',')
        p++;
    if (held)
        pr_key(held, 0);
    held = 0;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (!strcmp(name, keys[i].name)) {
            held = keys[i].sym;
            pr_key(held, 1);
            release_at = hold ? 0 : now + 0.2;
            c->log("piraterevenge: script %.1f s %s%s", now, name, hold ? " (held)" : "");
        }
}

static void pr_status(char *buf, size_t n)
{
    if (g.tickets_owed > 0)
        snprintf(buf, n, "paying %d tickets", g.tickets_owed);
    else
        buf[0] = 0;
}

static const struct e2000_game game = {
    .version = E2000_API_VERSION,
    .name = "piraterevenge",
    .key_to_game = "PRIGS036",
    .key_from_game = "PR_LINUX",
    .bios = bios,
    .reset = pr_reset,
    .exchange = pr_exchange,
    .key = pr_key,
    .draw = pr_draw,
    .file_opened = pr_file_opened,
    .status = pr_status,
};

__attribute__((visibility("default")))
const struct e2000_game *e2000_game_entry(const struct e2000_api *a)
{
    api = a;
    return &game;
}
