/* Ultimate Spider-Man (2005, PC) runtime shims.
 *
 * Everything here exists because USM.exe imports it and the shared runtime does
 * not cover it yet. Two groups:
 *
 *   1. msvcr71.dll / msvcp71.dll  -- the VC7.1 CRT the game was built against.
 *      The C half is the same API as msvcrt.dll, so msvcrt's table is
 *      re-registered under the msvcr71 name (rt_register_shims_alias) after the
 *      handful of functions it does not implement are registered here. The C++
 *      half is the MSVC 7.1 std::string/std::exception/ostream ABI, implemented
 *      against the documented object layouts below.
 *   2. Win32 APIs the shared runtime lacks: profile (win.ini) calls, Global*,
 *      dialogs/menus/clipboard, comctl32 image lists, a few GDI calls.
 *
 * Rules: a call we cannot emulate fails loudly (rt_fatal) instead of returning
 * something invented; calls whose Windows result is "nothing happened" (a
 * dialog that does not exist, a clipboard no one reads) return the Windows
 * failure value and are logged once.
 */
#define _GNU_SOURCE
#include "rt.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

void msvcrt_register_as_msvcr71(void);

static void mkdirs_for(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
}

/* ====================================================================== */
/* msvcr71: stdio over host FILE*, with a guest FILE* handle             */
/* ====================================================================== */

#define USM_MAX_FILES 48
#define USM_FILE_MAGIC 0x46494C45u /* "FILE" */

static FILE *g_files[USM_MAX_FILES];

static uint32_t file_new(FILE *f) {
    for (int i = 0; i < USM_MAX_FILES; i++) {
        if (g_files[i]) continue;
        g_files[i] = f;
        uint32_t o = gmalloc(16);
        M32(o) = USM_FILE_MAGIC;
        M32(o + 4) = (uint32_t)i;
        return o;
    }
    return 0;
}

static FILE *file_of(uint32_t o) {
    if (!o || M32(o) != USM_FILE_MAGIC) return NULL;
    uint32_t i = M32(o + 4);
    return i < USM_MAX_FILES ? g_files[i] : NULL;
}

static void file_drop(uint32_t o) {
    if (!o || M32(o) != USM_FILE_MAGIC) return;
    uint32_t i = M32(o + 4);
    if (i < USM_MAX_FILES && g_files[i]) {
        fclose(g_files[i]);
        g_files[i] = NULL;
    }
    M32(o) = 0;
    gfree(o);
}

SHIM(fopen) {
    const char *name = GSTR(ARG(0));
    const char *mode = GSTR(ARG(1));
    char host[1024];
    rt_host_path(name, host, sizeof host);
    if (strchr(mode ? mode : "", 'w') || strchr(mode ? mode : "", 'a')) mkdirs_for(host);
    FILE *f = fopen(host, mode ? mode : "rb");
    TRACE("msvcr71 fopen %s (%s) -> %s", name, mode, f ? "ok" : "fail");
    if (!f) { rt_set_last_error(c, 2); RETC(0); }
    uint32_t o = file_new(f);
    if (!o) { fclose(f); RETC(0); }
    RETC(o);
}

SHIM(fclose) {
    if (!ARG(0)) RETC(-1);
    file_drop(ARG(0));
    RETC(0);
}

SHIM(fread) {
    FILE *f = file_of(ARG(3));
    if (!f) RETC(0);
    RETC(fread(MEM + ARG(0), ARG(1), ARG(2), f));
}

SHIM(fwrite) {
    FILE *f = file_of(ARG(3));
    if (!f) RETC(0);
    RETC(fwrite(MEM + ARG(0), ARG(1), ARG(2), f));
}

SHIM(fseek) {
    FILE *f = file_of(ARG(0));
    if (!f) RETC(-1);
    RETC(fseek(f, (long)(int32_t)ARG(1), (int)ARG(2)));
}

SHIM(ftell) {
    FILE *f = file_of(ARG(0));
    if (!f) RETC(-1);
    RETC(ftell(f));
}

SHIM(fflush) {
    if (!ARG(0)) { fflush(NULL); RETC(0); }
    FILE *f = file_of(ARG(0));
    if (!f) RETC(-1);
    RETC(fflush(f));
}

