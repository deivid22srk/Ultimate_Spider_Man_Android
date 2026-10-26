/* Host entry: maps guest memory, loads LEGOBatman.exe's sections and runs
 * the recompiled entry point. The exe itself is read from the user's game
 * directory at runtime; nothing from it is embedded in the build. */
#define _GNU_SOURCE
#include "game.h"
#include "rt.h"
#include "rt_table.h"
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <SDL2/SDL.h>
#endif

uint8_t *MEM;
int g_log_level = 1;
char g_game_dir[1024] = ".";
char g_save_dir[1024] = "./save";
uint32_t g_image_size;
uint32_t g_entry;

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
#ifdef __ANDROID__
    {   /* show it on screen too: the log is easy to miss on a phone */
        char msg[2048];
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_TITLE, msg, NULL);
    }
#endif
    abort();
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Loads a PE image at `want_base` (applying relocations if it differs from
 * the preferred base), resolves its imports and registers its exports.
 * Returns the entry point. */
static uint32_t load_pe(const char *path, uint32_t want_base, const char *modname) {
    FILE *f = NULL;
#ifdef GAME_SKYRIM
    /* The Steam TESV.exe is SteamStub-encrypted; prefer a Steamless copy. */
    if (!strcmp(modname, GAME_EXE)) {
        static const char *const k_unpacked[] = {"TESV.exe.unpacked.exe", "TESV_unpacked.exe", NULL};
        for (int i = 0; !f && k_unpacked[i]; i++)
            if ((f = fopen(k_unpacked[i], "rb"))) LOG("using %s", k_unpacked[i]);
    }
#endif
    if (!f) f = fopen(path, "rb");
    if (!f) {  /* DirectX DLLs may sit in redist/, any case */
        static const char *const k_dirs[] = {"redist/", "Redist/", "REDIST/", NULL};
        char alt[512];
        for (int i = 0; !f && k_dirs[i]; i++) {
            snprintf(alt, sizeof alt, "%s%s", k_dirs[i], path);
            f = fopen(alt, "rb");
            if (!f && !strcasecmp(path, "d3dx9_43.dll")) {
                snprintf(alt, sizeof alt, "%sD3DX9_43.dll", k_dirs[i]);
                f = fopen(alt, "rb");
            }
        }
    }
    if (!f) rt_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) rt_fatal("short read %s", path);
    fclose(f);
    uint32_t pe = rd32(buf + 0x3C);
    if (rd32(buf + pe) != 0x4550) rt_fatal("not a PE file");
    uint16_t nsec = rd16(buf + pe + 6);
    uint16_t optsz = rd16(buf + pe + 20);
    const uint8_t *opt = buf + pe + 24;
    uint32_t pref = rd32(opt + 28);
    uint32_t base = want_base ? want_base : pref;
    uint32_t entry = rd32(opt + 16) ? base + rd32(opt + 16) : 0;
    uint32_t image_size = rd32(opt + 56);
    uint32_t hdrsz = rd32(opt + 60);
    memcpy(MEM + base, buf, hdrsz);
    const uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++)
        if (!memcmp(opt + optsz + 40 * i, ".bind", 6))
            rt_fatal("%s is the Steam-protected exe (SteamStub). Use the unpacked TESV.exe from the port "
                     "folder instead (or run Steamless on it).", path);
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsz = rd32(sec + 8), va = rd32(sec + 12), rawsz = rd32(sec + 16), raw = rd32(sec + 20);
        uint32_t cp = rawsz < vsz ? rawsz : vsz;
        if (!vsz) cp = rawsz;
        memcpy(MEM + base + va, buf + raw, cp);
    }
    /* relocations */
    if (base != pref) {
        uint32_t delta = base - pref;
        uint32_t rva = rd32(opt + 136), size = rd32(opt + 140);
        for (uint32_t p = base + rva; p < base + rva + size;) {
            uint32_t page = M32(p), bsz = M32(p + 4);
            if (!bsz) break;
            for (uint32_t e = p + 8; e < p + bsz; e += 2) {
                uint16_t ent = M16(e);
                if ((ent >> 12) == 3) M32(base + page + (ent & 0xFFF)) += delta;
            }
            p += bsz;
        }
    }
    /* exports */
    uint32_t exp_rva = rd32(opt + 96);
    if (exp_rva) {
        uint32_t ed = base + exp_rva;
        uint32_t nnames = M32(ed + 24), funcs = base + M32(ed + 28), names = base + M32(ed + 32),
                 ords = base + M32(ed + 36), ordbase = M32(ed + 16), nfuncs = M32(ed + 20);
        for (uint32_t i = 0; i < nnames; i++) {
            const char *nm = (const char *)(MEM + base + M32(names + 4 * i));
            uint16_t o = M16(ords + 2 * i);
            rt_register_data(modname, nm, base + M32(funcs + 4 * o));
        }
        for (uint32_t i = 0; i < nfuncs; i++) {
            char nm[16];
            snprintf(nm, sizeof nm, "#%u", ordbase + i);
            if (M32(funcs + 4 * i)) rt_register_data(modname, nm, base + M32(funcs + 4 * i));
        }
    }
    /* imports */
    uint32_t imp_rva = rd32(opt + 104);
    for (uint32_t d = base + imp_rva; imp_rva && M32(d + 12); d += 20) {
        const char *dll = (const char *)(MEM + base + M32(d + 12));
        uint32_t ilt = M32(d) ? base + M32(d) : base + M32(d + 16);
        uint32_t iat = base + M32(d + 16);
        for (; M32(ilt); ilt += 4, iat += 4) {
            uint32_t e = M32(ilt);
            char name[128];
            if (e & 0x80000000u) snprintf(name, sizeof name, "#%u", e & 0xFFFF);
            else snprintf(name, sizeof name, "%s", (const char *)(MEM + base + e + 2));
            M32(iat) = rt_import(dll, name);
        }
    }
    uint32_t tls_rva = rd32(opt + 168), tls_size = rd32(opt + 172);
    free(buf);
    extern void rt_module_add(const char *, uint32_t);
    rt_module_add(modname, base);
    LOG("loaded %s at %08x: entry %08x image %08x", path, base, entry, image_size);
    if (!strcmp(modname, GAME_EXE)) {
        g_entry = entry;
        g_image_size = image_size;
        if (tls_rva && tls_size) {
            extern void rt_set_static_tls(uint32_t, uint32_t, uint32_t, uint32_t);
            uint32_t d = base + tls_rva;
            rt_set_static_tls(M32(d), M32(d + 4), M32(d + 8), M32(d + 16));
            if (M32(d + 12) && M32(M32(d + 12))) LOG("warning: TLS callbacks not run");
        }
    }
    return entry;
}

