/* Runtime contract for statically recompiled AArch64 code.
 *
 * Guest data addresses are host addresses (modules are mapped at their guest base), so
 * memory accesses are plain dereferences. Guest code addresses are only ever looked up
 * (rt_lookup) - recompiled functions are ordinary C functions taking the CPU context. */
#pragma once
#ifdef _WIN32
#include <excpt.h>
#endif
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

/* Linux builds compile the generated files that have exception landing sites as C++ (see
 * rt_eh_filter below); everything here keeps C linkage either way. */
#ifdef __cplusplus
#define _Alignas(n) alignas(n)
#define _Noreturn __attribute__((noreturn))
extern "C" {
#endif

typedef uint8_t u8;   typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t i8;    typedef int16_t i16;  typedef int32_t i32;  typedef int64_t i64;
typedef float f32;    typedef double f64;

typedef union V128 {
    u8 b[16]; u16 h[8]; u32 s[4]; u64 d[2];
    i8 ib[16]; i16 ih[8]; i32 is[4]; i64 id[2];
    f32 f[4]; f64 g[2];
} V128;

typedef struct Cpu {
    u64 x[31];
    u64 sp;
    _Alignas(16) V128 v[32];
    u32 fpcr, fpsr;
    u64 tpidr;     /* TPIDR_EL0: free for the guest's use */
    u64 tpidrro;   /* TPIDRRO_EL0: address of the thread's thread-local region */
    void *thread;  /* the native kernel's object for the thread running on this context */
} Cpu;

typedef void GuestFn(Cpu *);

GuestFn *rt_lookup(u64 guest_addr);
/* A branch site's memory of its last target, which saves the lookup while the target stays
 * the same. The key is the address mixed with the function, so an entry another thread is
 * halfway through writing never matches. */
typedef struct RtCache { volatile u64 key; GuestFn *volatile fn; } RtCache;
#define RT_CACHE_MIX(fn) ((u64)(fn) * 0x9E3779B97F4A7C15ull)
GuestFn *rt_lookup_cached(u64 guest_addr, RtCache *cache);   /* rt_lookup, remembering the result when it may be */
static inline GuestFn *rt_target(u64 guest_addr, RtCache *cache) {
    GuestFn *fn = cache->fn;
    return (cache->key ^ RT_CACHE_MIX(fn)) == guest_addr ? fn : rt_lookup_cached(guest_addr, cache);
}
jmp_buf *rt_setjmp(Cpu *c);   /* a guest setjmp(X0): its host buffer, the guest registers saved */
/* C++ exceptions: a call that may unwind into landing pad `lp` of its function catches the
 * host exception rt_unwind_jumpto raises for that pad and this frame's stack pointer. */
#ifdef _WIN32
struct _EXCEPTION_POINTERS;
int rt_eh_filter(struct _EXCEPTION_POINTERS *e, u64 sp, u64 lp);
#ifndef GetExceptionInformation
#define GetExceptionInformation() ((struct _EXCEPTION_POINTERS *)_exception_info())
#endif
#else
/* Linux has no structured exception handling in C: the landing sites' __try / __except become
 * a C++ try and a catch of the exception rt_unwind_jumpto throws (runtime/linux/eh.cpp), which
 * goes on unwinding unless the filter matches this frame - the same test, the same result. */
typedef struct RtEhSignal { u64 lp, sp; } RtEhSignal;
int rt_eh_filter(const RtEhSignal *e, u64 sp, u64 lp);
#ifdef __cplusplus
#define __try try
#define __except(filter) catch (const RtEhSignal &rt_e_) { if (!(filter)) throw; lp_ = 1; } if (0)
#define GetExceptionInformation() (&rt_e_)
#endif
#endif
_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why);
void rt_svc(Cpu *c, u32 number);   /* a guest system call */
u64 rt_ticks(void);                /* the 19.2 MHz system counter */

#define X(n) (c->x[n])
#define SP (c->sp)
#define RS(n) (c->v[n].f[0])
#define RD(n) (c->v[n].g[0])
#define TAIL(f) { __attribute__((musttail)) return f(c); }

