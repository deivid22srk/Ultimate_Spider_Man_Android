/* Shims for imports LEGO Marvel uses beyond LEGO Batman's: Steam (offline),
 * Winsock and IP Helper (no network), AVI capture, dbghelp symbols. */
#include "rt.h"
#include <string.h>

/* ---- steam_api: no Steam client. SteamAPI_Init must succeed (the game
 * exits otherwise); the interfaces are fake objects whose methods are added
 * here as the game turns out to call them (an unknown one stops with its slot). */
#define STEAM_SLOTS 64
typedef struct { const char *name; uint32_t obj; const ShimFn *impl; int nimpl; } SteamIface;
static SteamIface g_steam[5] = {
    {"ISteamUser", 0, NULL, 0}, {"ISteamApps", 0, NULL, 0}, {"ISteamUtils", 0, NULL, 0},
    {"ISteamUserStats", 0, NULL, 0}, {"ISteamRemoteStorage", 0, NULL, 0},
};
static void steam_tables(void);
static uint32_t steam_obj(int k) {
    SteamIface *s = &g_steam[k];
    if (s->obj) return s->obj;
    steam_tables();
    uint32_t vt = gmalloc(4 * STEAM_SLOTS);
    for (int i = 0; i < STEAM_SLOTS; i++) {
        char *n = malloc(48);
        snprintf(n, 48, "%s#%d", s->name, i);
        ShimFn fn = i < s->nimpl && s->impl[i] ? s->impl[i] : NULL;  /* NULL: stops with the name */
        M32(vt + 4 * i) = rt_thunk(fn, n);
    }
    s->obj = gmalloc(16);
    M32(s->obj) = vt;
    return s->obj;
}
/* ISteamRemoteStorage006 (thiscall, this in ecx): Steam Cloud files are
 * kept in <save>/steamcloud. */