/* Decodes a movie with the recompiled binkw32 only and writes a few frames
 * as PPM files: isolates Bink/MMX lifting problems from the game. */
static int bink_test(CPU *c, const char *movie, const char *outdir) {
    uint32_t e = load_pe("binkw32.dll", 0x09000000u, "binkw32.dll");
    uint32_t a3[3] = {0x09000000u, 1, 0};
    rt_invoke(c, e, 3, a3);
    uint32_t name = gstrdup(movie);
    uint32_t a2[2] = {name, 0};
    uint32_t bink = rt_invoke(c, rt_import("binkw32.dll", "_BinkOpen@8"), 2, a2);
    if (!bink) { LOG("BinkOpen failed"); return 1; }
    uint32_t w = M32(bink), h = M32(bink + 4), frames = M32(bink + 8);
    LOG("bink: %ux%u, %u frames", w, h, frames);
    uint32_t buf = gmalloc(w * h * 4);
    for (uint32_t f = 1; f <= frames && f <= 90; f++) {
        uint32_t a1[1] = {bink};
        rt_invoke(c, rt_import("binkw32.dll", "_BinkDoFrame@4"), 1, a1);
        uint32_t a7[7] = {bink, buf, w * 4, h, 0, 0, 3 /* BINKSURFACE32 */};
        rt_invoke(c, rt_import("binkw32.dll", "_BinkCopyToBuffer@28"), 7, a7);
        if (f == 1 || f == 30 || f == 90) {
            char path[1024];
            snprintf(path, sizeof path, "%s/frame%03u.ppm", outdir, f);
            FILE *o = fopen(path, "wb");
            fprintf(o, "P6 %u %u 255\n", w, h);
            for (uint32_t i = 0; i < w * h; i++) {
                uint8_t *px = MEM + buf + 4 * i;
                uint8_t rgb[3] = {px[2], px[1], px[0]};
                fwrite(rgb, 1, 3, o);
            }
            fclose(o);
            LOG("wrote %s", path);
        }
        rt_invoke(c, rt_import("binkw32.dll", "_BinkNextFrame@4"), 1, a1);
    }
    return 0;
}

