/*
 * E1000 machine plugin: the AMD Geode GX2 board and the IGS027A PC card,
 * for E1000 games.
 *
 *   - /dev/mem: the GX2's frame buffer (16 MB of plain memory) and its
 *     register blocks at the physical addresses the PCI BARs report: the
 *     display controller (DC), the 2D engine (GP) and the video processor
 *     (VP) are MMIO traps.  A write to GP_BLT_MODE runs the blit at once
 *     (16 bpp: ROPs, source colour key, constant alpha); mono host data
 *     (the text) is expanded as it arrives through GP_HST_SRC; the status
 *     reads idle.  DC_FB_ST_OFFSET is the scan-out start; the vertical blank bit
 *     follows a 60 Hz clock.
 *   - Port I/O (the game calls iopl and uses in/out): PCI configuration
 *     (0xCF8/0xCFC: the GX2 host bridge and graphics device, their BARs) and
 *     the VSA virtual registers (0xAC1C/0xAC1E: frame buffer size, core
 *     clock).
 *   - /dev/cpu/0/msr: reads 0 (the game then skips the GeodeLink tree).
 *   - /dev/pccard0: the a27.  lseek(sequence) + read(0 bytes) fetches the
 *     chip's packet (the command list for the frame, checksummed, the first
 *     256 bytes XOR the encode table); write sends the executable's answer.
 *     Each read is a frame: the machine shows the frame buffer (core
 *     present: window, overlay, keys, frame cap), then the game plugin
 *     builds the next command list from the last answer.
 * Protocol: wiki E1000.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "e1000.h"

#define FB_SIZE   (16u << 20)
#define FB_PHYS   0x40000000u
#define GP_PHYS   0x41000000u
#define DC_PHYS   0x41004000u
#define VP_PHYS   0x41008000u
#define REGS_SIZE 0x4000u

/* GP (2D engine) registers, byte offsets */
#define GP_DST_OFFSET   0x00
#define GP_SRC_OFFSET   0x04
#define GP_STRIDE       0x08               /* dst pitch | src pitch << 16 */
#define GP_WID_HEIGHT   0x0C               /* width << 16 | height */
#define GP_SRC_COLOR_FG 0x10               /* colour key */
#define GP_SRC_COLOR_BG 0x14               /* colour key mask */
#define GP_PAT_COLOR_0  0x18
#define GP_RASTER_MODE  0x38
#define GP_VECTOR_MODE  0x3C
#define GP_BLT_MODE     0x40
#define GP_BLT_STATUS   0x44
#define GP_HST_SRC      0x48

#define RM_SRC_TRANS    0x00000800u
#define RM_PAT_FLAGS    0x00000700u
#define RM_ALPHA_EN     0x00C00000u
#define RM_ALPHA_OP     0x00300000u
#define RM_ALPHA_SEL    0x000E0000u
#define RM_DEST_FROM_A  0x00010000u
#define BM_SRC_FB       0x0001u
#define BM_SRC_HOST     0x0002u            /* source dwords written to GP_HST_SRC */
#define BM_SRC_MONO     0x0040u            /* rows padded to dwords */
#define BM_SRC_TEXT     0x0080u            /* byte-packed rows (gfx2_text_blt) */
#define BS_HALF_EMPTY   0x0008u            /* GP_BLT_STATUS: room for host data */
#define BM_REVERSE_Y    0x0100u
#define BM_REVERSE_X    0x0200u

/* DC (display controller) registers */
#define DC_DISPLAY_CFG  0x08
#define DC_FB_ST_OFFSET 0x10
#define DC_GFX_PITCH    0x34
#define DC_H_ACTIVE     0x40
#define DC_V_ACTIVE     0x50
#define DC_LINE_CNT     0x6C               /* bit 30: vertical blank */

/* the a27's packet (the executable's g_rBufferRead) and its answer (g_rBufferWrite) */
#define A27_READ_SIZE   0x468
#define A27_DATA        0x68
#define A27_KEY         0x24
#define A27_SUM1        0x26
#define A27_SUM2        0x27
#define A27_ANSWER_DATA 0x18
#define A27_MODE_START  29

enum { DEV_NONE, DEV_MEM, DEV_MSR, DEV_PCCARD };

static const struct igs_core *core;
static const struct e1000_game *game;

