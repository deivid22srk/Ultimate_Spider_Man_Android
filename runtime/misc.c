/* ADVAPI32, SHELL32, OLE32, OLEAUT32 and WINMM shims. */
#include "rt.h"
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- registry: report "not found" so the game uses its defaults ---- */
SHIM(RegOpenKeyExA) {
    TRACE("RegOpenKeyExA %s", GSTR(ARG(1)));
    if (ARG(4)) M32(ARG(4)) = 0;
    RET(2, 5);
}
SHIM(RegOpenKeyA) { if (ARG(2)) M32(ARG(2)) = 0; RET(2, 3); }
SHIM(RegQueryValueExA) { RET(2, 6); }
SHIM(RegSetValueExA) { RET(0, 6); }
SHIM(RegCloseKey) { RET(0, 1); }

/* ---- shell ---- */
SHIM(SHGetFolderPathA) {
    /* (hwnd, csidl, token, flags, path) */
    snprintf((char *)MEM + ARG(4), 260, "C:\\Users\\Player\\Documents");
    RET(0, 5);
}
SHIM(SHCreateDirectoryExA) {
    char host[1024];
    rt_host_path(GSTR(ARG(1)), host, sizeof host);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s/", host);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    RET(0, 3);
}
SHIM(ShellExecuteA) { RET(42, 6); }

/* ---- COM ---- */
SHIM(CoInitialize) { RET(0, 1); }
SHIM(CoInitializeEx) { RET(0, 2); }
SHIM(CoUninitialize) { RET(0, 0); }
SHIM(CoCreateInstance) {
    extern int xaudio2_create(uint32_t clsid, uint32_t *out);
    uint32_t obj;
    if (ARG(4) && xaudio2_create(ARG(0), &obj)) { M32(ARG(4)) = obj; RET(0, 5); }
    LOG("CoCreateInstance -> E_NOINTERFACE");
    if (ARG(4)) M32(ARG(4)) = 0;
    RET(0x80004002u, 5);
}
SHIM(CoSetProxyBlanket) { RET(0, 8); }
SHIM(SysAllocString) {
    uint32_t s = ARG(0);
    if (!s) RET(0, 1);
    uint32_t n = 0;
    while (M16(s + 2 * n)) n++;
    uint32_t p = gmalloc(4 + 2 * n + 2);
    M32(p) = 2 * n;
    memcpy(MEM + p + 4, MEM + s, 2 * n + 2);
    RET(p + 4, 1);
}
SHIM(SysFreeString) { if (ARG(0)) gfree(ARG(0) - 4); RET(0, 1); }

/* ---- winmm ---- */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
/* waveOut reports no device (MMSYSERR_NODRIVER) so Bink uses DirectSound. */
SHIM(waveOutOpen) { RET(6, 6); }
SHIM(waveOutClose) { RET(0, 1); }
SHIM(waveOutWrite) { RET(0, 3); }
SHIM(waveOutPrepareHeader) { RET(0, 3); }
SHIM(waveOutUnprepareHeader) { RET(0, 3); }
SHIM(waveOutReset) { RET(0, 1); }
SHIM(waveOutPause) { RET(0, 1); }
SHIM(waveOutRestart) { RET(0, 1); }
SHIM(waveOutSetVolume) { RET(0, 2); }
SHIM(timeBeginPeriod) { RET(0, 1); }
SHIM(timeEndPeriod) { RET(0, 1); }
SHIM(timeGetTime) { RET((uint32_t)now_ms(), 0); }

typedef struct {
    uint32_t id, delay, cb, user, flags;
    volatile int stop;
} GTimer;

static void *timer_main(void *arg) {
    GTimer *t = arg;
    CPU *c = rt_new_cpu();
    rt_thread_init(c);
    do {
        usleep(t->delay * 1000);
        if (t->stop) break;
        if (t->flags & 0x30) {
            /* TIME_CALLBACK_EVENT_SET / _PULSE: cb is an event handle. A pulse
             * is approximated by a set; the waiter's wait resets it. */
            extern void k32_set_event(uint32_t h);
            k32_set_event(t->cb);
        } else {
            uint32_t args[5] = {t->id, 0, t->user, 0, 0};
            rt_invoke(c, t->cb, 5, args);
        }
    } while (t->flags & 1); /* TIME_PERIODIC */
    extern void rt_thread_exit(CPU *c);
    rt_thread_exit(c);
    free(t);
    return NULL;
}