#if defined(__GLIBC__)
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
/* Crash report: host backtrace plus the faulting address. */
static void on_crash(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    fprintf(stderr, "CRASH: signal %d at address %p\n", sig, si->si_addr);
    fprintf(stderr, "  guest mem base %p (fault guest addr %08x)\n", (void *)MEM,
            (uint32_t)((uintptr_t)si->si_addr - (uintptr_t)MEM));
    extern void rt_dump_guest(void);
    rt_dump_guest();
#if defined(GAME_USM)
    { extern void usm_dump_state(void); usm_dump_state(); }
#endif
    void *bt[64];
    int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}
static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
#elif defined(__ANDROID__)
#include <signal.h>
#include <dlfcn.h>
#include <unwind.h>
#include <ucontext.h>
/* Crash report for log.txt: fault address, registers and a backtrace as
 * library+offset (symbolise with the unstripped libmain.so from the build).
 * The previous handler (debuggerd) runs afterwards so a tombstone is kept. */
static struct sigaction g_old_sa[32];
static void print_pc(int i, uintptr_t pc) {
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        fprintf(stderr, "  #%02d %s+0x%lx", i, b ? b + 1 : di.dli_fname, (unsigned long)(pc - (uintptr_t)di.dli_fbase));
        if (di.dli_sname) fprintf(stderr, " (%s+0x%lx)", di.dli_sname, (unsigned long)(pc - (uintptr_t)di.dli_saddr));
        fputc('\n', stderr);
    } else fprintf(stderr, "  #%02d 0x%lx\n", i, (unsigned long)pc);
}
typedef struct { int n; } BtState;
static _Unwind_Reason_Code bt_step(struct _Unwind_Context *ctx, void *arg) {
    BtState *st = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) print_pc(st->n, pc);
    return ++st->n >= 48 ? _URC_END_OF_STACK : _URC_NO_REASON;
}
static void on_crash(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    fprintf(stderr, "CRASH: signal %d code %d at address %p (thread %d)\n", sig, si->si_code, si->si_addr, gettid());
#if defined(__aarch64__)
    uintptr_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    fprintf(stderr, "  sp %llx\n", (unsigned long long)uc->uc_mcontext.sp);
    for (int r = 0; r < 31; r += 4)
        fprintf(stderr, "  x%-2d %016llx %016llx %016llx %016llx\n", r,
                (unsigned long long)uc->uc_mcontext.regs[r], (unsigned long long)uc->uc_mcontext.regs[r + 1 < 31 ? r + 1 : 30],
                (unsigned long long)uc->uc_mcontext.regs[r + 2 < 31 ? r + 2 : 30], (unsigned long long)uc->uc_mcontext.regs[r + 3 < 31 ? r + 3 : 30]);
    fprintf(stderr, "  guest mem base %p\n", (void *)MEM);
    extern void rt_dump_guest(void);
    rt_dump_guest();
    { extern void rt_wd_dump(void); rt_wd_dump(); }
#if defined(GAME_USM)
    { extern void usm_dump_state(void); usm_dump_state(); }
#endif
    fprintf(stderr, "backtrace:\n");
    print_pc(-1, pc);
    print_pc(-2, lr);
#endif
    BtState st = {0};
    _Unwind_Backtrace(bt_step, &st);
    fflush(stderr);
    sigaction(sig, &g_old_sa[sig], NULL);  /* let debuggerd write its tombstone */
}
void rt_altstack(void) {
    stack_t ss;
    ss.ss_size = 64 * 1024;
    ss.ss_sp = malloc(ss.ss_size);
    ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}