#define LOG(...) core->log("e1000: " __VA_ARGS__)
#define VLOG(...) do { if (core->verbose) LOG(__VA_ARGS__); } while (0)

/*
 * The a27 packet's encode table, row 0 (the executable's m_baaEncodeTable
 * at 0x807E760; row 1 follows).  The chip picks the key: with key 0, data
 * byte i is XOR row 0 [i] for the first 256 bytes.
 */
static const uint8_t a27_table[256] = {
    0x31, 0xcb, 0xe0, 0xfe, 0x35, 0x3d, 0x59, 0xeb, 0xd2, 0x5c, 0x2c, 0x32, 0xe1, 0x04, 0xb3, 0x6a,
    0x72, 0x22, 0x98, 0xda, 0xa1, 0x60, 0xfc, 0x88, 0x4d, 0x9a, 0x81, 0x18, 0x8b, 0x65, 0x69, 0x73,
    0x97, 0x92, 0xf0, 0xcf, 0xc7, 0x54, 0x6b, 0x5a, 0x0e, 0x16, 0xd0, 0xb1, 0x74, 0xc4, 0x0d, 0x79,
    0x43, 0x19, 0xbd, 0xb5, 0x56, 0xa4, 0x58, 0x8e, 0xbf, 0xb2, 0x2e, 0xb6, 0xc1, 0x83, 0x82, 0xa0,
    0xb0, 0x1a, 0x0f, 0x17, 0xb4, 0x66, 0x53, 0x1c, 0xbb, 0x4c, 0xf9, 0x36, 0xa7, 0x9f, 0x12, 0x49,
    0xec, 0x9d, 0x2b, 0x03, 0x63, 0x5d, 0x94, 0x61, 0x78, 0x02, 0x1b, 0x7f, 0x62, 0xc0, 0x29, 0x7c,
    0x7a, 0x9b, 0xf3, 0x08, 0xc3, 0xdd, 0x8f, 0xed, 0x23, 0x95, 0x42, 0xc2, 0xd8, 0x44, 0x3b, 0x57,
    0x1d, 0x01, 0x87, 0x3e, 0xca, 0x96, 0x85, 0x70, 0xc6, 0x14, 0xf6, 0x48, 0x33, 0x4a, 0xef, 0xfb,
    0x37, 0xf8, 0xd5, 0xdb, 0xdf, 0x4e, 0x47, 0x52, 0xe8, 0x8d, 0x07, 0xce, 0xd9, 0x76, 0xd7, 0xea,
    0x9e, 0xa6, 0xcd, 0xee, 0x3f, 0x91, 0xc9, 0x50, 0x41, 0xd6, 0xd4, 0x9c, 0x3c, 0x45, 0x2d, 0xba,
    0xc5, 0xac, 0xa3, 0xcc, 0xb8, 0xaa, 0xb9, 0x1e, 0xd3, 0x75, 0x5b, 0x34, 0x5e, 0x26, 0x2f, 0xe9,
    0xe7, 0x13, 0x8a, 0x06, 0xf4, 0x15, 0xa5, 0xbc, 0xdc, 0x3a, 0x99, 0x25, 0x46, 0x51, 0x0a, 0x77,
    0x05, 0x6f, 0xfa, 0x5f, 0xf2, 0xde, 0xe2, 0xbe, 0x4b, 0x11, 0xae, 0xa2, 0x38, 0x93, 0x0b, 0x68,
    0x6c, 0xa9, 0x39, 0x2a, 0x30, 0x7b, 0x21, 0x28, 0x1f, 0x55, 0x6e, 0x7e, 0x71, 0x64, 0xab, 0x67,
    0xa8, 0xff, 0x24, 0x7d, 0xe5, 0x80, 0x8c, 0xe4, 0x27, 0x10, 0xf7, 0xfd, 0x00, 0xf1, 0x84, 0xc8,
    0xad, 0xb7, 0x20, 0x4f, 0xe6, 0xd1, 0x90, 0x40, 0xf5, 0xe3, 0x86, 0x6d, 0x09, 0x89, 0xaf, 0x0c,
};

/* ---- state -------------------------------------------------------------------- */