#include <sys/stat.h>
#include <dirent.h>
static void cloud_path(const char *name, char *out, size_t n) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/steamcloud", g_save_dir);
    mkdir(dir, 0755);
    snprintf(out, n, "%s/", dir);
    size_t l = strlen(out);
    for (const char *p = name; *p && l + 1 < n; p++) out[l++] = (*p == '/' || *p == '\\') ? '_' : *p;
    out[l] = 0;
}
static void rs_FileWrite(CPU *c) {
    char p[800];
    cloud_path(GSTR(ARG(0)), p, sizeof p);
    FILE *f = fopen(p, "wb");
    int ok = f && fwrite(MEM + ARG(1), 1, ARG(2), f) == ARG(2);
    if (f) fclose(f);
    LOG("steam cloud: write %s (%u bytes) %s", GSTR(ARG(0)), ARG(2), ok ? "ok" : "failed");
    RET(ok, 3);
}
static void rs_FileRead(CPU *c) {
    char p[800];
    cloud_path(GSTR(ARG(0)), p, sizeof p);
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(MEM + ARG(1), 1, ARG(2), f) : 0;
    if (f) fclose(f);
    RET((uint32_t)n, 3);
}
static void rs_FileForget(CPU *c) { RET(1, 1); }
static void rs_FileDelete(CPU *c) {
    char p[800];
    cloud_path(GSTR(ARG(0)), p, sizeof p);
    RET(remove(p) == 0, 1);
}
static void rs_FileShare(CPU *c) { c->r[2] = 0; RET(0, 1); }
static void rs_SetSyncPlatforms(CPU *c) { RET(1, 2); }
static void rs_FileExists(CPU *c) {
    char p[800];
    struct stat st;
    cloud_path(GSTR(ARG(0)), p, sizeof p);
    RET(stat(p, &st) == 0, 1);
}
static void rs_FilePersisted(CPU *c) { rs_FileExists(c); }
static void rs_GetFileSize(CPU *c) {
    char p[800];
    struct stat st;
    cloud_path(GSTR(ARG(0)), p, sizeof p);
    RET(stat(p, &st) == 0 ? (uint32_t)st.st_size : 0, 1);
}
static void rs_GetFileTimestamp(CPU *c) {
    char p[800];
    struct stat st;
    cloud_path(GSTR(ARG(0)), p, sizeof p);
    int64_t t = stat(p, &st) == 0 ? (int64_t)st.st_mtime : 0;
    c->r[2] = (uint32_t)((uint64_t)t >> 32);
    RET((uint32_t)t, 1);
}
static void rs_GetSyncPlatforms(CPU *c) { RET(0xFFFFFFFFu, 1); }
static char g_cloud_names[64][260];
static int g_cloud_sizes[64];
static void rs_GetFileCount(CPU *c) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/steamcloud", g_save_dir);
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && n < 64 && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[900];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        snprintf(g_cloud_names[n], sizeof g_cloud_names[n], "%s", e->d_name);
        g_cloud_sizes[n++] = (int)st.st_size;
    }
    if (d) closedir(d);
    RET(n, 0);
}
static void rs_GetFileNameAndSize(CPU *c) {
    static uint32_t g_name_buf;
    uint32_t i = ARG(0);
    if (!g_name_buf) g_name_buf = gmalloc(260);
    if (i >= 64) RET(0, 2);
    strcpy((char *)MEM + g_name_buf, g_cloud_names[i]);
    if (ARG(1)) M32(ARG(1)) = (uint32_t)g_cloud_sizes[i];
    RET(g_name_buf, 2);
}
static void rs_GetQuota(CPU *c) { M32(ARG(0)) = 100u << 20; M32(ARG(1)) = 100u << 20; RET(1, 2); }
static void rs_IsCloudEnabledForAccount(CPU *c) { RET(1, 0); }
static void rs_IsCloudEnabledForApp(CPU *c) { RET(1, 0); }
static void rs_SetCloudEnabledForApp(CPU *c) { RET(0, 1); }
static const ShimFn k_rs[] = {
    rs_FileWrite, rs_FileRead, rs_FileForget, rs_FileDelete, rs_FileShare, rs_SetSyncPlatforms,
    rs_FileExists, rs_FilePersisted, rs_GetFileSize, rs_GetFileTimestamp, rs_GetSyncPlatforms,
    rs_GetFileCount, rs_GetFileNameAndSize, rs_GetQuota, rs_IsCloudEnabledForAccount,
    rs_IsCloudEnabledForApp, rs_SetCloudEnabledForApp,
};
/* ISteamApps005 */
static void ap_true0(CPU *c) { RET(1, 0); }
static void ap_false0(CPU *c) { RET(0, 0); }
static void ap_language(CPU *c) {
    static uint32_t s;
    if (!s) s = gstrdup("english");
    RET(s, 0);
}
static void ap_IsSubscribedApp(CPU *c) { RET(1, 1); }  /* DLC owned */
static const ShimFn k_apps[] = {ap_true0, ap_false0, ap_false0, ap_false0, ap_language, ap_language, ap_IsSubscribedApp};
/* ISteamUtils005 */
static void ut_zero0(CPU *c) { RET(0, 0); }
static void ut_universe(CPU *c) { RET(1, 0); }
static void ut_GetAppID(CPU *c) { RET(249130, 0); }
static const ShimFn k_utils[] = {ut_zero0, ut_zero0, ut_universe, ut_zero0, NULL, NULL, NULL, NULL, NULL, ut_GetAppID};

/* ISteamUser016 */
static void us_GetHSteamUser(CPU *c) { RET(1, 0); }
static void us_BLoggedOn(CPU *c) { RET(1, 0); }
/* CSteamID GetSteamID(): returned through a hidden pointer */
static void us_GetSteamID(CPU *c) {
    M32(ARG(0)) = 1;            /* account id */
    M32(ARG(0) + 4) = 0x01100001u; /* public universe, individual */
    RET(ARG(0), 1);
}
static const ShimFn k_user[] = {us_GetHSteamUser, us_BLoggedOn, us_GetSteamID};
/* ISteamUserStats010 (MSVC puts overloads in reverse order in the vtable) */
static void st_true0(CPU *c) { RET(1, 0); }
static void st_false1(CPU *c) { RET(0, 1); }
static void st_false2(CPU *c) { RET(0, 2); }
static void st_false3(CPU *c) { RET(0, 3); }
static void st_false4(CPU *c) { RET(0, 4); }
static void st_SetAchievement(CPU *c) { LOG("steam: achievement %s", GSTR(ARG(0))); RET(1, 1); }
static void st_GetAchievement(CPU *c) { if (ARG(1)) M8(ARG(1)) = 0; RET(1, 2); }
static const ShimFn k_stats[] = {
    st_true0, st_false2, st_false2, st_false2, st_false2, st_false4, st_GetAchievement, st_SetAchievement,
    st_false1, st_false3, st_true0, st_false1, st_false2, st_false3,
};

