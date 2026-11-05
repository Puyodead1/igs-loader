/*
 * A SafeNet Sentinel Keys USB dongle, emulated at its client API: the game
 * links the Sentinel Keys client statically and talks to the key through
 * /var/run/sentinel/u.daemon; the plugin replaces the client's entry points
 * (igs_core.hook) with the key's behaviour.  wiki Fish Hunter Final 6.
 */
#ifndef SENTINEL_H
#define SENTINEL_H

#include <stdint.h>

#include "../../core/igs.h"

#define SNT_RAW_MAX 256        /* bytes in the key's raw data object */

struct sentinel_key {
    uint32_t developer_id;     /* SFNTGetLicense's developer id */
    uint32_t raw_feature;      /* the raw data object's feature id */
    uint32_t aes_feature;      /* the AES feature's id */
    uint8_t aes_key[16];       /* its key (inside the dongle) */
    uint8_t raw[SNT_RAW_MAX];  /* the data object's plaintext; stored AES-encrypted */
    uint32_t raw_len;          /* a multiple of 16 */
};

/*
 * Find the client's API entries in the game executable and hook those the
 * games call; k is kept.  The number of entries hooked, or -1.
 */
int sentinel_install(const struct igs_core *core, const struct sentinel_key *k);

#endif
