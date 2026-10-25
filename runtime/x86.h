/* Guest CPU model and helpers used by the generated (recompiled) C code.
 *
 * The guest is a 32-bit x86 Windows process. Guest memory is a flat 4 GiB
 * region reserved at MEM; guest pointers are 32-bit offsets into it.
 * General-purpose registers and flags live in locals inside each generated
 * function and are spilled to CPU around calls, so the C compiler can
 * remove dead flag computations.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
    float f[4];
    double d[2];
    uint8_t b[16];
    uint16_t w[8];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} Xmm;

typedef struct CPU {
    uint32_t r[8];          /* eax ecx edx ebx esp ebp esi edi */
    uint32_t fs_base;       /* guest address of the TEB */
    uint32_t df;            /* direction flag */
    uint32_t eflags_hi;     /* other eflags bits for pushfd/popfd */
    /* x87 */
    double st[8];
    uint32_t top;
    uint32_t c0, c1, c2, c3;
    uint16_t fpu_cw;
    uint16_t fpu_tag;
    /* SSE */
    Xmm xmm[8];
    uint32_t mxcsr;
    uint64_t mm[8];
    /* host-side per-thread data */
    void *thread;
} CPU;

extern uint8_t *MEM;

typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;

#define M8(a)  (*(uint8_t *)(MEM + (uint32_t)(a)))
#define M16(a) (*(u16u *)(MEM + (uint32_t)(a)))
#define M32(a) (*(u32u *)(MEM + (uint32_t)(a)))
#define M64(a) (*(u64u *)(MEM + (uint32_t)(a)))
#define MF32(a) (*(f32u *)(MEM + (uint32_t)(a)))
#define MF64(a) (*(f64u *)(MEM + (uint32_t)(a)))

typedef struct { uint32_t cf, zf, sf, of, pf; } Flags;

static inline uint32_t par8(uint32_t r) { return !__builtin_parity(r & 0xFF); }

#define DEFOPS(N, T, ST)                                                          \
static inline T add##N(T a, T b, Flags *f) {                                      \
    T r = (T)(a + b);                                                             \
    f->cf = r < a; f->zf = r == 0; f->sf = (ST)r < 0;                             \
    f->of = ((~(a ^ b) & (a ^ r)) >> (N - 1)) & 1; f->pf = par8(r); return r; }   \
static inline T adc##N(T a, T b, Flags *f) {                                      \
    T c = (T)f->cf; T r = (T)(a + b + c);                                         \
    f->cf = c ? (r <= a) : (r < a); f->zf = r == 0; f->sf = (ST)r < 0;            \
    f->of = ((~(a ^ b) & (a ^ r)) >> (N - 1)) & 1; f->pf = par8(r); return r; }   \
static inline T sub##N(T a, T b, Flags *f) {                                      \
    T r = (T)(a - b);                                                             \
    f->cf = a < b; f->zf = r == 0; f->sf = (ST)r < 0;                             \
    f->of = (((a ^ b) & (a ^ r)) >> (N - 1)) & 1; f->pf = par8(r); return r; }    \
static inline T sbb##N(T a, T b, Flags *f) {                                      \
    T c = (T)f->cf; T r = (T)(a - b - c);                                         \
    f->cf = c ? (a <= b) : (a < b); f->zf = r == 0; f->sf = (ST)r < 0;            \
    f->of = (((a ^ b) & (a ^ r)) >> (N - 1)) & 1; f->pf = par8(r); return r; }    \
static inline T logic##N(T r, Flags *f) {                                         \
    f->cf = 0; f->of = 0; f->zf = r == 0; f->sf = (ST)r < 0; f->pf = par8(r);     \
    return r; }                                                                   \
static inline T inc##N(T a, Flags *f) {                                           \
    T r = (T)(a + 1); f->zf = r == 0; f->sf = (ST)r < 0;                          \
    f->of = r == (T)((T)1 << (N - 1)); f->pf = par8(r); return r; }               \
static inline T dec##N(T a, Flags *f) {                                           \
    T r = (T)(a - 1); f->zf = r == 0; f->sf = (ST)r < 0;                          \
    f->of = a == (T)((T)1 << (N - 1)); f->pf = par8(r); return r; }               \
static inline T neg##N(T a, Flags *f) {                                           \
    T r = (T)(0 - a); f->cf = a != 0; f->zf = r == 0; f->sf = (ST)r < 0;          \
    f->of = a == (T)((T)1 << (N - 1)); f->pf = par8(r); return r; }               \