SHIM(SteamAPI_RestartAppIfNecessary) { RETC(0); }
static void steam_tables(void) {
    g_steam[0].impl = k_user; g_steam[0].nimpl = sizeof k_user / sizeof k_user[0];
    g_steam[1].impl = k_apps; g_steam[1].nimpl = sizeof k_apps / sizeof k_apps[0];
    g_steam[2].impl = k_utils; g_steam[2].nimpl = sizeof k_utils / sizeof k_utils[0];
    g_steam[3].impl = k_stats; g_steam[3].nimpl = sizeof k_stats / sizeof k_stats[0];
    g_steam[4].impl = k_rs; g_steam[4].nimpl = sizeof k_rs / sizeof k_rs[0];
}
SHIM(SteamAPI_Init) { LOG("SteamAPI_Init: offline stub"); RETC(1); }
SHIM(SteamAPI_Shutdown) { RETC(0); }
SHIM(SteamAPI_RunCallbacks) { RETC(0); }
SHIM(SteamAPI_RegisterCallback) { RETC(0); }
SHIM(SteamAPI_UnregisterCallback) { RETC(0); }
SHIM(SteamUser) { RETC(steam_obj(0)); }
SHIM(SteamApps) { RETC(steam_obj(1)); }
SHIM(SteamUtils) { RETC(steam_obj(2)); }
SHIM(SteamUserStats) { RETC(steam_obj(3)); }
SHIM(SteamRemoteStorage) { RETC(steam_obj(4)); }

/* ---- WS2_32: every socket operation fails as if the network is down ---- */
#define WSAENETDOWN 10050
static __thread uint32_t t_wsa_error;
SHIM(WSAStartup) {
    uint32_t d = ARG(1);
    if (d) { memset(MEM + d, 0, 400); M16(d) = 0x0202; M16(d + 2) = 0x0202; }
    RET(0, 2);
}
SHIM(WSACleanup) { RET(0, 0); }
SHIM(WSAGetLastError) { RET(t_wsa_error ? t_wsa_error : WSAENETDOWN, 0); }
SHIM(socket) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 3); }
SHIM(closesocket) { RET(0, 1); }
SHIM(shutdown) { RET(0, 2); }
SHIM(setsockopt) { RET(0xFFFFFFFFu, 5); }
SHIM(ioctlsocket) { RET(0xFFFFFFFFu, 3); }
SHIM(connect) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 3); }
SHIM(send) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 4); }
SHIM(recv) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 4); }
SHIM(gethostbyname) { t_wsa_error = 11001; RET(0, 1); }  /* WSAHOST_NOT_FOUND */
SHIM(inet_addr) { RET(0xFFFFFFFFu, 1); }
SHIM(htons) { uint16_t v = (uint16_t)ARG(0); RET((uint16_t)(v << 8 | v >> 8), 1); }
/* Skyrim (WSOCK32): only reached by its debug/remote tools. */
SHIM(select) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 5); }
SHIM(WSAAsyncSelect) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 4); }
SHIM(inet_ntoa) { static uint32_t s; if (!s) s = gstrdup("0.0.0.0"); RET(s, 1); }
SHIM(accept) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 3); }
SHIM(__WSAFDIsSet) { RET(0, 2); }
SHIM(gethostname) { if (ARG(1) > 8) strcpy((char *)MEM + ARG(0), "android"); RET(0, 2); }
SHIM(listen) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 2); }
SHIM(bind) { t_wsa_error = WSAENETDOWN; RET(0xFFFFFFFFu, 3); }

/* ---- IPHLPAPI ---- */
SHIM(GetAdaptersInfo) { RET(232, 2); }  /* ERROR_NO_DATA */

/* ---- ADVAPI32 ---- */
SHIM(GetUserNameA) {
    const char *n = "Player";
    uint32_t buf = ARG(0), len = ARG(1);
    if (M32(len) < 7) { M32(len) = 7; RET(0, 2); }
    strcpy((char *)MEM + buf, n);
    M32(len) = 7;
    RET(1, 2);
}

