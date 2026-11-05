/*
 * Percussion Master 3 media/rom/<name>.rom: the zl engine's action groups with the magic
 * "ACT_OGRE" (Ogre textures referenced by name in TEXTURE0 chunks, no embedded
 * pictures).  Same chunk layout as ACT_ZL3D, parsed by fmt_act.c.  wiki Format-ACT.
 */
#include <string.h>

#include "igsa.h"

extern const igsa_format igsa_fmt_act;

static int probe(const uint8_t *d, size_t n, const char *path)
{
    (void)path;
    return n >= 12 && !memcmp(d, "ACT_OGRE", 8) ? 100 : 0;
}

static int parse(igsa_ctx *c)
{
    return igsa_fmt_act.parse(c);
}

const igsa_format igsa_fmt_actogre = { "actogre", "ACT_OGRE action group (Percussion Master 3): texture names and actions", probe,
                                       parse };