SHIM(fputs) {
    FILE *f = file_of(ARG(1));
    const char *s = GSTR(ARG(0));
    if (!f || !s) RETC(-1);
    RETC(fputs(s, f));
}

SHIM(printf) {
    char buf[4096];
    guest_vformat(buf, sizeof buf, GSTR(ARG(0)), c->r[4] + 8);
    fputs(buf, stderr);
    RETC((int32_t)strlen(buf));
}

SHIM(fprintf) {
    FILE *f = file_of(ARG(0));
    char buf[4096];
    guest_vformat(buf, sizeof buf, GSTR(ARG(1)), c->r[4] + 12);
    if (!f) RETC(-1);
    RETC(fputs(buf, f));
}

SHIM(sprintf) {
    char buf[4096];
    size_t n = guest_vformat(buf, sizeof buf, GSTR(ARG(1)), c->r[4] + 12);
    memcpy(MEM + ARG(0), buf, n + 1);
    RETC((int32_t)n);
}

SHIM(vsprintf) {
    char buf[4096];
    size_t n = guest_vformat(buf, sizeof buf, GSTR(ARG(1)), ARG(2));
    memcpy(MEM + ARG(0), buf, n + 1);
    RETC((int32_t)n);
}

SHIM(tmpfile) { RETC(0); }

SHIM(_fstat) {
    if (ARG(1)) memset(MEM + ARG(1), 0, 44);
    RETC(0);
}

SHIM(_flushall) { fflush(NULL); RETC(0); }

SHIM(_msize) { RETC(gsize(ARG(0))); }

SHIM(_aligned_malloc) {
    uint32_t align = ARG(1);
    if (align > 16) LOG("msvcr71 _aligned_malloc(%u, %u): 16-byte alignment", ARG(0), align);
    RETC(gmalloc(ARG(0) ? ARG(0) : 1));
}

SHIM(_aligned_free) { gfree(ARG(0)); RETC(0); }

/* ====================================================================== */
/* msvcr71: strings, numbers, time                                        */
/* ====================================================================== */

