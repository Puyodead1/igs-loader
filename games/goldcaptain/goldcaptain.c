/*
 * Gold Captain (E2000): what its IGS036 firmware does, as far as the game
 * needs.  docs/GoldCaptain.md.
 *
 * Every exchange the game sends 8 words:
 *   [2] lamp bits (word_8701440), [3]/[7] a state byte, [4] an event byte,
 *   [5] command (0 = none, up to 22), [6] its argument;
 * and takes back:
 *   [5] button bits, [6] VR (the lever, 0..512), [9] ticket state,
 *   [12]/[13] coin 1/2 pulses since the last reply, [14] tickets paid out,
 *   [15] ack: the command being acknowledged (the game waits for it),
 *   [16] the command's result, [17..] bulk results (version text, tables).
 *
 * The IGS036 also keeps game state.  Emulated here:
 *   6 allocate a sprite group, 7 free one, 8 get / 9 set / 10 add a field of
 *   a group's table (arg = value << 11 | field << 8 | group).
 * Reconstructed, since the IGS036's internal ROM is not dumped: 5 stage
 * parameters (from the older SOD copies), 15 the weapon table (values made
 * up from how the code uses each field), 19 the game-flow state machine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../machines/e2000/e2000.h"

#define REPLY_WORDS 17
#define GROUPS      128

#define NV_WORDS    970           /* operator settings and bookkeeping */
#define NV_FILE     "var/igs036.nv"

enum {
    CMD_TICKETS_ADD = 4, CMD_WARZONE = 5,
    CMD_GROUP_ALLOC = 6, CMD_GROUP_FREE = 7, CMD_GET = 8, CMD_SET = 9, CMD_ADD = 10,
    CMD_NV_STORE = 13, CMD_NV_LOAD = 14, CMD_WEAPONS = 15, CMD_VERSION = 17,
    CMD_FLOW = 19, CMD_TICKETS_OWED = 20, CMD_TICKETS_OUT = 21, CMD_TICKETS_RESET = 22,
};

#define TICKET_EXCHANGES 12       /* exchanges per ticket paid out (about 0.2 s) */

/* lever travel per exchange (about 60 a second): GC_STEER overrides the turn rate */
#define VR_TURN   3               /* centre to full lock in about 1.4 s */
#define VR_RETURN 8

/*
 * Game flow (command 19): the IGS036 names the next scene for an event.
 * Reconstructed from the scenes' code (main loop sub_8075BE0), not dumped:
 *   scenes  1 title, 2 ship select, 3 stage progress, 4 area pick,
 *           5 fight stage (SOD type 0), 6 type-1 stage, 7 boss stage (type 2),
 *           9 type-3 stage, 10 ending, 11 results / game over, 12 leave the game.
 *   events  0 continue declined, 1 fight over, 3 game start, 4/5/6 area picked
 *           with a type-0/2/3 stage, 7 results done, 8 boss beaten, 9 ending done,
 *           10 type-3 stage over, 11 ship chosen, 12 last chapter cleared,
 *           13 chapter continues, 14 test switch.
 * Scene 0 is a hidden "GAME TEST SELECT" menu: never chosen.
 * A chosen ship goes to scene 3 first: it advances the stage counter, which
 * starts at -1, and sends 13 (next area) or 12 (game complete).  From the
 * second chapter on, scene 3 itself switches to scene 11, which ends the game.
 */
static const int8_t flow_next[] = {
    [0] = 11, [1] = 3, [3] = 2, [4] = 5, [5] = 7, [6] = 9, [7] = 12, [8] = 10,
    [9] = 3, [10] = 3, [11] = 3, [12] = 10, [13] = 4, [14] = 12,
};

/*
 * Weapon table (command 15): 168 object types x 8 words.  The game's names
 * come from Pirate Revenge (same engine, symbols kept): WeaponDataDefault,
 * WeaponPlayerAttackPlus / SpeedPlus / RangePlus / ReloadMinus.
 *   [0] attack: a ship it hits loses attack * (100 - armour) / 100
 *       (types 47..162 scaled up per chapter)
 *   [1] speed: forward move per frame, animation vector * speed / 10
 *   [2] range: shot dies once it has travelled this far
 *   [3] reload: frames between shots
 *   [4] life: two shots that meet each lose the other's life (max 999)
 *   [5] unused
 *   [6] hit half-width at [7]; [7] hit-cone length (0: enemy shots never hit)
 * Types 0..46 are the player's weapons, 47..162 enemy and boss shots,
 * 163..167 the pick-up "weapon X" specials.  The values are not the
 * IGS036's: they are chosen to play sensibly.
 */