/* ---- dbghelp: no symbols (used by the crash handler) ---- */
SHIM(SymInitialize) { RET(0, 3); }
SHIM(SymSetOptions) { RET(ARG(0), 1); }
SHIM(SymSetSearchPath) { RET(0, 2); }
SHIM(SymGetSearchPath) { if (ARG(2)) M8(ARG(1)) = 0; RET(0, 3); }
SHIM(SymFromAddr) { RET(0, 4); }

/* ---- AVIFIL32: video capture is unavailable ---- */
SHIM(AVIFileInit) { RET(0, 0); }
SHIM(AVIFileOpenA) { if (ARG(0)) M32(ARG(0)) = 0; RET(0x80004005u, 4); }
SHIM(AVIFileRelease) { RET(0, 1); }
SHIM(AVIFileCreateStreamA) { if (ARG(1)) M32(ARG(1)) = 0; RET(0x80004005u, 3); }
SHIM(AVIMakeCompressedStream) { if (ARG(0)) M32(ARG(0)) = 0; RET(0x80004005u, 4); }
SHIM(AVIStreamSetFormat) { RET(0x80004005u, 4); }
SHIM(AVIStreamWrite) { RET(0x80004005u, 8); }
SHIM(AVIStreamRelease) { RET(0, 1); }

static const ShimDef defs[] = {
#define S(d, n) {d, #n, shim_##n},
    S("steam_api.dll", SteamAPI_RestartAppIfNecessary) S("steam_api.dll", SteamAPI_Init)
    S("steam_api.dll", SteamAPI_Shutdown) S("steam_api.dll", SteamAPI_RunCallbacks)
    S("steam_api.dll", SteamAPI_RegisterCallback) S("steam_api.dll", SteamAPI_UnregisterCallback)
    S("steam_api.dll", SteamUser) S("steam_api.dll", SteamApps) S("steam_api.dll", SteamUtils)
    S("steam_api.dll", SteamUserStats) S("steam_api.dll", SteamRemoteStorage)
    S("WS2_32.dll", WSAStartup) S("WS2_32.dll", WSACleanup) S("WS2_32.dll", WSAGetLastError)
    S("WS2_32.dll", socket) S("WS2_32.dll", closesocket) S("WS2_32.dll", shutdown)
    S("WS2_32.dll", setsockopt) S("WS2_32.dll", ioctlsocket) S("WS2_32.dll", connect)
    S("WS2_32.dll", send) S("WS2_32.dll", recv) S("WS2_32.dll", gethostbyname)
    S("WS2_32.dll", inet_addr) S("WS2_32.dll", htons)
    S("WSOCK32.dll", WSAStartup) S("WSOCK32.dll", WSAGetLastError) S("WSOCK32.dll", socket)
    S("WSOCK32.dll", closesocket) S("WSOCK32.dll", setsockopt) S("WSOCK32.dll", ioctlsocket)
    S("WSOCK32.dll", connect) S("WSOCK32.dll", send) S("WSOCK32.dll", recv) S("WSOCK32.dll", gethostbyname)
    S("WSOCK32.dll", inet_addr) S("WSOCK32.dll", htons) S("WSOCK32.dll", select) S("WSOCK32.dll", WSAAsyncSelect)
    S("WSOCK32.dll", inet_ntoa) S("WSOCK32.dll", accept) S("WSOCK32.dll", __WSAFDIsSet)
    S("WSOCK32.dll", gethostname) S("WSOCK32.dll", listen) S("WSOCK32.dll", bind)
    {"WSOCK32.dll", "ntohs", shim_htons},
    {"WSOCK32.dll", "#1", shim_accept},
    {"WSOCK32.dll", "#2", shim_bind},
    {"WSOCK32.dll", "#3", shim_closesocket},
    {"WSOCK32.dll", "#4", shim_connect},
    {"WSOCK32.dll", "#9", shim_htons},
    {"WSOCK32.dll", "#10", shim_ioctlsocket},
    {"WSOCK32.dll", "#11", shim_inet_addr},
    {"WSOCK32.dll", "#12", shim_inet_ntoa},
    {"WSOCK32.dll", "#13", shim_listen},
    {"WSOCK32.dll", "#15", shim_htons},
    {"WSOCK32.dll", "#16", shim_recv},
    {"WSOCK32.dll", "#18", shim_select},
    {"WSOCK32.dll", "#19", shim_send},
    {"WSOCK32.dll", "#21", shim_setsockopt},
    {"WSOCK32.dll", "#23", shim_socket},
    {"WSOCK32.dll", "#52", shim_gethostbyname},
    {"WSOCK32.dll", "#57", shim_gethostname},
    {"WSOCK32.dll", "#101", shim_WSAAsyncSelect},
    {"WSOCK32.dll", "#111", shim_WSAGetLastError},
    {"WSOCK32.dll", "#115", shim_WSAStartup},
    {"WSOCK32.dll", "#151", shim___WSAFDIsSet},
    S("IPHLPAPI.DLL", GetAdaptersInfo) S("ADVAPI32.dll", GetUserNameA)
    S("dbghelp.dll", SymInitialize) S("dbghelp.dll", SymSetOptions) S("dbghelp.dll", SymSetSearchPath)
    S("dbghelp.dll", SymGetSearchPath) S("dbghelp.dll", SymFromAddr)
    S("AVIFIL32.dll", AVIFileInit) S("AVIFIL32.dll", AVIFileOpenA) S("AVIFIL32.dll", AVIFileRelease)
    S("AVIFIL32.dll", AVIFileCreateStreamA) S("AVIFIL32.dll", AVIMakeCompressedStream)
    S("AVIFIL32.dll", AVIStreamSetFormat) S("AVIFIL32.dll", AVIStreamWrite) S("AVIFIL32.dll", AVIStreamRelease)
#undef S
    {"WS2_32.dll", "#111", shim_WSAGetLastError},
    {"WS2_32.dll", "#10", shim_ioctlsocket},
    {"WS2_32.dll", "#21", shim_setsockopt},
    {"WS2_32.dll", "#22", shim_shutdown},
    {"WS2_32.dll", "#3", shim_closesocket},
    {"WS2_32.dll", "#116", shim_WSACleanup},
    {"WS2_32.dll", "#115", shim_WSAStartup},
    {"WS2_32.dll", "#52", shim_gethostbyname},
    {"WS2_32.dll", "#4", shim_connect},
    {"WS2_32.dll", "#11", shim_inet_addr},
    {"WS2_32.dll", "#19", shim_send},
    {"WS2_32.dll", "#16", shim_recv},
    {"WS2_32.dll", "#23", shim_socket},
    {"WS2_32.dll", "#9", shim_htons},
    {NULL, NULL, NULL}
};

