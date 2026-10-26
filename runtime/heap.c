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


/* ---- USM entity-name hash_map integrity watch ---------------------------
 * The game keeps a hash_map at 0x95B79C (node = {next, prev, hash, entity}).
 * A corrupted link surfaces much later as a crash inside the map code, so
 * check the list after every heap operation and remember the first time it
 * is seen broken: that names the operation that corrupted it. */
/* The entity-map watch walks the game's map on every heap operation. It was
 * on by default, which cost a full walk per malloc/free during gameplay and,
 * worse, made the game's startup timing depend on a read-only observer: on
 * Android 14 the extra work desynchronises the load path and the map ends up
 * with a bogus link (node.next = 9 -> a write at guest address 0x0D). It is a
 * diagnostic, so it now runs only when debug logging asks for it. */
static int g_map_watch = 0;

void usm_map_watch_set(int on) { g_map_watch = on ? 1 : 0; }
static int g_map_hard;              /* first hard failure seen */
static int g_map_soft;              /* walk length != map size */
static uint32_t g_map_op_ret, g_map_op_ptr, g_map_op_node, g_map_op_i;
static char g_map_op_name[8], g_map_why[96];

#define HOP 12
static struct { const char *op; uint32_t size, ret; } g_hop[HOP];
static int g_hop_n;

static void hop_note(const char *op, uint32_t size, uint32_t ret) {
    int i = g_hop_n++ % HOP;
    g_hop[i].op = op; g_hop[i].size = size; g_hop[i].ret = ret;
}

static void hop_dump(const char *why) {
    fprintf(stderr, "usm: last heap ops before %s:\n", why);
    for (int k = 0; k < HOP; k++) {
        int i = (g_hop_n - HOP + k + 2 * HOP) % HOP;
        if (!g_hop[i].op) continue;
        fprintf(stderr, "usm:   #%d %s(%x) ret=%08x\n", g_hop_n - HOP + k + 1, g_hop[i].op,
                g_hop[i].size, g_hop[i].ret);
    }
}

static void heap_walk_check(void) {
    uint32_t b = GUEST_HEAP_BASE + 8;
    int n = 0, bad = 0;
    while (b < g_top) {
        uint32_t raw = M32(b + 4), sz = raw & ~15u;
        if (sz < MINBLK || sz > 0x10000000u || b + sz > g_top) {
            if (bad++ < 8)
                fprintf(stderr, "usm: heap block %08x: bad header %08x (prev %08x)\n",
                        b, raw, M32(b));
            break;
        }
        if (b + sz < g_top && M32(b + sz) != sz && bad++ < 8)
            fprintf(stderr, "usm: heap block %08x size %x: next prev-size %08x\n",
                    b, sz, M32(b + sz));
        b += sz; n++;
    }
    fprintf(stderr, "usm: heap walk: %d blocks, reached %08x (top %08x)%s\n",
            n, b, g_top, bad ? " -- BROKEN" : "");
}

static int on_guest_stack(CPU *c, uint32_t want) {
    if (!c) return 0;
    uint32_t sp = c->r[4];
    for (int i = 0; i < 20; i++)
        if (M32(sp + 4 * i) == want) return 1;
    return 0;
}

