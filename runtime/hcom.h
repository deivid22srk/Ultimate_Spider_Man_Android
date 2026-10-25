/* Host-implemented COM objects exposed to the guest (DirectInput,
 * DirectSound). Guest object layout: { vtable, refcount, host slot, 0 }. */
#pragma once
#include "rt.h"

typedef struct {
    const char *name;
    uint32_t vtbl;
} HComClass;

static inline void hcom_class_init(HComClass *k, const char *name, const ShimFn *fns, int n) {
    k->name = name;
    k->vtbl = gmalloc(4 * n);
    for (int i = 0; i < n; i++) {
        char *buf = malloc(64);
        snprintf(buf, 64, "%s#%d", name, i);
        M32(k->vtbl + 4 * i) = rt_thunk(fns[i], buf);
    }
}

#define HCOM_MAX 4096
extern void *g_hcom_slots[HCOM_MAX];

static inline uint32_t hcom_new(HComClass *k, void *host) {
    uint32_t g = gmalloc(16);
    int slot = -1;
    for (int i = 1; i < HCOM_MAX; i++)
        if (!g_hcom_slots[i]) { slot = i; break; }
    if (slot < 0) rt_fatal("out of host COM slots");
    g_hcom_slots[slot] = host;
    M32(g) = k->vtbl;
    M32(g + 4) = 1;
    M32(g + 8) = (uint32_t)slot;
    M32(g + 12) = 0;
    return g;
}

static inline void *hcom_host(uint32_t g) { return g ? g_hcom_slots[M32(g + 8)] : NULL; }

/* AddRef/Release helpers; release returns 1 when the object died. */
static inline uint32_t hcom_addref(uint32_t g) { return ++M32(g + 4); }
static inline int hcom_release(uint32_t g, uint32_t *left) {
    uint32_t r = --M32(g + 4);
    if (left) *left = r;
    if (r == 0) {
        g_hcom_slots[M32(g + 8)] = NULL;
        M32(g) = 0xDEAD0001u;
        gfree(g);
        return 1;
    }
    return 0;
}

static inline int guid_eq(uint32_t g, const uint8_t *ref) { return memcmp(MEM + g, ref, 16) == 0; }

#define E_NOINTERFACE_ 0x80004002u
#define E_NOTIMPL_ 0x80004001u
#define E_FAIL_ 0x80004005u
