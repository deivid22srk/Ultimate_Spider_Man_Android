/* Import resolution: maps dll!name to host shims (or to a stub that aborts
 * with the import's name when it is first called). */
#include "rt.h"
#include <string.h>
#include <strings.h>

#define MAX_DEFS 2048
static const ShimDef *g_defs[MAX_DEFS];
static int g_ndefs;
static struct { char key[128]; uint32_t thunk; } g_imports[MAX_DEFS];
static int g_nimports;

void rt_register_shims(const ShimDef *defs) {
    for (; defs->name; defs++) {
        if (g_ndefs >= MAX_DEFS) rt_fatal("too many shims");
        g_defs[g_ndefs++] = defs;
    }
}

static const char *dll_base(const char *dll, char *buf, size_t n) {
    /* "C:\\x\\KERNEL32.DLL" -> "kernel32" */
    const char *p = strrchr(dll, '\\');
    p = p ? p + 1 : dll;
    snprintf(buf, n, "%s", p);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = 0;
    return buf;
}

static ShimFn find_shim(const char *want, const char *name) {
    char have[64];
    for (int i = 0; i < g_ndefs; i++) {
        if (strcmp(g_defs[i]->name, name)) continue;
        if (strcasecmp(dll_base(g_defs[i]->dll, have, sizeof have), want)) continue;
        return g_defs[i]->fn;
    }
    return NULL;
}

/* Data imports (variables) and exports of recompiled DLLs resolve to plain
 * guest addresses rather than thunks. */
#define MAX_ADDRS 4096
static struct { char key[128]; uint32_t addr; } g_addrs[MAX_ADDRS];
static int g_naddrs;

void rt_register_data(const char *dll, const char *name, uint32_t addr) {
    char want[64];
    dll_base(dll, want, sizeof want);
    if (g_naddrs >= MAX_ADDRS) rt_fatal("too many data imports");
    snprintf(g_addrs[g_naddrs].key, sizeof g_addrs[0].key, "%s!%s", want, name);
    g_addrs[g_naddrs++].addr = addr;
}

/* GetProcAddress semantics: NULL when we have no implementation. */
uint32_t rt_import_opt(const char *dll, const char *name) {
    char want[64];
    dll_base(dll, want, sizeof want);
    char key[128];
    snprintf(key, sizeof key, "%s!%s", want, name);
    for (int i = 0; i < g_naddrs; i++)
        if (!strcasecmp(g_addrs[i].key, key)) return g_addrs[i].addr;
    if (!find_shim(want, name)) return 0;
    return rt_import(dll, name);
}

uint32_t rt_import(const char *dll, const char *name) {
    char want[64], have[64], key[128];
    dll_base(dll, want, sizeof want);
    snprintf(key, sizeof key, "%s!%s", want, name);
    for (int i = 0; i < g_naddrs; i++)
        if (!strcasecmp(g_addrs[i].key, key)) return g_addrs[i].addr;
    for (int i = 0; i < g_nimports; i++)
        if (!strcasecmp(g_imports[i].key, key)) return g_imports[i].thunk;
    ShimFn fn = NULL;
    for (int i = 0; i < g_ndefs; i++) {
        if (strcmp(g_defs[i]->name, name)) continue;
        if (strcasecmp(dll_base(g_defs[i]->dll, have, sizeof have), want)) continue;
        fn = g_defs[i]->fn;
        break;
    }
    if (!fn) LOG("import %s has no shim yet", key);
    if (g_nimports >= MAX_DEFS) rt_fatal("too many imports");
    snprintf(g_imports[g_nimports].key, sizeof g_imports[0].key, "%s", key);
    char *stored = strdup(key);
    g_imports[g_nimports].thunk = rt_thunk(fn, stored);
    return g_imports[g_nimports++].thunk;
}

void k32_init(void);
void user32_init(void);
void misc_init(void);
void input_init(void);
void d3d9_init(void);
void dinput_init(void);
void dsound_init(void);
void msvcrt_init(void);
void marvel_shims_init(void);
void skyrim_shims_init(void);
void xaudio2_init(void);

void rt_init_shims(void) {
    k32_init();
    user32_init();
    misc_init();
    input_init();
    d3d9_init();
    dinput_init();
    dsound_init();
    msvcrt_init();
    marvel_shims_init();
    skyrim_shims_init();
    xaudio2_init();
}
