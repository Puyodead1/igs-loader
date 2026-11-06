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
    /* signals: pending and blocked (bit sig - 1), in an alertable wait, handlers run */
    volatile LONG pend[2];
    uint32_t blocked[2];
    volatile LONG alertable, delivered;
    uint32_t stack_lo, stack_hi;
    uint32_t ss_sp, ss_size;                       /* sigaltstack (0: none) */
};

static __thread struct lthread *self_t;
static struct lthread *l_pthread_self(void);

/* the stack's range from the TEB: StackBase, DeallocationStack */
static void stack_range(struct lthread *t)
{
    uint8_t *teb = (uint8_t *)NtCurrentTeb();

    t->stack_hi = *(uint32_t *)(teb + 4);
    t->stack_lo = *(uint32_t *)(teb + 0xE0C);
}

static DWORD WINAPI thread_main(void *p)
{
    struct lthread *t = p;
    ULONG guarantee = 64 << 10;

    self_t = t;
    stack_range(t);
    SetThreadStackGuarantee(&guarantee);           /* room to report a stack overflow */
    _control87(_PC_64, _MCW_PC);
    elf_thread_tp();
    t->ret = (void *)(uintptr_t)elf_call1(t->fn, U32(t->arg));
    return 0;
}

/* ---- signals ----------------------------------------------------------------------------------- */

/*
 * Delivered as the kernel does, for the runtimes that live on them (Unity's
 * Mono: Boehm GC stops the world with pthread_kill and sigsuspend; managed
 * null references and divisions arrive as SIGSEGV / SIGFPE).
 *   - a fault in guest code (ELF or JIT, not a PE image) with a handler: the
 *     thread resumes in the handler, on a frame below its stack with the
 *     siginfo and an i386 ucontext; returning from it restores the
 *     (possibly changed) registers and FPU state, as sigreturn;
 *   - pthread_kill: the target is suspended; in guest code it is redirected
 *     the same way, in an alertable wait (sem, cond, mutex, sleep,
 *     sigsuspend) an APC runs the handler there; in other native code it is
 *     resumed and tried again;
 *   - masks per thread; a blocked signal stays pending until unblocked.
 * The waits become alertable once the game installs a handler for an
 * asynchronous signal.  SA_ONSTACK handlers of faults run on the thread's
 * sigaltstack.
 */
struct l_sigact { void *handler; uint32_t mask[2]; uint32_t flags; };
static struct l_sigact sigacts[65];
static volatile LONG signals_live;

#define SA_SIGINFO_ 4
#define SA_NODEFER_ 0x40000000u
#define SA_ONSTACK_ 0x08000000u
#define UC_SIZE 352
#define UC_GREGS 20
#define UC_SIGMASK 108

static int sig_is_fn(void *h) { return h && h != (void *)1; }

static void sig_bits_or(uint32_t *d, const uint32_t *s) { d[0] |= s[0]; d[1] |= s[1]; }

/* lowest pending, unblocked signal of t, taken off its pending set; 0 if none */
static int sig_take(struct lthread *t)
{
    int w;

    for (w = 0; w < 2; w++)
        for (;;) {
            LONG p = t->pend[w], ready = p & ~(LONG)t->blocked[w];
            int b;

            if (!ready)
                break;
            b = __builtin_ctz((unsigned)ready);
            if (InterlockedCompareExchange(&t->pend[w], (LONG)((unsigned)p & ~(1u << b)), p) == p)
                return 32 * w + b + 1;
        }
    return 0;
}

static int sig_ready(struct lthread *t)
{
    return (t->pend[0] & ~(LONG)t->blocked[0]) || (t->pend[1] & ~(LONG)t->blocked[1]);
}

