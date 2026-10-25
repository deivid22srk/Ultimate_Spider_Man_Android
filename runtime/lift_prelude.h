/* Included by generated files only. */
#pragma once
#include <setjmp.h>
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t guest_jb);
void rt_setjmp3(CPU *c);
#define SPILL() do { c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx; \
    c->r[4] = esp; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi; } while (0)
#define RELOAD() do { eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; \
    esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7]; } while (0)
static inline void rt_movsd(uint32_t n, uint32_t dst, uint32_t src) {
    /* forward rep movsd; overlapping copies must behave like the dword loop */
    if (dst <= src || dst >= src + n * 4) memmove(MEM + dst, MEM + src, (size_t)n * 4);
    else for (uint32_t k = 0; k < n; k++) M32(dst + 4 * k) = M32(src + 4 * k);
}
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

/* Memory ordering for multi-core ARM (see mem_order in tools/lift.py). */
#if defined(__aarch64__)
#define ST_FENCE() __asm__ volatile("dmb ishst" ::: "memory")
#define FULL_FENCE() __asm__ volatile("dmb ish" ::: "memory")
#define LD_FENCE() __asm__ volatile("dmb ishld" ::: "memory")
#else
#define ST_FENCE() ((void)0)
#define FULL_FENCE() ((void)0)
#define LD_FENCE() ((void)0)
#endif

/* x86 (TSO) ordering for guest memory other threads can see (see TSO in
 * tools/lift.py): load-acquire / store-release on aligned addresses; an
 * unaligned access (no ldar/stlr form) is a plain access with a fence. */
#if defined(__aarch64__)
static inline uint8_t TL8(uint32_t a) { return __atomic_load_n(MEM + a, __ATOMIC_ACQUIRE); }
static inline void TS8(uint32_t a, uint8_t v) { __atomic_store_n(MEM + a, v, __ATOMIC_RELEASE); }
#define TSO_PAIR(n, T, mask) \
static inline T TL##n(uint32_t a) { \
    if (__builtin_expect(!(a & mask), 1)) return __atomic_load_n((T *)(MEM + a), __ATOMIC_ACQUIRE); \
    T v; memcpy(&v, MEM + a, sizeof v); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; } \
static inline void TS##n(uint32_t a, T v) { \
    if (__builtin_expect(!(a & mask), 1)) { __atomic_store_n((T *)(MEM + a), v, __ATOMIC_RELEASE); return; } \
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(MEM + a, &v, sizeof v); }
TSO_PAIR(16, uint16_t, 1)
TSO_PAIR(32, uint32_t, 3)
#else
#define TL8(a) M8(a)
#define TL16(a) M16(a)
#define TL32(a) M32(a)
#define TS8(a, v) (M8(a) = (v))
#define TS16(a, v) (M16(a) = (v))
#define TS32(a, v) (M32(a) = (v))
#endif