static struct {
    pthread_mutex_t lock;
    int fds[8], kinds[8], nfds;
    long pos[8];                         /* lseek position per fd */
    uint8_t *fb;
    void *dc_map, *vp_map;
    uint32_t dc[REGS_SIZE / 4], gp[0x20], vp[0x400];
    uint32_t pci_addr, vsa_index;
    double t0;
    /* a27 */
    uint32_t answer_mode;
    uint8_t answer[0x400], cmd[0x400];
    int answer_n, started;
    uint16_t *tmp;                       /* blit source copy */
    /* a mono host-source blit (text): rows of bits arrive through GP_HST_SRC */
    struct {
        int active, w, h, y, bit0, row_bytes, step, n;
        uint32_t dst;
        uint8_t row[512];
    } host;
} s ={ .lock = PTHREAD_MUTEX_INITIALIZER };

static int fd_kind(int fd)
{
    int i;

    for (i = 0; i < s.nfds; i++)
        if (s.fds[i] == fd)
            return s.kinds[i];
    return DEV_NONE;
}

static long *fd_pos(int fd)
{
    int i;

    for (i = 0; i < s.nfds; i++)
        if (s.fds[i] == fd)
            return &s.pos[i];
    return NULL;
}

/* ---- GP: the 2D engine ------------------------------------------------------------ */

static inline uint16_t rop3(int rop, uint16_t p, uint16_t src, uint16_t d)
{
    uint16_t r = 0;
    int i;

    for (i = 0; i < 8; i++)
        if (rop & (1 << i))
            r |= (i & 4 ? p : ~p) & (i & 2 ? src : ~src) & (i & 1 ? d : ~d);
    return r;
}

