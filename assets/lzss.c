/*
 * The LZSS the IGS engines use (Okumura's LZSS.C): a 4096-byte ring
 * starting at 4078 filled with spaces, flag bytes LSB first (1 literal,
 * 0 a match of 2 bytes: offset low 8 | (high nibble << 4) << 4, length low
 * nibble + 3).  No end marker: the input length bounds it.
 */
#include <stdlib.h>
#include <string.h>

#include "igsa.h"

int igsa_lzss(const uint8_t *in, size_t n, uint8_t **out, size_t *outn)
{
    uint8_t ring[4096], *o;
    size_t cap = n * 4 + 256, on = 0, i = 0;
    unsigned flags = 0, r = 4078;

    memset(ring, ' ', sizeof(ring));
    if (!(o = malloc(cap)))
        return -1;
    for (;;) {
        unsigned k, len, off;

        flags >>= 1;
        if (!(flags & 0x100)) {
            if (i >= n)
                break;
            flags = in[i++] | 0xFF00;
        }
        if (on + 18 > cap) {
            uint8_t *t = realloc(o, cap * 2);

            if (!t) {
                free(o);
                return -1;
            }
            o = t;
            cap *= 2;
        }
        if (flags & 1) {
            if (i >= n)
                break;
            o[on++] = ring[r] = in[i++];
            r = (r + 1) & 4095;
            continue;
        }
        if (i + 1 >= n)
            break;
        off = in[i] | (in[i + 1] & 0xF0) << 4;
        len = (in[i + 1] & 15) + 3;
        i += 2;
        for (k = 0; k < len; k++) {
            o[on++] = ring[r] = ring[(off + k) & 4095];
            r = (r + 1) & 4095;
        }
    }
    *out = o;
    *outn = on;
    return 0;
}