SHIM(strncpy) {
    char *d = GSTR(ARG(0));
    const char *s = GSTR(ARG(1));
    size_t n = ARG(2);
    if (!d) RETC(0);
    size_t i = 0;
    for (; i < n && s && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    RETC(ARG(0));
}

SHIM(strncat) {
    char *d = GSTR(ARG(0));
    const char *s = GSTR(ARG(1));
    size_t n = ARG(2);
    if (!d) RETC(0);
    size_t l = strlen(d);
    size_t i = 0;
    for (; i < n && s && s[i]; i++) d[l + i] = s[i];
    d[l + i] = 0;
    RETC(ARG(0));
}

SHIM(strtok) {
    static char *save;
    char *s = ARG(0) ? GSTR(ARG(0)) : save;
    if (!s) RETC(0);
    char *r = strtok_r(s, GSTR(ARG(1)), &save);
    RETC(r ? (uint32_t)(r - (char *)MEM) : 0);
}

SHIM(strcspn) { RETC(strcspn(GSTR(ARG(0)), GSTR(ARG(1)))); }
SHIM(strtol) { RETC((int32_t)strtol(GSTR(ARG(0)), NULL, (int)ARG(2))); }
SHIM(strtoul) { RETC((uint32_t)strtoul(GSTR(ARG(0)), NULL, (int)ARG(2))); }
SHIM(_strcmpi) { RETC(strcasecmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
SHIM(_strnicmp) { RETC(strncasecmp(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2))); }

SHIM(_strlwr) {
    char *s = GSTR(ARG(0));
    if (s) for (; *s; s++) *s = (char)tolower((unsigned char)*s);
    RETC(ARG(0));
}

SHIM(_strupr) {
    char *s = GSTR(ARG(0));
    if (s) for (; *s; s++) *s = (char)toupper((unsigned char)*s);
    RETC(ARG(0));
}

SHIM(_mbsdup) { RETC(ARG(0) ? gstrdup(GSTR(ARG(0))) : 0); }
SHIM(_ismbblead) { RETC(0); }

SHIM(bsearch) {
    /* void *bsearch(key, base, n, width, cmp) */
    uint32_t key = ARG(0), base = ARG(1), n = ARG(2), w = ARG(3), cmp = ARG(4);
    if (!n || !w) RETC(0);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t a[2] = {key, base + mid * w};
        int32_t r = (int32_t)rt_invoke(c, cmp, 2, a);
        if (r == 0) RETC(base + mid * w);
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    RETC(0);
}

SHIM(time_) {
    time_t t = time(NULL);
    if (ARG(0)) M32(ARG(0)) = (uint32_t)t;
    RETC((uint32_t)t);
}

SHIM(clock_) { RETC((uint32_t)(clock() / (CLOCKS_PER_SEC / 1000))); }

SHIM(_mkdir) {
    char host[1024];
    rt_host_path(GSTR(ARG(0)), host, sizeof host);
    mkdir(host, 0755);
    RETC(0);
}

SHIM(_chdir) { RETC(0); }

SHIM(_getcwd) {
    char host[1024];
    snprintf(host, sizeof host, "C:\\USM");
    if (ARG(0)) snprintf(GSTR(ARG(0)), ARG(1), "%s", host);
    RETC(ARG(0));
}

/* ====================================================================== */
/* msvcr71: CRT plumbing                                                  */
/* ====================================================================== */

static uint32_t g_acmdln, g_adjust_fdiv, g_iob71, g_fmode71, g_commode71;

SHIM(__getmainargs) {
    /* int *argc, char ***argv, char ***env, int wildcard, void *startinfo */
    uint32_t argc = ARG(0), argv = ARG(1);
    if (argc) M32(argc) = 1;
    if (argv) {
        uint32_t arr = gmalloc(8);
        M32(arr) = g_acmdln ? M32(g_acmdln) : 0;
        M32(arr + 4) = 0;
        M32(argv) = arr;
    }
    RETC(0);
}

SHIM(__set_app_type) { RETC(0); }
SHIM(__setusermatherr) { RETC(0); }
SHIM(_callnewh) { RETC(1); }
SHIM(_security_error_handler) { rt_fatal("msvcr71 buffer security check failed"); }
SHIM(__p__fmode) { RETC(g_fmode71); }
SHIM(__p__commode) { RETC(g_commode71); }

SHIM(_cexit) { LOG("msvcr71 _cexit"); RETC(0); }
SHIM(_c_exit) { LOG("msvcr71 _c_exit"); RETC(0); }
SHIM(_exit) { LOG("msvcr71 _exit(%d)", (int)ARG(0)); fflush(NULL); _exit((int)ARG(0)); }

/* SEH: the game only stores this address in its exception frames. Exceptions
 * are not dispatched by this runtime (see _CxxThrowException). */
SHIM(_except_handler3) { rt_fatal("msvcr71 _except_handler3 invoked"); }

/* std::exception (MSVC 7.1): { vfptr; char *_Myptr; } */
static uint32_t g_exc_vtbl, g_exc_msg;

static void shim_exc_dtor(CPU *c) { RET(c->r[1], 0); }
static void shim_exc_what(CPU *c) { RET(g_exc_msg, 0); }

SHIM(exc_ctor) {
    uint32_t self = c->r[1];
    M32(self) = g_exc_vtbl;
    M32(self + 4) = 0;
    RET(self, 0);
}

SHIM(exc_ctor_copy) {
    uint32_t self = c->r[1], other = ARG(0);
    M32(self) = g_exc_vtbl;
    M32(self + 4) = other ? M32(other + 4) : 0;
    RET(self, 1);
}

SHIM(exc_dtor_member) { RET(0, 0); }

SHIM(operator_delete_array) { gfree(ARG(0)); RETC(0); }

/* ====================================================================== */
/* msvcp71: std::string and std::ofstream                                 */
/* ====================================================================== */

/* MSVC 7.1 std::string: union { char _Buf[16]; char *_Ptr; } _Bx;
 *                       size_t _Mysize; size_t _Myres;   (24 bytes)
 * _Myres == 15 means the small buffer is in use. */
#define STR_SIZE(o) M32((o) + 16)
#define STR_RES(o) M32((o) + 20)

static void str_assign(uint32_t o, const char *s, uint32_t n) {
    if (n <= 15) {
        if (s && n) memcpy(MEM + o, s, n);
        M8(o + n) = 0;
        STR_SIZE(o) = n;
        STR_RES(o) = 15;
    } else {
        uint32_t p = gmalloc(n + 1);
        if (s) memcpy(MEM + p, s, n);
        M8(p + n) = 0;
        M32(o) = p;
        STR_SIZE(o) = n;
        STR_RES(o) = n;
    }
}

SHIM(str_ctor_cstr) {
    uint32_t self = c->r[1];
    const char *s = GSTR(ARG(0));
    str_assign(self, s, s ? (uint32_t)strlen(s) : 0);
    RET(self, 1);
}

SHIM(str_ctor_copy) {
    uint32_t self = c->r[1], other = ARG(0);
    uint32_t n = other ? STR_SIZE(other) : 0;
    const char *src = NULL;
    if (other) src = (const char *)(MEM + (STR_RES(other) >= 16 ? M32(other) : other));
    str_assign(self, src, n);
    RET(self, 1);
}

SHIM(str_dtor) {
    uint32_t self = c->r[1];
    if (STR_RES(self) >= 16) gfree(M32(self));
    STR_SIZE(self) = 0;
    STR_RES(self) = 15;
    M32(self) = 0;
    RET(0, 0);
}

SHIM(std_nomemory) { rt_fatal("std::_Nomemory: operator new failed"); }

/* std::ofstream: the game only constructs, opens, closes and destroys them
 * (operator<< is never called). Treat the object as opaque. */
SHIM(ofstream_ctor) { RET(c->r[1], 0); }
SHIM(ofstream_ctor_open) { RET(c->r[1], 3); }
SHIM(ofstream_open) { RET(0, 3); }
SHIM(ofstream_close) { RET(0, 0); }
SHIM(ofstream_dtor) { RET(c->r[1], 0); }
SHIM(ofstream_write) { RETC(ARG(0)); }

/* ====================================================================== */
/* kernel32                                                               */
/* ====================================================================== */

static void ini_path(char *out, size_t n) {
    snprintf(out, n, "%s/win.ini", g_save_dir);
}

static int ini_get(const char *app, const char *key, char *out, size_t cap) {
    char path[1024];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int in = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) { *e = 0; in = !strcasecmp(p + 1, app); }
            continue;
        }
        if (!in) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = p, *ke = eq;
        while (ke > k && isspace((unsigned char)ke[-1])) *--ke = 0;
        if (strcasecmp(k, key)) continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && isspace((unsigned char)ve[-1])) *--ve = 0;
        snprintf(out, cap, "%s", v);
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

