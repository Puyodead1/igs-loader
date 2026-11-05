/*
 * FPGA-encrypted WAVs (the E2100 sound/<name>.wav): the whole file, padded to 16
 * bytes, under the asset cipher (wiki Sound Crypto).  Claimed only after
 * igsa_open decrypted it; plain WAVs are standard files.
 */
#include <string.h>

#include "igsa.h"

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return igsa_probe_decrypted && n >= 44 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4) ? 100 : 0;
}

static int parse(igsa_ctx *c)
{
    const uint8_t *d = c->d;
    size_t riff = (size_t)igsa_le32(d + 4) + 8, o = 12, data = 0, dn = 0;
    unsigned fmt = 0, ch = 0, rate = 0, bits = 0;
    char name[300];
    const char *b = strrchr(c->path, '/'), *b2 = strrchr(c->path, '\\');

    if (b2 > b)
        b = b2;
    if (riff > c->n || c->n - riff >= 16 + 16)  /* the cipher pads to 16 */
        return igsa_fail(c, "RIFF size %zu, file %zu", riff, c->n);
    while (o + 8 <= riff) {
        uint32_t len = igsa_le32(d + o + 4);

        if (o + 8 + len > riff)
            return igsa_fail(c, "chunk %.4s at %zu runs past RIFF", (const char *)d + o, o);
        if (!memcmp(d + o, "fmt ", 4) && len >= 16) {
            fmt = igsa_le16(d + o + 8);
            ch = igsa_le16(d + o + 10);
            rate = igsa_le32(d + o + 12);
            bits = igsa_le16(d + o + 22);
        } else if (!memcmp(d + o, "data", 4)) {
            data = o + 8;
            dn = len;
        }
        o += 8 + len + (len & 1);
    }
    if (!fmt || !data || !ch || !rate)
        return igsa_fail(c, "no fmt/data chunk");
    igsa_jobj(c, NULL);
    igsa_jstr(c, "format", "FPGA-encrypted WAV");
    igsa_jint(c, "codec", fmt);
    igsa_jint(c, "channels", ch);
    igsa_jint(c, "rate", rate);
    igsa_jint(c, "bits", bits);
    igsa_juint(c, "data_bytes", dn);
    igsa_juint(c, "padding", c->n - riff);
    igsa_jend(c);
    snprintf(name, sizeof(name), "%s", b ? b + 1 : c->path);
    return igsa_emit_raw(c, name, d, riff);
}

const igsa_format igsa_fmt_fpgawav = { "fpga-wav", "FPGA-encrypted WAV (E2100 sound): decrypted to a plain WAV", probe, parse };