void marvel_shims_init(void) { rt_register_shims(defs); }

#ifdef GAME_MARVEL
/* ---- runtime shader compiles. On a GPU vendor it has no prebuilt shaders
 * for, the game compiles HLSL through D3DCompiler_43 (0x6E7AA0 wraps
 * D3DCompile) on several worker threads. 0.1.5 crashed in the compiler with
 * that; 0.2 serialised every compile. Now (0.2.2) compiles run in parallel,
 * except that the first compiles of each target profile run alone, so the
 * compiler's one-time lazy setup happens on a single thread and is published
 * (rwlock release/acquire) before others use it. save/serial_shaders.txt
 * restores one-at-a-time. The gate itself is in f_9066C20 below. */
#include <pthread.h>
extern void f_6E7AA0_guest(CPU *c);
void f_6E7AA0(CPU *c) { f_6E7AA0_guest(c); }
#endif

#ifdef GAME_MARVEL
/* ---- D3DCompile (D3DCompiler_43 export, stdcall 11 args): shows which shader
 * is compiling on screen (DXVK HUD "portstatus" line) and times it. The name
 * is the cache file the game just failed to find (kernel32.c). */
#include <time.h>
extern char g_last_cache_miss[64];
void dxvk_port_status(const char *text);
extern void f_9066C20_guest(CPU *c);
static pthread_rwlock_t g_compile_gate = PTHREAD_RWLOCK_INITIALIZER;
static int shader_compiles_serial(void) {
    static int v = -1;
    if (v < 0) {
        char path[1100];
        snprintf(path, sizeof path, "%s/serial_shaders.txt", g_save_dir);
        FILE *f = fopen(path, "r");
        v = f != NULL;
        if (f) fclose(f);
        LOG("shader compiles: %s", v ? "one at a time (serial_shaders.txt)" : "parallel after warm-up");
    }
    return v;
}
/* Async shaders: the per-material fetch (f_652EA0, patched in gen/exe_74.c)
 * always takes the game's own queued path, so compiles run on its worker
 * threads and the slot stays "pending" until done (objects pop in).
 * Off by default since 0.2.4 (the game's own behaviour, as in 0.1.5);
 * save/async_shaders.txt turns it on. */