static inline T shl##N(T a, uint32_t n, Flags *f) {                               \
    n &= 31; if (!n) return a;                                                    \
    T r = n >= N ? 0 : (T)(a << n);                                               \
    f->cf = n <= N ? (a >> (N - n)) & 1 : 0; f->zf = r == 0; f->sf = (ST)r < 0;   \
    f->of = ((r >> (N - 1)) & 1) ^ f->cf; f->pf = par8(r); return r; }            \
static inline T shr##N(T a, uint32_t n, Flags *f) {                               \
    n &= 31; if (!n) return a;                                                    \
    T r = n >= N ? 0 : (T)(a >> n);                                               \
    f->cf = n <= N ? (a >> (n - 1)) & 1 : 0; f->zf = r == 0; f->sf = (ST)r < 0;   \
    f->of = (a >> (N - 1)) & 1; f->pf = par8(r); return r; }                      \
static inline T sar##N(T a, uint32_t n, Flags *f) {                               \
    n &= 31; if (!n) return a;                                                    \
    uint32_t m = n >= N ? N - 1 : n;                                              \
    T r = (T)((ST)a >> m);                                                        \
    f->cf = ((ST)a >> (m - 1)) & 1; if (n >= N) f->cf = (a >> (N - 1)) & 1;       \
    f->zf = r == 0; f->sf = (ST)r < 0; f->of = 0; f->pf = par8(r); return r; }    \
static inline T rol##N(T a, uint32_t n, Flags *f) {                               \
    n &= 31; if (!n) return a; n %= N;                                            \
    T r = n ? (T)((a << n) | (a >> (N - n))) : a;                                 \
    f->cf = r & 1; f->of = ((r >> (N - 1)) & 1) ^ f->cf; return r; }              \
static inline T ror##N(T a, uint32_t n, Flags *f) {                               \
    n &= 31; if (!n) return a; n %= N;                                            \
    T r = n ? (T)((a >> n) | (a << (N - n))) : a;                                 \
    f->cf = (r >> (N - 1)) & 1; f->of = f->cf ^ ((r >> (N - 2)) & 1); return r; } \
static inline T rcl##N(T a, uint32_t n, Flags *f) {                               \
    n = (n & 31) % (N + 1);                                                       \
    while (n--) { uint32_t c = (a >> (N - 1)) & 1; a = (T)((a << 1) | f->cf);     \
                  f->cf = c; }                                                    \
    f->of = ((a >> (N - 1)) & 1) ^ f->cf; return a; }                             \
static inline T rcr##N(T a, uint32_t n, Flags *f) {                               \
    n = (n & 31) % (N + 1);                                                       \
    while (n--) { uint32_t c = a & 1;                                             \
                  a = (T)((a >> 1) | ((T)f->cf << (N - 1))); f->cf = c; }         \
    f->of = ((a >> (N - 1)) ^ (a >> (N - 2))) & 1; return a; }

DEFOPS(8, uint8_t, int8_t)
DEFOPS(16, uint16_t, int16_t)
DEFOPS(32, uint32_t, int32_t)

static inline uint32_t shld32(uint32_t a, uint32_t b, uint32_t n, Flags *f) {
    n &= 31; if (!n) return a;
    uint32_t r = (a << n) | (b >> (32 - n));
    f->cf = (a >> (32 - n)) & 1; f->zf = r == 0; f->sf = (int32_t)r < 0; f->pf = par8(r);
    f->of = ((r ^ a) >> 31) & 1; return r;
}
static inline uint32_t shrd32(uint32_t a, uint32_t b, uint32_t n, Flags *f) {
    n &= 31; if (!n) return a;
    uint32_t r = (a >> n) | (b << (32 - n));
    f->cf = (a >> (n - 1)) & 1; f->zf = r == 0; f->sf = (int32_t)r < 0; f->pf = par8(r);
    f->of = ((r ^ a) >> 31) & 1; return r;
}

