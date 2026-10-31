/*
 * IGS loader, Windows core: glibc stand-in, part 3: pthreads on Win32.
 *
 * Every pthread object keeps its Win32 state in its first word, zero being
 * the static initializer: mutexes a pointer to a CRITICAL_SECTION created on
 * first use, condition variables and rwlocks a CONDITION_VARIABLE / SRWLOCK
 * in place.  That fits both the GLIBC_2.0 (12-byte) and GLIBC_2.3.2
 * (48-byte) pthread_cond_t, so both symbol versions share one implementation.
 * Threads start through elf_call (16-byte aligned stack, x87 at 64-bit
 * precision as on Linux).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "lx.h"

struct lthread {
    HANDLE h;
    DWORD tid;
    void *fn, *arg;
    void *ret;
};

static __thread struct lthread *self_t;

static DWORD WINAPI thread_main(void *p)
{
    struct lthread *t = p;
    ULONG guarantee = 64 << 10;

    self_t = t;
    SetThreadStackGuarantee(&guarantee);           /* room to report a stack overflow */
    _control87(_PC_64, _MCW_PC);
    t->ret = (void *)(uintptr_t)elf_call1(t->fn, U32(t->arg));
    return 0;
}

void *lx_thread_create(void *fn, void *arg, size_t stack)
{
    struct lthread *t = calloc(1, sizeof(*t));

    t->fn = fn;
    t->arg = arg;
    if (lx_verbose)
        LOG("thread %p: stack %u KiB", fn, (unsigned)(stack ? stack : 8 << 20) >> 10);
    /*
     * Committed in full: Linux code has no stack probes, and a frame larger
     * than a page can skip the guard page of a growing Windows stack (a fault
     * Windows cannot even report).
     */
    t->h = CreateThread(NULL, stack ? stack : 8 << 20, thread_main, t, 0, &t->tid);
    if (!t->h) {
        free(t);
        return NULL;
    }
    return t;
}

void lx_thread_wait(void *p, int *ret)
{
    struct lthread *t = p;

    if (!t)
        return;
    WaitForSingleObject(t->h, INFINITE);
    if (ret)
        *ret = (int)(uintptr_t)t->ret;
}

void lx_thread_kill(void *p)
{
    struct lthread *t = p;

    if (t)
        TerminateThread(t->h, 0);
}

uint32_t lx_thread_id(void *p)
{
    return p ? ((struct lthread *)p)->tid : GetCurrentThreadId();
}