static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    rt_altstack();
    int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, &g_old_sa[sigs[i]]);
}
#else
static void install_crash_handler(void) {}
#endif
#if !defined(__ANDROID__)
void rt_altstack(void) {}
#endif

#ifdef __ANDROID__
#include <SDL2/SDL_main.h>
#include <SDL2/SDL_system.h>
#include <sys/stat.h>
#endif

/* Fails with a message naming exactly what is missing from the game folder. */
static int file_present(const char *name) {
    static const char *const k_dirs[] = {"", "redist/", "Redist/", "REDIST/", NULL};
    char lower[256];
    snprintf(lower, sizeof lower, "%s", name);
    for (char *q = lower; *q; q++) *q = (char)tolower((unsigned char)*q);
    for (int i = 0; k_dirs[i]; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s%s", k_dirs[i], name);
        if (!access(p, R_OK)) return 1;
        snprintf(p, sizeof p, "%s%s", k_dirs[i], lower);
        if (!access(p, R_OK)) return 1;
        /* any case: scan the folder */
        DIR *d = opendir(k_dirs[i][0] ? k_dirs[i] : ".");
        if (!d) continue;
        struct dirent *de;
        int hit = 0;
        while ((de = readdir(d)) && !hit) hit = !strcasecmp(de->d_name, name);
        closedir(d);
        if (hit) return 1;
    }
    return 0;
}