/* ---- memory -------------------------------------------------------------------- */
#define RT_MEM(T, N) \
    static inline T ld##N(u64 a) { T v; memcpy(&v, (const void *)a, sizeof v); return v; } \
    static inline void st##N(u64 a, T v) { memcpy((void *)a, &v, sizeof v); } \
    static inline u32 cas##N(u64 a, u64 expect, u64 val) { \
        T e = (T)expect; \
        return __atomic_compare_exchange_n((T *)a, &e, (T)val, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); } \
    static inline void sta##N(u64 a, T v) { __atomic_store_n((T *)a, v, __ATOMIC_SEQ_CST); }
RT_MEM(u8, 8) RT_MEM(u16, 16) RT_MEM(u32, 32) RT_MEM(u64, 64)

static inline void ldv8(Cpu *c, int n, u64 a)   { c->v[n].d[0] = ld8(a);  c->v[n].d[1] = 0; }
static inline void ldv16(Cpu *c, int n, u64 a)  { c->v[n].d[0] = ld16(a); c->v[n].d[1] = 0; }
static inline void ldv32(Cpu *c, int n, u64 a)  { c->v[n].d[0] = ld32(a); c->v[n].d[1] = 0; }
static inline void ldv64(Cpu *c, int n, u64 a)  { c->v[n].d[0] = ld64(a); c->v[n].d[1] = 0; }
static inline void ldv128(Cpu *c, int n, u64 a) { memcpy(&c->v[n], (const void *)a, 16); }
static inline void stv8(Cpu *c, int n, u64 a)   { st8(a, c->v[n].b[0]); }
static inline void stv16(Cpu *c, int n, u64 a)  { st16(a, c->v[n].h[0]); }
static inline void stv32(Cpu *c, int n, u64 a)  { st32(a, c->v[n].s[0]); }
static inline void stv64(Cpu *c, int n, u64 a)  { st64(a, c->v[n].d[0]); }
static inline void stv128(Cpu *c, int n, u64 a) { memcpy((void *)a, &c->v[n], 16); }

/* Scalar FP writes clear the rest of the vector register, as on AArch64. */
static inline void wS(Cpu *c, int n, f32 v) { u32 b; memcpy(&b, &v, 4); c->v[n].d[0] = b; c->v[n].d[1] = 0; }
static inline void wD(Cpu *c, int n, f64 v) { c->v[n].g[0] = v; c->v[n].d[1] = 0; }

/* ---- integer helpers ----------------------------------------------------------- */
static inline u32 ror32(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline u64 ror64(u64 v, u32 n) { n &= 63; return n ? (v >> n) | (v << (64 - n)) : v; }
static inline u32 udiv32(u32 a, u32 b) { return b ? a / b : 0; }
static inline u64 udiv64(u64 a, u64 b) { return b ? a / b : 0; }
static inline u32 sdiv32(u32 a, u32 b) {
    if (!b) return 0;
    if (a == 0x80000000u && b == 0xFFFFFFFFu) return a;
    return (u32)((i32)a / (i32)b);
}
static inline u64 sdiv64(u64 a, u64 b) {
    if (!b) return 0;
    if (a == 0x8000000000000000ull && b == ~0ull) return a;
    return (u64)((i64)a / (i64)b);
}
static inline u32 clz32(u32 v) { return v ? (u32)__builtin_clz(v) : 32; }
static inline u64 clz64(u64 v) { return v ? (u64)__builtin_clzll(v) : 64; }
static inline u32 cls32(u32 v) { return clz32(v ^ (u32)((i32)v >> 1)) - 1; }
static inline u64 cls64(u64 v) { return clz64(v ^ (u64)((i64)v >> 1)) - 1; }
static inline u32 rev16_32(u32 v) { return ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8); }
static inline u64 rev16_64(u64 v) { return ((v & 0xFF00FF00FF00FF00ull) >> 8) | ((v & 0x00FF00FF00FF00FFull) << 8); }
static inline u64 rev32_64(u64 v) { return ((u64)__builtin_bswap32((u32)(v >> 32)) << 32) | __builtin_bswap32((u32)v); }
static inline u64 umulh64(u64 a, u64 b) { return (u64)(((unsigned __int128)a * b) >> 64); }
static inline u64 smulh64(u64 a, u64 b) { return (u64)(((__int128)(i64)a * (i64)b) >> 64); }

