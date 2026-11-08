/*
 * Speed Driver 3 (E3000) game plugin: the Speed engine's chip side
 * (speed_chip.h) with this game's flow table and Lua chunks.  wiki Speed
 * Driver 3.
 */
#include "speed_chip.h"
#include "speed_lua.h"

/*
 * The main flow (game.state.map: 1 Logo .. 51 DownloadPatch), rebuilt from
 * the scripts (each branch's meaning, the autotest flows).
 */
static const uint8_t flow[52][5] = {
    [1] = { 5, 2, 43 },        /* Logo: key, done, TEST */
    [2] = { 5, 3, 43 },        /* IGSLogo */
    [3] = { 5, 4, 43 },        /* Warning */
    [4] = { 5, 43 },           /* Demo: key or done, TEST */
    [5] = { 8, 7, 43 },        /* CoinPage: start, timeout, TEST */
    [6] = { 5, 49, 43 },       /* Announcement */
    [7] = { 5, 6, 43 },        /* Ranking */
    [8] = { 19, 9, 17, 16 },   /* UseCard: new card, card, no card, zombie card */
    [9] = { 12, 13, 10 },      /* InformKicked */
    [10] = { 12, 11 },         /* InformCaptain */
    [11] = { 12, 12 },         /* InformTeamName */
    [12] = { 0, 12, 13 },      /* InformCup: next trophy, done */
    [13] = { 14, 40 },         /* InformLocked */
    [14] = { 16, 15 },         /* StoreOrIdentity */
    [15] = { 16 },             /* Store */
    [16] = { 24 },             /* Identity */
    [17] = { 20 },             /* SelectCar_NoCard */
    [18] = { 21 },             /* SelectCar_NewCard */
    [19] = { 18 },             /* SelectCharacter */
    [20] = { 24 },             /* ATMT_NoCard */
    [21] = { 22 },             /* ATMT_NewCard */
    [22] = { 23 },             /* SignUp */
    [23] = { 24, 24 },         /* ConfirmCard */
    [24] = { 25 },             /* SetVersusState */
    [25] = { 27, 26, 28 },     /* SingleMode: TA, story, crash */
    [26] = { 29 },             /* SelectStory */
    [27] = { 30 }, [28] = { 30 }, [29] = { 30 },   /* SoloTA, SoloCrash, SoloStory */
    [30] = { 36 },             /* AdjustCar */
    [31] = { 32, 33 },         /* VersusMode */
    [32] = { 34 }, [33] = { 34 },                  /* NetTA, NetCrash */
    [34] = { 30 },             /* Weather */
    [35] = { 36 },             /* Local */
    [36] = { 46 },             /* Game -> Dispatch */
    [37] = { 46 }, [38] = { 46 }, [41] = { 46 }, [42] = { 46 },
    [45] = { 46 }, [47] = { 46 }, [48] = { 46 },
    [39] = { 25, 40 },         /* Continue: yes, no */
    [40] = { 5 },              /* GameOver */
    [43] = { 44 },             /* OperatorSetting */
    [44] = { 1 },              /* OperatorSettingCleanUp */
    [49] = { 1, 43, 5 },       /* HowToPlay: done, TEST, key */
};

static const char chunk0[] = SPEED_LUA_MACHINE SPEED_LUA_END;
static const char chunk1[] = SPEED_LUA_BOOT("SD3Main");
/* the sizes the game asks for (its arguments 24836 7056 3728), less the trailer */
_Static_assert(sizeof(chunk0) - 1 + 16 <= 7056, "lib.machine chunk too large");
_Static_assert(sizeof(chunk1) - 1 + 16 <= 3728, "boot chunk too large");

static const struct speed_title title = {
    .name = "Speed Driver 3",
    .log = "speeddriver3",
    .fram = "/SD3/readwrite/a36_fram.bin",
    .flow = flow,
    .flow_states = 52,
    .chunk = { chunk0, chunk1 },
    .handshake = E2100_HS_LADDER,
};
