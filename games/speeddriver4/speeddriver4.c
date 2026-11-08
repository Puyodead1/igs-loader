/*
 * Speed Driver 4 (E3000) game plugin: the Speed engine's chip side
 * (games/speeddriver3/speed_chip.h) with this game's flow table, its Lua
 * chunks (a newer lib.machine, sd4_lua.h) and the cipher's CBC mode.
 * wiki Speed Driver 4.
 */
#include "../speeddriver3/speed_chip.h"
#include "../speeddriver3/speed_lua.h"
#include "sd4_lua.h"

/*
 * The main flow (game.state.map: 1 Logo .. 58 InformMoney), rebuilt from
 * game/sd3_main.lua, its submain files and the test scripts.  New against
 * Speed Driver 3: a card read goes to InformTitle (56); SingleMode's
 * branches 3 and 4 are Solo_QP (51) and Slam (52); a solo stage goes to
 * DeclarePath (53).
 */
static const uint8_t flow[59][5] = {
    [1] = { 5, 2, 43 },             /* Logo: key, done, TEST */
    [2] = { 5, 3, 43 },             /* IGSLogo */
    [3] = { 5, 4, 43 },             /* Warning */
    [4] = { 5, 43 },                /* Demo: key or done, TEST */
    [5] = { 8, 7, 43 },             /* CoinPage: start, timeout, TEST */
    [6] = { 5, 49, 43 },            /* Announcement */
    [7] = { 5, 6, 43 },             /* Ranking */
    [8] = { 19, 56, 17, 16 },       /* UseCard: new card, card, no card, zombie */
    [9] = { 12, 13, 10 },           /* InformKicked */
    [10] = { 12, 11 },              /* InformCaptain */
    [11] = { 12, 12 },              /* InformTeamName */
    [12] = { 0, 12, 13 },           /* InformCup: next cup, done */
    [13] = { 14, 40 },              /* InformLocked */
    [14] = { 16, 15 },              /* StoreOrIdentity */
    [15] = { 16 },                  /* Store */
    [16] = { 24 },                  /* Identity */
    [17] = { 20 },                  /* SelectCar_NoCard */
    [18] = { 21 },                  /* SelectCar_NewCard */
    [19] = { 18 },                  /* SelectCharacter */
    [20] = { 24 },                  /* ATMT_NoCard */
    [21] = { 22 },                  /* ATMT_NewCard */
    [22] = { 23 },                  /* SignUp */
    [23] = { 24, 24 },              /* ConfirmCard */
    [24] = { 25 },                  /* SetVersusState */
    [25] = { 27, 26, 28, 51, 52 },  /* SingleMode: TA, story, crash, QP, slam */
    [26] = { 29 },                  /* SelectStory */
    [27] = { 53 }, [28] = { 53 }, [29] = { 53 },  /* SoloTA, SoloCrash, SoloStory */
    [30] = { 36 },                  /* AdjustCar */
    [31] = { 32, 33 },              /* VersusMode */
    [32] = { 34 }, [33] = { 34 },   /* NetTA, NetCrash */
    [34] = { 30 },                  /* Weather */
    [35] = { 36 },                  /* Local */
    [36] = { 46 },                  /* Game -> Dispatch */
    [37] = { 46 }, [38] = { 46 }, [41] = { 46 }, [42] = { 46 },
    [45] = { 46 }, [47] = { 46 }, [48] = { 46 },
    [39] = { 25, 40 },              /* Continue: again, end */
    [40] = { 5 },                   /* GameOver */
    [43] = { 44 },                  /* OperatorSetting */
    [44] = { 1 },                   /* OperatorSettingCleanUp */
    [49] = { 1, 43, 5 },            /* HowToPlay: done, TEST, key */
    [50] = { 51 },                  /* SelectCar_QP */
    [51] = { 53 },                  /* Solo_QP */
    [56] = { 57 },                  /* InformTitle */
};

/*
 * The cipher's CBC mode: 64 KiB chunk k of a request is under the mask XORed
 * with these (measured from the scripts: every chunk's trailer checks out)
 */