#define WEAPON_TYPES 168

static void weapon_record(uint32_t type, uint32_t *w)
{
    static const uint32_t player[8] = { 40, 40, 4000, 12, 40, 0, 40, 0 };
    static const uint32_t enemy[8] = { 8, 20, 4000, 90, 20, 0, 60, 300 };
    static const uint32_t special[8] = { 200, 40, 4000, 30, 999, 0, 80, 0 };

    memcpy(w, type < 47 ? player : type < 163 ? enemy : special, 8 * sizeof(*w));
}

#define WARZONE_MAX 30

/* SDL 1.2 key symbols (the core translates X11 key events to these) */
enum {
    K_SPACE = 32, K_5 = 53, K_6 = 54, K_x = 120, K_z = 122,
    K_UP = 273, K_DOWN = 274, K_RIGHT = 275, K_LEFT = 276,
    K_F1 = 282, K_F2 = 283, K_RCTRL = 305, K_LCTRL = 306,
};

/* button bits in reply word 5 (the game's key table byte_8701320) */
enum {
    B_START = 1 << 0,    /* GameKey: start / fire (keyboard fallback: space) */
    B_RBTN = 1 << 1,
    B_LBTN = 1 << 2,
    B_SETTING = 1 << 3,
    B_SERVICE = 1 << 4,
    B_TEST5 = 1 << 5,
    B_TEST6 = 1 << 6,
    B_TICKET = 1 << 7,   /* ticket switch */
};

static const struct e2000_api *api;

static struct {
    uint32_t buttons;
    int left, right;
    int vr;                       /* 0..512, 256 = centre */
    int steer_rate;               /* VR units per exchange while a key is held */
    uint32_t coins[2];
    int32_t tickets_owed;         /* the ticket dispenser: still to pay out (word 14) */
    int dispensing, dispense_wait;
    int help;
    int32_t table[GROUPS][3];     /* the IGS036's sprite-group fields */
    uint32_t nv[NV_WORDS];        /* its NVRAM, kept in the game's var/ */
    int nv_valid;
    int32_t warzone[WARZONE_MAX]; /* the loading stage's WarZone values */
} g;

/*
 * The stage files the game loads (sod/release36/) have their WarZone
 * section zeroed: the IGS036 supplies it (command 5).  The older copies in
 * sod/ still carry it, so read it from there when the game opens a stage.
 */
static void warzone_load(const char *path)
{
    static const char tag[] = "                  Sod37_WarZone";  /* 32-byte field */
    const char *base = strstr(path, "/release36/");
    char old[256], buf[64];
    FILE *f;
    long n = 0;
    int c, i;

    memset(g.warzone, 0xff, sizeof(g.warzone));   /* -1: no zone (sodReader_IsWarZoneEmpty) */
    if (!base)
        return;
    snprintf(old, sizeof(old), "%.*s/%s", (int)(base - path), path, base + 11);
    if (!(f = fopen(old, "rb")))
        return;
    /* find the section name, then a 6-byte decimal length and the data */
    i = 0;
    while ((c = fgetc(f)) != EOF) {
        i = c == tag[i] ? i + 1 : c == tag[0] ? 1 : 0;
        if (i == (int)sizeof(tag) - 1)
            break;
    }
    if (c != EOF && fgetc(f) != EOF && fread(buf, 1, 6, f) == 6) {
        buf[6] = 0;
        n = strtol(buf, NULL, 10);
        if (n > (long)sizeof(g.warzone))
            n = sizeof(g.warzone);
        n = fread(g.warzone, 1, n, f);
    }
    fclose(f);
    api->core->log("goldcaptain: %s: WarZone from %s (%ld bytes): %d %d %d", path, old, n,
                   g.warzone[0], g.warzone[1], g.warzone[2]);
}

static void gc_file_opened(const char *path)
{
    if (strstr(path, ".sod"))
        warzone_load(path);
}

static void gc_script(void);

/*
 * The wheel calibration (test menu "VR setting"): NVRAM words 958..960 hold
 * the raw wheel at full lock one way, the other way, and centred.  The raw
 * value is 100 - VR / 5.12, so the lever's 0..512 gives 100..0.  Factory
 * settings leave all three 0, which pins the steering to one side, so fill
 * in the lever's own range until an operator calibrates.
 */
#define NV_VR 958

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
    if (g.nv_valid)
        nv_calibrate();
}

static void nv_save(void)
{
    FILE *f = fopen(NV_FILE, "wb");

    if (!f) {
        api->core->log("goldcaptain: cannot write %s", NV_FILE);
        return;
    }
    fwrite(g.nv, 4, NV_WORDS, f);
    fclose(f);
}