static void ini_set(const char *app, const char *key, const char *val) {
    char path[1024];
    ini_path(path, sizeof path);
    mkdirs_for(path);
    FILE *in = fopen(path, "r");
    char *text = NULL;
    size_t len = 0;
    if (in) {
        fseek(in, 0, SEEK_END);
        len = (size_t)ftell(in);
        fseek(in, 0, SEEK_SET);
        text = malloc(len + 1);
        if (text && len) len = fread(text, 1, len, in);
        else len = 0;
        if (text) text[len] = 0;
        fclose(in);
    }
    FILE *out = fopen(path, "w");
    if (!out) { free(text); return; }
    int in_sec = 0, done = 0;
    if (text) {
        char *save = NULL;
        for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '[') {
                if (in_sec && !done) { fprintf(out, "%s=%s\n", key, val); done = 1; }
                char *e = strchr(p, ']');
                if (e) { *e = 0; in_sec = !strcasecmp(p + 1, app); }
                fprintf(out, "[%s]\n", p + 1);
                continue;
            }
            if (in_sec) {
                char *eq = strchr(p, '=');
                if (eq) {
                    char kbuf[256];
                    size_t kl = (size_t)(eq - p);
                    if (kl >= sizeof kbuf) kl = sizeof kbuf - 1;
                    memcpy(kbuf, p, kl);
                    kbuf[kl] = 0;
                    char *ke = kbuf + strlen(kbuf);
                    while (ke > kbuf && isspace((unsigned char)ke[-1])) *--ke = 0;
                    if (!strcasecmp(kbuf, key)) {
                        if (!done) { fprintf(out, "%s=%s\n", key, val); done = 1; }
                        continue;
                    }
                }
            }
            fprintf(out, "%s\n", line);
        }
    }
    if (!done) {
        if (!in_sec && !text) fprintf(out, "[%s]\n", app);
        else if (!in_sec) fprintf(out, "\n[%s]\n", app);
        fprintf(out, "%s=%s\n", key, val);
    }
    fclose(out);
    free(text);
}