static const uint8_t cbc_delta[15][16] = {
    { 0x13, 0x24, 0x4a, 0x3c, 0x07, 0x75, 0x50, 0x5d, 0x18, 0x30, 0xdd, 0xc5, 0xc6, 0x45, 0x2d, 0xe5 },
    { 0x0b, 0x37, 0xdc, 0x00, 0x1f, 0x8e, 0x4b, 0x91, 0x7d, 0x6a, 0xa5, 0x94, 0x6d, 0xf8, 0xb2, 0x0c },
    { 0xad, 0x5a, 0xfa, 0x8a, 0x87, 0x0c, 0x2c, 0xca, 0xc1, 0xb5, 0xce, 0x36, 0x8c, 0x59, 0xb7, 0x24 },
    { 0xe2, 0xf9, 0xe4, 0x5f, 0xa5, 0x6b, 0x4b, 0xf6, 0xa8, 0xcb, 0xc2, 0x28, 0x0a, 0xb0, 0xd5, 0x2e },
    { 0x26, 0x0c, 0xcc, 0x79, 0xaf, 0xfc, 0xec, 0xa5, 0x27, 0xbf, 0xb4, 0xfb, 0x7e, 0x69, 0x0b, 0xa6 },
    { 0x11, 0x84, 0x9e, 0xe4, 0xdf, 0x19, 0x6c, 0x02, 0x04, 0x15, 0x92, 0xfa, 0xf2, 0x21, 0x58, 0x31 },
    { 0x15, 0x9b, 0xd9, 0xbc, 0x16, 0xa3, 0x35, 0xb2, 0xac, 0x02, 0x77, 0x63, 0x9f, 0x30, 0x5b, 0xd8 },
    { 0x54, 0xf0, 0x94, 0x4c, 0x64, 0x12, 0x4d, 0x9b, 0xf3, 0xf3, 0x8b, 0xea, 0x4e, 0x04, 0x77, 0xb8 },
    { 0x3e, 0x7e, 0x51, 0x6b, 0x1b, 0x12, 0x77, 0x78, 0xa7, 0xa1, 0xaa, 0x6b, 0x12, 0x92, 0xce, 0x2e },
    { 0x9e, 0x8b, 0x1a, 0x3a, 0x9a, 0x3c, 0x49, 0xb9, 0x4d, 0xc9, 0x83, 0xef, 0x08, 0x88, 0xd3, 0xdf },
    { 0xff, 0xf8, 0x00, 0x0e, 0x9e, 0x54, 0x5b, 0x34, 0xb9, 0xa5, 0x80, 0x43, 0x89, 0xfd, 0xbb, 0xce },
    { 0x61, 0x74, 0x0b, 0x54, 0xeb, 0x85, 0xd3, 0x62, 0xc3, 0x45, 0x07, 0x41, 0xa3, 0xab, 0x57, 0xdc },
    { 0x22, 0xa0, 0x10, 0x13, 0xf0, 0x68, 0x3e, 0xd8, 0x82, 0xad, 0xf6, 0xea, 0xf2, 0x30, 0x9f, 0xef },
    { 0x2d, 0xb6, 0xb8, 0x3b, 0x5d, 0x79, 0xc6, 0xf2, 0xb8, 0xb5, 0x71, 0x7f, 0x19, 0x05, 0x1d, 0x3f },
    { 0x16, 0x76, 0xd9, 0x1c, 0x83, 0xd3, 0xd8, 0x26, 0xc5, 0x08, 0xcd, 0xea, 0x23, 0xa0, 0x42, 0xef },
};

static const char chunk0[] = SD4_LUA_MACHINE SD4_LUA_GLOBALS;
static const char chunk1[] = SPEED_LUA_BOOT("SD3Main");
/* the sizes the game asks for (its arguments 24636 9344 2016), less the trailer */
_Static_assert(sizeof(chunk0) - 1 + 16 <= 9344, "lib.machine chunk too large");
_Static_assert(sizeof(chunk1) - 1 + 16 <= 2016, "boot chunk too large");

static const struct speed_title title = {
    .name = "Speed Driver 4",
    .log = "speeddriver4",
    .fram = "/SD4/readwrite/a36_fram.bin",
    .flow = flow,
    .flow_states = 59,
    .chunk = { chunk0, chunk1 },
    .handshake = E2100_HS_ADV,
    .cbc_delta = cbc_delta,
    .cbc_chunks = 15,
};