void usm_map_watch_note(const char *op, uint32_t req, uint32_t ptr) {
    if (!g_map_watch || op[0] != 'm') return;   /* malloc only: that is where the
                                                * insert walks the chain */
    uint32_t map = 0x95B79Cu;
    uint32_t sent = M32(map + 0x08), size = M32(map + 0x24), mask = M32(map + 0x20);
    if (sent < GUEST_HEAP_BASE || sent >= GUEST_HEAP_END) return;
    if (size > 4096 || mask > 4096) return;
    CPU *cc = rt_current_cpu();
    hop_note(op, req, cc ? M32(cc->r[4]) : 0);
    uint32_t n = M32(sent);
    int i = 0;
    while (n != sent) {
        if (n < GUEST_HEAP_BASE || n >= GUEST_HEAP_END) {
            if (!g_map_hard) {
                g_map_hard = 1;
                CPU *c = rt_current_cpu();
                g_map_op_ret = c ? M32(c->r[4]) : 0;
                g_map_op_ptr = ptr;
                g_map_op_node = n;
                g_map_op_i = (uint32_t)i;
                snprintf(g_map_op_name, sizeof g_map_op_name, "%s", op);
                snprintf(g_map_why, sizeof g_map_why, "node[%d]=%08x", i, n);
                fprintf(stderr, "usm: MAP BROKEN after %s(%08x) ret=%08x: %s (size %u mask %x)\n",
                        op, ptr, g_map_op_ret, g_map_why, size, mask);
                hop_dump("the break");
                heap_walk_check();
            }
            g_map_watch = 0;
            return;
        }
        if (++i > 512) {
            /* the entity map outgrew the watch (1000+ entries once the game
             * is up): stop walking every heap op instead of misreporting */
            g_map_watch = 0;
            return;
        }
        n = M32(n);
    }
    if (req == 16 && op[0] == 'm' && on_guest_stack(rt_current_cpu(), 0x5095F6u) && g_log_level > 1) {
        static int nins;
        fprintf(stderr, "usm: map insert #%d node=%08x size=%u mask=%x walk=%d\n",
                ++nins, ptr, size, mask, i);
    }
    if ((uint32_t)i != size) {
        g_map_soft++;
        if (g_map_soft < 4 && g_log_level > 1)
            fprintf(stderr, "usm: map soft mismatch: walk %d != size %u after %s(%08x)\n",
                    i, size, op, ptr);
    }
}

void usm_map_watch_dump(void) {
    if (g_map_hard)
        fprintf(stderr, "usm: first break: %s(%08x) ret=%08x %s (i=%u node=%08x)\n",
                g_map_op_name, g_map_op_ptr, g_map_op_ret, g_map_why, g_map_op_i, g_map_op_node);
    else
        fprintf(stderr, "usm: map watch: no hard break seen (%d soft mismatches)\n", g_map_soft);
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
            usm_map_watch_note("malloc", n, b + HDR);
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
    usm_map_watch_note("malloc", n, b + HDR);
    return b + HDR;
}

void gfree(uint32_t p) {
    if (!p) return;
    if (p < GUEST_HEAP_BASE || p >= GUEST_HEAP_END) {
        static int warned;
        if (warned++ < 10) LOG("gfree(%08x): not a guest heap pointer", p);
        return;
    }
    pthread_mutex_lock(&g_heap_lock);
    uint32_t b = p - HDR;
    if (!BUSED(b)) {
        pthread_mutex_unlock(&g_heap_lock);
        LOG("double free of %08x", p);
        return;
    }
    /* A pointer that is not a block start (interior pointer, or one handed out
     * by a private heap) has a nonsense header. Freeing it used to splice a bogus
     * block into a bin, and the next malloc then handed out memory that was still
     * in use: that is how the game's entity map ends up with a link like 9. Drop
     * such a free instead of corrupting the heap. */
    uint32_t sz = BSIZE(b);
    if ((b & 15u) || sz < MINBLK || sz > (GUEST_HEAP_END - GUEST_HEAP_BASE) ||
        (uint32_t)(b + sz) > g_top || (uint32_t)(b + sz) < b) {
        static int warned;
        if (warned++ < 10)
            LOG("gfree(%08x): block %08x header is %08x/%08x, dropped", p, b,
                M32(b), M32(b + 4));
        pthread_mutex_unlock(&g_heap_lock);
        return;
    }
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
        usm_map_watch_note("free", 0, p);
        return;
    }
    set_block(b, sz, 0);
    link_free(b);
    pthread_mutex_unlock(&g_heap_lock);
    usm_map_watch_note("free", 0, p);
}

uint32_t gsize(uint32_t p) {
    if (!p) return 0;
    if (p < GUEST_HEAP_BASE || p >= GUEST_HEAP_END) {
        static int warned;
        if (warned++ < 10) LOG("gsize(%08x): not a guest heap pointer", p);
        return 0;
    }
    return BSIZE(p - HDR) - HDR;
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