SHIM(GetProfileStringA) {
    char v[512];
    const char *app = GSTR(ARG(0)), *key = GSTR(ARG(1)), *def = GSTR(ARG(2));
    uint32_t buf = ARG(3), cap = ARG(4);
    if (!ini_get(app, key, v, sizeof v)) snprintf(v, sizeof v, "%s", def ? def : "");
    if (buf && cap) snprintf(GSTR(buf), cap, "%s", v);
    RET((uint32_t)strlen(v), 5);
}

SHIM(GetProfileIntA) {
    char v[512];
    const char *app = GSTR(ARG(0)), *key = GSTR(ARG(1));
    if (!ini_get(app, key, v, sizeof v)) RET((uint32_t)(int32_t)ARG(2), 3);
    RET((uint32_t)(int32_t)strtol(v, NULL, 0), 3);
}

SHIM(WriteProfileStringA) {
    const char *app = GSTR(ARG(0)), *key = GSTR(ARG(1)), *val = GSTR(ARG(2));
    if (!app || !key) RET(0, 3);
    if (!val) { /* delete: not implemented, report it */ LOG("WriteProfileStringA: delete %s/%s ignored", app, key); RET(1, 3); }
    ini_set(app, key, val);
    RET(1, 3);
}

SHIM(GlobalAlloc) {
    uint32_t flags = ARG(0), size = ARG(1);
    if (flags & 0x0040u) { /* GMEM_ZEROINIT */
        uint32_t p = gmalloc(size ? size : 1);
        if (p) memset(MEM + p, 0, size);
        RET(p, 2);
    }
    RET(gmalloc(size ? size : 1), 2);
}

SHIM(GlobalLock) { RET(ARG(0), 1); }
SHIM(GlobalUnlock) { RET(0, 1); }
SHIM(GlobalFree) { gfree(ARG(0)); RET(0, 1); }

SHIM(OpenProcess) {
    LOG("OpenProcess(access %08x, pid %u) -> denied", ARG(0), ARG(1));
    rt_set_last_error(c, 5 /* ERROR_ACCESS_DENIED */);
    RET(0, 3);
}

SHIM(SwitchToThread) { sched_yield(); RET(1, 0); }

SHIM(FindFirstFileExA) {
    /* fwd to the shared FindFirstFileA shim (name, data): same layout for the
     * basic info level, which is all the game asks for. */
    uint32_t a[2] = {ARG(0), ARG(2)};
    uint32_t thunk = rt_import("kernel32.dll", "FindFirstFileA");
    uint32_t r = rt_invoke(c, thunk, 2, a);
    RET(r, 6);
}

SHIM(GetOverlappedResult) {
    if (ARG(2)) M32(ARG(2)) = 0;
    RET(1, 4);
}

SHIM(CancelIo) { RET(1, 1); }

SHIM(lstrcmpi) { RET((uint32_t)(int32_t)strcasecmp(GSTR(ARG(0)), GSTR(ARG(1))), 2); }

/* ====================================================================== */
/* user32: windows, dialogs, menus, clipboard                             */
/* ====================================================================== */

#define FAKE_HWND 0x00010010u

SHIM(GetKeyboardState) {
    if (ARG(0)) memset(MEM + ARG(0), 0, 256);
    RET(1, 1);
}

SHIM(AdjustWindowRectEx) { RET(1, 4); }

SHIM(GetWindowPlacement) {
    if (ARG(1)) memset(MEM + ARG(1), 0, 44);
    RET(1, 2);
}

