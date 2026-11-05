/* zl engine formats (fmt_zl.c, fmt_zltext.c) and the zlPack archive (fmt_zpk.c) */
#ifndef FMT_ZL_H
#define FMT_ZL_H

#include <stddef.h>
#include <stdint.h>

/* set by the zpk parser while it parses members: another member by path, or NULL */
extern const uint8_t *(*igsa_zl_sibling)(const char *name, size_t *n);

#endif