/* one command; its result goes to reply[16], bulk data from reply[17] */
static uint32_t gc_command(uint32_t cmd, uint32_t arg, const uint32_t *req, uint32_t n,
                           uint32_t *reply, uint32_t *words)
{
    uint32_t grp = arg & 0x7f, field = (arg >> 8) & 7, i;
    int32_t value = (int32_t)arg >> 11;

    if (field > 2)
        field = 2;
    switch (cmd) {
    /*
     * The ticket dispenser (Pirate Revenge names: IOCardOther_AddTicketOut 4,
     * _GetResumeTicket 20, _TicketOut 21, _InitSSR 22).  The game watches
     * reply word 14 (tickets still to pay) change and word 9 bits 2/3 for faults.
     */
    case CMD_TICKETS_ADD:
        g.tickets_owed = (int32_t)arg < 0 ? 0 : g.tickets_owed + (int32_t)arg;
        return 0;
    case CMD_TICKETS_OWED:
        return g.tickets_owed;
    case CMD_TICKETS_OUT:
        g.dispensing = 1;
        return 0;
    case CMD_TICKETS_RESET:
        g.dispensing = 0;
        return 0;
    case CMD_WARZONE:
        /* arg = entry << 11 | field << 8 | difficulty, players, area, type bits */
        i = (arg >> 11) * 3 + field;
        return i < WARZONE_MAX ? (uint32_t)g.warzone[i] : 0;
    case CMD_FLOW:
        if (arg < sizeof(flow_next) && flow_next[arg]) {
            api->core->log("goldcaptain: flow event %u -> scene %d", arg, flow_next[arg]);
            return flow_next[arg];
        }
        api->core->log("goldcaptain: flow event %u unknown, leaving the game", arg);
        return 12;
    case CMD_GROUP_ALLOC:
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
        /* the settings follow the 8-word header; arg is their sum */
        if (n >= 8 + NV_WORDS) {
            memcpy(g.nv, req + 8, sizeof(g.nv));
            nv_calibrate();
            g.nv_valid = 1;
            nv_save();
        }
        return 0;
    case CMD_NV_LOAD:
        /* result 1 = NVRAM blank: the game writes its factory settings back */
        memcpy(reply + 17, g.nv, sizeof(g.nv));
        *words = 17 + NV_WORDS;
        return !g.nv_valid;
    case CMD_WEAPONS:
        for (i = 0; i < WEAPON_TYPES; i++)
            weapon_record(i, reply + 17 + 8 * i);
        *words = 17 + WEAPON_TYPES * 8;
        return 0;
    case CMD_VERSION: {
        /* words 17..23: char[8], short, char[10], char[8] ("36IntRom:" + the last) */
        char v[28] = { 0 };

        memcpy(v, "IGS036", 6);
        memcpy(v + 10, "emulated", 8);
        memcpy(v + 20, "HLE", 3);
        memcpy(reply + 17, v, sizeof(v));
        *words = 24;
        return 0;
    }
    default:
        return 0;
    }
}

static const struct e2000_bios bios[] = {
    { 0xFE0C1, "L4S5MG3" },
    { 0xFEC84, "6A6IXE19C-00" },
    { 0xFECDF, "I6A6IXE19" },
    { 0, NULL },
};

static void gc_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.vr = 256;
    g.steer_rate = getenv("GC_STEER") ? atoi(getenv("GC_STEER")) : VR_TURN;
    if (g.steer_rate < 1)
        g.steer_rate = VR_TURN;
    g.help = 1;
    nv_load();
}

static uint32_t gc_exchange(const uint32_t *req, uint32_t n, uint32_t *reply)
{
    uint32_t words = REPLY_WORDS;
    int target, step;

    gc_script();
    /* VR 512 is full left lock (raw wheel 100 - VR / 5.12, see nv_calibrate) */
    target = g.left && !g.right ? 512 : g.right && !g.left ? 0 : 256;

    /*
     * Turn the wheel at a hand's pace: holding a key steers further the
     * longer it is held; letting go springs back to the centre faster.
     */
    step = target == 256 ? VR_RETURN : g.steer_rate;
    g.vr += target > g.vr ? (target - g.vr < step ? target - g.vr : step)
          : target < g.vr ? -(g.vr - target < step ? g.vr - target : step) : 0;

    /* a command is sent once (the game clears it after copying it out) */
    if (req && n > 6 && req[5]) {
        if (api->core->verbose || (req[5] != CMD_GET && req[5] != CMD_SET && req[5] != CMD_ADD))
            api->core->log("goldcaptain: command %u (0x%x)", req[5], req[6]);
        reply[16] = gc_command(req[5], req[6], req, n, reply, &words);
    }
    reply[5] = g.buttons;
    reply[6] = g.vr;
    reply[9] = 0;
    reply[12] = g.coins[0];
    reply[13] = g.coins[1];
    g.coins[0] = g.coins[1] = 0;
    if (g.dispensing && g.tickets_owed > 0 && ++g.dispense_wait >= TICKET_EXCHANGES) {
        g.dispense_wait = 0;
        g.tickets_owed--;
        api->core->log("goldcaptain: ticket out, %d to go", g.tickets_owed);
    }
    if (!g.tickets_owed)
        g.dispensing = 0;
    reply[14] = g.tickets_owed;
    reply[15] = req && n > 5 ? req[5] : 0;
    return words;
}