SHIM(SetWindowPlacement) { RET(1, 2); }

SHIM(OpenClipboard) { RET(1, 1); }
SHIM(EmptyClipboard) { RET(1, 0); }
SHIM(SetClipboardData) { RET(ARG(1), 2); }
SHIM(CloseClipboard) { RET(1, 0); }

SHIM(RegisterClipboardFormatA) {
    static uint32_t next = 0xC000;
    RET(next++, 1);
}

SHIM(GetDesktopWindow) { RET(FAKE_HWND, 0); }
SHIM(ClipCursor) { RET(1, 1); }
SHIM(SetFocus) { RET(0, 1); }
SHIM(GetFocus) { RET(0, 0); }
SHIM(IsWindow) { RET(ARG(0) ? 1 : 0, 1); }
SHIM(EnableWindow) { RET(1, 2); }
SHIM(GetParent) { RET(0, 1); }

SHIM(CreateDialogParamA) {
    static int warned;
    if (!warned++) LOG("CreateDialogParamA(%s): dialogs are not implemented", GSTR(ARG(1)));
    RET(0, 5);
}

SHIM(LoadImageA) { RET(0, 6); }
SHIM(GetDlgCtrlID) { RET(0, 1); }
SHIM(GetNextDlgGroupItem) { RET(0, 3); }
SHIM(IsDlgButtonChecked) { RET(0, 2); }
SHIM(GetDlgItemInt) { RET(0, 4); }

SHIM(GetDlgItemTextA) {
    if (ARG(2) && ARG(3)) M8(ARG(2)) = 0;
    RET(0, 4);
}

SHIM(SetDlgItemInt) { RET(1, 4); }
SHIM(SetDlgItemTextA) { RET(1, 3); }
SHIM(IsDialogMessage) { RET(0, 2); }
SHIM(GetMenuState) { RET(0, 3); }
SHIM(CheckMenuItem) { RET(0, 3); }
SHIM(EnableMenuItem) { RET(0, 3); }
SHIM(GetMenu) { RET(0, 1); }
SHIM(SetMenuItemInfoA) { RET(1, 4); }
SHIM(DrawMenuBar) { RET(1, 1); }
SHIM(ValidateRect) { RET(1, 2); }

SHIM(PtInRect) {
    int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    int32_t l = (int32_t)M32(ARG(0)), t = (int32_t)M32(ARG(0) + 4);
    int32_t r = (int32_t)M32(ARG(0) + 8), b = (int32_t)M32(ARG(0) + 12);
    RET(x >= l && x < r && y >= t && y < b, 3);
}

/* ====================================================================== */
/* gdi32 / comctl32 / comdlg32 / shell32 / oleaut32 / psapi / advapi32    */
/* ====================================================================== */

SHIM(GetTextExtentPoint32A) {
    /* No text rendering in the shim layer: report a plausible monospace box. */
    uint32_t n = ARG(2);
    if (ARG(3)) {
        M32(ARG(3)) = n * 7;
        M32(ARG(3) + 4) = 13;
    }
    RET(1, 4);
}

SHIM(CreateBitmap) {
    static uint32_t next = 0x00020001;
    RET(next++, 5);
}

SHIM(ImageList_Create) {
    static uint32_t next = 0x00030001;
    RET(next++, 5);
}

SHIM(ImageList_Add) { RET(0, 3); }
SHIM(ImageList_Remove) { RET(1, 2); }
SHIM(ImageList_Destroy) { RET(1, 1); }
SHIM(InitCommonControlsEx) { RET(1, 1); }

SHIM(ChooseFontA) { RET(0, 1); }
SHIM(ChooseColorA) { RET(0, 1); }
SHIM(GetOpenFileNameA) { RET(0, 1); }
SHIM(GetSaveFileNameA) { RET(0, 1); }
SHIM(CommDlgExtendedError) { RET(0, 0); }

SHIM(SHGetMalloc) {
    if (ARG(0)) M32(ARG(0)) = 0;
    RET(0x80004005u, 1); /* E_FAIL */
}