/* ---- x87 ---- */
#define ST(i) (c->st[(c->top + (i)) & 7])
static inline void fpush(CPU *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline void fpop(CPU *c) { c->top = (c->top + 1) & 7; }
static inline void fcompare(CPU *c, double a, double b) {
    if (a != a || b != b) { c->c0 = c->c2 = c->c3 = 1; }
    else { c->c0 = a < b; c->c2 = 0; c->c3 = a == b; }
}
static inline uint16_t fstatus(CPU *c) {
    return (uint16_t)((c->c3 << 14) | ((c->top & 7) << 11) | (c->c2 << 10) |
                      (c->c1 << 9) | (c->c0 << 8));
}
static inline double fround(CPU *c, double v) {
    switch ((c->fpu_cw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline int32_t fist32(CPU *c, double v) {
    v = fround(c, v);
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return INT32_MIN;
    return (int32_t)v;
}
static inline int16_t fist16(CPU *c, double v) {
    v = fround(c, v);
    if (!(v >= -32768.0 && v <= 32767.0)) return INT16_MIN;
    return (int16_t)v;
}
static inline int64_t fist64(CPU *c, double v) {
    v = fround(c, v);
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)v;
}
double f80_load(uint32_t addr);
void f80_store(uint32_t addr, double v);
void fxam(CPU *c);
void fprem(CPU *c, int ieee);
void fpu_env(CPU *c, uint32_t addr, int op); /* 0 fnstenv 1 fldenv 2 fnsave 3 frstor */

/* ---- MMX (64-bit lanes) ---- */
/* float -> int32 as SSE converts: out of range and NaN give 0x80000000 */
static inline int32_t cvt_f2i(float v) {
    return (v != v || v >= 2147483648.0f || v < -2147483648.0f) ? INT32_MIN : (int32_t)v;
}
/* fisttp: truncate, out of range and NaN give the integer indefinite value */
static inline int32_t fistt32(double v) { v = trunc(v); return v >= -2147483648.0 && v <= 2147483647.0 ? (int32_t)v : INT32_MIN; }
static inline int16_t fistt16(double v) { v = trunc(v); return v >= -32768.0 && v <= 32767.0 ? (int16_t)v : INT16_MIN; }
static inline int64_t fistt64(double v) { v = trunc(v); return v >= -9223372036854775808.0 && v < 9223372036854775808.0 ? (int64_t)v : INT64_MIN; }
static inline int32_t cvt_d2i(double v) {
    return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v;
}
#define LANES(T, n) union { uint64_t q; T v[n]; }
static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint8_t satu8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }
#define MMX_W(name, expr) static inline uint64_t name(uint64_t a_, uint64_t b_) { \
    LANES(int16_t, 4) a, b, r; a.q = a_; b.q = b_; \
    for (int i = 0; i < 4; i++) { int32_t x = a.v[i], y = b.v[i]; r.v[i] = (int16_t)(expr); } return r.q; }
MMX_W(mmx_paddw, x + y)
MMX_W(mmx_psubw, x - y)
MMX_W(mmx_paddsw, sat16(x + y))
MMX_W(mmx_psubsw, sat16(x - y))
MMX_W(mmx_pmulhw, (x * y) >> 16)
MMX_W(mmx_pmullw, x * y)
MMX_W(mmx_psubusw, ((uint16_t)x > (uint16_t)y) ? (uint16_t)x - (uint16_t)y : 0)
MMX_W(mmx_paddusw, ((uint32_t)(uint16_t)x + (uint16_t)y) > 0xFFFF ? 0xFFFF : (uint16_t)x + (uint16_t)y)
static inline uint64_t mmx_packuswb(uint64_t a_, uint64_t b_) {
    LANES(int16_t, 4) a, b; LANES(uint8_t, 8) r; a.q = a_; b.q = b_;
    for (int i = 0; i < 4; i++) { r.v[i] = satu8(a.v[i]); r.v[i + 4] = satu8(b.v[i]); }
    return r.q;
}
static inline uint64_t mmx_packsswb(uint64_t a_, uint64_t b_) {
    LANES(int16_t, 4) a, b; LANES(int8_t, 8) r; a.q = a_; b.q = b_;
    for (int i = 0; i < 4; i++) {
        r.v[i] = (int8_t)(a.v[i] > 127 ? 127 : a.v[i] < -128 ? -128 : a.v[i]);
        r.v[i + 4] = (int8_t)(b.v[i] > 127 ? 127 : b.v[i] < -128 ? -128 : b.v[i]);
    }
    return r.q;
}
static inline uint64_t mmx_packssdw(uint64_t a_, uint64_t b_) {
    LANES(int32_t, 2) a, b; LANES(int16_t, 4) r; a.q = a_; b.q = b_;
    for (int i = 0; i < 2; i++) {
        r.v[i] = (int16_t)(a.v[i] > 32767 ? 32767 : a.v[i] < -32768 ? -32768 : a.v[i]);
        r.v[i + 2] = (int16_t)(b.v[i] > 32767 ? 32767 : b.v[i] < -32768 ? -32768 : b.v[i]);
    }
    return r.q;
}
#define MMX_UNPACK(name, T, n, half) static inline uint64_t name(uint64_t a_, uint64_t b_) { \
    LANES(T, n) a, b, r; a.q = a_; b.q = b_; \
    for (int i = 0; i < n / 2; i++) { r.v[2 * i] = a.v[i + half]; r.v[2 * i + 1] = b.v[i + half]; } return r.q; }