static void gc_key(int sym, int down)
{
    uint32_t bit = 0;

    switch (sym) {
    case K_LEFT: g.left = down; return;
    case K_RIGHT: g.right = down; return;
    case K_SPACE: case K_LCTRL: case K_UP: bit = B_START; break;
    case K_z: bit = B_LBTN; break;
    case K_x: bit = B_RBTN; break;
    case K_F2: bit = B_SETTING; break;
    case K_F1:
        if (down)
            g.help = !g.help;
        return;
    case 57: bit = B_SERVICE; break;                    /* 9 */
    case K_5: if (down) g.coins[0]++; return;
    case K_6: if (down) g.coins[1]++; return;
    default: return;
    }
    if (down)
        g.buttons |= bit;
    else
        g.buttons &= ~bit;
}

static void gc_draw(int w, int h)
{
    static const char *lines[] = {
        "5 coin  Space start/fire",
        "Left/Right lever  Z/X L/R",
        "F2 setting  9 service",
        "F1 hide this",
    };
    const struct igs_core *c = api->core;
    int i, y;

    (void)w;
    if (!g.help)
        return;
    y = h - 4 - 4 * c->font_h;
    c->fill(0, y - 4, 26 * c->font_w, 4 * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < 4; i++)
        c->text(6, y + (3 - i) * c->font_h + 6, lines[i], 1, 1, 0.6f);
}

/*
 * GC_SCRIPT="sec:key[+],..." presses a key at a time from start-up (key: coin,
 * start, left, right, l, r, setting, service).  A press lasts 0.2 s, or with
 * a trailing + until the next entry: for testing without a keyboard.  Runs
 * from the exchange, i.e. every frame.
 */
static void gc_script(void)
{
    static const struct { const char *name; int sym; } keys[] = {
        { "coin", K_5 }, { "start", K_SPACE }, { "left", K_LEFT }, { "right", K_RIGHT },
        { "l", K_z }, { "r", K_x }, { "setting", K_F2 }, { "service", 57 },
    };
    static const char *p;
    static double t0, release_at;
    static int held;
    const struct igs_core *c = api->core;
    double now, at;
    char name[16];
    int n = 0, hold, i;

    if (!p) {
        p = c->env("GC_SCRIPT", "");
        t0 = c->now();
    }
    now = c->now() - t0;
    if (held && release_at && now >= release_at) {
        gc_key(held, 0);
        held = 0;
    }
    if (!*p || sscanf(p, "%lf:%15[a-z]%n", &at, name, &n) != 2 || now < at)
        return;
    hold = p[n] == '+';
    p += n + hold;
    if (*p == ',')
        p++;
    if (held)
        gc_key(held, 0);
    held = 0;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (!strcmp(name, keys[i].name)) {
            held = keys[i].sym;
            gc_key(held, 1);
            release_at = hold ? 0 : now + 0.2;
            c->log("goldcaptain: script %.1f s %s%s", now, name, hold ? " (held)" : "");
        }
}

static void gc_status(char *buf, size_t n)
{
    if (g.tickets_owed > 0)
        snprintf(buf, n, "paying %d tickets", g.tickets_owed);
    else
        buf[0] = 0;
}

static const struct e2000_game game = {
    .version = E2000_API_VERSION,
    .name = "goldcaptain",
    .key_to_game = "PRIGS036",
    .key_from_game = "PR_LINUX",
    .bios = bios,
    .reset = gc_reset,
    .exchange = gc_exchange,
    .key = gc_key,
    .draw = gc_draw,
    .file_opened = gc_file_opened,
    .status = gc_status,
};

__attribute__((visibility("default")))
const struct e2000_game *e2000_game_entry(const struct e2000_api *a)
{
    api = a;
    return &game;
}
