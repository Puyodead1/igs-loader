/*
 * Hero of Robots: NFC tags standing in for the QR cards and the points card.
 * The reader code (nfc.c) is built into the preloaded card library
 * (libcasmcard-hero), which exports hero_nfc_api(); the ASIC plugin
 * (heroofrobots.c) finds it at run time.  docs/HeroOfRobots.md, NFC.
 *
 * Tags: NTAG215 (or any Type 2 tag with an NDEF capability container) on a
 * PC/SC reader (ACR122U), or a 540-byte file (IGS_NFC_FAKE).  The loader's
 * data is one NDEF external record, type "igs-loader:hero", 32-byte
 * payload:
 *   0  "HERO"          4  version (1)       5  flags: 1 card, 2 points
 *   8  card code, ASCII, NUL-padded (12)   20  points, big-endian (2)
 *  24  write count, little-endian (4)      28  CRC-32 of bytes 0..27, LE (4)
 */
#ifndef HERO_NFC_H
#define HERO_NFC_H

#include <stdint.h>

#define HERO_NFC_VERSION 1
#define HERO_NFC_SYMBOL "hero_nfc_api"

/* HERO_NFC: features */
enum { NFC_SCAN = 1, NFC_DISPENSE = 2, NFC_POINTS = 4 };

/* what is on the reader */
enum { TAG_NONE, TAG_BLANK, TAG_LOADER, TAG_FOREIGN };

struct hero_nfc_tag {
    int state;                  /* TAG_* */
    unsigned serial;            /* +1 for every tag placed on the reader */
    uint8_t uid[10];
    int uid_len;
    char code[13];              /* TAG_LOADER: card code, "" none */
    int points;                 /* TAG_LOADER: points, -1 none */
};

struct hero_nfc {
    int version;
    unsigned features;          /* NFC_* */
    /* the tag on the reader (a copy; never blocks on the reader) */
    void (*get)(struct hero_nfc_tag *t);
    /* write a dispensed card's code to the next blank or card-less loader
       tag placed within HERO_NFC_WAIT seconds (queued) */
    void (*dispense)(const char *code);
    /* the points of tag `serial` (queued; the copy changes at once);
       0 if that tag is gone or not writable */
    int (*set_points)(unsigned serial, int points);
};

typedef const struct hero_nfc *(*hero_nfc_api_fn)(void);

#endif
