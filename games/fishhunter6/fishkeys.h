/*
 * The six-station fish cabinets' keys (Fish Hunter 6, Fish Hunter Final 6):
 * SDL 1.2 key symbols -> input bits (Player_KeySeq: station p bits 8p+0..5
 * = up, down, right, left, bet, fire; 78 = Test, 104 on eight stations), or a chip action for a
 * station: coin, key-in, key-out, the payout key.  And the key help.
 */
#ifndef FISHKEYS_H
#define FISHKEYS_H

#include "../../core/igs.h"

#define FK_COIN   0x100
#define FK_KEYIN  0x200
#define FK_KEYOUT 0x400
#define FK_PAYOUT 0x800

static const struct { int sym; int bit; } fish_keymap[] = {
    { 273, 0 }, { 274, 1 }, { 275, 2 }, { 276, 3 },          /* arrows */
    { 308, 4 }, { 307, 4 }, { 306, 5 }, { 305, 5 },           /* Alt bet, Ctrl fire */
    { 'r', 8 }, { 'f', 9 }, { 'g', 10 }, { 'd', 11 }, { 's', 12 }, { 'a', 13 },
    { 'i', 16 }, { 'k', 17 }, { 'l', 18 }, { 'j', 19 }, { 'v', 20 }, { 'b', 21 },
    { 264, 24 }, { 258, 25 }, { 262, 26 }, { 260, 27 }, { 266, 28 }, { 256, 29 },
    { 278, 32 }, { 279, 33 }, { 281, 34 }, { 127, 35 }, { 280, 36 }, { 277, 37 },  /* Home End PgDn Del PgUp Ins */
    { 'y', 40 }, { 'n', 41 }, { 'm', 42 }, { 'h', 43 }, { 'z', 44 }, { 'x', 45 },
    { 283, 78 },                                               /* F2 = Test */
    { '5', FK_COIN | 0 }, { '6', FK_COIN | 1 }, { '7', FK_COIN | 2 }, { '8', FK_COIN | 3 },
    { '9', FK_COIN | 4 }, { '0', FK_COIN | 5 },
    { 286, FK_KEYIN | 0 }, { 287, FK_KEYIN | 1 }, { 288, FK_KEYIN | 2 }, { 289, FK_KEYIN | 3 },  /* F5..F8 */
    { '-', FK_KEYIN | 4 }, { '=', FK_KEYIN | 5 },
    { 'q', FK_KEYOUT | 0 }, { 'w', FK_KEYOUT | 1 }, { 'e', FK_KEYOUT | 2 }, { 't', FK_KEYOUT | 3 },
    { 'u', FK_KEYOUT | 4 }, { 'o', FK_KEYOUT | 5 },
    { 290, FK_PAYOUT | 0 }, { 291, FK_PAYOUT | 1 }, { 292, FK_PAYOUT | 2 }, { 293, FK_PAYOUT | 3 },  /* F9..F12 */
    { '[', FK_PAYOUT | 4 }, { ']', FK_PAYOUT | 5 },
};

/* eight-station cabinets (Fish Hunter SKL2 8P): stations 7 and 8, bits 48..61 */
static const struct { int sym; int bit; } fish_keymap8[] = {
    { ',', 51 }, { '.', 50 }, { ';', 48 }, { '/', 49 }, { 'c', 52 }, { 'p', 53 },
    { 263, 59 }, { 265, 58 }, { 267, 56 }, { 268, 57 }, { 269, 60 }, { 270, 61 },   /* KP7 KP9 KP/ KP* KP- KP+ */
    { '1', FK_COIN | 6 }, { '2', FK_COIN | 7 },
    { 284, FK_KEYIN | 6 }, { 285, FK_KEYIN | 7 },                                    /* F3, F4 */
    { '`', FK_KEYOUT | 6 }, { 8, FK_KEYOUT | 7 },                                     /* Backspace */
    { '\\', FK_PAYOUT | 6 }, { '\'', FK_PAYOUT | 7 },
};

/* the key help, top left (the overlay's y runs up), for the first n stations */
static void fish_keys_draw(const struct igs_core *c, int h, int n)
{
    static const char *lines[] = {
        "St Turn    Up/Dn   Fire Bet Coin In Out Pay",
        "1  Lt Rt   Up Dn   Ctrl Alt 5    F5 Q   F9",
        "2  D G     R F     A    S   6    F6 W   F10",
        "3  J L     I K     B    V   7    F7 E   F11",
        "4  KP4 6   KP8 2   KP0  KP. 8    F8 T   F12",
        "5  Del PgD Hom End Ins  PgU 9    -  U   [",
        "6  H M     Y N     X    Z   0    =  O   ]",
        "7  , .     ; /     P    C   1    F3 `   \\",
        "8  KP7 9   KP/ *   KP+  KP- 2    F4 Bs  '",
        "F2 menu  F1 hide this",
    };
    int i, y, rows = n + 2;

    y = h - 4 - rows * c->font_h;
    c->fill(0, y - 4, 45 * c->font_w, rows * c->font_h + 8, 0, 0, 0, 0.6f);
    for (i = 0; i < rows; i++)
        c->text(6, y + (rows - 1 - i) * c->font_h + 6, lines[i == rows - 1 ? 9 : i], 1, 1, 0.6f);
}

#endif
