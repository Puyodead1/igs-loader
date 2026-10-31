/* SHA-1 (set verification) and CRC-32 (zip/7z integrity checks). */
#include <string.h>

#include "romset.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;

        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void rs_sha1_init(struct rs_sha1 *c)
{
    static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };

    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
}

void rs_sha1_update(struct rs_sha1 *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    size_t have = (size_t)(c->len & 63);

    c->len += n;
    if (have) {
        size_t k = 64 - have < n ? 64 - have : n;

        memcpy(c->buf + have, p, k);
        p += k;
        n -= k;
        if (have + k < 64)
            return;
        block(c->h, c->buf);
    }
    for (; n >= 64; p += 64, n -= 64)
        block(c->h, p);
    memcpy(c->buf, p, n);
}

void rs_sha1_final(struct rs_sha1 *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t have = (size_t)(c->len & 63), np = have < 56 ? 56 - have : 120 - have;
    int i;

    for (i = 0; i < 8; i++)
        pad[np + i] = (uint8_t)(bits >> (56 - 8 * i));
    rs_sha1_update(c, pad, np + 8);
    for (i = 0; i < 20; i++)
        out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void rs_sha1(const void *data, size_t n, uint8_t out[20])
{
    struct rs_sha1 c;

    rs_sha1_init(&c);
    rs_sha1_update(&c, data, n);
    rs_sha1_final(&c, out);
}

uint32_t rs_crc32(uint32_t crc, const void *data, size_t n)
{
    static uint32_t table[256];
    const uint8_t *p = data;

    if (!table[1]) {
        uint32_t i, j, r;

        for (i = 0; i < 256; i++) {
            for (r = i, j = 0; j < 8; j++)
                r = r & 1 ? (r >> 1) ^ 0xEDB88320 : r >> 1;
            table[i] = r;
        }
    }
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
