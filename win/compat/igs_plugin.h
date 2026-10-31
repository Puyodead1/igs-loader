/*
 * Force-included into the machine and game plugins when they are built as
 * Windows DLLs (win/build-win.sh), so their sources stay as they are.
 * With win/compat/ on the include path it also supplies <dlfcn.h> and
 * <sys/mman.h>.
 */
#ifndef IGS_PLUGIN_COMPAT_H
#define IGS_PLUGIN_COMPAT_H

/* localtime_r and friends from <time.h> */
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L
#endif

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

/* glibc's name for it (machines/e2000) */
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

#endif