/* i386 ucontext from a CONTEXT: gregs GS FS ES DS EDI ESI EBP ESP EBX EDX ECX EAX TRAPNO ERR EIP CS EFL UESP SS */
static void uc_from_ctx(uint8_t *uc, const CONTEXT *c, const uint32_t *mask)
{
    uint32_t *g = (uint32_t *)(uc + UC_GREGS);

    memset(uc, 0, UC_SIZE);
    g[0] = c->SegGs; g[1] = c->SegFs; g[2] = c->SegEs; g[3] = c->SegDs;
    g[4] = c->Edi; g[5] = c->Esi; g[6] = c->Ebp; g[7] = c->Esp; g[8] = c->Ebx; g[9] = c->Edx;
    g[10] = c->Ecx; g[11] = c->Eax; g[14] = c->Eip; g[15] = c->SegCs; g[16] = c->EFlags;
    g[17] = c->Esp; g[18] = c->SegSs;
    *(uint32_t *)(uc + UC_GREGS + 76) = (uint32_t)(uintptr_t)(uc + 240);   /* fpregs */
    memcpy(uc + UC_SIGMASK, mask, 8);
}

static void siginfo_make(uint8_t *si, int sig, int code, uint32_t addr)
{
    memset(si, 0, 128);
    ((int32_t *)si)[0] = sig;
    ((int32_t *)si)[2] = code;
    ((uint32_t *)si)[3] = addr;                    /* si_addr (faults), si_pid (kill) */
}

/* the handler on this thread, now (an APC, a raise, an unblock) */
static void sig_run_here(int sig, int code, uint32_t addr)
{
    struct lthread *t = l_pthread_self();
    struct l_sigact a = sigacts[sig];
    uint8_t si[128], uc[UC_SIZE];
    uint32_t old[2] = { t->blocked[0], t->blocked[1] }, args[3];
    CONTEXT c;

    if (!sig_is_fn(a.handler))
        return;
    memset(&c, 0, sizeof(c));
    RtlCaptureContext(&c);
    uc_from_ctx(uc, &c, old);
    siginfo_make(si, sig, code, addr);
    sig_bits_or(t->blocked, a.mask);
    if (!(a.flags & SA_NODEFER_))
        t->blocked[(sig - 1) / 32] |= 1u << ((sig - 1) % 32);
    args[0] = (uint32_t)sig;
    args[1] = (uint32_t)(uintptr_t)si;
    args[2] = (uint32_t)(uintptr_t)uc;
    InterlockedIncrement(&t->delivered);
    elf_call(a.handler, 3, args);
    t->blocked[0] = old[0];
    t->blocked[1] = old[1];
}

static void sig_run_pending(void)
{
    struct lthread *t = l_pthread_self();
    int s;

    while ((s = sig_take(t)))
        sig_run_here(s, 0, 0);
}

static void CALLBACK sig_apc(ULONG_PTR p) { (void)p; sig_run_pending(); }

/*
 * The frame a redirected thread resumes on: [return to sig_tramp, sig,
 * &info, &uc], the FPU state (fxsave, 16-aligned), info, uc, a pad.  The
 * trampoline restores the mask, the FPU, then every register from uc,
 * through a 16-byte stub below the new esp.
 */
struct sig_frame {
    uint32_t ret, sig, psi, puc;
    uint8_t fx[512];
    uint8_t si[128];
    uint8_t uc[UC_SIZE];
    uint8_t pad[32];
};

void __attribute__((used)) sig_tramp_mask(uint8_t *uc)
{
    struct lthread *t = l_pthread_self();

    memcpy(t->blocked, uc + UC_SIGMASK, 8);
    if (sig_ready(t))
        sig_run_pending();
}

void sig_tramp(void);
__asm__(
    ".globl _sig_tramp\n"
    "_sig_tramp:\n"                            /* esp = &f->sig; the handler may have reused its arg slots */
    "    leal 652(%esp), %eax\n"               /* f->uc: 12 + 512 + 128 */
    "    pushl %eax\n"
    "    call _sig_tramp_mask\n"
    "    addl $4, %esp\n"
    "    leal 652(%esp), %eax\n"
    "    fxrstor -640(%eax)\n"                 /* the frame's fx: uc - 128 - 512 */
    "    movl 48(%eax), %ecx\n"                /* gregs[ESP] */
    "    subl $16, %ecx\n"
    "    movl 84(%eax), %edx\n"                /* EFL */
    "    movl %edx, 0(%ecx)\n"
    "    movl 60(%eax), %edx\n"                /* ECX */
    "    movl %edx, 4(%ecx)\n"
    "    movl 64(%eax), %edx\n"                /* EAX */
    "    movl %edx, 8(%ecx)\n"
    "    movl 76(%eax), %edx\n"                /* EIP */
    "    movl %edx, 12(%ecx)\n"
    "    movl 36(%eax), %edi\n"
    "    movl 40(%eax), %esi\n"
    "    movl 44(%eax), %ebp\n"
    "    movl 52(%eax), %ebx\n"
    "    movl 56(%eax), %edx\n"
    "    movl %ecx, %esp\n"
    "    popfl\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    ret\n");

