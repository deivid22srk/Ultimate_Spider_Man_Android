/* Guest heap (HeapAlloc/malloc) and VirtualAlloc region allocator.
 *
 * Heap: boundary-tag allocator with segregated free lists, living in
 * [GUEST_HEAP_BASE, GUEST_HEAP_END). Block layout (all guest memory):
 *   +0 prev block size | free bit (bit0 of +4)
 *   +4 this block size | flags
 *   +8 payload (16-byte aligned); free blocks store next/prev links here
 */
#include "rt.h"
#include <string.h>
#include <sys/mman.h>

#define HDR 8u
#define MINBLK 32u
#define NBINS 48
#define F_USED 1u

static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_top = GUEST_HEAP_BASE + 8; /* first block header; payload 16-aligned */
static uint32_t g_last_block = 0;
static uint32_t g_bins[NBINS];

#define BSIZE(b) (M32((b) + 4) & ~15u)
#define BUSED(b) (M32((b) + 4) & F_USED)
#define BPREV(b) (M32(b))
#define NEXTF(b) M32((b) + 8)
#define PREVF(b) M32((b) + 12)

static int bin_of(uint32_t sz) {
    /* 16-byte classes up to 512, then logarithmic */
    if (sz <= 512) return (int)(sz >> 4);           /* 0..32 */
    int l = 31 - __builtin_clz(sz);                 /* 9.. */
    int b = 33 + (l - 9);
    return b < NBINS ? b : NBINS - 1;
}

static void unlink_free(uint32_t b) {
    uint32_t n = NEXTF(b), p = PREVF(b);
    if (p) NEXTF(p) = n;
    else g_bins[bin_of(BSIZE(b))] = n;
    if (n) PREVF(n) = p;
}

static void link_free(uint32_t b) {
    int k = bin_of(BSIZE(b));
    NEXTF(b) = g_bins[k];
    PREVF(b) = 0;
    if (g_bins[k]) PREVF(g_bins[k]) = b;
    g_bins[k] = b;
}

static void set_block(uint32_t b, uint32_t size, int used) {
    M32(b + 4) = size | (used ? F_USED : 0);
    uint32_t nx = b + size;
    if (nx < g_top) M32(nx) = size;
}

uint32_t gmalloc(uint32_t n) {
    uint32_t need = (n + HDR + 15) & ~15u;
    if (need < MINBLK) need = MINBLK;
    if (n > 0x7FFFFFFF) return 0;
    pthread_mutex_lock(&g_heap_lock);
    for (int k = bin_of(need); k < NBINS; k++) {
        for (uint32_t b = g_bins[k]; b; b = NEXTF(b)) {
            uint32_t sz = BSIZE(b);
            if (sz < need) continue;
            unlink_free(b);
            if (sz - need >= MINBLK) {
                set_block(b, need, 1);
                uint32_t r = b + need;
                M32(r) = need;
                set_block(r, sz - need, 0);
                if (g_last_block == b) g_last_block = r;
                link_free(r);
            } else {
                set_block(b, sz, 1);
            }
            pthread_mutex_unlock(&g_heap_lock);
            return b + HDR;
        }
    }
    if (g_top + need > GUEST_HEAP_END) {
        pthread_mutex_unlock(&g_heap_lock);
        LOG("guest heap exhausted (%u bytes requested)", n);
        return 0;
    }
    uint32_t b = g_top;
    M32(b) = g_last_block ? BSIZE(g_last_block) : 0;
    g_top += need;
    set_block(b, need, 1);
    g_last_block = b;
    pthread_mutex_unlock(&g_heap_lock);
    return b + HDR;
}

void gfree(uint32_t p) {
    if (!p) return;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t b = p - HDR;
    if (!BUSED(b)) {
        pthread_mutex_unlock(&g_heap_lock);
        LOG("double free of %08x", p);
        return;
    }
    uint32_t sz = BSIZE(b);
    /* merge with next */
    uint32_t nx = b + sz;
    if (nx < g_top && !BUSED(nx)) {
        unlink_free(nx);
        if (g_last_block == nx) g_last_block = b;
        sz += BSIZE(nx);
    }
    /* merge with previous */
    if (b > GUEST_HEAP_BASE + 8 && BPREV(b)) {
        uint32_t pv = b - BPREV(b);
        if (!BUSED(pv)) {
            unlink_free(pv);
            if (g_last_block == b) g_last_block = pv;
            sz += BSIZE(pv);
            b = pv;
        }
    }
    if (b + sz == g_top) {
        /* give the tail back to the bump region */
        g_top = b;
        g_last_block = 0;
        if (b > GUEST_HEAP_BASE + 8 && BPREV(b)) g_last_block = b - BPREV(b);
        pthread_mutex_unlock(&g_heap_lock);
        return;
    }
    set_block(b, sz, 0);
    link_free(b);
    pthread_mutex_unlock(&g_heap_lock);
}

uint32_t gsize(uint32_t p) {
    return p ? BSIZE(p - HDR) - HDR : 0;
}

uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p) return gmalloc(n);
    uint32_t old = gsize(p);
    if (n <= old) return p;
    uint32_t q = gmalloc(n);
    if (!q) return 0;
    memcpy(MEM + q, MEM + p, old);
    gfree(p);
    return q;
}

uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = gmalloc(n);
    memcpy(MEM + p, s, n);
    return p;
}

/* ---- VirtualAlloc: 64 KiB granularity, first-fit over a region list ---- */
#define MAX_REGIONS 4096
static struct { uint32_t base, size; } g_regions[MAX_REGIONS];
static int g_nregions;
static pthread_mutex_t g_va_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t gvalloc(uint32_t addr, uint32_t size) {
    size = (size + 0xFFFF) & ~0xFFFFu;
    pthread_mutex_lock(&g_va_lock);
    if (addr) {
        /* commit inside an existing reservation: already backed */
        for (int i = 0; i < g_nregions; i++)
            if (addr >= g_regions[i].base && addr < g_regions[i].base + g_regions[i].size) {
                pthread_mutex_unlock(&g_va_lock);
                return addr;
            }
    }
    uint32_t cand = GUEST_VA_BASE;
    for (;;) {
        int clash = 0;
        for (int i = 0; i < g_nregions; i++) {
            uint32_t b = g_regions[i].base, e = b + g_regions[i].size;
            if (cand < e && cand + size > b) { cand = e; clash = 1; }
        }
        if (!clash) break;
    }
    if ((uint64_t)cand + size > GUEST_VA_END || g_nregions == MAX_REGIONS) {
        pthread_mutex_unlock(&g_va_lock);
        return 0;
    }
    g_regions[g_nregions].base = cand;
    g_regions[g_nregions].size = size;
    g_nregions++;
    pthread_mutex_unlock(&g_va_lock);
    return cand; /* pages are zero: never touched, or dropped by gvfree */
}

void gvfree(uint32_t addr) {
    pthread_mutex_lock(&g_va_lock);
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].base == addr) {
            madvise(MEM + addr, g_regions[i].size, MADV_DONTNEED);
            g_regions[i] = g_regions[--g_nregions];
            break;
        }
    pthread_mutex_unlock(&g_va_lock);
}