static void check_game_dir(void) {
    if (chdir(g_game_dir) != 0) {
        int e = errno;
        struct stat st;
        const char *why = stat(g_game_dir, &st) != 0 ? "the folder does not exist"
                        : !S_ISDIR(st.st_mode) ? "it is a file, not a folder"
                        : "the app is not allowed to open it";
#ifdef __ANDROID__
        if (e == EACCES)
            rt_fatal("cannot open the game folder %s: permission denied. Android gives the folder to "
                     "whoever created it first; if it was copied in before the app had run, the app "
                     "cannot read it. Move the game files out, delete the " GAME_PKG_DIR " folder "
                     "in Android/data, start the app once (it recreates the folder), then copy the "
                     "files into files/game again.", g_game_dir);
#endif
        rt_fatal("cannot open the game folder %s: %s (%s). Copy the contents of the "
                 GAME_TITLE " install folder into it.", g_game_dir, why, strerror(e));
    }
#ifdef GAME_USM
    /* Ultimate Spider-Man: the exe, Bink, and the asset folders the game opens
     * (data/packs holds the 1.6 GB of level/character packs). */
    static const char *const k_need[] = {GAME_EXE, "binkw32.dll", "data", "data/packs", NULL};
#elif defined(GAME_SKYRIM)
    static const char *const k_need[] = {GAME_EXE, DLL_D3DX9, DLL_EXTRA, "X3DAudio1_7.dll", "Data", NULL};
#else
    static const char *const k_need[] = {GAME_EXE, DLL_D3DX9, DLL_EXTRA, NULL};
#endif
    char missing[1024] = "";
    for (int i = 0; k_need[i]; i++)
        if (k_need[i][0] && !file_present(k_need[i])) {
            if (missing[0]) strncat(missing, ", ", sizeof missing - strlen(missing) - 1);
            strncat(missing, k_need[i], sizeof missing - strlen(missing) - 1);
        }
#ifdef GAME_USM
    int dats = 1; /* USM keeps its assets in data/ (packs, Sound, movies) */
#else
    int dats = 0;
    DIR *d = opendir(".");
    for (struct dirent *de; d && (de = readdir(d));) {
        size_t n = strlen(de->d_name);
        if (n >= 8 && !strncasecmp(de->d_name, "GAME", 4) && !strcasecmp(de->d_name + n - 4, ".DAT")) dats++;
    }
    if (d) closedir(d);
#endif
#ifdef GAME_SKYRIM
    {   /* 0.2: report whether the data folder is readable (0.1 found no archives on a phone) */
        static const char *const k_probe[] = {"Data", "Data/Skyrim.esm", "Data/Skyrim - Misc.bsa", NULL};
        for (int i = 0; k_probe[i]; i++) {
            struct stat st;
            int s = stat(k_probe[i], &st), e1 = errno;
            int fd = open(k_probe[i], O_RDONLY), e2 = errno;
            LOG("probe %s: stat %s%s, open %s%s, size %lld", k_probe[i], s ? "FAIL " : "ok", s ? strerror(e1) : "",
                fd < 0 ? "FAIL " : "ok", fd < 0 ? strerror(e2) : "", s ? -1LL : (long long)st.st_size);
            if (fd >= 0) close(fd);
        }
        DIR *dd = opendir("Data");
        int n = 0;
        for (struct dirent *de; dd && (de = readdir(dd));) if (n++ < 40) LOG("  Data/%s", de->d_name);
        LOG("Data listing: %s, %d entries", dd ? "ok" : strerror(errno), n);
        if (dd) closedir(dd);
        int fd = open("Data/Skyrim.esm", O_RDONLY);
        if (fd < 0)
            rt_fatal("cannot read %s/Data/Skyrim.esm: %s. Keep the game files outside Android/data "
                     "(for example Internal storage/Skyrim), allow file access in the launcher and choose that folder.",
                     g_game_dir, strerror(errno));
        close(fd);
    }
    dats = 1;  /* Skyrim keeps its data in Data (esm + bsa files) */
#endif
    if (!dats) {
        if (missing[0]) strncat(missing, ", ", sizeof missing - strlen(missing) - 1);
        strncat(missing, "GAME*.DAT", sizeof missing - strlen(missing) - 1);
    }
    if (missing[0])
        rt_fatal("missing from %s: %s (the DLLs may also sit in a redist subfolder)", g_game_dir, missing);
}

/* Set by the launcher through argv (see UsmActivity.getArguments): the folder of
 * a user supplied libvulkan.so, empty for the system driver, and the APK's
 * native library dir, where libadrenotools' hook libraries live. */
char g_driver_dir[1024];
char g_native_lib_dir[1024];