/* redirect c (a thread's context, below = an address under which the frame
   may go) into the handler for sig */
static void sig_redirect(struct lthread *t, CONTEXT *c, uint32_t below, int sig, int code, uint32_t addr)
{
    struct l_sigact a = sigacts[sig];
    int on_alt = t->ss_size && c->Esp - t->ss_sp < t->ss_size;
    struct sig_frame *f;

    if ((a.flags & SA_ONSTACK_) && t->ss_size && !on_alt)
        below = t->ss_sp + t->ss_size;
    f = (struct sig_frame *)(((uintptr_t)below - sizeof(struct sig_frame)) & ~(uintptr_t)15);
    uc_from_ctx(f->uc, c, t->blocked);
    ((uint32_t *)f->uc)[2] = t->ss_sp;             /* uc_stack */
    ((uint32_t *)f->uc)[3] = t->ss_size ? (on_alt ? 1 : 0) : 2;
    ((uint32_t *)f->uc)[4] = t->ss_size;
    siginfo_make(f->si, sig, code, addr);
    if (c->ContextFlags & 0x20)                    /* CONTEXT_EXTENDED_REGISTERS: the FXSAVE image */
        memcpy(f->fx, c->ExtendedRegisters, 512);
    else
        __asm__ volatile("fxsave %0" : "=m"(*(uint8_t (*)[512])f->fx));
    f->ret = (uint32_t)(uintptr_t)sig_tramp;
    f->sig = (uint32_t)sig;
    f->psi = (uint32_t)(uintptr_t)f->si;
    f->puc = (uint32_t)(uintptr_t)f->uc;
    sig_bits_or(t->blocked, a.mask);
    if (!(a.flags & SA_NODEFER_))
        t->blocked[(sig - 1) / 32] |= 1u << ((sig - 1) % 32);
    InterlockedIncrement(&t->delivered);
    c->Esp = (uint32_t)(uintptr_t)f;
    c->Eip = (uint32_t)(uintptr_t)a.handler;
    c->EFlags &= ~0x400u;                          /* DF */
}

/* guest code: ELF, or anything not in a PE image (Mono's JIT) */
static int guest_code(uint32_t a)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (elf_owns(a))
        return 1;
    return VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE;
}

/* the VEH: a fault in guest code with a handler becomes its signal */
int lx_sig_fault(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    int sig, code = 0;
    uint32_t addr = (uint32_t)(uintptr_t)er->ExceptionAddress, here;

    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        sig = 11;
        code = 1;                                  /* SEGV_MAPERR */
        if (er->NumberParameters >= 2)
            addr = (uint32_t)er->ExceptionInformation[1];
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
        sig = 4;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        sig = 8;
        code = 1;                                  /* FPE_INTDIV */
        break;
    case EXCEPTION_INT_OVERFLOW:
        sig = 8;
        code = 2;
        break;
    default:
        return 0;
    }
    /* SA_SIGINFO handlers only: the older games' signal() handlers log and return, which
       would fault again for ever; theirs stay fatal (core_win.c's report) */
    if (!sig_is_fn(sigacts[sig].handler) || !(sigacts[sig].flags & SA_SIGINFO_) || !guest_code(c->Eip))
        return 0;
    if (lx_verbose) {
        char where[160];

        elf_describe(c->Eip, where, sizeof(where));
        LOG("signal %d at %s (address 0x%08x)", sig, where, addr);
    }
    /* below the exception dispatcher's frames, which this function runs on */
    here = (uint32_t)(uintptr_t)&here;
    sig_redirect(l_pthread_self(), c, (c->Esp < here ? c->Esp : here) - 256, sig, code, addr);
    return 1;
}