/* RGB565 a·x + (255 − a)·y, a in 0..255 */
static inline uint16_t blend565(uint16_t x, uint16_t y, int a)
{
    int b = 255 - a;
    int r = (((x >> 11) & 31) * a + ((y >> 11) & 31) * b) / 255;
    int g = (((x >> 5) & 63) * a + ((y >> 5) & 63) * b) / 255;
    int bl = ((x & 31) * a + (y & 31) * b) / 255;

    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* the blit itself: source pixels (use_src) from s.tmp, w per row, into the frame buffer */
static void gp_draw(uint32_t rm, uint32_t dst, int dp, int w, int h, int use_src)
{
    uint16_t key = s.gp[GP_SRC_COLOR_FG / 4], keymask = s.gp[GP_SRC_COLOR_BG / 4];
    uint16_t pat = s.gp[GP_PAT_COLOR_0 / 4];
    int trans = (rm & RM_SRC_TRANS) != 0, rop = rm & 0xff, x, y;

    for (y = 0; y < h; y++) {
        uint16_t *d = (uint16_t *)(s.fb + dst + (size_t)y * dp);
        const uint16_t *sr = s.tmp + (size_t)y * w;

        if (rm & RM_ALPHA_EN) {
            int sel = rm & RM_ALPHA_SEL, op = rm & RM_ALPHA_OP;
            int a = sel == 0x60000 ? 255 : sel == 0x40000 ? (int)(rm & 0xff) : 255;

            for (x = 0; x < w; x++) {
                uint16_t sv = use_src ? sr[x] : pat, dv = d[x];
                uint16_t A = rm & RM_DEST_FROM_A ? dv : sv, B = rm & RM_DEST_FROM_A ? sv : dv;

                if (trans && (sv & keymask) == (key & keymask))
                    continue;
                if (op == 0x300000)
                    d[x] = blend565(A, B, a);
                else if (op == 0)
                    d[x] = a == 255 ? A : blend565(A, 0, a);
                else
                    d[x] = blend565(B, 0, 255 - a);
            }
        } else if (rop == 0xCC && use_src) {
            if (!trans) {
                memcpy(d, sr, w * 2);
                continue;
            }
            for (x = 0; x < w; x++)
                if ((sr[x] & keymask) != (key & keymask))
                    d[x] = sr[x];
        } else if (rop == 0xF0) {
            for (x = 0; x < w; x++)
                d[x] = pat;
        } else {
            for (x = 0; x < w; x++) {
                uint16_t sv = use_src ? sr[x] : 0;

                if (trans && use_src && (sv & keymask) == (key & keymask))
                    continue;
                d[x] = rop3(rop, pat, sv, d[x]);
            }
        }
    }
}

static void gp_blt(uint32_t mode)
{
    uint32_t rm = s.gp[GP_RASTER_MODE / 4];
    uint32_t dst = s.gp[GP_DST_OFFSET / 4] & 0x00FFFFFF, src = s.gp[GP_SRC_OFFSET / 4] & 0x00FFFFFF;
    uint32_t stride = s.gp[GP_STRIDE / 4];
    int w = s.gp[GP_WID_HEIGHT / 4] >> 16, h = s.gp[GP_WID_HEIGHT / 4] & 0xffff;
    int dp = stride & 0xffff, sp = stride >> 16, use_src = mode & BM_SRC_FB, y;
    static int logged;

    if (w <= 0 || h <= 0)
        return;
    if ((rm >> 28) != 6 && (rm >> 28) != 5) {         /* 565 / 1555: 16 bpp */
        if (!logged++)
            LOG("GP: blit in format %u not emulated (logged once)", rm >> 28);
        return;
    }
    if ((rm & RM_PAT_FLAGS) && !(rm & RM_ALPHA_EN) && logged++ < 8)
        LOG("GP: pattern flags 0x%x not emulated (solid colour used)", rm & RM_PAT_FLAGS);
    /* reversed blits address the last byte of the first pixel they write */
    if (mode & BM_REVERSE_X) {
        dst -= w * 2 - 1;
        src -= w * 2 - 1;
    }
    if (mode & BM_REVERSE_Y) {
        dst -= (h - 1) * dp;
        src -= (h - 1) * sp;
    }
    if ((uint64_t)dst + (uint64_t)(h - 1) * dp + w * 2 > FB_SIZE ||
        (use_src && (uint64_t)src + (uint64_t)(h - 1) * sp + w * 2 > FB_SIZE)) {
        if (logged++ < 8)
            LOG("GP: blit outside the frame buffer (dst 0x%x src 0x%x %dx%d)", dst, src, w, h);
        return;
    }
    if (use_src) {                                    /* a copy: overlap-safe in any direction */
        for (y = 0; y < h; y++)
            memcpy(s.tmp + (size_t)y * w, s.fb + src + (size_t)y * sp, w * 2);
    }
    gp_draw(rm, dst, dp, w, h, use_src);
}

/*
 * Mono expansion from host data (gfx2_mono_bitmap_to_screen_blt; byte-packed
 * for gfx2_text_blt, the text): each row is (w + first bit + 7) / 8 bytes,
 * MSB first, padded to dwords unless byte-packed; set
 * bits are SRC_COLOR_FG, clear ones SRC_COLOR_BG unless the raster mode makes
 * the background transparent.
 */
static void gp_host_start(uint32_t mode)
{
    uint32_t wh = s.gp[GP_WID_HEIGHT / 4];

    s.host.w = wh >> 16;
    s.host.h = wh & 0xffff;
    s.host.y = 0;
    s.host.n = 0;
    s.host.bit0 = (s.gp[GP_SRC_OFFSET / 4] >> 26) & 7;
    s.host.dst = s.gp[GP_DST_OFFSET / 4] & 0x00FFFFFF;
    s.host.row_bytes = (s.host.w + s.host.bit0 + 7) >> 3;
    s.host.step = mode & BM_SRC_TEXT ? s.host.row_bytes : (s.host.row_bytes + 3) & ~3;
    s.host.active = s.host.w > 0 && s.host.h > 0 && s.host.row_bytes <= (int)sizeof(s.host.row) - 4;
}

static void gp_host_row(void)
{
    uint16_t fg = s.gp[GP_SRC_COLOR_FG / 4], bg = s.gp[GP_SRC_COLOR_BG / 4], pat = s.gp[GP_PAT_COLOR_0 / 4];
    uint32_t rm = s.gp[GP_RASTER_MODE / 4];
    int trans = (rm & RM_SRC_TRANS) != 0, rop = rm & 0xff, dp = s.gp[GP_STRIDE / 4] & 0xffff, x;
    uint32_t d = s.host.dst + (uint32_t)s.host.y * dp;

    /* TextBlt: ROP 0xF0 (the pattern colour) through a transparent mono mask */
    if (d + (uint64_t)s.host.w * 2 <= FB_SIZE)
        for (x = 0; x < s.host.w; x++) {
            int b = s.host.bit0 + x, on = s.host.row[b >> 3] & (0x80 >> (b & 7));
            uint16_t *px = (uint16_t *)(s.fb + d) + x;

            if (on || !trans)
                *px = rop == 0xCC ? (on ? fg : bg) : rop == 0xF0 ? pat : rop3(rop, pat, on ? fg : bg, *px);
        }
    s.host.n -= s.host.step;
    memmove(s.host.row, s.host.row + s.host.step, s.host.n);
    if (++s.host.y >= s.host.h)
        s.host.active = 0;
}

static void gp_host_data(uint32_t v)
{
    if (!s.host.active)
        return;
    memcpy(s.host.row + s.host.n, &v, 4);
    s.host.n += 4;
    while (s.host.active && s.host.n >= s.host.step)
        gp_host_row();
}

static uint32_t gp_read(void *opaque, uint32_t off, int size)
{
    uint32_t v = 0;

    (void)opaque;
    if (off >= GP_BLT_STATUS && off < GP_BLT_STATUS + 4)
        return BS_HALF_EMPTY;                        /* idle, room for host data */
    pthread_mutex_lock(&s.lock);
    if (off < sizeof(s.gp))
        memcpy(&v, (uint8_t *)s.gp + off, size);
    pthread_mutex_unlock(&s.lock);
    return v;
}

static void gp_write(void *opaque, uint32_t off, int size, uint32_t v)
{
    static int logged;

    (void)opaque;
    pthread_mutex_lock(&s.lock);
    if (off < sizeof(s.gp))
        memcpy((uint8_t *)s.gp + off, &v, size);
    if (off == GP_BLT_MODE && (v & BM_SRC_HOST) && (v & (BM_SRC_MONO | BM_SRC_TEXT)))
        gp_host_start(v);
    else if (off == GP_BLT_MODE)
        gp_blt(v & 0xffff);
    else if (off == GP_HST_SRC)
        gp_host_data(v);
    else if (off == GP_VECTOR_MODE && logged++ < 4)
        LOG("GP: vectors not emulated");
    pthread_mutex_unlock(&s.lock);
}

/* ---- DC, VP --------------------------------------------------------------------- */

static uint32_t dc_read(void *opaque, uint32_t off, int size)
{
    uint32_t v = 0;

    (void)opaque;
    pthread_mutex_lock(&s.lock);
    if (off < REGS_SIZE)
        memcpy(&v, (uint8_t *)s.dc + off, size);
    if (off == DC_LINE_CNT) {                        /* blank for the last 5 % of each 1/60 s */
        double t = (core->now() - s.t0) * 60;

        v = (v & ~0x40000000u) | (t - (long)t > 0.95 ? 0x40000000u : 0);
    }
    pthread_mutex_unlock(&s.lock);
    return v;
}

static void dc_write(void *opaque, uint32_t off, int size, uint32_t v)
{
    (void)opaque;
    pthread_mutex_lock(&s.lock);
    if (off < REGS_SIZE)
        memcpy((uint8_t *)s.dc + off, &v, size);
    pthread_mutex_unlock(&s.lock);
}

static uint32_t vp_read(void *opaque, uint32_t off, int size)
{
    uint32_t v = 0;

    (void)opaque;
    pthread_mutex_lock(&s.lock);
    if (off < sizeof(s.vp))
        memcpy(&v, (uint8_t *)s.vp + off, size);
    pthread_mutex_unlock(&s.lock);
    return v;
}

static void vp_write(void *opaque, uint32_t off, int size, uint32_t v)
{
    (void)opaque;
    pthread_mutex_lock(&s.lock);
    if (off < sizeof(s.vp))
        memcpy((uint8_t *)s.vp + off, &v, size);
    pthread_mutex_unlock(&s.lock);
}

/* the scan-out: 16 bpp from DC_FB_ST_OFFSET, the mode the game set (640x480 before that) */
static void present(void)
{
    int w = 640, h = 480, pitch;
    uint32_t off;

    pthread_mutex_lock(&s.lock);
    if (s.dc[DC_H_ACTIVE / 4] & 0xfff)
        w = (s.dc[DC_H_ACTIVE / 4] & 0xfff) + 1;
    if (s.dc[DC_V_ACTIVE / 4] & 0x7ff)
        h = (s.dc[DC_V_ACTIVE / 4] & 0x7ff) + 1;
    pitch = (s.dc[DC_GFX_PITCH / 4] & 0xffff) * 8;
    if (pitch < w * 2)
        pitch = w * 2;
    off = s.dc[DC_FB_ST_OFFSET / 4] & 0x00FFFFFF;
    if (off + (uint64_t)pitch * h > FB_SIZE)
        off = 0;
    pthread_mutex_unlock(&s.lock);
    core->present(s.fb + off, w, h, pitch);
}

/* ---- port I/O: PCI configuration, VSA registers ---------------------------------- */

static uint32_t pci_config(uint32_t addr)
{
    switch (addr) {
    case 0x80000800: return 0x0028100B;              /* bus 0 dev 1 fn 0: GX2 host bridge */
    case 0x80000900: return 0x0030100B;              /* fn 1: GX2 graphics */
    case 0x80000910: return FB_PHYS;
    case 0x80000914: return GP_PHYS;
    case 0x80000918: return DC_PHYS;
    case 0x8000091C: return VP_PHYS;
    default: return 0xFFFFFFFF;
    }
}

static uint32_t port_in(uint16_t port, int size)
{
    uint32_t v = 0xFFFFFFFF;

    pthread_mutex_lock(&s.lock);
    if (port >= 0xCFC && port < 0xD00)
        v = pci_config(s.pci_addr & ~3u) >> (8 * (port - 0xCFC));
    else if (port == 0xCF8)
        v = s.pci_addr;
    else if (port == 0xAC1E)
        v = s.vsa_index == 0x0200 ? FB_SIZE >> 19 :  /* frame buffer, 512 KB units */
            s.vsa_index == 0x1201 ? 400 : 0;         /* core clock, MHz */
    else
        VLOG("in 0x%x (%d) not emulated", port, size);
    pthread_mutex_unlock(&s.lock);
    return size == 1 ? v & 0xff : size == 2 ? v & 0xffff : v;
}

static void port_out(uint16_t port, int size, uint32_t v)
{
    pthread_mutex_lock(&s.lock);
    if (port == 0xCF8 && size == 4)
        s.pci_addr = v;
    else if (port == 0xAC1C && v != 0xFC53)         /* 0xFC53 unlocks, then the index */
        s.vsa_index = v & 0xffff;
    else if (port != 0xAC1C)
        VLOG("out 0x%x = 0x%x (%d) not emulated", port, v, size);
    pthread_mutex_unlock(&s.lock);
}

/* ---- the a27 -------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/* the chip's packet into the executable's read buffer */
static void a27_packet(uint8_t *p, const uint8_t *data, int n)
{
    uint8_t sum = 0;
    int i;

    memset(p, 0, A27_READ_SIZE);
    put32(p, n);                                     /* size; mode, flags, key 0 */
    memcpy(p + A27_DATA, data, n);
    /* sum 1: the header fields (size, mode, flags, key); sum 2: 2 * sum 1 + data bytes 0..11 */
    p[A27_SUM1] = (uint8_t)n;
    for (i = 0; i < 12; i++)
        sum += p[A27_DATA + i];
    p[A27_SUM2] = (uint8_t)(2 * p[A27_SUM1] + sum);
    for (i = 0; i < 256; i++)
        p[A27_DATA + i] ^= a27_table[i];
}

static long a27_read(void *buf)
{
    int n;

    present();
    pthread_mutex_lock(&s.lock);
    n = game->frame(s.answer_mode, s.answer, s.answer_n, s.cmd);
    if (n < 0 || n > E1000_DATA_MAX)
        n = 0;
    s.answer_n = 0;
    a27_packet(buf, s.cmd, n);
    pthread_mutex_unlock(&s.lock);
    return 1;                                        /* the status byte: ok */
}

static long a27_write(const uint8_t *p, size_t n)
{
    uint32_t size, mode;

    /* the start-up write has length 0; the header is in the buffer all the same */
    memcpy(&size, p, 4);
    memcpy(&mode, p + 4, 4);
    pthread_mutex_lock(&s.lock);
    if (n < A27_ANSWER_DATA) {
        if (mode == A27_MODE_START) {
            LOG("a27 start");
            s.started = 1;
            s.answer_n = 0;
            if (game->reset)
                game->reset();
        }
    } else if (mode == A27_MODE_START && !size) {
        LOG("a27 start");
        s.started = 1;
        s.answer_n = 0;
        if (game->reset)
            game->reset();
    } else {
        if (size > n - A27_ANSWER_DATA || size > sizeof(s.answer))
            size = 0;
        s.answer_mode = mode;
        memcpy(s.answer, p + A27_ANSWER_DATA, size);
        s.answer_n = size;
    }
    pthread_mutex_unlock(&s.lock);
    return n;
}

/* ---- device files ---------------------------------------------------------------- */

static int dev_kind(const char *path)
{
    if (!strcmp(path, "/dev/mem"))
        return DEV_MEM;
    if (!strcmp(path, "/dev/cpu/0/msr"))
        return DEV_MSR;
    if (!strcmp(path, "/dev/pccard0"))
        return DEV_PCCARD;
    return DEV_NONE;
}

static int dev_handles(const char *path)
{
    return dev_kind(path) != DEV_NONE;
}

static int dev_open(const char *path)
{
    int kind = dev_kind(path), fd;

    if (kind == DEV_NONE) {
        errno = ENOENT;
        return -1;
    }
    fd = core->placeholder_fd();
    if (fd < 0)
        return -1;
    pthread_mutex_lock(&s.lock);
    if (s.nfds < 8) {
        s.fds[s.nfds] = fd;
        s.pos[s.nfds] = 0;
        s.kinds[s.nfds++] = kind;
    }
    pthread_mutex_unlock(&s.lock);
    VLOG("open %s -> %d", path, fd);
    return fd;
}

static int dev_ioctl(int fd, unsigned long req, void *arg)
{
    (void)arg;
    VLOG("ioctl %d 0x%lx ignored", fd, req);
    return 0;
}

static void *dev_mmap_at(int fd, size_t len, uint32_t off)
{
    void *p = MAP_FAILED;

    if (fd_kind(fd) != DEV_MEM) {
        errno = EINVAL;
        return p;
    }
    if (off == FB_PHYS && len <= FB_SIZE)
        p = s.fb;
    else if (off == GP_PHYS && len <= REGS_SIZE)
        p = core->mmio_map(REGS_SIZE, NULL, gp_read, gp_write);
    else if (off == DC_PHYS && len <= REGS_SIZE)
        p = core->mmio_map(REGS_SIZE, NULL, dc_read, dc_write);
    else if (off == VP_PHYS && len <= REGS_SIZE)
        p = core->mmio_map(REGS_SIZE, NULL, vp_read, vp_write);
    LOG("map /dev/mem 0x%08x (%zu bytes) -> %p", off, len, p);
    if (p == MAP_FAILED)
        errno = EINVAL;
    else if (off == VP_PHYS)
        s.vp_map = p;
    else if (off == DC_PHYS)
        s.dc_map = p;
    /* GX2Init ANDs the DC, VP and frame buffer pointers and quits on 0 */
    if (off == FB_PHYS && s.dc_map && s.vp_map &&
        !((uintptr_t)s.dc_map & (uintptr_t)s.vp_map & (uintptr_t)p))
        LOG("the DC, VP and frame buffer addresses share no bit: the game will quit");
    return p;
}

static void *dev_mmap(int fd, size_t len)
{
    return dev_mmap_at(fd, len, 0);
}

static long dev_seek(int fd, long off, int whence)
{
    long *pos;
    long r = -1;

    pthread_mutex_lock(&s.lock);
    if ((pos = fd_pos(fd)) && whence == 0)
        r = *pos = off;
    else if (pos && whence == 1)
        r = *pos += off;
    pthread_mutex_unlock(&s.lock);
    if (r < 0)
        errno = EINVAL;
    return r;
}

static long dev_read(int fd, void *buf, size_t n)
{
    switch (fd_kind(fd)) {
    case DEV_PCCARD:
        return a27_read(buf);
    case DEV_MSR:                                    /* no GeodeLink devices */
        memset(buf, 0, n);
        return n;
    default:
        return 0;
    }
}

static long dev_write(int fd, const void *buf, size_t n)
{
    if (fd_kind(fd) == DEV_PCCARD)
        return a27_write(buf, n);
    return n;
}

static void dev_close(int fd)
{
    (void)fd;
}

static void machine_key(int sym, int down)
{
    if (!game->key)
        return;
    pthread_mutex_lock(&s.lock);
    game->key(sym, down);
    pthread_mutex_unlock(&s.lock);
}

static void machine_draw(int w, int h)
{
    if (!game->draw)
        return;
    pthread_mutex_lock(&s.lock);
    game->draw(w, h);
    pthread_mutex_unlock(&s.lock);
}

static void machine_status(char *buf, size_t n)
{
    if (!game->status)
        return;
    pthread_mutex_lock(&s.lock);
    game->status(buf, n);
    pthread_mutex_unlock(&s.lock);
}

/* ---- plugin entry --------------------------------------------------------------- */

static const void *api_game_ptr(uint32_t va, uint32_t len);

static void api_bitmap(const void *src, int pitch, uint32_t dst, int dp, int w, int h, uint32_t rm)
{
    int y;

    if (w <= 0 || h <= 0 || (size_t)w * h > 1024 * 1024)
        return;
    pthread_mutex_lock(&s.lock);
    dst &= 0x00FFFFFF;
    if (dst + (uint64_t)(h - 1) * dp + w * 2 <= FB_SIZE) {
        for (y = 0; y < h; y++)
            memcpy(s.tmp + (size_t)y * w, (const uint8_t *)src + (size_t)y * pitch, w * 2);
        gp_draw(rm, dst, dp, w, h, 1);
    }
    pthread_mutex_unlock(&s.lock);
}

/* a jmp rel32 at va (the executable's text: made writable first) */
static int api_patch_jump(uint32_t va, void *target, const uint8_t *expect, int n)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    int32_t rel = (int32_t)((uintptr_t)target - (va + 5));
    uintptr_t page = va & ~(uintptr_t)0xfff;

    if (!api_game_ptr(va, 5) || (expect && memcmp(p, expect, n)))
        return 0;
    if (mprotect((void *)page, ((va + 5 - page) + 0xfff) & ~(uintptr_t)0xfff, PROT_READ | PROT_WRITE | PROT_EXEC))
        return 0;
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    return 1;
}