#define MAX_TIMERS 64
static GTimer *g_timers[MAX_TIMERS];

SHIM(timeSetEvent) {
    /* (delay, resolution, callback, user, flags) */
    for (uint32_t i = 1; i < MAX_TIMERS; i++)
        if (!g_timers[i]) {
            GTimer *t = calloc(1, sizeof *t);
            t->id = i;
            t->delay = ARG(0) ? ARG(0) : 1;
            t->cb = ARG(2);
            t->user = ARG(3);
            t->flags = ARG(4);
            g_timers[i] = t;
            pthread_t pt;
            pthread_create(&pt, NULL, timer_main, t);
            pthread_detach(pt);
            LOG("timeSetEvent %ums cb=%08x flags=%x", t->delay, t->cb, t->flags);
            RET(i, 5);
        }
    RET(0, 5);
}
SHIM(timeKillEvent) {
    uint32_t i = ARG(0);
    if (i < MAX_TIMERS && g_timers[i]) { g_timers[i]->stop = 1; g_timers[i] = NULL; }
    RET(0, 1);
}

static const ShimDef defs[] = {
    {"ADVAPI32.dll", "RegOpenKeyExA", shim_RegOpenKeyExA},
    {"ADVAPI32.dll", "RegOpenKeyA", shim_RegOpenKeyA},
    {"ADVAPI32.dll", "RegQueryValueExA", shim_RegQueryValueExA},
    {"ADVAPI32.dll", "RegSetValueExA", shim_RegSetValueExA},
    {"ADVAPI32.dll", "RegCloseKey", shim_RegCloseKey},
    {"SHELL32.dll", "SHGetFolderPathA", shim_SHGetFolderPathA},
    {"SHELL32.dll", "SHCreateDirectoryExA", shim_SHCreateDirectoryExA},
    {"SHELL32.dll", "ShellExecuteA", shim_ShellExecuteA},
    {"ole32.dll", "CoInitialize", shim_CoInitialize},
    {"ole32.dll", "CoInitializeEx", shim_CoInitializeEx},
    {"ole32.dll", "CoUninitialize", shim_CoUninitialize},
    {"ole32.dll", "CoCreateInstance", shim_CoCreateInstance},
    {"ole32.dll", "CoSetProxyBlanket", shim_CoSetProxyBlanket},
    {"OLEAUT32.dll", "SysAllocString", shim_SysAllocString},
    {"OLEAUT32.dll", "SysFreeString", shim_SysFreeString},
    {"OLEAUT32.dll", "#2", shim_SysAllocString},
    {"OLEAUT32.dll", "#6", shim_SysFreeString},
    {"WINMM.dll", "timeGetTime", shim_timeGetTime},
    {"WINMM.dll", "waveOutOpen", shim_waveOutOpen},
    {"WINMM.dll", "waveOutClose", shim_waveOutClose},
    {"WINMM.dll", "waveOutWrite", shim_waveOutWrite},
    {"WINMM.dll", "waveOutPrepareHeader", shim_waveOutPrepareHeader},
    {"WINMM.dll", "waveOutUnprepareHeader", shim_waveOutUnprepareHeader},
    {"WINMM.dll", "waveOutReset", shim_waveOutReset},
    {"WINMM.dll", "waveOutPause", shim_waveOutPause},
    {"WINMM.dll", "waveOutRestart", shim_waveOutRestart},
    {"WINMM.dll", "waveOutSetVolume", shim_waveOutSetVolume},
    {"WINMM.dll", "timeBeginPeriod", shim_timeBeginPeriod},
    {"WINMM.dll", "timeEndPeriod", shim_timeEndPeriod},
    {"WINMM.dll", "timeSetEvent", shim_timeSetEvent},
    {"WINMM.dll", "timeKillEvent", shim_timeKillEvent},
    {NULL, NULL, NULL}
};

void misc_init(void) { rt_register_shims(defs); }