/* deliver what is pending for t, from another thread */
static void sig_kick(struct lthread *t)
{
    CONTEXT c;
    int tries, s;

    if (!t->h)
        return;
    for (tries = 0; tries < 2000 && sig_ready(t); tries++) {
        if (t->alertable) {
            QueueUserAPC(sig_apc, t->h, 0);
            return;
        }
        if (SuspendThread(t->h) == (DWORD)-1)
            return;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_FULL | CONTEXT_EXTENDED_REGISTERS;
        if (GetThreadContext(t->h, &c) && !t->alertable && guest_code(c.Eip) && (s = sig_take(t))) {
            sig_redirect(t, &c, c.Esp, s, 0, 0);
            SetThreadContext(t->h, &c);
            ResumeThread(t->h);
            continue;
        }
        ResumeThread(t->h);
        if (t->alertable) {
            QueueUserAPC(sig_apc, t->h, 0);
            return;
        }
        Sleep(tries < 50 ? 0 : 1);
    }
    if (sig_ready(t))
        QueueUserAPC(sig_apc, t->h, 0);            /* at its next alertable wait */
}

/* the alertable waits (once signals are in use): run APCs, then say "interrupted" */
static void wait_begin(void) { if (signals_live) l_pthread_self()->alertable = 1; }
static void wait_end(void) { if (signals_live) l_pthread_self()->alertable = 0; }

/* a wait on h; alertable once signals are in use, continued after a handler */
void lx_wait(HANDLE h)
{
    DWORD r;

    if (!signals_live) {
        WaitForSingleObject(h, INFINITE);
        return;
    }
    do {
        wait_begin();
        r = WaitForSingleObjectEx(h, INFINITE, TRUE);
        wait_end();
    } while (r == WAIT_IO_COMPLETION);
}

void lx_sleep_ms(DWORD ms)
{
    if (!signals_live) {
        Sleep(ms);
        return;
    }
    wait_begin();
    SleepEx(ms, TRUE);
    wait_end();
}

/* signal / sigaction / masks for libc_sys.c */
void *lx_signal(int sig, void *h)
{
    void *old;

    if (sig <= 0 || sig > 64)
        return (void *)-1;
    old = sigacts[sig].handler;
    memset(&sigacts[sig], 0, sizeof(sigacts[sig]));
    sigacts[sig].handler = h;
    if (lx_verbose)
        LOG("signal(%d, %p) recorded", sig, h);
    return old;
}

/* struct sigaction (i386): handler 0, mask 4 (128 bytes), flags 132, restorer 136 */
int lx_sigaction(int sig, const uint8_t *act, uint8_t *old)
{
    if (sig <= 0 || sig > 64)
        return -1;
    if (old) {
        memset(old, 0, 140);
        *(void **)old = sigacts[sig].handler;
        memcpy(old + 4, sigacts[sig].mask, 8);
        *(uint32_t *)(old + 132) = sigacts[sig].flags;
    }
    if (act) {
        sigacts[sig].handler = *(void *const *)act;
        memcpy(sigacts[sig].mask, act + 4, 8);
        sigacts[sig].flags = *(const uint32_t *)(act + 132);
        /* a thread-to-thread signal (USR1/2, XCPU, PWR, the real-time ones: Boehm GC's suspend and
           restart, Mono's interrupt): every wait from now on is alertable, before any thread waits */
        if (sig_is_fn(sigacts[sig].handler) && (sig == 10 || sig == 12 || sig == 24 || sig == 30 || sig >= 32))
            signals_live = 1;
        if (lx_verbose)
            LOG("sigaction(%d, %p, flags 0x%x)", sig, sigacts[sig].handler, sigacts[sig].flags);
    }
    return 0;
}

void *lx_sig_handler(int sig) { return sig > 0 && sig <= 64 ? sigacts[sig].handler : NULL; }

