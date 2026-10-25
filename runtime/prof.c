/* Sampling profiler: SIGPROF every 1 ms of process CPU time records the
 * interrupted PC; the watchdog prints the hottest functions every 30 s, so a
 * player's log.txt shows where the time goes. save/noprof.txt turns it off. */
#define _GNU_SOURCE
#include "rt.h"
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

#define PROF_N (1u << 20)
static uintptr_t *g_samp;
static volatile uint32_t g_samp_n;
static uint32_t g_samp_done;
static int g_prof_on;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
#if defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = 0;
#endif
    uint32_t i = __atomic_fetch_add(&g_samp_n, 1, __ATOMIC_RELAXED);
    g_samp[i & (PROF_N - 1)] = pc;
}

void prof_start(void) {
    extern char g_save_dir[];
    char path[1100];
    snprintf(path, sizeof path, "%s/noprof.txt", g_save_dir);
    if (access(path, F_OK) == 0) return;
    g_samp = calloc(PROF_N, sizeof *g_samp);
    if (!g_samp) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
    g_prof_on = 1;
    LOG("profiler on (save/noprof.txt turns it off)");
}

typedef struct { uintptr_t key; const char *name; uintptr_t base; uint32_t n; } Bin;

static int bin_cmp(const void *a, const void *b) {
    uint32_t x = ((const Bin *)a)->n, y = ((const Bin *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Called from the watchdog thread: top functions since the last report. */
void prof_report(void) {
    if (!g_prof_on) return;
    uint32_t end = g_samp_n, start = g_samp_done;
    if (end - start > PROF_N) start = end - PROF_N;
    g_samp_done = end;
    uint32_t total = end - start;
    if (!total) return;
    enum { NB = 8192 };
    Bin *bins = calloc(NB, sizeof *bins);
    if (!bins) return;
    for (uint32_t k = start; k != end; k++) {
        uintptr_t pc = g_samp[k & (PROF_N - 1)];
        Dl_info di;
        uintptr_t key = 0;
        const char *name = "?";
        uintptr_t base = 0;
        if (pc && dladdr((void *)pc, &di)) {
            // unexported code: bin by 256-byte block, printed as lib+offset
            key = di.dli_saddr ? (uintptr_t)di.dli_saddr : (pc & ~(uintptr_t)0xFF);
            name = di.dli_sname ? di.dli_sname : di.dli_fname ? di.dli_fname : "?";
            base = di.dli_sname ? 0 : (uintptr_t)di.dli_fbase;
        }
        uint32_t h = (uint32_t)((key >> 2) * 2654435761u) % NB;
        for (uint32_t probe = 0; probe < NB; probe++, h = (h + 1) % NB) {
            if (!bins[h].n) { bins[h].key = key; bins[h].name = name; bins[h].base = base; }
            if (bins[h].key == key) { bins[h].n++; break; }
        }
    }
    qsort(bins, NB, sizeof *bins, bin_cmp);
    LOG("profile: %u samples (1 ms CPU each)", total);
    for (int i = 0; i < 30 && bins[i].n; i++) {
        const char *b = strrchr(bins[i].name, '/');
        if (bins[i].base)
            LOG("  %5.1f%%  %s+0x%lx", 100.0 * bins[i].n / total, b ? b + 1 : bins[i].name,
                (unsigned long)(bins[i].key - bins[i].base));
        else
            LOG("  %5.1f%%  %s", 100.0 * bins[i].n / total, b ? b + 1 : bins[i].name);
    }
    free(bins);
}

/* Per-thread CPU use since the last call, from /proc/self/task: the SIGPROF
 * sampler above is limited by the kernel tick, so its sample count is not a
 * measure of how busy the cores are. */
void cpu_report(void) {
    enum { NT = 256 };
    static int tids[NT];
    static unsigned long long last[NT];
    static double last_t;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9, dt = now - last_t;
    long hz = sysconf(_SC_CLK_TCK);
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    char line[2048], out[1600];
    int n = 0;
    double total = 0;
    out[0] = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t len = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        line[len] = 0;
        char *lp = strchr(line, '('), *rp = strrchr(line, ')');
        if (!lp || !rp) continue;
        unsigned long long ut = 0, st = 0;
        int cpu = -1;
        /* fields after ')': state(3) ... utime(14) stime(15) ... processor(39) */
        char *p = rp + 2;
        for (int field = 3; field <= 39 && p && *p; field++) {
            if (field == 14) ut = strtoull(p, NULL, 10);
            if (field == 15) st = strtoull(p, NULL, 10);
            if (field == 39) cpu = atoi(p);
            p = strchr(p, ' ');
            if (p) p++;
        }
        unsigned long long t = ut + st, prev = 0;
        int slot = -1;
        for (int k = 0; k < NT; k++) if (tids[k] == tid) { slot = k; break; }
        if (slot < 0) for (int k = 0; k < NT; k++) if (!tids[k]) { slot = k; tids[k] = tid; last[k] = t; break; }
        if (slot < 0) continue;
        prev = last[slot];
        last[slot] = t;
        if (last_t == 0 || dt <= 0) continue;
        double pct = 100.0 * (double)(t - prev) / hz / dt;
        total += pct;
        if (pct >= 3 && strlen(out) < sizeof out - 64) {
            *rp = 0;
            char tmp[64];
            snprintf(tmp, sizeof tmp, " %.15s/%d:%.0f%%@%d", lp + 1, tid, pct, cpu);
            strcat(out, tmp);
        }
        n++;
    }
    closedir(d);
    if (last_t != 0) LOG("cpu: %.0f%% of one core in total;%s", total, out);
    last_t = now;
}
