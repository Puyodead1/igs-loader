/*
 * Force-included into the machine and game plugins when they are built as
 * Windows DLLs (win/build-win.sh), so their sources stay as they are.
 * With win/compat/ on the include path it also supplies <dlfcn.h> and
 * <sys/mman.h>.
 */
#ifndef IGS_PLUGIN_COMPAT_H
#define IGS_PLUGIN_COMPAT_H

#include <errno.h>
#include <pthread.h>
#include <time.h>

/*
 * The plugins time pthread_cond_timedwait deadlines with
 * clock_gettime(CLOCK_MONOTONIC) after pthread_condattr_setclock; winpthreads
 * measures deadlines against CLOCK_REALTIME.  Use that clock throughout.
 */
static inline int igs_condattr_setclock(pthread_condattr_t *a, clockid_t c)
{
    (void)a;
    (void)c;
    return 0;
}
#define pthread_condattr_setclock igs_condattr_setclock

static inline int igs_clock_gettime(clockid_t c, struct timespec *t)
{
    (void)c;
    return clock_gettime(CLOCK_REALTIME, t);
}
#define clock_gettime igs_clock_gettime

#endif