int lx_sigmask(int how, const uint32_t *set, uint32_t *old)
{
    struct lthread *t = l_pthread_self();

    if (old) {
        memset(old, 0, 128);
        old[0] = t->blocked[0];
        old[1] = t->blocked[1];
    }
    if (set) {
        if (how == 0) {                            /* SIG_BLOCK */
            t->blocked[0] |= set[0];
            t->blocked[1] |= set[1];
        } else if (how == 1) {                     /* SIG_UNBLOCK */
            t->blocked[0] &= ~set[0];
            t->blocked[1] &= ~set[1];
        } else {                                   /* SIG_SETMASK */
            t->blocked[0] = set[0];
            t->blocked[1] = set[1];
        }
        t->blocked[(9 - 1) / 32] &= ~(1u << (9 - 1));    /* SIGKILL, SIGSTOP stay unblockable */
        t->blocked[0] &= ~(1u << (19 - 1));
        if (sig_ready(t))
            sig_run_pending();
    }
    return 0;
}

/* raise: this thread, now; 1 when a handler ran */
int lx_raise(int sig)
{
    if (sig <= 0 || sig > 64 || !sig_is_fn(sigacts[sig].handler))
        return 0;
    sig_run_here(sig, 0, 0);
    return 1;
}

static int l_pthread_kill(struct lthread *t, int sig)
{
    if (!t)
        return 3;                                  /* ESRCH */
    if (sig <= 0 || sig > 64)
        return sig ? 22 : 0;
    if (!sig_is_fn(sigacts[sig].handler))
        return 0;                                  /* default or ignored: nothing to run here */
    InterlockedOr(&t->pend[(sig - 1) / 32], (LONG)(1u << ((sig - 1) % 32)));
    if (t == l_pthread_self()) {
        sig_run_pending();
        return 0;
    }
    signals_live = 1;
    sig_kick(t);
    return 0;
}

/* sigsuspend: the mask meanwhile, until a handler has run */
static int l_sigsuspend(const uint32_t *mask)
{
    struct lthread *t = l_pthread_self();
    uint32_t old[2] = { t->blocked[0], t->blocked[1] };
    LONG d = t->delivered;

    t->blocked[0] = mask[0];
    t->blocked[1] = mask[1];
    if (sig_ready(t))
        sig_run_pending();
    while (t->delivered == d) {
        t->alertable = 1;
        SleepEx(INFINITE, TRUE);
        t->alertable = 0;
    }
    t->blocked[0] = old[0];
    t->blocked[1] = old[1];
    SET_ERRNO(4);                                  /* EINTR */
    return -1;
}

/* sigaltstack: handlers run on the thread's own stack */
/* stack_t: ss_sp, ss_flags (SS_ONSTACK 1, SS_DISABLE 2), ss_size */
static int l_sigaltstack(const uint32_t *ss, uint32_t *old)
{
    struct lthread *t = l_pthread_self();
    uint32_t here = (uint32_t)(uintptr_t)&here;
    int on = t->ss_size && here - t->ss_sp < t->ss_size;

    if (old) {
        old[0] = t->ss_sp;
        old[1] = t->ss_size ? (on ? 1 : 0) : 2;
        old[2] = t->ss_size;
    }
    if (ss) {
        if (on) {
            SET_ERRNO(1);                          /* EPERM */
            return -1;
        }
        if (ss[1] & 2) {
            t->ss_sp = t->ss_size = 0;
        } else if (ss[2] < 2048) {                 /* MINSIGSTKSZ */
            SET_ERRNO(12);                         /* ENOMEM */
            return -1;
        } else {
            t->ss_sp = ss[0];
            t->ss_size = ss[2];
        }
    }
    return 0;
}

static int l_sigsetmask(int mask)
{
    uint32_t set[2] = { (uint32_t)mask, 0 }, old[32];

    lx_sigmask(2, set, old);
    return (int)old[0];
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
        stack_range(self_t);
    }
    return self_t;
}