static struct lthread *l_pthread_self(void)
{
    if (!self_t) {
        self_t = calloc(1, sizeof(*self_t));
        self_t->tid = GetCurrentThreadId();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self_t->h,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    return self_t;
}

/* pthread_attr_t (36 bytes): ours is [0] detached, [1] stack size */
static int l_pthread_attr_init(uint32_t *a) { memset(a, 0, 36); return 0; }
static int l_pthread_attr_destroy(uint32_t *a) { (void)a; return 0; }
static int l_pthread_attr_setdetachstate(uint32_t *a, int s) { a[0] = s; return 0; }
static int l_pthread_attr_getdetachstate(const uint32_t *a, int *s) { *s = a[0]; return 0; }
static int l_pthread_attr_setstacksize(uint32_t *a, size_t s) { a[1] = s; return 0; }
static int l_pthread_attr_getstacksize(const uint32_t *a, size_t *s) { *s = a[1] ? a[1] : 8 << 20; return 0; }
static int l_pthread_attr_ok(void) { return 0; }
static int l_pthread_attr_getschedparam(const void *a, int *p) { (void)a; *p = 0; return 0; }

static int l_pthread_create(struct lthread **out, const uint32_t *attr, void *fn, void *arg)
{
    struct lthread *t = lx_thread_create(fn, arg, attr ? attr[1] : 0);

    if (!t)
        return 11;                           /* EAGAIN */
    *out = t;
    if (attr && attr[0] == 1) {              /* PTHREAD_CREATE_DETACHED */
        CloseHandle(t->h);
        t->h = NULL;
    }
    return 0;
}

static int l_pthread_join(struct lthread *t, void **ret)
{
    if (!t || !t->h)
        return 3;                            /* ESRCH */
    WaitForSingleObject(t->h, INFINITE);
    if (ret)
        *ret = t->ret;
    CloseHandle(t->h);
    t->h = NULL;
    return 0;
}

static int l_pthread_detach(struct lthread *t)
{
    (void)t;
    return 0;
}

static int l_pthread_equal(void *a, void *b) { return a == b; }

static void __attribute__((noreturn)) l_pthread_exit(void *v)
{
    l_pthread_self()->ret = v;
    ExitThread(0);
}

static int l_pthread_cancel(struct lthread *t)
{
    LOG("pthread_cancel(%p): not supported", (void *)t);
    return 0;
}
static int l_pthread_kill(void *t, int sig) { (void)t; (void)sig; return 0; }
static int l_pthread_setcancelstate(int s, int *old) { (void)s; if (old) *old = 0; return 0; }
static int l_pthread_setschedparam(void *t, int pol, const void *p) { (void)t; (void)pol; (void)p; return 0; }
static int l_pthread_getschedparam(void *t, int *pol, int *p) { (void)t; *pol = 0; *p = 0; return 0; }
static int l_pthread_sigmask(int how, const void *s, void *old) { (void)how; (void)s; if (old) memset(old, 0, 128); return 0; }
static int l_sched_yield(void) { SwitchToThread(); return 0; }
static int l_sched_prio(int p) { (void)p; return 0; }

/* ---- mutexes ------------------------------------------------------------------------------ */

static CRITICAL_SECTION *mtx(void *m)
{
    CRITICAL_SECTION *volatile *slot = m, *cs = *slot;

    if (cs)
        return cs;
    cs = malloc(sizeof(*cs));
    InitializeCriticalSectionAndSpinCount(cs, 1000);
    if (InterlockedCompareExchangePointer((void *volatile *)slot, cs, NULL) != NULL) {
        DeleteCriticalSection(cs);
        free(cs);
    }
    return *slot;
}

static int l_pthread_mutex_init(void *m, const void *attr)
{
    (void)attr;
    memset(m, 0, 24);
    mtx(m);
    return 0;
}

static int l_pthread_mutex_destroy(void *m)
{
    CRITICAL_SECTION **slot = m;

    if (*slot) {
        DeleteCriticalSection(*slot);
        free(*slot);
        *slot = NULL;
    }
    return 0;
}

static int l_pthread_mutex_lock(void *m) { EnterCriticalSection(mtx(m)); return 0; }
static int l_pthread_mutex_unlock(void *m) { LeaveCriticalSection(mtx(m)); return 0; }
static int l_pthread_mutex_trylock(void *m) { return TryEnterCriticalSection(mtx(m)) ? 0 : 16; }
static int l_pthread_mutex_timedlock(void *m, const struct l_timespec *ts)
{
    (void)ts;
    return l_pthread_mutex_lock(m);
}
static int l_pthread_mutexattr_init(uint32_t *a) { *a = 0; return 0; }
static int l_pthread_mutexattr_destroy(uint32_t *a) { (void)a; return 0; }
static int l_pthread_mutexattr_settype(uint32_t *a, int t) { *a = t; return 0; }
static int l_pthread_mutexattr_gettype(const uint32_t *a, int *t) { *t = *a; return 0; }

/* ---- condition variables: [0] CONDITION_VARIABLE, [1] clock ------------------------------- */

static int l_pthread_condattr_init(uint32_t *a) { *a = 0; return 0; }
static int l_pthread_condattr_destroy(uint32_t *a) { (void)a; return 0; }
static int l_pthread_condattr_setclock(uint32_t *a, int clk) { *a = clk; return 0; }
static int l_pthread_condattr_setpshared(uint32_t *a, int s) { (void)a; (void)s; return 0; }

static int l_pthread_cond_init(uint32_t *c, const uint32_t *attr)
{
    memset(c, 0, 12);
    c[1] = attr ? *attr : 0;
    return 0;
}

static int l_pthread_cond_destroy(void *c) { (void)c; return 0; }
static int l_pthread_cond_signal(void *c) { WakeConditionVariable(c); return 0; }
static int l_pthread_cond_broadcast(void *c) { WakeAllConditionVariable(c); return 0; }

static int l_pthread_cond_wait(void *c, void *m)
{
    SleepConditionVariableCS(c, mtx(m), INFINITE);
    return 0;
}

extern int l_clock_gettime_pub(int clk, struct l_timespec *ts);

static int l_pthread_cond_timedwait(uint32_t *c, void *m, const struct l_timespec *abst)
{
    struct l_timespec now;
    int64_t ms;

    l_clock_gettime_pub(c[1], &now);
    ms = (int64_t)(abst->tv_sec - now.tv_sec) * 1000 + (abst->tv_nsec - now.tv_nsec) / 1000000;
    if (ms < 0)
        ms = 0;
    if (!SleepConditionVariableCS((PCONDITION_VARIABLE)c, mtx(m), (DWORD)ms))
        return GetLastError() == ERROR_TIMEOUT ? L_ETIMEDOUT : 22;
    return 0;
}

/* ---- rwlocks: [0] SRWLOCK, [1] the exclusive owner ---------------------------------------- */

static int l_pthread_rwlock_init(uint32_t *l, const void *a) { (void)a; memset(l, 0, 32); return 0; }
static int l_pthread_rwlock_destroy(void *l) { (void)l; return 0; }
static int l_pthread_rwlock_rdlock(void *l) { AcquireSRWLockShared(l); return 0; }
static int l_pthread_rwlock_wrlock(uint32_t *l)
{
    AcquireSRWLockExclusive((PSRWLOCK)l);
    l[1] = GetCurrentThreadId();
    return 0;
}
static int l_pthread_rwlock_unlock(uint32_t *l)
{
    if (l[1] == GetCurrentThreadId()) {
        l[1] = 0;
        ReleaseSRWLockExclusive((PSRWLOCK)l);
    } else {
        ReleaseSRWLockShared((PSRWLOCK)l);
    }
    return 0;
}

/* ---- thread-specific data, once ------------------------------------------------------------- */

static int l_pthread_key_create(uint32_t *key, void *dtor)
{
    DWORD k = TlsAlloc();

    (void)dtor;
    if (k == TLS_OUT_OF_INDEXES)
        return 11;
    *key = k;
    return 0;
}

static int l_pthread_key_delete(uint32_t key) { TlsFree(key); return 0; }
static void *l_pthread_getspecific(uint32_t key) { return TlsGetValue(key); }
static int l_pthread_setspecific(uint32_t key, const void *v) { TlsSetValue(key, (void *)v); return 0; }

static CRITICAL_SECTION once_lock;

static int l_pthread_once(volatile int32_t *once, void *fn)
{
    if (*once == 2)
        return 0;
    EnterCriticalSection(&once_lock);
    if (*once == 0) {
        *once = 1;
        elf_call0(fn);
        *once = 2;
    }
    LeaveCriticalSection(&once_lock);
    return 0;
}

/* ---- semaphores: [0] a Win32 semaphore --------------------------------------------------------- */

static int l_sem_init(HANDLE *s, int pshared, unsigned v)
{
    (void)pshared;
    *s = CreateSemaphoreA(NULL, v, 0x7fffffff, NULL);
    return *s ? 0 : -1;
}
static int l_sem_destroy(HANDLE *s) { CloseHandle(*s); return 0; }
static int l_sem_wait(HANDLE *s) { WaitForSingleObject(*s, INFINITE); return 0; }
static int l_sem_trywait(HANDLE *s)
{
    if (WaitForSingleObject(*s, 0) == WAIT_OBJECT_0)
        return 0;
    SET_ERRNO(11);
    return -1;
}
static int l_sem_post(HANDLE *s) { ReleaseSemaphore(*s, 1, NULL); return 0; }
static int l_sem_getvalue(HANDLE *s, int *v)
{
    LONG prev = 0;

    if (WaitForSingleObject(*s, 0) == WAIT_OBJECT_0) {
        ReleaseSemaphore(*s, 1, &prev);
        *v = prev + 1;
    } else {
        *v = 0;
    }
    return 0;
}

const struct nsym thread_syms[] = {
    N(pthread_create), N(pthread_join), N(pthread_detach), N(pthread_self), N(pthread_equal),
    N(pthread_exit), N(pthread_cancel), N(pthread_kill), N(pthread_setcancelstate),
    NA(pthread_setcanceltype, l_pthread_setcancelstate), N(pthread_setschedparam),
    N(pthread_getschedparam), N(pthread_sigmask),
    N(pthread_attr_init), N(pthread_attr_destroy), N(pthread_attr_setdetachstate),
    N(pthread_attr_getdetachstate), N(pthread_attr_setstacksize), N(pthread_attr_getstacksize),
    NA(pthread_attr_setschedpolicy, l_pthread_attr_ok), NA(pthread_attr_setschedparam, l_pthread_attr_ok),
    NA(pthread_attr_setinheritsched, l_pthread_attr_ok), NA(pthread_attr_setscope, l_pthread_attr_ok),
    NA(pthread_attr_setguardsize, l_pthread_attr_ok), N(pthread_attr_getschedparam),
    N(pthread_mutex_init), N(pthread_mutex_destroy), N(pthread_mutex_lock), N(pthread_mutex_unlock),
    N(pthread_mutex_trylock), N(pthread_mutex_timedlock), N(pthread_mutexattr_init),
    N(pthread_mutexattr_destroy), N(pthread_mutexattr_settype), N(pthread_mutexattr_gettype),
    NA(pthread_mutexattr_setpshared, l_pthread_attr_ok),
    N(pthread_condattr_init), N(pthread_condattr_destroy), N(pthread_condattr_setclock),
    N(pthread_condattr_setpshared),
    N(pthread_cond_init), N(pthread_cond_destroy), N(pthread_cond_signal), N(pthread_cond_broadcast),
    N(pthread_cond_wait), N(pthread_cond_timedwait),
    N(pthread_rwlock_init), N(pthread_rwlock_destroy), N(pthread_rwlock_rdlock),
    N(pthread_rwlock_wrlock), N(pthread_rwlock_unlock),
    N(pthread_key_create), NA(__pthread_key_create, l_pthread_key_create), N(pthread_key_delete),
    N(pthread_getspecific), N(pthread_setspecific), N(pthread_once),
    NA(__pthread_once, l_pthread_once),
    N(sem_init), N(sem_destroy), N(sem_wait), N(sem_trywait), N(sem_post), N(sem_getvalue),
    N(sched_yield), NA(sched_get_priority_max, l_sched_prio), NA(sched_get_priority_min, l_sched_prio),
    { NULL, NULL },
};

void thread_init(void)
{
    InitializeCriticalSection(&once_lock);
    l_pthread_self();
}