/* the executable maps at its link address */
static const void *api_game_ptr(uint32_t va, uint32_t len)
{
    return va >= 0x08048000u && va + (uint64_t)len <= 0x0A000000u ? (const void *)(uintptr_t)va : NULL;
}

static const struct igs_machine machine = {
    .version = IGS_API_VERSION,
    .name = "e1000",
    .handles = dev_handles,
    .open = dev_open,
    .ioctl = dev_ioctl,
    .mmap = dev_mmap,
    .close = dev_close,
    .key = machine_key,
    .draw = machine_draw,
    .read = dev_read,
    .write = dev_write,
    .status = machine_status,
    .mmap_at = dev_mmap_at,
    .seek = dev_seek,
    .port_in = port_in,
    .port_out = port_out,
};

static struct e1000_api api = {
    .version = E1000_API_VERSION,
    .game_ptr = api_game_ptr,
    .bitmap = api_bitmap,
    .patch_jump = api_patch_jump,
};

__attribute__((visibility("default")))
const struct igs_machine *igs_machine_entry(const struct igs_core *c)
{
    const char *path = getenv("IGS_GAME");
    e1000_game_entry_fn entry;
    void *h;

    core = c;
    api.core = c;
    if (!core->present) {
        LOG("the core cannot show a frame buffer");
        return NULL;
    }
    h = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    entry = h ? (e1000_game_entry_fn)dlsym(h, E1000_GAME_ENTRY) : NULL;
    game = entry ? entry(&api) : NULL;
    if (!game || game->version != E1000_API_VERSION) {
        LOG("cannot load game plugin %s: %s", path ? path : "(IGS_GAME unset)",
            h ? "bad plugin" : dlerror());
        return NULL;
    }
    s.fb = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    s.tmp = malloc(1024 * 1024 * 2);
    if (s.fb == MAP_FAILED || !s.tmp) {
        LOG("no memory for the frame buffer");
        return NULL;
    }
    s.t0 = core->now();
    LOG("game %s", game->name);
    return &machine;
}