int main(int argc, char **argv) {
    install_crash_handler();
    const char *env = getenv("LB_LOG");
    if (env) g_log_level = atoi(env);
#ifdef __ANDROID__
    /* With the launcher (argv[1] = the Skyrim folder) everything the port
     * writes lives in <game>/port: log.txt, save/ (Documents, INIs, saves).
     * Without it, the app's external files folder is used as before. */
    {
        char base[1024] = "";
        if (argc > 2) snprintf(g_driver_dir, sizeof g_driver_dir, "%s", argv[2]);
        if (argc > 3) snprintf(g_native_lib_dir, sizeof g_native_lib_dir, "%s", argv[3]);
        if (argc > 1) {
            snprintf(g_game_dir, sizeof g_game_dir, "%s", argv[1]);
            snprintf(base, sizeof base, "%s/port", argv[1]);
            snprintf(g_save_dir, sizeof g_save_dir, "%s/save", base);
        } else {
            const char *ext = SDL_AndroidGetExternalStoragePath();
            if (ext) {
                snprintf(base, sizeof base, "%s", ext);
                snprintf(g_game_dir, sizeof g_game_dir, "%s/game", ext);
                snprintf(g_save_dir, sizeof g_save_dir, "%s/save", ext);
                mkdir(g_game_dir, 0777);
            }
        }
        if (base[0]) {
            char logp[1100];
            mkdir(base, 0777);
            mkdir(g_save_dir, 0777);
            snprintf(logp, sizeof logp, "%s/log.txt", base);
            if (!env) {
                /* log level from save/touch.cfg (4th field); default 1 */
                char cfgp[1100];
                snprintf(cfgp, sizeof cfgp, "%s/touch.cfg", g_save_dir);
                int lvl = 1;
                FILE *cf = fopen(cfgp, "r");
                if (cf) {
                    int t, nn, k, l;
                    if (fscanf(cf, "%d %d %d %d", &t, &nn, &k, &l) == 4) lvl = l;
                    fclose(cf);
                }
                g_log_level = lvl;
            }
            if (g_log_level > 0 && freopen(logp, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
            else if (g_log_level == 0) {
                /* logs fully off: send stderr to nowhere (a fresh log.txt is still removed) */
                unlink(logp);
                freopen("/dev/null", "w", stderr);
            }
        }

        argc = 1;  /* consumed */
    }
#endif
    if (argc > 1) snprintf(g_game_dir, sizeof g_game_dir, "%s", argv[1]);
    if (argc > 2) snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[2]);
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) rt_fatal("cannot reserve guest address space");
    /* keep the null page inaccessible to catch null derefs */
    mprotect(MEM, 0x10000, PROT_NONE);
    rt_init_shims();
    { extern void rt_watchdog_start(void); rt_watchdog_start(); }
    /* dxvk writes its pipeline state cache to the working directory; keep the game folder clean */
    if (!getenv("DXVK_STATE_CACHE_PATH")) setenv("DXVK_STATE_CACHE_PATH", g_save_dir, 1);
    /* 0.2.6: no compile status HUD (as in 0.1.5); DXVK_HUD=portstatus enables it. */
    check_game_dir();
#ifdef GAME_USM
    {   /* CI "app-only" builds carry ci/gen_stub instead of recompiled code:
         * say so plainly instead of dying on the first guest call. */
        extern unsigned rt_module_count(unsigned);
        if (rt_module_count(0) == 0)
            rt_fatal("this APK was built without the recompiled game code (app-only build). "
                     "Build gen/ from your own copy with tools/regen_gen.sh, then build the full "
                     "APK in a private fork of the repository (or locally) - see README, "
                     "\"Building the full APK\".");
    }
#endif
#ifdef GAME_MARVEL
    { extern void port_restore_defaults(void); port_restore_defaults(); }
#endif
    extern void platform_init(void);
    platform_init();
#ifdef GAME_SKYRIM
    {
        extern void skyrim_seed_prefs(int, int);
        extern void platform_window_size(int *, int *);
        int w = 1280, h = 720;
        platform_window_size(&w, &h);
        skyrim_seed_prefs(w, h);
    }
#endif
    CPU *c = rt_new_cpu();
    rt_thread_init(c);
    { extern void prof_start(void); prof_start(); }
    if (argc > 4 && !strcmp(argv[3], "--bink-test")) return bink_test(c, argv[4], argc > 5 ? argv[5] : ".");
    /* DLLs first so the exe's imports bind to their exports */
    extern const char *const g_dll_modules[];
    extern const uint32_t g_dll_bases[];
    uint32_t dll_entries[8];
    int ndll = 0;
    for (; g_dll_modules[ndll]; ndll++)
        dll_entries[ndll] = load_pe(g_dll_modules[ndll], g_dll_bases[ndll], g_dll_modules[ndll]);
    load_pe(GAME_EXE, 0, GAME_EXE);
    extern void touch_setup_guest(void);
    touch_setup_guest();
    for (int i = 0; i < ndll; i++) {
        uint32_t args[3] = {g_dll_bases[i], 1, 0};
        if (dll_entries[i] && !rt_invoke(c, dll_entries[i], 3, args))
            rt_fatal("DllMain of %s failed", g_dll_modules[i]);
    }
    /* push a return address that ends the process when main returns */
    c->r[4] -= 4;
    M32(c->r[4]) = 0;
    rt_call(c, g_entry);
    LOG("entry returned eax=%u", c->r[0]);
    return (int)c->r[0];
}
