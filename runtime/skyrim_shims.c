/* Shims for imports Skyrim (TESV.exe 1.9.32) uses beyond the LEGO ports:
 * INI files (Get/WritePrivateProfile*), file times, misc kernel32/user32. */
#define _GNU_SOURCE
#include "rt.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ===================== INI files =====================
 * Parsed files are cached by host path and reloaded when their mtime or size
 * changes; Skyrim reads every setting with a separate call. */
typedef struct { char *sec, *key, *val; } IniEnt;
typedef struct IniFile {
    char path[1024];
    time_t mtime;
    off_t size;
    IniEnt *e;
    int n, cap;
    struct IniFile *next;
} IniFile;
static IniFile *g_inis;
static pthread_mutex_t g_ini_lock = PTHREAD_MUTEX_INITIALIZER;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void ini_free(IniFile *f) {
    for (int i = 0; i < f->n; i++) { free(f->e[i].sec); free(f->e[i].key); free(f->e[i].val); }
    free(f->e);
    f->e = NULL;
    f->n = f->cap = 0;
}

static void ini_add(IniFile *f, const char *sec, const char *key, const char *val) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 64;
        f->e = realloc(f->e, sizeof(IniEnt) * f->cap);
    }
    f->e[f->n++] = (IniEnt){strdup(sec), key ? strdup(key) : NULL, val ? strdup(val) : NULL};
}

/* Section header entries have key == NULL, so empty sections are kept. */
static void ini_parse(IniFile *f) {
    ini_free(f);
    FILE *fp = fopen(f->path, "rb");
    if (!fp) return;
    char line[4096], sec[256] = "";
    while (fgets(line, sizeof line, fp)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) *e = 0;
            snprintf(sec, sizeof sec, "%s", trim(s + 1));
            ini_add(f, sec, NULL, NULL);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        ini_add(f, sec, trim(s), trim(eq + 1));
    }
    fclose(fp);
}

static IniFile *ini_get(const char *winpath) {
    char host[1024];
    if (!winpath) return NULL;
    /* a bare name means the Windows directory; treat it as the game dir */
    if (!strchr(winpath, '\\') && !strchr(winpath, '/')) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s/%s", g_game_dir, winpath);
        snprintf(host, sizeof host, "%s", tmp);
    } else {
        rt_host_path(winpath, host, sizeof host);
    }
    IniFile *f;
    for (f = g_inis; f; f = f->next) if (!strcmp(f->path, host)) break;
    if (!f) {
        f = calloc(1, sizeof *f);
        snprintf(f->path, sizeof f->path, "%s", host);
        f->mtime = -1;
        f->next = g_inis;
        g_inis = f;
    }
    struct stat st;
    if (stat(host, &st)) {
        if (f->mtime != 0) { ini_free(f); f->mtime = 0; f->size = 0; }
    } else if (st.st_mtime != f->mtime || st.st_size != f->size) {
        f->mtime = st.st_mtime;
        f->size = st.st_size;
        ini_parse(f);
    }
    return f;
}

static const char *ini_find(IniFile *f, const char *sec, const char *key) {
    for (int i = 0; i < f->n; i++)
        if (f->e[i].key && !strcasecmp(f->e[i].sec, sec) && !strcasecmp(f->e[i].key, key)) return f->e[i].val;
    return NULL;
}

/* Copies a double-NUL list or a string into a guest buffer, Windows style:
 * returns chars written excluding the final NUL; truncates to size-1 (or
 * size-2 for lists). */
static uint32_t put_str(uint32_t buf, uint32_t size, const char *s, size_t len, int list) {
    if (!buf || !size) return 0;
    char *d = (char *)MEM + buf;
    if (!list) {
        if (len >= size) len = size - 1;
        memcpy(d, s, len);
        d[len] = 0;
        return (uint32_t)len;
    }
    if (size < 2) { d[0] = 0; return 0; }
    if (len + 1 > size) {  /* truncated list */
        len = size - 2;
        memcpy(d, s, len);
        d[len] = d[len + 1] = 0;
        return size - 2;
    }
    memcpy(d, s, len);
    d[len] = 0;
    return (uint32_t)(len ? len - 1 : 0);
}