MMX_UNPACK(mmx_punpcklbw, uint8_t, 8, 0)
MMX_UNPACK(mmx_punpckhbw, uint8_t, 8, 4)
MMX_UNPACK(mmx_punpcklwd, uint16_t, 4, 0)
MMX_UNPACK(mmx_punpckhwd, uint16_t, 4, 2)
MMX_UNPACK(mmx_punpckldq, uint32_t, 2, 0)
MMX_UNPACK(mmx_punpckhdq, uint32_t, 2, 1)
#define MMX_SHIFT(name, T, n, bits, op) static inline uint64_t name(uint64_t a_, uint64_t cnt) { \
    LANES(T, n) a; a.q = a_; if (cnt >= bits) { if (#op[0] == '>' && (T)-1 < 0) cnt = bits - 1; else return 0; } \
    for (int i = 0; i < n; i++) a.v[i] = (T)(a.v[i] op cnt); return a.q; }
MMX_SHIFT(mmx_psllw, uint16_t, 4, 16, <<)
MMX_SHIFT(mmx_pslld, uint32_t, 2, 32, <<)
MMX_SHIFT(mmx_psllq, uint64_t, 1, 64, <<)
MMX_SHIFT(mmx_psrlw, uint16_t, 4, 16, >>)
MMX_SHIFT(mmx_psrld, uint32_t, 2, 32, >>)
MMX_SHIFT(mmx_psrlq, uint64_t, 1, 64, >>)
MMX_SHIFT(mmx_psraw, int16_t, 4, 16, >>)
MMX_SHIFT(mmx_psrad, int32_t, 2, 32, >>)
#define MMX_B(name, T, n, expr) static inline uint64_t name(uint64_t a_, uint64_t b_) { \
    LANES(T, n) a, b, r; a.q = a_; b.q = b_; \
    for (int i = 0; i < n; i++) { T x = a.v[i], y = b.v[i]; r.v[i] = (T)(expr); } return r.q; }
MMX_B(mmx_paddb, uint8_t, 8, x + y)
MMX_B(mmx_psubb, uint8_t, 8, x - y)
MMX_B(mmx_paddd, uint32_t, 2, x + y)
MMX_B(mmx_psubd, uint32_t, 2, x - y)
MMX_B(mmx_paddusb, uint8_t, 8, (x + y) > 255 ? 255 : x + y)
MMX_B(mmx_psubusb, uint8_t, 8, x > y ? x - y : 0)
MMX_B(mmx_pcmpeqb, uint8_t, 8, x == y ? 0xFF : 0)
MMX_B(mmx_pcmpeqw, uint16_t, 4, x == y ? 0xFFFF : 0)
MMX_B(mmx_pcmpgtw, int16_t, 4, x > y ? -1 : 0)
MMX_B(mmx_pavgb, uint8_t, 8, (x + y + 1) >> 1)
static inline uint64_t mmx_pmaddwd(uint64_t a_, uint64_t b_) {
    LANES(int16_t, 4) a, b; LANES(int32_t, 2) r; a.q = a_; b.q = b_;
    r.v[0] = a.v[0] * b.v[0] + a.v[1] * b.v[1];
    r.v[1] = a.v[2] * b.v[2] + a.v[3] * b.v[3];
    return r.q;
}

/* ---- runtime services ---- */
typedef void (*GuestFn)(CPU *c);
void rt_call(CPU *c, uint32_t target);        /* dispatch an indirect call/jump */
void rt_unimpl(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));
void rt_int3(CPU *c, uint32_t addr) __attribute__((noreturn));
void rt_cpuid(CPU *c, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
uint64_t rt_rdtsc(void);

#ifdef __cplusplus
}
#endif