/* ---- floating point: AArch64 semantics where they differ from x86 -------------- */
static inline i32 f2i32(f64 v) { return v != v ? 0 : v >= 2147483648.0 ? INT32_MAX : v <= -2147483649.0 ? INT32_MIN : (i32)v; }
static inline u32 f2u32(f64 v) { return v != v ? 0 : v >= 4294967296.0 ? UINT32_MAX : v <= -1.0 ? 0 : (u32)v; }
static inline i64 f2i64(f64 v) { return v != v ? 0 : v >= 9223372036854775808.0 ? INT64_MAX : v < -9223372036854775808.0 ? INT64_MIN : (i64)v; }
static inline u64 f2u64(f64 v) { return v != v ? 0 : v >= 18446744073709551616.0 ? UINT64_MAX : v <= -1.0 ? 0 : (u64)v; }
static inline f32 fmaxS(f32 a, f32 b) { return a != a || b != b ? NAN : a > b ? a : b > a ? b : signbit(a) ? b : a; }
static inline f32 fminS(f32 a, f32 b) { return a != a || b != b ? NAN : a < b ? a : b < a ? b : signbit(a) ? a : b; }
static inline f64 fmaxD(f64 a, f64 b) { return a != a || b != b ? NAN : a > b ? a : b > a ? b : signbit(a) ? b : a; }
static inline f64 fminD(f64 a, f64 b) { return a != a || b != b ? NAN : a < b ? a : b < a ? b : signbit(a) ? a : b; }
static inline f32 fmaxnmS(f32 a, f32 b) { return a != a ? b : b != b ? a : fmaxS(a, b); }
static inline f32 fminnmS(f32 a, f32 b) { return a != a ? b : b != b ? a : fminS(a, b); }
static inline f64 fmaxnmD(f64 a, f64 b) { return a != a ? b : b != b ? a : fmaxD(a, b); }
static inline f64 fminnmD(f64 a, f64 b) { return a != a ? b : b != b ? a : fminD(a, b); }

/* fmulx / Newton-Raphson step helpers: fused, and inf * 0 has a defined result. */
#define RT_INF_ZERO(a, b) ((isinf(a) && (b) == 0) || ((a) == 0 && isinf(b)))
static inline f32 rt_fmulx32(f32 a, f32 b) { return RT_INF_ZERO(a, b) ? (signbit(a) != signbit(b) ? -2.0f : 2.0f) : a * b; }
static inline f64 rt_fmulx64(f64 a, f64 b) { return RT_INF_ZERO(a, b) ? (signbit(a) != signbit(b) ? -2.0 : 2.0) : a * b; }
static inline f32 rt_frecps32(f32 a, f32 b) { return RT_INF_ZERO(a, b) ? 2.0f : fmaf(-a, b, 2.0f); }
static inline f64 rt_frecps64(f64 a, f64 b) { return RT_INF_ZERO(a, b) ? 2.0 : fma(-a, b, 2.0); }
/* (3 - a*b) / 2 with one rounding: halve whichever operand is larger, exactly, then fuse. */
static inline f32 rt_frsqrts32(f32 a, f32 b) {
    if (RT_INF_ZERO(a, b)) return 1.5f;
    return fabsf(a) >= fabsf(b) ? fmaf(-(a * 0.5f), b, 1.5f) : fmaf(-a, b * 0.5f, 1.5f);
}
static inline f64 rt_frsqrts64(f64 a, f64 b) {
    if (RT_INF_ZERO(a, b)) return 1.5;
    return fabs(a) >= fabs(b) ? fma(-(a * 0.5), b, 1.5) : fma(-a, b * 0.5, 1.5);
}

/* Reciprocal and reciprocal-square-root estimates: the architecture's table algorithm,
 * which x86's rcpps / rsqrtps do not reproduce. */