/* pthread_attr_t (36 bytes): ours is [0] detached, [1] stack size, [2] stack address */
static int l_pthread_getattr_np(struct lthread *t, uint32_t *a)
{
    memset(a, 0, 36);
    a[1] = t->stack_hi - t->stack_lo;
    a[2] = t->stack_lo;
    return 0;
}
static int l_pthread_attr_getstack(const uint32_t *a, void **addr, size_t *size)
{
    *addr = (void *)(uintptr_t)a[2];
    *size = a[1];
    return 0;
}
static int l_pthread_attr_setstack(uint32_t *a, void *addr, size_t size)
{
    a[1] = (uint32_t)size;
    a[2] = (uint32_t)(uintptr_t)addr;
    return 0;
}
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
static int l_pthread_setcancelstate(int s, int *old) { (void)s; if (old) *old = 0; return 0; }
static int l_pthread_setschedparam(void *t, int pol, const void *p) { (void)t; (void)pol; (void)p; return 0; }
static int l_pthread_getschedparam(void *t, int *pol, int *p) { (void)t; *pol = 0; *p = 0; return 0; }
static int l_pthread_sigmask(int how, const uint32_t *s, uint32_t *old) { return lx_sigmask(how, s, old); }
static int l_sched_yield(void) { SwitchToThread(); return 0; }
static int l_sched_prio(int p) { (void)p; return 0; }
/* one CPU set per processor, as many as the mask holds */
static int l_sched_getaffinity(int pid, size_t size, uint8_t *mask)
{
    SYSTEM_INFO si;
    DWORD i;

    (void)pid;
    GetSystemInfo(&si);
    memset(mask, 0, size);
    for (i = 0; i < si.dwNumberOfProcessors && i / 8 < size; i++)
        mask[i / 8] |= 1 << (i % 8);
    return 0;
}

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

/* with signals in use, a contended lock waits alertably (a GC's suspend reaches the waiter) */
static int l_pthread_mutex_lock(void *m)
{
    CRITICAL_SECTION *cs = mtx(m);
    int spins = 0;

    if (!signals_live) {
        EnterCriticalSection(cs);
        return 0;
    }
    while (!TryEnterCriticalSection(cs)) {
        wait_begin();
        SleepEx(spins++ < 50 ? 0 : 1, TRUE);
        wait_end();
    }
    return 0;
}
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

/* with signals in use: 10 ms slices, a pending signal's handler run with the mutex released (a spurious wakeup) */
static void cond_signals(void *m)
{
    LeaveCriticalSection(mtx(m));
    wait_begin();
    SleepEx(0, TRUE);
    wait_end();
    l_pthread_mutex_lock(m);
}

static int l_pthread_cond_wait(void *c, void *m)
{
    if (!signals_live) {
        SleepConditionVariableCS(c, mtx(m), INFINITE);
        return 0;
    }
    if (!SleepConditionVariableCS(c, mtx(m), 10))
        cond_signals(m);
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
    if (signals_live && ms > 10) {
        if (!SleepConditionVariableCS((PCONDITION_VARIABLE)c, mtx(m), 10))
            cond_signals(m);
        return 0;                                  /* a spurious wakeup: the caller checks its time */
    }
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
static int l_sem_wait(HANDLE *s)
{
    DWORD r;

    if (!signals_live) {
        WaitForSingleObject(*s, INFINITE);
        return 0;
    }
    do {                                           /* restarted after a handler, as SA_RESTART */
        wait_begin();
        r = WaitForSingleObjectEx(*s, INFINITE, TRUE);
        wait_end();
    } while (r == WAIT_IO_COMPLETION);
    return 0;
}

static int l_sem_timedwait(HANDLE *s, const struct l_timespec *abst)
{
    struct l_timespec now;
    int64_t ms;
    DWORD r;

    l_clock_gettime_pub(0, &now);
    ms = (int64_t)(abst->tv_sec - now.tv_sec) * 1000 + (abst->tv_nsec - now.tv_nsec) / 1000000;
    wait_begin();
    r = WaitForSingleObjectEx(*s, ms < 0 ? 0 : (DWORD)ms, TRUE);
    wait_end();
    if (r == WAIT_OBJECT_0)
        return 0;
    SET_ERRNO(r == WAIT_IO_COMPLETION ? 4 : L_ETIMEDOUT);
    return -1;
}
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
    N(sem_timedwait), N(sigsuspend), N(sigaltstack), N(sigsetmask), N(pthread_getattr_np),
    N(pthread_attr_getstack), N(pthread_attr_setstack),
    N(sched_yield), N(sched_getaffinity), NA(sched_get_priority_max, l_sched_prio), NA(sched_get_priority_min, l_sched_prio),
    { NULL, NULL },
};

void thread_init(void)
{
    InitializeCriticalSection(&once_lock);
    l_pthread_self();
}
