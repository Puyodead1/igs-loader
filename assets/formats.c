/* every libigsa format, in probe order (igsa.h) */
#include "igsa.h"

extern const igsa_format igsa_fmt_act, igsa_fmt_fon, igsa_fmt_fpgawav;
extern const igsa_format igsa_fmt_tsgrom, igsa_fmt_tsgfont, igsa_fmt_sod, igsa_fmt_mac, igsa_fmt_sndtab,
    igsa_fmt_tsgdemo, igsa_fmt_keylog;
extern const igsa_format igsa_fmt_prset, igsa_fmt_ggbbook, igsa_fmt_ggbinfo, igsa_fmt_ggbtest, igsa_fmt_pfrank,
    igsa_fmt_pfvol, igsa_fmt_pfstg, igsa_fmt_keyintab, igsa_fmt_keyinbox, igsa_fmt_keyindir, igsa_fmt_storename;
extern const igsa_format igsa_fmt_zpk, igsa_fmt_zpkhdr, igsa_fmt_zlmodel, igsa_fmt_zlscene, igsa_fmt_zlcam,
    igsa_fmt_zxp, igsa_fmt_zlshader, igsa_fmt_zlmap, igsa_fmt_sd2rpd, igsa_fmt_sd2car, igsa_fmt_sd2ai,
    igsa_fmt_sd2bin, igsa_fmt_sd2txt, igsa_fmt_sd2tex;   /* Speed Driver 2 */
extern const igsa_format igsa_fmt_wedoemi, igsa_fmt_wedozpk, igsa_fmt_wedochart, igsa_fmt_wedoeff, igsa_fmt_wedolist,
    igsa_fmt_wedotable, igsa_fmt_wedoavset, igsa_fmt_wedokeyerr, igsa_fmt_wedozhl, igsa_fmt_wedobook, igsa_fmt_wedosound,
    igsa_fmt_wedostore, igsa_fmt_wedonet, igsa_fmt_wedogameset, igsa_fmt_wedo_titles, igsa_fmt_wedo_titletest;   /* We Dance */
extern const igsa_format igsa_fmt_pm3chart, igsa_fmt_pm3songs, igsa_fmt_pm3book, igsa_fmt_pm3setting,
    igsa_fmt_pm3seltime, igsa_fmt_pm3total, igsa_fmt_pm3rank, igsa_fmt_pm3top, igsa_fmt_pm3hot, igsa_fmt_pm3vol,
    igsa_fmt_pm3ccd, igsa_fmt_pm3list, igsa_fmt_srchamp, igsa_fmt_srrank, igsa_fmt_srghost, igsa_fmt_srvehicle,
    igsa_fmt_srdesc, igsa_fmt_srinfo, igsa_fmt_srhe, igsa_fmt_srtexinf, igsa_fmt_zipsep, igsa_fmt_osmenc, igsa_fmt_herobook,
    igsa_fmt_herocards, igsa_fmt_herotime, igsa_fmt_herofont, igsa_fmt_sbin, igsa_fmt_actogre;   /* Ogre games */

const igsa_format *const igsa_formats[] = {
    &igsa_fmt_act,
    &igsa_fmt_fon,
    &igsa_fmt_fpgawav,
    &igsa_fmt_tsgrom, &igsa_fmt_tsgfont, &igsa_fmt_sod, &igsa_fmt_mac, &igsa_fmt_sndtab,
    &igsa_fmt_tsgdemo, &igsa_fmt_keylog,
    &igsa_fmt_prset, &igsa_fmt_ggbbook, &igsa_fmt_ggbinfo, &igsa_fmt_ggbtest, &igsa_fmt_pfrank,
    &igsa_fmt_pfvol, &igsa_fmt_pfstg, &igsa_fmt_keyintab, &igsa_fmt_keyinbox, &igsa_fmt_keyindir,
    &igsa_fmt_storename,
    &igsa_fmt_zpk, &igsa_fmt_zpkhdr, &igsa_fmt_zlmodel, &igsa_fmt_zlscene, &igsa_fmt_zlcam,
    &igsa_fmt_zxp, &igsa_fmt_zlshader, &igsa_fmt_zlmap, &igsa_fmt_sd2rpd, &igsa_fmt_sd2car,
    &igsa_fmt_sd2ai, &igsa_fmt_sd2bin, &igsa_fmt_sd2txt, &igsa_fmt_sd2tex,   /* Speed Driver 2 */
    &igsa_fmt_wedoemi, &igsa_fmt_wedozpk, &igsa_fmt_wedochart, &igsa_fmt_wedoeff, &igsa_fmt_wedolist,
    &igsa_fmt_wedotable, &igsa_fmt_wedoavset, &igsa_fmt_wedokeyerr, &igsa_fmt_wedozhl, &igsa_fmt_wedobook,
    &igsa_fmt_wedosound, &igsa_fmt_wedostore, &igsa_fmt_wedonet, &igsa_fmt_wedogameset, &igsa_fmt_wedo_titles,
    &igsa_fmt_wedo_titletest,   /* We Dance */
    &igsa_fmt_pm3chart, &igsa_fmt_pm3songs, &igsa_fmt_pm3book, &igsa_fmt_pm3setting, &igsa_fmt_pm3seltime,
    &igsa_fmt_pm3total, &igsa_fmt_pm3rank, &igsa_fmt_pm3top, &igsa_fmt_pm3hot, &igsa_fmt_pm3vol, &igsa_fmt_pm3ccd,
    &igsa_fmt_pm3list, &igsa_fmt_srchamp, &igsa_fmt_srrank, &igsa_fmt_srghost, &igsa_fmt_srvehicle, &igsa_fmt_srdesc,
    &igsa_fmt_srinfo, &igsa_fmt_srhe, &igsa_fmt_srtexinf, &igsa_fmt_zipsep, &igsa_fmt_osmenc, &igsa_fmt_herobook,
    &igsa_fmt_herocards, &igsa_fmt_herotime, &igsa_fmt_herofont, &igsa_fmt_sbin, &igsa_fmt_actogre,   /* Ogre games */
    NULL
};
