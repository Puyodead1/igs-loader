/*
 * Pirate Revenge /fec_data/gameset.bin: "SHIP_800" + SaveData (754 words,
 * sship 0x8796480), the operator settings, daily and monthly bookkeeping and
 * the ranking.  Written by SettingSaveData (0x8076F20); the game loads the
 * block from the IGS036 (ShareramOperator_ShareramToOSD), never from the file.
 * Field meanings from SettingDataInit, SettingCoinSet, SettingSoundSet,
 * SettingSoundTest, SettingVRSetting, SettingRudderTest, SettingTicketSetting,
 * SettingDifficultySetting, SettingSetPassword, SettingBookkeeping,
 * SettingGetMonthBookkeep, BookkeepAddRecord, BookkeepGameEnd,
 * SettingSavePlayInfo2, RankingSaveData / RankingGetData.  wiki Format-PRSET.
 */
#include <string.h>

#include "igsa.h"

#define WORDS 754
#define SIZE (8 + 4 * WORDS)

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n == SIZE && !memcmp(d, "SHIP_800", 8) ? 100 : 0;
}

static int32_t w(const uint8_t *d, int i) { return (int32_t)igsa_le32(d + 8 + 4 * i); }

static const char *const days_f[] = { "year", "month0", "day", "coins", "plays", "tickets", "service",
                                      "power_on_min", "idle_min" };
static const char *const months_f[] = { "year", "month0", "coins", "plays", "tickets", "service", "days" };
static const char *const lang[] = { "English", "Chinese", "Chinese (2)" };
static const char *const diff[] = { "Super Easy", "Easy", "Normal", "Hard", "Super Hard" };

static int parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    int i, j;

    if (c->n != SIZE || memcmp(d, "SHIP_800", 8))
        return igsa_fail(c, "%zu bytes: not SHIP_800 + %d words", c->n, WORDS);
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "Pirate Revenge gameset");
    igsa_jstr(c, "magic", "SHIP_800");

    igsa_jobj(c, "settings");
    igsa_jint(c, "language", w(d, 0));
    if (w(d, 0) >= 0 && w(d, 0) <= 2)
        igsa_jstr(c, "language_name", lang[w(d, 0)]);
    igsa_jbool(c, "free_play", w(d, 1));
    igsa_jint(c, "coin_rate_coins", w(d, 2));
    igsa_jint(c, "coin_rate_credits", w(d, 3));
    igsa_jint(c, "continue_coins", w(d, 4));
    igsa_jint(c, "master_volume", w(d, 5));
    igsa_jint(c, "play_volume", w(d, 6));
    igsa_jint(c, "demo_volume", w(d, 7));
    igsa_jint(c, "rudder_right", w(d, 742));
    igsa_jint(c, "rudder_left", w(d, 743));
    igsa_jint(c, "rudder_center", w(d, 744));
    igsa_jint(c, "motor_test_function", w(d, 745));
    igsa_jstr(c, "motor_test_function_name", w(d, 745) ? "Motor" : "Mechanism");
    igsa_jint(c, "motor_rest_delay_min", w(d, 746));
    igsa_jint(c, "motor_test_time_min", w(d, 747));
    igsa_jbool(c, "ticket_dispenser", w(d, 748));
    igsa_jint(c, "max_tickets", w(d, 749));
    igsa_jint(c, "min_tickets", w(d, 750));
    igsa_jint(c, "word_751", w(d, 751));
    igsa_jint(c, "difficulty", w(d, 752));
    if (w(d, 752) >= 0 && w(d, 752) <= 4)
        igsa_jstr(c, "difficulty_name", diff[w(d, 752)]);
    igsa_jint(c, "password", w(d, 753));
    igsa_jend(c);

    igsa_jobj(c, "today");
    igsa_jint(c, "year", w(d, 8));
    igsa_jint(c, "month0", w(d, 9));
    igsa_jint(c, "day", w(d, 10));
    igsa_jint(c, "day_index", w(d, 11));
    igsa_jint(c, "month_index", w(d, 336));
    igsa_jint(c, "last_save_time", w(d, 737));
    igsa_jint(c, "power_on_s", w(d, 738));
    igsa_jint(c, "play_s", w(d, 739));
    igsa_jint(c, "coins_pending", w(d, 740));
    igsa_jint(c, "credits", w(d, 741));
    igsa_jend(c);

    igsa_jarr(c, "days");
    for (i = 0; i < 36; i++) {
        igsa_jobj(c, NULL);
        igsa_jint(c, "index", i);
        for (j = 0; j < 9; j++)
            igsa_jint(c, days_f[j], w(d, 12 + 9 * i + j));
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);
    igsa_jarr(c, "months");
    for (i = 0; i < 36; i++) {
        igsa_jobj(c, NULL);
        igsa_jint(c, "index", i);
        for (j = 0; j < 7; j++)
            igsa_jint(c, months_f[j], w(d, 337 + 7 * i + j));
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);

    igsa_jarr(c, "ranking");
    for (i = 0; i < 20; i++) {
        const uint8_t *r = d + 8 + 4 * (589 + 7 * i);
        char name[4];

        for (j = 0; j < 3; j++)
            name[j] = r[4 * j] < 26 ? (char)('A' + r[4 * j]) : '?';
        name[3] = 0;
        igsa_jobj(c, NULL);
        igsa_jint(c, "rank", i + 1);
        igsa_jstr(c, "name", name);
        igsa_jarr(c, "name_codes");
        for (j = 0; j < 3; j++)
            igsa_jint(c, NULL, w(d, 589 + 7 * i + j));
        igsa_jend(c);
        igsa_jint(c, "score", w(d, 589 + 7 * i + 3));
        igsa_jint(c, "max_hit", w(d, 589 + 7 * i + 4));
        igsa_jint(c, "ship", w(d, 589 + 7 * i + 5));
        igsa_jint(c, "title", r[24]);
        igsa_jbytes(c, "pad", r + 25, 3);
        igsa_jend(c);
        c->entries++;
    }
    igsa_jend(c);

    igsa_jarr(c, "routes");      /* GameStage 0 / 1 */
    for (i = 0; i < 2; i++) {
        igsa_jobj(c, NULL);
        igsa_jint(c, "route", i);
        igsa_jint(c, "starts", w(d, 729 + 4 * i));
        igsa_jint(c, "continues", w(d, 730 + 4 * i));
        igsa_jint(c, "staff_rolls", w(d, 731 + 4 * i));
        igsa_jint(c, "avg_play_s", w(d, 732 + 4 * i));
        igsa_jend(c);
    }
    igsa_jend(c);
    igsa_jend(c);
    return 0;
}

const igsa_format igsa_fmt_prset = { "prset", "Pirate Revenge gameset.bin: settings, bookkeeping, ranking", probe, parse };