int port_async_shaders(void) {
    static int v = -1;
    if (v < 0) {
        char path[1100];
        snprintf(path, sizeof path, "%s/async_shaders.txt", g_save_dir);
        FILE *f = fopen(path, "r");
        v = f != NULL;
        if (f) fclose(f);
        LOG("shader loading: %s", v ? "async (async_shaders.txt)" : "game default (no async_shaders.txt)");
    }
    return v;
}
/* Compiles seen per target profile ("vs_3_0", "ps_3_0", ...); the first
 * WARMUP of each run exclusively. */
#define WARMUP 0  /* 0.2.5: no warm-up, fully parallel (TJ) */
static struct { char target[16]; unsigned n; } g_targets[16];
static pthread_mutex_t g_targets_mu = PTHREAD_MUTEX_INITIALIZER;
static int needs_warmup(const char *target) {
    int r = 1;
    pthread_mutex_lock(&g_targets_mu);
    for (int i = 0; i < 16; i++) {
        if (!g_targets[i].target[0]) {
            snprintf(g_targets[i].target, sizeof g_targets[i].target, "%s", target);
            g_targets[i].n = 1;
            break;
        }
        if (!strcmp(g_targets[i].target, target)) {
            r = g_targets[i].n++ < WARMUP;
            break;
        }
    }
    pthread_mutex_unlock(&g_targets_mu);
    return r;
}

void f_9066C20(CPU *c) {
    /* 0.2.6: plain 0.1.5 behaviour (no lock, no status line); serial_shaders.txt keeps the old path. */
    if (!shader_compiles_serial()) { f_9066C20_guest(c); return; }
    static unsigned n, done;
    static double total_ms;
    static pthread_mutex_t stats_mu = PTHREAD_MUTEX_INITIALIZER;
    char msg[160];
    const char *target = ARG(6) ? (const char *)GSTR(ARG(6)) : "?";
    char name[64];
    snprintf(name, sizeof name, "%s", g_last_cache_miss[0] ? g_last_cache_miss : "shader");
    pthread_mutex_lock(&stats_mu);
    unsigned k = ++n;
    snprintf(msg, sizeof msg, "Compiling shader %u: %s (%s)%s", k, name, target,
             done ? "" : "  - first launch builds the shader cache");
    if (done) {
        size_t l = strlen(msg);
        snprintf(msg + l, sizeof msg - l, "  avg %.0f ms", total_ms / done);
    }
    pthread_mutex_unlock(&stats_mu);
    dxvk_port_status(msg);

    int exclusive = shader_compiles_serial() || needs_warmup(target);
    if (exclusive) pthread_rwlock_wrlock(&g_compile_gate);
    else pthread_rwlock_rdlock(&g_compile_gate);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    f_9066C20_guest(c);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_rwlock_unlock(&g_compile_gate);

    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    pthread_mutex_lock(&stats_mu);
    total_ms += ms;
    unsigned d = ++done;
    double avg = total_ms / d;
    pthread_mutex_unlock(&stats_mu);
    if (d % 25 == 1) LOG("shader compile %u: %s %s %.0f ms (avg %.0f ms)%s", d, name, target, ms, avg,
                         exclusive ? " [alone]" : "");
}
/* D3DPreprocess shares the compiler's setup: same gate. */
extern void f_9067AD0_guest(CPU *c);
void f_9067AD0(CPU *c) {
    if (!shader_compiles_serial()) { f_9067AD0_guest(c); return; }
    int exclusive = shader_compiles_serial() || needs_warmup("preprocess");
    if (exclusive) pthread_rwlock_wrlock(&g_compile_gate);
    else pthread_rwlock_rdlock(&g_compile_gate);
    f_9067AD0_guest(c);
    pthread_rwlock_unlock(&g_compile_gate);
}
#endif
