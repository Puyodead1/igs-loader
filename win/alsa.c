/*
 * libasound.so.2 playback, natively: the PCM calls FMOD Ex (Unity 4) makes,
 * onto the core's waveOut stream (core_win.c, the one OSS /dev/dsp uses).
 * One playback stream, interleaved S16_LE or U8, 1 or 2 channels; writes
 * block while about IGS_DSP_LATENCY_MS is queued.  No capture.  The device
 * list is one "default" output.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "lx.h"

enum { A_EINVAL = 22, A_EBADFD = 77, A_ENOENT = 2 };
enum { F_S8 = 0, F_U8 = 1, F_S16_LE = 2 };

struct a_params {                       /* the guest's snd_pcm_hw_params_t buffer */
    int rate, channels, format;
    unsigned long period, buffer;
};

struct a_pcm {
    struct a_params p;
    int started;
};

static int a_bits(int fmt) { return fmt == F_S16_LE ? 16 : 8; }

static int l_snd_pcm_open(struct a_pcm **pcm, const char *name, int stream, int mode)
{
    (void)mode;
    if (stream != 0) {                  /* capture */
        LOG("alsa: no capture device (%s)", name ? name : "?");
        return -A_ENOENT;
    }
    *pcm = calloc(1, sizeof(**pcm));
    (*pcm)->p.rate = 48000;
    (*pcm)->p.channels = 2;
    (*pcm)->p.format = F_S16_LE;
    return 0;
}

static int l_snd_pcm_close(struct a_pcm *pcm)
{
    if (pcm && pcm->started)
        core_pcm_close();
    free(pcm);
    return 0;
}

static size_t l_snd_pcm_hw_params_sizeof(void) { return 64; }

static int l_snd_pcm_hw_params_any(struct a_pcm *pcm, struct a_params *p)
{
    (void)pcm;
    memset(p, 0, sizeof(*p));
    p->rate = 48000;
    p->channels = 2;
    p->format = F_S16_LE;
    p->period = 1024;
    p->buffer = 4096;
    return 0;
}

static int l_snd_pcm_hw_params_set_access(struct a_pcm *pcm, struct a_params *p, int access)
{
    (void)pcm; (void)p;
    return access == 3 ? 0 : -A_EINVAL;           /* SND_PCM_ACCESS_RW_INTERLEAVED */
}

static int l_snd_pcm_hw_params_set_format(struct a_pcm *pcm, struct a_params *p, int fmt)
{
    (void)pcm;
    if (fmt != F_S16_LE && fmt != F_U8)
        return -A_EINVAL;
    p->format = fmt;
    return 0;
}

static int l_snd_pcm_hw_params_set_rate(struct a_pcm *pcm, struct a_params *p, unsigned val, int dir)
{
    (void)pcm; (void)dir;
    if (val < 4000 || val > 192000)
        return -A_EINVAL;
    p->rate = (int)val;
    return 0;
}

static int l_snd_pcm_hw_params_set_channels(struct a_pcm *pcm, struct a_params *p, unsigned val)
{
    (void)pcm;
    if (val < 1 || val > 2)
        return -A_EINVAL;
    p->channels = (int)val;
    return 0;
}

static int l_snd_pcm_hw_params_set_period_size_near(struct a_pcm *pcm, struct a_params *p,
                                                    unsigned long *val, int *dir)
{
    (void)pcm;
    if (dir)
        *dir = 0;
    if (*val < 64)
        *val = 64;
    p->period = *val;
    return 0;
}

static int l_snd_pcm_hw_params_set_buffer_size_near(struct a_pcm *pcm, struct a_params *p, unsigned long *val)
{
    (void)pcm;
    if (*val < 2 * p->period)
        *val = 2 * p->period;
    p->buffer = *val;
    return 0;
}

static int l_snd_pcm_hw_params(struct a_pcm *pcm, struct a_params *p)
{
    pcm->p = *p;
    if (pcm->started)
        core_pcm_close();
    core_pcm_config(p->rate, p->channels, a_bits(p->format));
    pcm->started = 1;
    LOG("alsa: %d Hz, %d ch, %d bit, period %lu, buffer %lu frames", p->rate, p->channels,
        a_bits(p->format), p->period, p->buffer);
    return 0;
}

static int l_snd_pcm_prepare(struct a_pcm *pcm) { (void)pcm; return 0; }
static int l_snd_pcm_nonblock(struct a_pcm *pcm, int nb) { (void)pcm; (void)nb; return 0; }

static long l_snd_pcm_writei(struct a_pcm *pcm, const void *buf, unsigned long frames)
{
    size_t fb = (size_t)pcm->p.channels * (size_t)(a_bits(pcm->p.format) / 8);

    if (!pcm->started)
        return -A_EBADFD;
    core_pcm_write(buf, frames * fb);
    return (long)frames;
}

static long l_snd_pcm_readi(struct a_pcm *pcm, void *buf, unsigned long frames)
{
    (void)pcm; (void)buf; (void)frames;
    return -A_EBADFD;
}

static const char *l_snd_strerror(int err)
{
    switch (-err) {
    case A_EINVAL: return "Invalid argument";
    case A_ENOENT: return "No such file or directory";
    case A_EBADFD: return "File descriptor in bad state";
    default: return err ? "Unknown error" : "Success";
    }
}

/* the device list: one hint, "default" */
static int l_snd_device_name_hint(int card, const char *iface, void ***hints)
{
    void **h = l_malloc(2 * sizeof(void *));

    (void)card; (void)iface;
    h[0] = (void *)"default";
    h[1] = NULL;
    *hints = h;
    return 0;
}

static char *l_snd_device_name_get_hint(const void *hint, const char *id)
{
    (void)hint;
    if (!strcmp(id, "NAME"))
        return l_strdup("default");
    if (!strcmp(id, "DESC"))
        return l_strdup("Windows audio");
    return NULL;                                     /* IOID: both directions */
}

static int l_snd_device_name_free_hint(void **hints)
{
    l_free(hints);
    return 0;
}

const struct nsym alsa_syms[] = {
    N(snd_pcm_open), N(snd_pcm_close), N(snd_pcm_hw_params_sizeof), N(snd_pcm_hw_params_any),
    N(snd_pcm_hw_params_set_access), N(snd_pcm_hw_params_set_format), N(snd_pcm_hw_params_set_rate),
    N(snd_pcm_hw_params_set_channels), N(snd_pcm_hw_params_set_period_size_near),
    N(snd_pcm_hw_params_set_buffer_size_near), N(snd_pcm_hw_params), N(snd_pcm_prepare),
    N(snd_pcm_nonblock), N(snd_pcm_writei), N(snd_pcm_readi), N(snd_strerror),
    N(snd_device_name_hint), N(snd_device_name_get_hint), N(snd_device_name_free_hint),
    { NULL, NULL }
};