static inline u32 rt_recip_est(u32 a) { a = a * 2 + 1; return ((1u << 19) / a + 1) / 2; }
static inline u32 rt_rsqrt_est(u32 a) {
    a = a < 256 ? a * 2 + 1 : ((a & ~1u) + 1) * 2;
    u32 b = 512;
    while ((u64)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (b + 1) / 2;
}
#define RT_FRAC52 0xFFFFFFFFFFFFFull
static inline u64 rt_frecpe_frac(u64 frac, int *exp, int top) {
    if (!*exp) {
        if (!(frac >> 51 & 1)) { *exp = -1; frac = (frac << 2) & RT_FRAC52; }
        else frac = (frac << 1) & RT_FRAC52;
    }
    u64 r = (u64)(rt_recip_est(0x100 | (u32)(frac >> 44)) & 0xFF) << 44;
    *exp = top - *exp;
    if (*exp == 0) r = (1ull << 51) | r >> 1;
    else if (*exp == -1) { r = (1ull << 50) | r >> 2; *exp = 0; }
    return r;
}
static inline f32 rt_frecpe32(f32 x) {
    u32 v, out; memcpy(&v, &x, 4);
    u32 sign = v & 0x80000000u, mant = v & 0x7FFFFFu; int exp = (v >> 23) & 0xFF;
    if (exp == 0xFF) out = mant ? (v | 0x400000u) : sign;
    else if (!exp && mant < (1u << 21)) out = sign | 0x7F800000u;   /* zero, or too small: infinity */
    else { u64 r = rt_frecpe_frac((u64)mant << 29, &exp, 253); out = sign | (u32)exp << 23 | (u32)(r >> 29); }
    f32 f; memcpy(&f, &out, 4); return f;
}
static inline f64 rt_frecpe64(f64 x) {
    u64 v, out; memcpy(&v, &x, 8);
    u64 sign = v & (1ull << 63), mant = v & RT_FRAC52; int exp = (v >> 52) & 0x7FF;
    if (exp == 0x7FF) out = mant ? (v | 1ull << 51) : sign;
    else if (!exp && mant < (1ull << 50)) out = sign | 0x7FF0000000000000ull;
    else { u64 r = rt_frecpe_frac(mant, &exp, 2045); out = sign | (u64)exp << 52 | r; }
    f64 f; memcpy(&f, &out, 8); return f;
}
static inline u32 rt_frsqrte_est(u64 frac, int *exp, int top) {
    if (!*exp) {
        while (!(frac >> 51 & 1)) { frac = (frac << 1) & RT_FRAC52; --*exp; }
        frac = (frac << 1) & RT_FRAC52;
    }
    u32 scaled = (*exp & 1) ? (0x80 | (u32)(frac >> 45)) : (0x100 | (u32)(frac >> 44));
    *exp = (top - *exp) / 2;
    return rt_rsqrt_est(scaled) & 0xFF;
}
static inline f32 rt_frsqrte32(f32 x) {
    u32 v, out; memcpy(&v, &x, 4);
    u32 sign = v & 0x80000000u, mant = v & 0x7FFFFFu; int exp = (v >> 23) & 0xFF;
    if (exp == 0xFF && mant) out = v | 0x400000u;
    else if (!exp && !mant) out = sign | 0x7F800000u;
    else if (sign) out = 0x7FC00000u;
    else if (exp == 0xFF) out = 0;
    else { u32 e = rt_frsqrte_est((u64)mant << 29, &exp, 380); out = (u32)exp << 23 | e << 15; }
    f32 f; memcpy(&f, &out, 4); return f;
}
static inline f64 rt_frsqrte64(f64 x) {
    u64 v, out; memcpy(&v, &x, 8);
    u64 sign = v & (1ull << 63), mant = v & RT_FRAC52; int exp = (v >> 52) & 0x7FF;
    if (exp == 0x7FF && mant) out = v | 1ull << 51;
    else if (!exp && !mant) out = sign | 0x7FF0000000000000ull;
    else if (sign) out = 0x7FF8000000000000ull;
    else if (exp == 0x7FF) out = 0;
    else { u64 e = rt_frsqrte_est(mant, &exp, 3068); out = (u64)exp << 52 | e << 44; }
    f64 f; memcpy(&f, &out, 8); return f;
}

/* Half precision <-> wider formats (round to nearest even, single rounding). */
static inline f32 rt_h2f(u16 h) {
    u32 sign = (u32)(h >> 15) << 31, exp = (h >> 10) & 0x1F, man = h & 0x3FF, out;
    if (exp == 31) out = sign | 0x7F800000u | man << 13 | (man ? 0x400000u : 0);
    else if (exp) out = sign | (exp + 112) << 23 | man << 13;
    else if (!man) out = sign;
    else {
        int e = 113;
        do { man <<= 1; e--; } while (!(man & 0x400));
        out = sign | (u32)e << 23 | (man & 0x3FF) << 13;
    }
    f32 f; memcpy(&f, &out, 4); return f;
}
static inline u16 rt_d2h(f64 d) {
    u64 v; memcpy(&v, &d, 8);
    u16 sign = (u16)((v >> 48) & 0x8000);
    int exp = (int)((v >> 52) & 0x7FF);
    u64 man = v & RT_FRAC52;
    if (exp == 0x7FF) return sign | 0x7C00 | (man ? 0x200 | (u16)(man >> 42) : 0);
    if (!exp) return sign;
    int e = exp - 1023;
    if (e > 15) return sign | 0x7C00;
    int shift = e >= -14 ? 42 : 42 + (-14 - e);
    if (shift > 54) return sign;
    man |= 1ull << 52;
    u64 q = man >> shift, rem = man & ((1ull << shift) - 1), half = 1ull << (shift - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    return sign | (u16)(e >= -14 ? ((u64)(e + 15) << 10) + (q - 0x400) : q);
}

/* ---- AdvSIMD helpers ----------------------------------------------------------- */
/* Variable shifts: the count is a signed byte; negative counts shift right. */
static inline u64 rt_ushl(u64 a, i8 sh, int esize) {
    if (sh >= esize || sh <= -esize) return 0;
    return sh >= 0 ? a << sh : a >> -sh;
}
static inline u64 rt_sshl(i64 a, i8 sh, int esize) {
    if (sh >= esize) return 0;
    if (sh <= -esize) return a < 0 ? ~0ull : 0;
    return sh >= 0 ? (u64)a << sh : (u64)(a >> -sh);
}

static inline void rt_tbl(Cpu *c, int rd, int rn, int len, int rm, int q, int tbx) {
    u8 table[64];
    for (int i = 0; i < len; i++) memcpy(table + 16 * i, &c->v[(rn + i) & 31], 16);
    V128 idx = c->v[rm], r = c->v[rd];
    for (int i = 0; i < (8 << q); i++) {
        u8 k = idx.b[i];
        if (k < 16 * len) r.b[i] = table[k];
        else if (!tbx) r.b[i] = 0;
    }
    if (!q) r.d[1] = 0;
    c->v[rd] = r;
}

/* ld1-ld4 / st1-st4 over whole registers: `selem` registers are interleaved per element,
 * `rpt` repeats that for consecutive registers. */
static inline void rt_ldst_multi(Cpu *c, int load, int rt, int rpt, int selem, int ebytes, int q, u64 addr) {
    int elements = (8 << q) / ebytes;
    for (int r = 0; r < rpt; r++)
        for (int e = 0; e < elements; e++)
            for (int s = 0; s < selem; s++, addr += ebytes) {
                u8 *lane = c->v[(rt + r + s) & 31].b + e * ebytes;
                if (load) memcpy(lane, (const void *)addr, ebytes);
                else memcpy((void *)addr, lane, ebytes);
            }
    if (load && !q)
        for (int k = 0; k < rpt * selem; k++) c->v[(rt + k) & 31].d[1] = 0;
}

/* Single-lane structure access, and ld1r-ld4r (load one element and replicate it). */
static inline void rt_ldst_single(Cpu *c, int load, int replicate, int rt, int selem, int ebytes, int index,
                                  int q, u64 addr) {
    for (int s = 0; s < selem; s++, addr += ebytes) {
        V128 *v = &c->v[(rt + s) & 31];
        if (replicate) {
            V128 r = {0};
            for (int e = 0; e < (8 << q) / ebytes; e++) memcpy(r.b + e * ebytes, (const void *)addr, ebytes);
            *v = r;
        } else if (load) memcpy(v->b + index * ebytes, (const void *)addr, ebytes);
        else memcpy((void *)addr, v->b + index * ebytes, ebytes);
    }
}

#ifdef __cplusplus
}
#endif