SHIM(SHBrowseForFolder) { RET(0, 1); }
SHIM(SHBrowseForFolderA) { RET(0, 1); }
SHIM(SHGetPathFromIDList) { RET(0, 2); }
SHIM(SHGetPathFromIDListA) { RET(0, 2); }

SHIM(SysStringLen) { RET(ARG(0) ? M32(ARG(0) - 4) : 0, 1); }

SHIM(VariantInit) {
    if (ARG(0)) memset(MEM + ARG(0), 0, 16);
    RET(0, 1);
}

SHIM(VariantClear) { RET(0, 1); }

SHIM(GetProcessMemoryInfo) {
    if (ARG(1)) memset(MEM + ARG(1), 0, ARG(2));
    RET(1, 3);
}

SHIM(RegCreateKeyA) {
    static uint32_t next = 0xC0DE0001;
    if (ARG(2)) M32(ARG(2)) = next++;
    RET(0, 3);
}

SHIM(gethostbyname) {
    LOG("gethostbyname(%s): no network in this port", GSTR(ARG(0)));
    RET(0, 1);
}

/* ====================================================================== */
/* registration                                                           */
/* ====================================================================== */

static const ShimDef defs[] = {
#define S(n) {"msvcr71.dll", #n, shim_##n},
    S(fopen) S(fclose) S(fread) S(fwrite) S(fseek) S(ftell) S(fflush) S(fputs)
    S(printf) S(fprintf) S(sprintf) S(vsprintf) S(tmpfile) S(_fstat) S(_flushall)
    S(_msize) S(_aligned_malloc) S(_aligned_free)
    S(strncpy) S(strncat) S(strtok) S(strcspn) S(strtol) S(strtoul)
    S(_strcmpi) S(_strnicmp) S(_strlwr) S(_strupr) S(_mbsdup) S(_ismbblead) S(bsearch)
    S(_mkdir) S(_chdir) S(_getcwd)
    S(__getmainargs) S(__set_app_type) S(__setusermatherr) S(_callnewh)
    S(_security_error_handler)
    {"msvcr71.dll", "__security_error_handler", shim__security_error_handler}, S(__p__fmode) S(__p__commode)
    S(_cexit) S(_c_exit) S(_exit) S(_except_handler3)
#undef S
    {"msvcr71.dll", "time", shim_time_},
    {"msvcr71.dll", "clock", shim_clock_},
    {"msvcr71.dll", "??0exception@@QAE@XZ", shim_exc_ctor},
    {"msvcr71.dll", "??0exception@@QAE@ABV0@@Z", shim_exc_ctor_copy},
    {"msvcr71.dll", "??1exception@@UAE@XZ", shim_exc_dtor_member},
    {"msvcr71.dll", "??_V@YAXPAX@Z", shim_operator_delete_array},

#define C(n) {"msvcp71.dll", #n, shim_##n},
    C(str_ctor_cstr) C(str_ctor_copy) C(str_dtor) C(std_nomemory)
    C(ofstream_ctor) C(ofstream_ctor_open) C(ofstream_open) C(ofstream_close)
    C(ofstream_dtor) C(ofstream_write)
#undef C
    {"msvcp71.dll", "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z", shim_str_ctor_cstr},
    {"msvcp71.dll", "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z", shim_str_ctor_copy},
    {"msvcp71.dll", "??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ", shim_str_dtor},
    {"msvcp71.dll", "?_Nomemory@std@@YAXXZ", shim_std_nomemory},
    {"msvcp71.dll", "??0?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAE@XZ", shim_ofstream_ctor},
    {"msvcp71.dll", "??0?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAE@PBDHH@Z", shim_ofstream_ctor_open},
    {"msvcp71.dll", "?open@?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAEXPBDHH@Z", shim_ofstream_open},
    {"msvcp71.dll", "?close@?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAEXXZ", shim_ofstream_close},
    {"msvcp71.dll", "??_D?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAEXXZ", shim_ofstream_dtor},
    {"msvcp71.dll", "??$?6U?$char_traits@D@std@@@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@0@AAV10@PBD@Z", shim_ofstream_write},

#define K(n) {"kernel32.dll", #n, shim_##n},
    K(GetProfileStringA) K(GetProfileIntA) K(WriteProfileStringA)
    K(GlobalAlloc) K(GlobalLock) K(GlobalUnlock) K(GlobalFree)
    K(OpenProcess) K(SwitchToThread) K(FindFirstFileExA)
    K(GetOverlappedResult) K(CancelIo) K(lstrcmpi)
#undef K

#define U(n) {"user32.dll", #n, shim_##n},
    U(GetKeyboardState) U(AdjustWindowRectEx) U(GetWindowPlacement)
    U(SetWindowPlacement) U(OpenClipboard) U(EmptyClipboard) U(SetClipboardData)
    U(CloseClipboard) U(GetDesktopWindow) U(ClipCursor) U(SetFocus)
    U(CreateDialogParamA) U(LoadImageA) U(GetDlgCtrlID) U(GetNextDlgGroupItem)
    U(IsDlgButtonChecked) U(GetDlgItemInt) U(GetDlgItemTextA) U(SetDlgItemInt)
    U(SetDlgItemTextA) U(IsWindow) U(EnableWindow) U(RegisterClipboardFormatA)
    U(GetParent) U(IsDialogMessage) U(GetFocus) U(GetMenuState) U(CheckMenuItem)
    U(EnableMenuItem) U(GetMenu) U(SetMenuItemInfoA) U(DrawMenuBar)
    U(ValidateRect) U(PtInRect)
#undef U

#define G(n) {"gdi32.dll", #n, shim_##n},
    G(GetTextExtentPoint32A) G(CreateBitmap)
#undef G

#define CT(n) {"comctl32.dll", #n, shim_##n},
    CT(ImageList_Create) CT(ImageList_Add) CT(ImageList_Remove)
    CT(ImageList_Destroy) CT(InitCommonControlsEx)
#undef CT

#define CD(n) {"comdlg32.dll", #n, shim_##n},
    CD(ChooseFontA) CD(ChooseColorA) CD(GetOpenFileNameA) CD(GetSaveFileNameA)
    CD(CommDlgExtendedError)
#undef CD

#define SH(n) {"shell32.dll", #n, shim_##n},
    SH(SHGetMalloc) SH(SHBrowseForFolder) SH(SHBrowseForFolderA)
    SH(SHGetPathFromIDList) SH(SHGetPathFromIDListA)
#undef SH

#define O(n) {"oleaut32.dll", #n, shim_##n},
    O(SysStringLen) O(VariantInit) O(VariantClear)
#undef O

    {"psapi.dll", "GetProcessMemoryInfo", shim_GetProcessMemoryInfo},
    {"advapi32.dll", "RegCreateKeyA", shim_RegCreateKeyA},
    {"ws2_32.dll", "gethostbyname", shim_gethostbyname},
    {NULL, NULL, NULL}
};