/* (app, key, default, buf, size, file) */
SHIM(GetPrivateProfileStringA) {
    const char *app = GSTR(ARG(0)), *key = GSTR(ARG(1)), *def = GSTR(ARG(2));
    uint32_t buf = ARG(3), size = ARG(4);
    pthread_mutex_lock(&g_ini_lock);
    IniFile *f = ini_get(GSTR(ARG(5)));
    uint32_t r;
    if (!app || !key) {
        /* list of sections, or of keys in a section */
        size_t cap = 4096, len = 0;
        char *tmp = malloc(cap);
        for (int i = 0; f && i < f->n; i++) {
            const char *item = NULL;
            if (!app && !f->e[i].key) item = f->e[i].sec;
            if (app && f->e[i].key && !strcasecmp(f->e[i].sec, app)) item = f->e[i].key;
            if (!item) continue;
            size_t l = strlen(item) + 1;
            if (len + l + 1 > cap) tmp = realloc(tmp, cap = (len + l + 1) * 2);
            memcpy(tmp + len, item, l);
            len += l;
        }
        tmp[len] = 0;
        r = put_str(buf, size, tmp, len + 1, 1);
        free(tmp);
    } else {
        const char *v = f ? ini_find(f, app, key) : NULL;
        if (!v) v = def ? def : "";
        r = put_str(buf, size, v, strlen(v), 0);
    }
    pthread_mutex_unlock(&g_ini_lock);
    RET(r, 6);
}

/* (app, key, default, file) */
SHIM(GetPrivateProfileIntA) {
    pthread_mutex_lock(&g_ini_lock);
    IniFile *f = ini_get(GSTR(ARG(3)));
    const char *v = f && ARG(0) && ARG(1) ? ini_find(f, GSTR(ARG(0)), GSTR(ARG(1))) : NULL;
    uint32_t r = v ? (uint32_t)strtol(v, NULL, 10) : ARG(2);
    pthread_mutex_unlock(&g_ini_lock);
    RET(r, 4);
}

static void ini_save(IniFile *f) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", f->path);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", f->path);
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    for (int i = 0; i < f->n; i++) {
        if (!f->e[i].key) fprintf(fp, "%s[%s]\r\n", i ? "\r\n" : "", f->e[i].sec);
        else fprintf(fp, "%s=%s\r\n", f->e[i].key, f->e[i].val);
    }
    fclose(fp);
    rename(tmp, f->path);
    struct stat st;
    if (!stat(f->path, &st)) { f->mtime = st.st_mtime; f->size = st.st_size; }
}

/* (app, key, value, file): key NULL deletes the section, value NULL the key. */
SHIM(WritePrivateProfileStringA) {
    const char *app = GSTR(ARG(0)), *key = GSTR(ARG(1)), *val = GSTR(ARG(2));
    if (!app) RET(1, 4);
    pthread_mutex_lock(&g_ini_lock);
    IniFile *f = ini_get(GSTR(ARG(3)));
    int last_in_sec = -1, have_sec = 0, done = 0;
    for (int i = 0; i < f->n; i++) {
        if (strcasecmp(f->e[i].sec, app)) continue;
        have_sec = 1;
        last_in_sec = i;
        if (!key) { free(f->e[i].sec); f->e[i].sec = strdup("\001DEL"); continue; }
        if (f->e[i].key && !strcasecmp(f->e[i].key, key)) {
            if (val) { free(f->e[i].val); f->e[i].val = strdup(val); }
            else { free(f->e[i].sec); f->e[i].sec = strdup("\001DEL"); }
            done = 1;
        }
    }
    if (key && val && !done) {
        if (!have_sec) { ini_add(f, app, NULL, NULL); ini_add(f, app, key, val); }
        else {
            ini_add(f, app, key, val);  /* append, then move after the section's last entry */
            IniEnt e = f->e[f->n - 1];
            memmove(&f->e[last_in_sec + 2], &f->e[last_in_sec + 1], sizeof(IniEnt) * (f->n - 2 - last_in_sec));
            f->e[last_in_sec + 1] = e;
        }
    }
    int w = 0;
    for (int i = 0; i < f->n; i++) {
        if (!strcmp(f->e[i].sec, "\001DEL")) { free(f->e[i].sec); free(f->e[i].key); free(f->e[i].val); }
        else f->e[w++] = f->e[i];
    }
    f->n = w;
    ini_save(f);
    pthread_mutex_unlock(&g_ini_lock);
    RET(1, 4);
}

