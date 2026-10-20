/* nn::os fibers, natively.
 *
 * On the console a fiber switch is a hand-written routine, DispatchFiberContext(save, load):
 * it pushes x0, x19-x30 and q8-q15 as a 0xF0-byte frame, stores the stack pointer in *save,
 * loads the stack pointer from *load, pops that frame and returns into it. Recompiled code
 * cannot do that - a guest call is a host call, so the host stack holds the real frames -
 * so each guest fiber runs on a Windows fiber with its own host stack and its own guest
 * register context, and this routine is the only part of the SDK's fiber code replaced.
 *
 * *save receives a token naming the suspended context. A load of anything else is a frame
 * prepared by nn::os::InitializeFiber on the new fiber's stack: a host fiber is created that
 * loads that frame exactly as the original would and jumps to the saved x30.
 *
 * Fibers may resume on a different thread from the one they were suspended on, so the
 * thread's identity (kernel thread, thread-local region, TPIDR_EL0) moves with every switch. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "host.h"

#define TOKEN_TAG 0xF1BE000000000000ull
#define FRAME 0xF0

typedef struct Fib { LPVOID host; Cpu *cpu; u64 token; } Fib;

static Fib *fibs[16384];
static u32 fib_count;
static SRWLOCK lock = SRWLOCK_INIT;

static Fib *by_token(u64 token) {
    if ((token & 0xFFFF000000000000ull) != TOKEN_TAG) return NULL;
    u64 i = token & 0xFFFFFFFF;
    return i < fib_count ? fibs[i] : NULL;
}

static Fib *new_fib(Cpu *cpu) {
    Fib *f = calloc(1, sizeof *f);
    f->cpu = cpu;
    AcquireSRWLockExclusive(&lock);
    if (fib_count == 16384) rt_fatal("too many fibers");
    f->token = TOKEN_TAG | fib_count;
    fibs[fib_count++] = f;
    ReleaseSRWLockExclusive(&lock);
    return f;
}

static void WINAPI fiber_main(void *param) {
    Fib *f = param;
    rt_lookup(f->cpu->x[30])(f->cpu);
    rt_fatal("a fiber's entry function returned");
}

HLE(_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_) {
    u64 save = c->x[0], load = c->x[1], target = ld64(load);

    /* The context being left: this host fiber (converting the thread on first use). */
    Fib *me;
    if (IsThreadAFiber() && GetFiberData()) me = GetFiberData();
    else {
        me = new_fib(c);
        me->host = ConvertThreadToFiberEx(me, FIBER_FLAG_FLOAT_SWITCH);
        if (!me->host) rt_fatal("cannot make a fiber of a guest thread (error %lu)", (unsigned long)GetLastError());
    }
    me->cpu = c;
    st64(save, me->token);

    Fib *next = by_token(target);
    if (!next) {   /* a fresh fiber: the frame InitializeFiber prepared at `target` */
        Cpu *n = _aligned_malloc(sizeof *n, 64);
        memset(n, 0, sizeof *n);
        n->x[0] = ld64(target);
        for (int r = 19; r <= 30; r++) n->x[r] = ld64(target + 8ull * (r - 18));
        for (int q = 8; q <= 15; q++) memcpy(&n->v[q], P(void, target + 0x70 + 16ull * (q - 8)), 16);
        n->sp = target + FRAME;
        next = new_fib(n);
        next->host = CreateFiberEx(64 << 10, 64 << 20, FIBER_FLAG_FLOAT_SWITCH, fiber_main, next);
        if (!next->host) rt_fatal("cannot create a host fiber (error %lu)", (unsigned long)GetLastError());
    } else {
        next->cpu->x[0] = 0;   /* what the frame of a suspended context holds in x0 */
    }
    Cpu *n = next->cpu;
    n->thread = c->thread; n->tpidrro = c->tpidrro; n->tpidr = c->tpidr; n->fpcr = c->fpcr; n->fpsr = c->fpsr;
    SwitchToFiber(next->host);
    /* Resumed - possibly on another thread, whose identity the switcher handed us. */
}