void usm_shims_init(void) {
    /* Ours first: the alias below registers msvcrt's table, and lookup takes
     * the first match, so these overrides win for msvcr71.dll. */
    rt_register_shims(defs);
    msvcrt_register_as_msvcr71();

    g_acmdln = gmalloc(4);
    uint32_t cmd = gstrdup("USM.exe");
    M32(g_acmdln) = cmd;
    rt_register_data("msvcr71.dll", "_acmdln", g_acmdln);
    g_adjust_fdiv = gmalloc(4);
    M32(g_adjust_fdiv) = 0;
    rt_register_data("msvcr71.dll", "_adjust_fdiv", g_adjust_fdiv);
    g_iob71 = gmalloc(32 * 20);
    memset(MEM + g_iob71, 0, 32 * 20);
    rt_register_data("msvcr71.dll", "_iob", g_iob71);
    g_fmode71 = gmalloc(4);
    g_commode71 = gmalloc(4);

    g_exc_vtbl = gmalloc(8);
    M32(g_exc_vtbl) = rt_thunk(shim_exc_dtor, "exception::~exception");
    M32(g_exc_vtbl + 4) = rt_thunk(shim_exc_what, "exception::what");
    g_exc_msg = gstrdup("Unknown exception");

    LOG("usm_shims: %d USM-specific imports registered (msvcr71/msvcp71, win32 gaps)",
        (int)(sizeof defs / sizeof defs[0] - 1));
}