/* ===================== kernel32 ===================== */
SHIM(lstrcpyA) { strcpy((char *)MEM + ARG(0), GSTR(ARG(1))); RET(ARG(0), 2); }
SHIM(lstrcatA) { strcat((char *)MEM + ARG(0), GSTR(ARG(1))); RET(ARG(0), 2); }
SHIM(GetCurrentDirectoryA) {
    const char *d = GAME_WIN_DIR;
    uint32_t n = (uint32_t)strlen(d);
    if (ARG(0) <= n) RET(n + 1, 2);
    strcpy((char *)MEM + ARG(1), d);
    RET(n, 2);
}
SHIM(CompareFileTime) {
    uint64_t a = M32(ARG(0)) | (uint64_t)M32(ARG(0) + 4) << 32;
    uint64_t b = M32(ARG(1)) | (uint64_t)M32(ARG(1) + 4) << 32;
    RET(a < b ? 0xFFFFFFFFu : a > b, 2);
}
SHIM(LocalFileTimeToFileTime) {
    M32(ARG(1)) = M32(ARG(0));
    M32(ARG(1) + 4) = M32(ARG(0) + 4);
    RET(1, 2);
}
static void put_tm(uint32_t o, const struct tm *tm, int ms) {
    M16(o) = (uint16_t)(tm->tm_year + 1900);
    M16(o + 2) = (uint16_t)(tm->tm_mon + 1);
    M16(o + 4) = (uint16_t)tm->tm_wday;
    M16(o + 6) = (uint16_t)tm->tm_mday;
    M16(o + 8) = (uint16_t)tm->tm_hour;
    M16(o + 10) = (uint16_t)tm->tm_min;
    M16(o + 12) = (uint16_t)tm->tm_sec;
    M16(o + 14) = (uint16_t)ms;
}
SHIM(GetLocalTime) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    put_tm(ARG(0), &tm, (int)(tv.tv_usec / 1000));
    RET(0, 1);
}
/* (tz, utc SYSTEMTIME*, local SYSTEMTIME*): keep UTC (no zone database needed). */
SHIM(SystemTimeToTzSpecificLocalTime) {
    memcpy(MEM + ARG(2), MEM + ARG(1), 16);
    RET(1, 3);
}
SHIM(GetFileTime) {
    /* (h, creation, access, write): handle -> fd is private to kernel32.c,
     * so report "now" for all three. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ft = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull;
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) { M32(ARG(i)) = (uint32_t)ft; M32(ARG(i) + 4) = (uint32_t)(ft >> 32); }
    RET(1, 4);
}
SHIM(GetDriveTypeA) { RET(3, 1); }  /* DRIVE_FIXED */
SHIM(GetDiskFreeSpaceExA) {
    struct statvfs sv;
    uint64_t free_b = 8ull << 30, total = 32ull << 30;
    if (!statvfs(g_save_dir, &sv)) { free_b = (uint64_t)sv.f_bavail * sv.f_frsize; total = (uint64_t)sv.f_blocks * sv.f_frsize; }
    for (int i = 1; i <= 3; i++) {
        uint64_t v = i == 2 ? total : free_b;
        if (ARG(i)) { M32(ARG(i)) = (uint32_t)v; M32(ARG(i) + 4) = (uint32_t)(v >> 32); }
    }
    RET(1, 4);
}
SHIM(InterlockedExchangeAdd) { __atomic_thread_fence(__ATOMIC_SEQ_CST); uint32_t r_ = __atomic_fetch_add((uint32_t *)(MEM + ARG(0)), ARG(1), __ATOMIC_SEQ_CST); __atomic_thread_fence(__ATOMIC_SEQ_CST); RET(r_, 2); }
SHIM(SuspendThread) { RET(0, 1); }
SHIM(SetThreadIdealProcessor) { RET(0, 2); }
/* (addr, MEMORY_BASIC_INFORMATION*, len) */
SHIM(VirtualQuery) {
    uint32_t a = ARG(0) & ~0xFFFu, o = ARG(1);
    if (ARG(2) < 28) RET(0, 3);
    M32(o) = a;
    M32(o + 4) = a;
    M32(o + 8) = 4;          /* PAGE_READWRITE */
    M32(o + 12) = 0x1000;
    M32(o + 16) = 0x1000;    /* MEM_COMMIT */
    M32(o + 20) = 4;
    M32(o + 24) = 0x20000;   /* MEM_PRIVATE */
    RET(28, 3);
}
SHIM(PeekNamedPipe) { RET(0, 6); }
SHIM(GetComputerNameA) {
    uint32_t len = ARG(1);
    if (M32(len) < 7) { M32(len) = 7; RET(0, 2); }
    strcpy((char *)MEM + ARG(0), "ANDROID");
    M32(len) = 7;
    RET(1, 2);
}
SHIM(OpenEventA) { rt_set_last_error(c, 2); RET(0, 3); }
SHIM(OpenMutexA) { rt_set_last_error(c, 2); RET(0, 3); }
SHIM(GetThreadPriority) { RET(0, 1); }
SHIM(GetEnvironmentVariableA) { rt_set_last_error(c, 203); RET(0, 3); }  /* ERROR_ENVVAR_NOT_FOUND */
SHIM(GlobalMemoryStatusEx) {
    uint32_t p = ARG(0);
    M32(p) = 64;
    M32(p + 4) = 40;
    uint64_t v[] = {3ull << 30, 2ull << 30, 4ull << 30, 3ull << 30, 0xFFFE0000ull, 0xC0000000ull, 0};
    for (int i = 0; i < 7; i++) { M32(p + 8 + 8 * i) = (uint32_t)v[i]; M32(p + 12 + 8 * i) = (uint32_t)(v[i] >> 32); }
    RET(1, 1);
}

/* ===================== user32 / gdi32 ===================== */
SHIM(GetAsyncKeyState) { RET(0, 1); }
SHIM(GetKeyState) { RET(0, 1); }
SHIM(SetWindowTextA) { RET(1, 2); }
SHIM(FindWindowA) { RET(0, 2); }
SHIM(SetWindowsHookExA) { RET(0x30001, 4); }
SHIM(CallNextHookEx) { RET(0, 4); }
SHIM(UnhookWindowsHookEx) { RET(1, 1); }
SHIM(GetDlgItem) { RET(0, 2); }
SHIM(EnumChildWindows) { RET(1, 3); }
SHIM(GetClassNameA) { if (ARG(2)) M8(ARG(1)) = 0; RET(0, 3); }
SHIM(GetWindowTextA) { if (ARG(2)) M8(ARG(1)) = 0; RET(0, 3); }
/* (vk, sc, keystate[256], wchar buf, n, flags) */
SHIM(ToUnicode) {
    uint32_t vk = ARG(0), buf = ARG(3), n = ARG(4);
    uint16_t ch = 0;
    int shift = ARG(2) && (M8(ARG(2) + 0x10) & 0x80);
    if (vk >= 'A' && vk <= 'Z') ch = (uint16_t)(shift ? vk : vk + 32);
    else if (vk >= '0' && vk <= '9') ch = (uint16_t)vk;
    else if (vk == 0x20 || vk == 0x0D || vk == 0x08) ch = (uint16_t)vk;
    if (!ch || !n) RET(0, 6);
    M16(buf) = ch;
    RET(1, 6);
}
SHIM(GetWindowInfo) {
    uint32_t p = ARG(1);
    memset(MEM + p + 4, 0, 56);
    M32(p) = 60;
    RET(1, 2);
}
SHIM(GetStockObject) { RET(0x50000 + ARG(0), 1); }

/* ===================== advapi32 / ole32 / d3d9 ===================== */
SHIM(RegCreateKeyExA) { if (ARG(7)) M32(ARG(7)) = 0x40001; if (ARG(8)) M32(ARG(8)) = 2; RET(0, 9); }
SHIM(CoTaskMemAlloc) { RET(gmalloc(ARG(0) ? ARG(0) : 1), 1); }
SHIM(CoTaskMemFree) { if (ARG(0)) gfree(ARG(0)); RET(0, 1); }
SHIM(D3DPERF_SetOptions) { RET(0, 1); }

static const ShimDef defs[] = {
#define K(n) {"KERNEL32.dll", #n, shim_##n},
#define U(n) {"USER32.dll", #n, shim_##n},
    K(GetPrivateProfileStringA) K(GetPrivateProfileIntA) K(WritePrivateProfileStringA)
    K(lstrcpyA) K(lstrcatA) K(GetCurrentDirectoryA) K(CompareFileTime) K(LocalFileTimeToFileTime)
    K(GetLocalTime) K(SystemTimeToTzSpecificLocalTime) K(GetFileTime) K(GetDriveTypeA)
    K(GetDiskFreeSpaceExA) K(InterlockedExchangeAdd) K(SuspendThread) K(SetThreadIdealProcessor)
    K(VirtualQuery) K(PeekNamedPipe) K(GetComputerNameA)
    K(OpenEventA) K(OpenMutexA) K(GlobalMemoryStatusEx) K(GetThreadPriority) K(GetEnvironmentVariableA)
    U(GetAsyncKeyState) U(GetKeyState) U(SetWindowTextA) U(FindWindowA) U(SetWindowsHookExA)
    U(CallNextHookEx) U(UnhookWindowsHookEx) U(GetDlgItem) U(EnumChildWindows) U(GetClassNameA)
    U(GetWindowTextA) U(ToUnicode) U(GetWindowInfo)
    {"GDI32.dll", "GetStockObject", shim_GetStockObject},
    {"ADVAPI32.dll", "RegCreateKeyExA", shim_RegCreateKeyExA},
    {"ole32.dll", "CoTaskMemAlloc", shim_CoTaskMemAlloc},
    {"ole32.dll", "CoTaskMemFree", shim_CoTaskMemFree},
    {"d3d9.dll", "D3DPERF_SetOptions", shim_D3DPERF_SetOptions},
#undef K
#undef U
    {NULL, NULL, NULL}
};

/* First run: SkyrimPrefs.ini starts from the install's low.ini preset (the
 * launcher normally writes it), full screen at the window's size. Low is the
 * phone default; later runs keep whatever the player changed. */
static void ini_set(IniFile *f, const char *sec, const char *key, const char *val) {
    for (int i = 0; i < f->n; i++)
        if (f->e[i].key && !strcasecmp(f->e[i].sec, sec) && !strcasecmp(f->e[i].key, key)) {
            free(f->e[i].val);
            f->e[i].val = strdup(val);
            return;
        }
    int have = 0;
    for (int i = 0; i < f->n; i++) if (!f->e[i].key && !strcasecmp(f->e[i].sec, sec)) have = 1;
    if (!have) ini_add(f, sec, NULL, NULL);
    ini_add(f, sec, key, val);  /* order inside a section does not matter to the game */
}
void skyrim_seed_prefs(int w, int h) {
    const char *prefs = "C:\\Users\\Player\\Documents\\My Games\\Skyrim\\SkyrimPrefs.ini";
    char host[1024];
    if (rt_host_path(prefs, host, sizeof host)) return;
    pthread_mutex_lock(&g_ini_lock);
    IniFile *f = ini_get(prefs);
    char low[1024];
    snprintf(low, sizeof low, "%s/low.ini", g_game_dir);
    IniFile src = {0};
    snprintf(src.path, sizeof src.path, "%s", low);
    ini_parse(&src);
    for (int i = 0; i < src.n; i++)
        if (src.e[i].key) ini_set(f, src.e[i].sec, src.e[i].key, src.e[i].val);
    ini_free(&src);
    char v[16];
    snprintf(v, sizeof v, "%d", w);
    ini_set(f, "Display", "iSize W", v);
    snprintf(v, sizeof v, "%d", h);
    ini_set(f, "Display", "iSize H", v);
    ini_set(f, "Display", "bFull Screen", "1");
    ini_set(f, "Display", "iPresentInterval", "1");
    ini_set(f, "MAIN", "bGamepadEnable", "1");
    ini_save(f);
    pthread_mutex_unlock(&g_ini_lock);
    LOG("seeded %s (low preset, %dx%d)", host, w, h);
}

void skyrim_shims_init(void) {
    rt_register_shims(defs);
    /* X3DAudio1_7 imports msvcrt's _adjust_fdiv (Pentium FDIV flag): 0 */
    uint32_t p = gmalloc(4);
    M32(p) = 0;
    rt_register_data("msvcrt.dll", "_adjust_fdiv", p);
}
