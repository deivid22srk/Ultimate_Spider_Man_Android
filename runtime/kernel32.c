/* KERNEL32 shims: threads, TLS, sync objects, files, heap, time, locale. */
#define _GNU_SOURCE
#include "game.h"
#include "rt.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sched.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_ALREADY_EXISTS 183
#define INVALID_HANDLE 0xFFFFFFFFu
#define WAIT_TIMEOUT 0x102u

/* ===================== handles ===================== */
struct HObj { int type; void *obj; };
#define MAX_HANDLES 4096
static struct HObj g_handles[MAX_HANDLES];
static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;

uint32_t h_new(int type, void *obj) {
    pthread_mutex_lock(&g_hlock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (g_handles[i].type == H_FREE) {
            g_handles[i].obj = obj;
            __atomic_store_n(&g_handles[i].type, type, __ATOMIC_RELEASE);  /* obj visible before the type */
            pthread_mutex_unlock(&g_hlock);
            return 0x1000u + i * 4;
        }
    pthread_mutex_unlock(&g_hlock);
    rt_fatal("out of handles");
}

static struct HObj *h_slot(uint32_t h) {
    if (h < 0x1000u || ((h - 0x1000u) & 3)) return NULL;
    uint32_t i = (h - 0x1000u) / 4;
    if (i >= MAX_HANDLES || __atomic_load_n(&g_handles[i].type, __ATOMIC_ACQUIRE) == H_FREE) return NULL;
    return &g_handles[i];
}

void *h_get(uint32_t h, int type) {
    struct HObj *s = h_slot(h);
    return s && (type < 0 || s->type == type) ? s->obj : NULL;
}

/* ===================== waitable objects ===================== */
typedef struct {
    int kind;          /* H_EVENT, H_MUTEX, H_SEM, H_THREAD */
    int signaled;
    int manual;
    int count, max;    /* semaphores */
    uint32_t owner;    /* mutex owner thread id */
    int recursion;
    int refs;
} Waitable;

static pthread_mutex_t g_wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wcond = PTHREAD_COND_INITIALIZER;

static uint32_t cur_tid(void);

static int w_ready(Waitable *w) {
    switch (w->kind) {
    case H_MUTEX: return w->owner == 0 || w->owner == cur_tid();
    case H_SEM: return w->count > 0;
    default: return w->signaled;
    }
}

static void w_acquire(Waitable *w) {
    switch (w->kind) {
    case H_MUTEX: w->owner = cur_tid(); w->recursion++; break;
    case H_SEM: w->count--; break;
    case H_EVENT: if (!w->manual) w->signaled = 0; break;
    default: break;
    }
}

static uint32_t wait_objects(uint32_t n, const uint32_t *hs, int all, uint32_t ms) {
    Waitable *w[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) {
        struct HObj *s = h_slot(hs[i]);
        w[i] = s ? (Waitable *)s->obj : NULL;
        if (!w[i] || (s->type != H_EVENT && s->type != H_MUTEX && s->type != H_SEM && s->type != H_THREAD)) {
            LOG("wait on bad handle %08x", hs[i]);
            return 0xFFFFFFFFu;
        }
    }
    struct timespec dl;
    if (ms != 0xFFFFFFFFu) {
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += ms / 1000;
        dl.tv_nsec += (long)(ms % 1000) * 1000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    }
    pthread_mutex_lock(&g_wlock);
    for (;;) {
        if (all) {
            uint32_t k;
            for (k = 0; k < n && w_ready(w[k]); k++) {}
            if (k == n) {
                for (k = 0; k < n; k++) w_acquire(w[k]);
                pthread_mutex_unlock(&g_wlock);
                return 0;
            }
        } else {
            for (uint32_t k = 0; k < n; k++)
                if (w_ready(w[k])) {
                    w_acquire(w[k]);
                    pthread_mutex_unlock(&g_wlock);
                    return k;
                }
        }
        if (ms == 0) break;
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&g_wcond, &g_wlock);
        else if (pthread_cond_timedwait(&g_wcond, &g_wlock, &dl) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g_wlock);
    return WAIT_TIMEOUT;
}

static Waitable *w_new(int kind) {
    Waitable *w = calloc(1, sizeof *w);
    w->kind = kind;
    return w;
}

static void w_signal(Waitable *w) {
    pthread_mutex_lock(&g_wlock);
    w->signaled = 1;
    pthread_cond_broadcast(&g_wcond);
    pthread_mutex_unlock(&g_wlock);
}

/* ===================== threads / TEB ===================== */
static __thread CPU *t_cpu;
static uint32_t g_next_tid = 0x100;
static int g_stack_used[GUEST_STACK_SLOTS];
static pthread_mutex_t g_tlock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_tls_used[64 + 512];

CPU *rt_new_cpu(void) {
    CPU *c = calloc(1, sizeof(CPU));
    c->fpu_cw = 0x27F;
    c->mxcsr = 0x1F80;
    return c;
}

CPU *rt_current_cpu(void) { return t_cpu; }

#define TEB(c) ((c)->fs_base)
static uint32_t cur_tid(void) { return t_cpu ? M32(TEB(t_cpu) + 0x24) : 1; }

/* Static TLS (__declspec(thread)) of the exe: each thread gets a copy of the
 * template, found through TEB+0x2C[_tls_index]. Set by main.c's PE loader. */
static uint32_t g_tls_start, g_tls_size, g_tls_zero;
void rt_set_static_tls(uint32_t start, uint32_t end, uint32_t index_addr, uint32_t zero_fill) {
    g_tls_start = start;
    g_tls_size = end - start;
    g_tls_zero = zero_fill;
    if (index_addr) M32(index_addr) = 0;
    if (t_cpu && !M32(M32(TEB(t_cpu) + 0x2C))) {  /* the thread that loaded the exe */
        uint32_t blk = gmalloc(g_tls_size + g_tls_zero + 16);
        memcpy(MEM + blk, MEM + g_tls_start, g_tls_size);
        memset(MEM + blk + g_tls_size, 0, g_tls_zero + 16);
        M32(M32(TEB(t_cpu) + 0x2C)) = blk;
    }
}

static __thread uint32_t t_preset_tid;  /* id CreateThread already handed out */

/* Guest threads run on all cores: the lifter keeps x86 memory ordering with
 * load-acquire/store-release (TSO in tools/lift.py). save/singlecore.txt pins
 * every guest thread to the fastest core instead, as 0.8-0.16 did. */
static int g_guest_cpu = -2;

static void pin_guest_thread(void) {
#ifdef __ANDROID__
#define cpu g_guest_cpu
    if (cpu == -2) {
        extern char g_save_dir[];
        char path[1100];
        snprintf(path, sizeof path, "%s/singlecore.txt", g_save_dir);
        if (access(path, F_OK) != 0) { cpu = -1; LOG("guest threads on all cores (save/singlecore.txt pins them to one)"); }
        else {
            long best = -1; cpu = 0;
            for (int i = 0; i < 16; i++) {
                snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
                FILE *f = fopen(path, "r");
                if (!f) continue;
                long khz = 0;
                if (fscanf(f, "%ld", &khz) == 1 && khz >= best) { best = khz; cpu = i; }
                fclose(f);
            }
            LOG("guest threads pinned to cpu %d (%ld kHz)", cpu, best);
        }
    }
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set)) LOG("sched_setaffinity failed: %d", errno);
    }
#undef cpu
#endif
}

/* Threads inherit the creator's affinity, so the threads dxvk, the Vulkan
 * driver and SDL audio start would all share the guest core. Bracket the calls
 * that create them: they run on every core except the guest's. */
void rt_host_affinity_begin(void) {
#ifdef __ANDROID__
    if (g_guest_cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (int i = 0; i < n && i < CPU_SETSIZE; i++)
        if (i != g_guest_cpu || n == 1) CPU_SET(i, &set);
    sched_setaffinity(0, sizeof set, &set);
#endif
}

void rt_host_affinity_end(void) { pin_guest_thread(); }

void rt_thread_init(CPU *c) {
    pin_guest_thread();
    extern void rt_altstack(void);
    rt_altstack();  /* crash reports work on stack overflows too */
    pthread_mutex_lock(&g_tlock);
    int slot = -1;
    for (int i = 0; i < GUEST_STACK_SLOTS; i++)
        if (!g_stack_used[i]) { slot = i; g_stack_used[i] = 1; break; }
    uint32_t tid = t_preset_tid ? t_preset_tid : (g_next_tid += 4);
    pthread_mutex_unlock(&g_tlock);
    if (slot < 0) rt_fatal("too many guest threads");
    LOG("thread slot %d taken (tid %u)", slot, tid);
    uint32_t stack_lo = GUEST_STACK_BASE + slot * GUEST_STACK_SIZE;
    uint32_t stack_hi = stack_lo + GUEST_STACK_SIZE - 64;
    uint32_t teb = GUEST_TEB_BASE + slot * 0x2000;
    memset(MEM + teb, 0, 0x2000);
    M32(teb + 0x00) = 0xFFFFFFFFu;     /* SEH chain end */
    M32(teb + 0x04) = stack_hi;
    M32(teb + 0x08) = stack_lo;
    M32(teb + 0x18) = teb;
    M32(teb + 0x20) = 0x42;            /* process id */
    M32(teb + 0x24) = tid;
    M32(teb + 0x2C) = teb + 0x1000;    /* static TLS pointer array */
    if (g_tls_size + g_tls_zero) {
        uint32_t blk = gmalloc(g_tls_size + g_tls_zero + 16);
        memcpy(MEM + blk, MEM + g_tls_start, g_tls_size);
        memset(MEM + blk + g_tls_size, 0, g_tls_zero + 16);
        M32(teb + 0x1000) = blk;  /* _tls_index is 0 */
    }
    M32(teb + 0x30) = GUEST_MISC_BASE; /* PEB */
    c->fs_base = teb;
    c->r[4] = stack_hi;
    c->thread = (void *)(intptr_t)slot;
    t_cpu = c;
}

static void thread_exit_slot(CPU *c) {
    LOG("thread slot %d released", (int)(intptr_t)c->thread);
    pthread_mutex_lock(&g_tlock);
    g_stack_used[(intptr_t)c->thread] = 0;
    pthread_mutex_unlock(&g_tlock);
}
/* For host-created helper threads (timers) that ran guest code. */
void rt_thread_exit(CPU *c) { thread_exit_slot(c); free(c); }

/* For crash reports: the faulting thread's guest state. */
void rt_dump_guest(void) {
    CPU *c = t_cpu;
    if (!c) { fprintf(stderr, "  (not a guest thread)\n"); return; }
    fprintf(stderr, "  guest slot %d fs %08x tls array %08x [0] %08x tid %u\n", (int)(intptr_t)c->thread, c->fs_base,
            M32(c->fs_base + 0x2C), M32(M32(c->fs_base + 0x2C) & ~3u), M32(c->fs_base + 0x24));
    fprintf(stderr, "  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n",
            c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    fprintf(stderr, "  slots used:");
    for (int i = 0; i < GUEST_STACK_SLOTS; i++) if (g_stack_used[i]) fprintf(stderr, " %d", i);
    fputc('\n', stderr);
}

void rt_set_last_error(CPU *c, uint32_t e) { M32(TEB(c) + 0x34) = e; }
uint32_t rt_get_last_error(CPU *c) { return M32(TEB(c) + 0x34); }

typedef struct {
    Waitable w;
    uint32_t start, param, tid, exit_code;
    int suspended;
    pthread_t pt;
} GThread;

static __thread GThread *t_gthread;
static void *thread_main(void *arg) {
    GThread *t = arg;
    t_gthread = t;
    CPU *c = rt_new_cpu();
    t_preset_tid = t->tid;
    rt_thread_init(c);
    pthread_mutex_lock(&g_wlock);
    while (t->suspended) pthread_cond_wait(&g_wcond, &g_wlock);
    pthread_mutex_unlock(&g_wlock);
    uint32_t args[1] = {t->param};
    t->exit_code = rt_invoke(c, t->start, 1, args);
    thread_exit_slot(c);
    w_signal(&t->w);
    return NULL;
}

SHIM(CreateThread) {
    GThread *t = calloc(1, sizeof *t);
    t->w.kind = H_THREAD;
    t->start = ARG(2);
    t->param = ARG(3);
    t->suspended = (ARG(4) & 4) != 0;
    t->exit_code = 0x103; /* STILL_ACTIVE */
    /* the id returned here must be what the thread's GetCurrentThreadId says:
     * Skyrim stores its IO thread's id from here and compares the two */
    pthread_mutex_lock(&g_tlock);
    t->tid = g_next_tid += 4;
    pthread_mutex_unlock(&g_tlock);
    uint32_t h = h_new(H_THREAD, t);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t->pt, &at, thread_main, t);
    if (ARG(5)) M32(ARG(5)) = t->tid;
    LOG("CreateThread start=%08x param=%08x", t->start, t->param);
    RET(h, 6);
}

SHIM(ResumeThread) {
    GThread *t = h_get(ARG(0), H_THREAD);
    uint32_t prev = 0;
    if (t) {
        pthread_mutex_lock(&g_wlock);
        prev = t->suspended;
        t->suspended = 0;
        pthread_cond_broadcast(&g_wcond);
        pthread_mutex_unlock(&g_wlock);
    }
    RET(prev, 1);
}

SHIM(GetExitCodeThread) {
    GThread *t = h_get(ARG(0), H_THREAD);
    if (ARG(1)) M32(ARG(1)) = t ? t->exit_code : 0;
    RET(t != NULL, 2);
}

SHIM(GetCurrentThreadId) { RET(M32(TEB(c) + 0x24), 0); }
SHIM(GetCurrentProcessId) { RET(0x42, 0); }
SHIM(GetCurrentThread) { RET(0xFFFFFFFEu, 0); }
SHIM(GetCurrentProcess) { RET(0xFFFFFFFFu, 0); }
SHIM(SetThreadPriority) { RET(1, 2); }
SHIM(SetThreadAffinityMask) { RET(1, 2); }
SHIM(SetProcessAffinityMask) { RET(1, 2); }
/* The game sizes its job system from the processor count; it used to see 1.
 * Report the cores, capped at 4 (the phone's big cores, and what a 2011 PC
 * had), since the game's threads now really run in parallel. */
static uint32_t guest_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return n < 1 ? 1 : n > 4 ? 4 : (uint32_t)n;
}
SHIM(GetProcessAffinityMask) {
    uint32_t mask = (1u << guest_cpus()) - 1;
    if (ARG(1)) M32(ARG(1)) = mask;
    if (ARG(2)) M32(ARG(2)) = mask;
    RET(1, 3);
}
SHIM(SetThreadExecutionState) { RET(0x80000000u, 1); }
SHIM(Sleep) {
    uint32_t ms = ARG(0);
    if (ms == 0) sched_yield();
    else usleep(ms * 1000);
    RET(0, 1);
}

/* ===================== TLS ===================== */
/* Windows has 64 TEB slots plus 1024 expansion slots, and Skyrim's per-thread
 * pools take one index each: with only 64, TlsAlloc failed while loading a
 * save, every pool shared index -1 and ran out of entries (0.17 crash).
 * Slots 64+ live in the TEB at +0x1800 (512 of them, to the end of the TEB). */
#define TLS_SLOTS (64 + 512)
static uint32_t tls_off(uint32_t i) { return i < 64 ? 0xE10 + 4 * i : 0x1800 + 4 * (i - 64); }

SHIM(TlsAlloc) {
    pthread_mutex_lock(&g_tlock);
    for (uint32_t i = 0; i < TLS_SLOTS; i++)
        if (!g_tls_used[i]) {
            g_tls_used[i] = 1;
            /* a new index reads 0 in every thread, even after TlsFree reuse */
            for (int k = 0; k < GUEST_STACK_SLOTS; k++) M32(GUEST_TEB_BASE + k * 0x2000 + tls_off(i)) = 0;
            pthread_mutex_unlock(&g_tlock);
            if (i == 64) LOG("TlsAlloc: using expansion slots");
            RET(i, 0);
        }
    pthread_mutex_unlock(&g_tlock);
    LOG("TlsAlloc: out of indices");
    RET(0xFFFFFFFFu, 0);
}
SHIM(TlsFree) {
    if (ARG(0) < TLS_SLOTS) g_tls_used[ARG(0)] = 0;
    RET(ARG(0) < TLS_SLOTS, 1);
}
SHIM(TlsGetValue) {
    rt_set_last_error(c, 0);
    RET(ARG(0) < TLS_SLOTS ? M32(TEB(c) + tls_off(ARG(0))) : 0, 1);
}
SHIM(TlsSetValue) {
    if (ARG(0) < TLS_SLOTS) M32(TEB(c) + tls_off(ARG(0))) = ARG(1);
    RET(ARG(0) < TLS_SLOTS, 2);
}

/* ===================== critical sections ===================== */
#define MAX_CS 65536
static pthread_mutex_t *g_cs[MAX_CS];
static int g_ncs;
static uint32_t g_cs_free[MAX_CS];  /* ids released by DeleteCriticalSection */
static int g_ncs_free;

static pthread_mutex_t *cs_get(CPU *c, uint32_t cs) {
    uint32_t id = M32(cs + 0x10);
    if (id == 0 || id > (uint32_t)g_ncs || M32(cs + 0x14) != 0xC5C5C5C5u) {
        /* not initialised through us; initialise lazily */
        pthread_mutex_lock(&g_tlock);
        id = M32(cs + 0x10);
        if (id == 0 || id > (uint32_t)g_ncs || M32(cs + 0x14) != 0xC5C5C5C5u) {
            if (g_ncs_free) id = g_cs_free[--g_ncs_free];
            else {
                if (g_ncs >= MAX_CS) rt_fatal("too many critical sections");
                pthread_mutexattr_t a;
                pthread_mutexattr_init(&a);
                pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
                g_cs[g_ncs] = malloc(sizeof(pthread_mutex_t));
                pthread_mutex_init(g_cs[g_ncs], &a);
                id = ++g_ncs;
            }
            M32(cs + 0x10) = id;
            M32(cs + 0x14) = 0xC5C5C5C5u;
        }
        pthread_mutex_unlock(&g_tlock);
    }
    return g_cs[id - 1];
}

SHIM(InitializeCriticalSection) {
    memset(MEM + ARG(0), 0, 24);
    cs_get(c, ARG(0));
    RET(0, 1);
}
SHIM(DeleteCriticalSection) {
    uint32_t cs = ARG(0), id = M32(cs + 0x10);
    pthread_mutex_lock(&g_tlock);
    if (id && id <= (uint32_t)g_ncs && M32(cs + 0x14) == 0xC5C5C5C5u) {
        M32(cs + 0x10) = 0;
        M32(cs + 0x14) = 0;
        g_cs_free[g_ncs_free++] = id;  /* the mutex is unlocked; reuse it as is */
    }
    pthread_mutex_unlock(&g_tlock);
    RET(0, 1);
}
SHIM(EnterCriticalSection) {
    uint32_t cs = ARG(0);
    pthread_mutex_lock(cs_get(c, cs));
    M32(cs + 8)++; /* RecursionCount, read by some CRT code */
    M32(cs + 0xC) = M32(TEB(c) + 0x24);
    RET(0, 1);
}
SHIM(LeaveCriticalSection) {
    uint32_t cs = ARG(0);
    if (--M32(cs + 8) == 0) M32(cs + 0xC) = 0;
    pthread_mutex_unlock(cs_get(c, cs));
    RET(0, 1);
}

/* ===================== events / mutexes / semaphores ===================== */
SHIM(CreateEventA) {
    Waitable *w = w_new(H_EVENT);
    w->manual = ARG(1) != 0;
    w->signaled = ARG(2) != 0;
    RET(h_new(H_EVENT, w), 4);
}
SHIM(SetEvent) {
    Waitable *w = h_get(ARG(0), H_EVENT);
    if (w) w_signal(w);
    RET(w != NULL, 1);
}
SHIM(ResetEvent) {
    Waitable *w = h_get(ARG(0), H_EVENT);
    if (w) { pthread_mutex_lock(&g_wlock); w->signaled = 0; pthread_mutex_unlock(&g_wlock); }
    RET(w != NULL, 1);
}
SHIM(CreateMutexA) {
    Waitable *w = w_new(H_MUTEX);
    if (ARG(1)) { w->owner = cur_tid(); w->recursion = 1; }
    RET(h_new(H_MUTEX, w), 3);
}
SHIM(ReleaseMutex) {
    Waitable *w = h_get(ARG(0), H_MUTEX);
    if (!w) RET(0, 1);
    pthread_mutex_lock(&g_wlock);
    if (w->owner == cur_tid() && --w->recursion == 0) {
        w->owner = 0;
        pthread_cond_broadcast(&g_wcond);
    }
    pthread_mutex_unlock(&g_wlock);
    RET(1, 1);
}
SHIM(CreateSemaphoreA) {
    Waitable *w = w_new(H_SEM);
    w->count = (int)ARG(1);
    w->max = (int)ARG(2);
    RET(h_new(H_SEM, w), 4);
}
SHIM(ReleaseSemaphore) {
    Waitable *w = h_get(ARG(0), H_SEM);
    if (!w) RET(0, 3);
    pthread_mutex_lock(&g_wlock);
    if (ARG(2)) M32(ARG(2)) = (uint32_t)w->count;
    w->count += (int)ARG(1);
    if (w->count > w->max) w->count = w->max;
    pthread_cond_broadcast(&g_wcond);
    pthread_mutex_unlock(&g_wlock);
    RET(1, 3);
}
SHIM(WaitForSingleObject) {
    uint32_t h = ARG(0);
    RET(wait_objects(1, &h, 0, ARG(1)), 2);
}
SHIM(WaitForMultipleObjects) {
    uint32_t n = ARG(0);
    RET(wait_objects(n, (const uint32_t *)(MEM + ARG(1)), ARG(2), ARG(3)), 4);
}
SHIM(WaitForMultipleObjectsEx) {
    uint32_t n = ARG(0);
    RET(wait_objects(n, (const uint32_t *)(MEM + ARG(1)), ARG(2), ARG(3)), 5);
}

/* ===================== files ===================== */
char g_last_cache_miss[64]; /* newest missing CachedShaders file: the shader about to be compiled */
typedef struct { int fd; char path[1024]; } GFile;
typedef struct { DIR *d; char dir[1024]; char pat[260]; } GFind;

static const char *k_win_game_prefix = GAME_WIN_PREFIX;
static const char *k_win_user_prefix = "\\users\\player\\";

/* Directory listings for case-insensitive lookups. Android's shared storage
   is slow per syscall, and the game probes thousands of missing files (the
   CachedShaders cache), each of which used to re-read the whole directory.
   Entries are dropped when the directory's mtime changes or we create in it. */
typedef struct { char dir[1024]; time_t mtime; long mtime_ns; char *names; size_t len; } DirCache;
static DirCache g_dircache[16];
static int g_dircache_next;
static pthread_mutex_t g_dircache_mu = PTHREAD_MUTEX_INITIALIZER;

static void dircache_forget(const char *dir) {
    pthread_mutex_lock(&g_dircache_mu);
    for (int i = 0; i < 16; i++)
        if (g_dircache[i].names && !strcmp(g_dircache[i].dir, dir)) g_dircache[i].dir[0] = 0;
    pthread_mutex_unlock(&g_dircache_mu);
}

/* Finds `comp` in directory `dir` ignoring case; copies the real name into comp. */
static int dir_lookup_ci(const char *dir, char *comp, size_t compsz) {
    struct stat st;
    if (stat(dir, &st) != 0) return 0;
    pthread_mutex_lock(&g_dircache_mu);
    DirCache *dc = NULL;
    for (int i = 0; i < 16; i++)
        if (g_dircache[i].names && !strcmp(g_dircache[i].dir, dir) &&
            g_dircache[i].mtime == st.st_mtim.tv_sec && g_dircache[i].mtime_ns == st.st_mtim.tv_nsec)
            dc = &g_dircache[i];
    if (!dc) {
        DIR *d = opendir(dir);
        if (!d) { pthread_mutex_unlock(&g_dircache_mu); return 0; }
        dc = &g_dircache[g_dircache_next++ & 15];
        free(dc->names);
        dc->names = NULL; dc->len = 0;
        size_t cap = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name) + 1;
            if (dc->len + l > cap) { cap = (cap + l) * 2; dc->names = realloc(dc->names, cap); }
            memcpy(dc->names + dc->len, de->d_name, l);
            dc->len += l;
        }
        closedir(d);
        if (!dc->names) dc->names = calloc(1, 1);
        snprintf(dc->dir, sizeof dc->dir, "%s", dir);
        dc->mtime = st.st_mtim.tv_sec; dc->mtime_ns = st.st_mtim.tv_nsec;
    }
    int hit = 0;
    for (size_t o = 0; o < dc->len; o += strlen(dc->names + o) + 1)
        if (!strcasecmp(dc->names + o, comp)) {
            snprintf(comp, compsz, "%s", dc->names + o);
            hit = 1;
            break;
        }
    pthread_mutex_unlock(&g_dircache_mu);
    return hit;
}

/* Resolve each path component case-insensitively under host directory `base`. */
static int resolve_ci(char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    /* Common miss: the directory exists with the right case, the file doesn't. */
    char *slash = strrchr(path, '/');
    if (slash && slash != path) {
        *slash = 0;
        int parent_ok = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        char comp[260];
        int hit = 0;
        if (parent_ok && strlen(slash + 1) < sizeof comp) {
            snprintf(comp, sizeof comp, "%s", slash + 1);
            hit = dir_lookup_ci(path, comp, sizeof comp);
        }
        *slash = '/';
        if (parent_ok) {
            if (hit) strcpy(slash + 1, comp);
            return hit;
        }
    }
    char out[1024];
    size_t on = 0;
    const char *p = path;
    if (*p == '/') { out[on++] = '/'; p++; }
    out[on] = 0;
    int found = 1;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char comp[260];
        if (len >= sizeof comp) return 0;
        memcpy(comp, p, len);
        comp[len] = 0;
        char probe[1024];
        snprintf(probe, sizeof probe, "%s%s", on ? out : "./", comp);
        if (found && stat(probe, &st) != 0) {
            if (!dir_lookup_ci(on ? out : ".", comp, sizeof comp)) found = 0;
        }
        on += (size_t)snprintf(out + on, sizeof out - on, "%s%s", comp, e ? "/" : "");
        p = e ? e + 1 : p + len;
    }
    strcpy(path, out);
    return found;
}

int rt_host_path(const char *win, char *out, size_t outsz) {
    char tmp[1024];
    size_t n = 0;
    const char *p = win;
    if (p[0] && p[1] == ':') p += 2;
    for (; *p && n < sizeof tmp - 1; p++) tmp[n++] = *p == '/' ? '\\' : *p;
    tmp[n] = 0;
    const char *rel = tmp;
    const char *root = NULL;
    if (!strncasecmp(tmp, k_win_game_prefix, strlen(k_win_game_prefix))) {
        root = g_game_dir;
        rel = tmp + strlen(k_win_game_prefix);
    } else if (!strncasecmp(tmp, k_win_user_prefix, strlen(k_win_user_prefix))) {
        root = g_save_dir;
        rel = tmp + strlen(k_win_user_prefix);
    } else if (tmp[0] == '\\') {
        root = g_save_dir; /* unknown absolute path: sandbox it */
        rel = tmp + 1;
    }
    if (root) snprintf(out, outsz, "%s/%s", root, rel);
    else snprintf(out, outsz, "%s", rel);
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
    /* collapse "//" and "./" */
    return resolve_ci(out);
}

static void mkdirs_for(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
}

SHIM(CreateFileA) {
    const char *name = GSTR(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char host[1024];
    int exists = rt_host_path(name, host, sizeof host);
    int fl = 0;
    if ((access & 0xC0000000u) == 0xC0000000u || (access & 0x10000000u)) fl = O_RDWR;
    else if (access & 0x40000000u) fl = O_WRONLY;
    else fl = O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 3: break;                           /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: fl |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    }
    if (fl & O_CREAT) mkdirs_for(host);
    int fd = open(host, fl, 0644);
    TRACE("CreateFileA %s -> %s fd=%d", name, host, fd);
    if (fd >= 0 && (fl & O_ACCMODE) == O_RDONLY && strstr(host, "/CachedShaders/")) {
        static uint32_t s_cache_hits;
        if (++s_cache_hits % 100 == 1)
            LOG("shader cache: %u loaded (e.g. %s)", s_cache_hits, strrchr(host, '/') + 1);
    }
    { const char *x = strrchr(host, '.');
      if (x && (!strcasecmp(x, ".esm") || !strcasecmp(x, ".esp") || !strcasecmp(x, ".bsa")))
          LOG("CreateFileA %s -> fd %d", host, fd); }
    if (fl & (O_WRONLY | O_RDWR))
        LOG("CreateFileA %s (write, disposition %u) -> fd %d%s%s", host, disp, fd,
            fd < 0 ? " " : "", fd < 0 ? strerror(errno) : "");
    if (fd >= 0 && (fl & O_CREAT)) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", host);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; dircache_forget(dir); }
    }
    if (fd < 0) {
        /* Shader-cache misses are expected until the game has written the cache:
           count them instead of logging thousands of lines. */
        static uint32_t s_cache_misses;
        if (errno == ENOENT && strstr(host, "/CachedShaders/")) {
            snprintf(g_last_cache_miss, sizeof g_last_cache_miss, "%s", strrchr(host, '/') + 1);
            if (++s_cache_misses % 500 == 1)
                LOG("shader cache: %u not cached yet (e.g. %s)", s_cache_misses, strrchr(host, '/') + 1);
        } else
            LOG("CreateFileA failed: %s (%s)", name, host);
        rt_set_last_error(c, errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
        RET(INVALID_HANDLE, 7);
    }
    GFile *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    rt_set_last_error(c, exists && (disp == 2 || disp == 4) ? ERROR_ALREADY_EXISTS : 0);
    RET(h_new(H_FILE, f), 7);
}

#define STD_IN 0x10u
#define STD_OUT 0x14u
#define STD_ERR 0x18u

SHIM(ReadFile) {
    GFile *f = h_get(ARG(0), H_FILE);
    uint32_t buf = ARG(1), n = ARG(2), ov = ARG(4);
    if (!f) { if (ARG(3)) M32(ARG(3)) = 0; rt_set_last_error(c, ERROR_INVALID_HANDLE); RET(0, 5); }
    ssize_t r;
    if (ov) r = pread(f->fd, MEM + buf, n, (off_t)M32(ov + 8) | ((off_t)M32(ov + 12) << 32));
    else r = read(f->fd, MEM + buf, n);
    if (r < 0) r = 0;
    if (ARG(3)) M32(ARG(3)) = (uint32_t)r;
    if (ov) {
        M32(ov) = 0;
        M32(ov + 4) = (uint32_t)r;
        if (M32(ov + 16)) { Waitable *w = h_get(M32(ov + 16), H_EVENT); if (w) w_signal(w); }
    }
    RET(1, 5);
}

/* ReadFileEx / WriteFileEx: the transfer is done at once, and the
 * completion routine is queued as an APC of the calling thread, run by its
 * next alertable SleepEx (Skyrim's file streaming waits that way). */
typedef struct { uint32_t fn, err, bytes, ov; } Apc;
static __thread Apc t_apc[64];
static __thread int t_napc;
static uint32_t file_ex(CPU *c, int write) {
    GFile *f = h_get(ARG(0), H_FILE);
    uint32_t buf = ARG(1), n = ARG(2), ov = ARG(3), fn = ARG(4);
    if (!f || !ov) { rt_set_last_error(c, ERROR_INVALID_HANDLE); return 0; }
    off_t off = (off_t)M32(ov + 8) | ((off_t)M32(ov + 12) << 32);
    ssize_t r = write ? pwrite(f->fd, MEM + buf, n, off) : pread(f->fd, MEM + buf, n, off);
    uint32_t err = r < 0 ? 30 /* ERROR_READ_FAULT */ : (!write && r == 0 && n) ? 38 /* ERROR_HANDLE_EOF */ : 0;
    if (r < 0) r = 0;
    M32(ov) = err ? 0xC0000011u : 0;
    M32(ov + 4) = (uint32_t)r;
    if (t_napc >= 64) rt_fatal("APC queue full");
    t_apc[t_napc++] = (Apc){fn, err, (uint32_t)r, ov};
    return 1;
}
SHIM(ReadFileEx) { RET(file_ex(c, 0), 5); }
SHIM(WriteFileEx) { RET(file_ex(c, 1), 5); }
static int run_apcs(CPU *c) {
    int ran = 0;
    while (t_napc) {
        Apc a = t_apc[0];
        memmove(t_apc, t_apc + 1, --t_napc * sizeof(Apc));
        uint32_t args[3] = {a.err, a.bytes, a.ov};
        if (a.fn) rt_invoke(c, a.fn, 3, args);
        ran = 1;
    }
    return ran;
}
SHIM(SleepEx) {
    uint32_t ms = ARG(0), alertable = ARG(1);
    if (alertable && run_apcs(c)) RET(0xC0, 2);  /* WAIT_IO_COMPLETION */
    if (ms == 0xFFFFFFFFu) ms = 1;  /* nothing else can queue an APC for us */
    if (ms == 0) sched_yield(); else usleep(ms * 1000u);
    RET(0, 2);
}

SHIM(WriteFile) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2);
    ssize_t r;
    if (h == STD_OUT || h == STD_ERR) r = write(2, MEM + buf, n);
    else {
        GFile *f = h_get(h, H_FILE);
        if (!f) RET(0, 5);
        r = write(f->fd, MEM + buf, n);
    }
    if (ARG(3)) M32(ARG(3)) = r < 0 ? 0 : (uint32_t)r;
    RET(r >= 0, 5);
}

SHIM(SetFilePointer) {
    GFile *f = h_get(ARG(0), H_FILE);
    if (!f) RET(0xFFFFFFFFu, 4);
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)(((uint64_t)M32(ARG(2)) << 32) | ARG(1));
    off_t r = lseek(f->fd, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (ARG(2)) M32(ARG(2)) = (uint32_t)((uint64_t)r >> 32);
    RET((uint32_t)r, 4);
}

SHIM(SetFilePointerEx) {
    GFile *f = h_get(ARG(0), H_FILE);
    if (!f) RET(0, 5);
    int64_t off = (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1));
    off_t r = lseek(f->fd, off, ARG(4) == 0 ? SEEK_SET : ARG(4) == 1 ? SEEK_CUR : SEEK_END);
    if (ARG(3)) { M32(ARG(3)) = (uint32_t)r; M32(ARG(3) + 4) = (uint32_t)((uint64_t)r >> 32); }
    RET(r >= 0, 5);
}

SHIM(GetFileSize) {
    GFile *f = h_get(ARG(0), H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0xFFFFFFFFu, 2);
    if (ARG(1)) M32(ARG(1)) = (uint32_t)((uint64_t)st.st_size >> 32);
    RET((uint32_t)st.st_size, 2);
}

SHIM(GetFileType) {
    uint32_t h = ARG(0);
    if (h == STD_IN || h == STD_OUT || h == STD_ERR) RET(2, 1);
    RET(h_get(h, H_FILE) ? 1 : 0, 1);
}

SHIM(FlushFileBuffers) { RET(1, 1); }
SHIM(SetEndOfFile) {
    GFile *f = h_get(ARG(0), H_FILE);
    if (!f) RET(0, 1);
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    RET(ftruncate(f->fd, pos) == 0, 1);
}

SHIM(CloseHandle) {
    uint32_t h = ARG(0);
    struct HObj *s = h_slot(h);
    if (!s) RET(0, 1);
    if (s->type == H_FILE) { close(((GFile *)s->obj)->fd); free(s->obj); }
    /* waitables may still be referenced by running threads: leak them */
    pthread_mutex_lock(&g_hlock);
    s->type = H_FREE;
    s->obj = NULL;
    pthread_mutex_unlock(&g_hlock);
    RET(1, 1);
}

static void unix_to_filetime(time_t t, uint32_t a) {
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    M32(a) = (uint32_t)ft;
    M32(a + 4) = (uint32_t)(ft >> 32);
}

static uint32_t attrs_of(struct stat *st) {
    return S_ISDIR(st->st_mode) ? 0x10 : 0x80;
}

SHIM(GetFileAttributesA) {
    char host[1024];
    struct stat st;
    if (!rt_host_path(GSTR(ARG(0)), host, sizeof host) || stat(host, &st)) {
        rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
        RET(0xFFFFFFFFu, 1);
    }
    RET(attrs_of(&st), 1);
}

SHIM(GetFileAttributesExA) {
    char host[1024];
    struct stat st;
    uint32_t out = ARG(2);
    if (!rt_host_path(GSTR(ARG(0)), host, sizeof host) || stat(host, &st)) {
        LOG("GetFileAttributesExA failed: %s (%s): %s", GSTR(ARG(0)), host, strerror(errno));
        rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
        RET(0, 3);
    }
    M32(out) = attrs_of(&st);
    unix_to_filetime(st.st_mtime, out + 4);
    unix_to_filetime(st.st_mtime, out + 12);
    unix_to_filetime(st.st_mtime, out + 20);
    M32(out + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
    M32(out + 32) = (uint32_t)st.st_size;
    RET(1, 3);
}

SHIM(DeleteFileA) {
    char host[1024];
    rt_host_path(GSTR(ARG(0)), host, sizeof host);
    RET(unlink(host) == 0, 1);
}
SHIM(MoveFileA) {
    char a[1024], b[1024];
    rt_host_path(GSTR(ARG(0)), a, sizeof a);
    rt_host_path(GSTR(ARG(1)), b, sizeof b);
    int ok = rename(a, b) == 0;
    LOG("MoveFileA %s -> %s: %s", a, b, ok ? "ok" : strerror(errno));
    RET(ok, 2);
}
SHIM(CreateDirectoryA) {
    char host[1024];
    rt_host_path(GSTR(ARG(0)), host, sizeof host);
    mkdirs_for(host);
    int ok = mkdir(host, 0755) == 0;
    if (!ok && errno == EEXIST) rt_set_last_error(c, ERROR_ALREADY_EXISTS);
    RET(ok, 2);
}

static int find_next(GFind *g, uint32_t out) {
    struct dirent *de;
    while ((de = readdir(g->d))) {
        if (fnmatch(g->pat, de->d_name, FNM_CASEFOLD)) continue;
        char full[1400];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", g->dir, de->d_name);
        if (stat(full, &st)) continue;
        memset(MEM + out, 0, 320);
        M32(out) = attrs_of(&st);
        unix_to_filetime(st.st_mtime, out + 4);
        unix_to_filetime(st.st_mtime, out + 12);
        unix_to_filetime(st.st_mtime, out + 20);
        M32(out + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
        M32(out + 32) = (uint32_t)st.st_size;
        snprintf((char *)MEM + out + 44, 260, "%s", de->d_name);
        return 1;
    }
    return 0;
}

SHIM(FindFirstFileA) {
    char host[1024];
    rt_host_path(GSTR(ARG(0)), host, sizeof host);
    GFind *g = calloc(1, sizeof *g);
    char *slash = strrchr(host, '/');
    if (slash) { *slash = 0; snprintf(g->dir, sizeof g->dir, "%s", host); snprintf(g->pat, sizeof g->pat, "%s", slash + 1); }
    else { strcpy(g->dir, "."); snprintf(g->pat, sizeof g->pat, "%s", host); }
    if (!strcmp(g->pat, "*.*")) strcpy(g->pat, "*");
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", g->dir);
    resolve_ci(dir);
    snprintf(g->dir, sizeof g->dir, "%s", dir);
    g->d = opendir(g->dir);
    LOG("FindFirstFileA %s -> %s/%s%s", GSTR(ARG(0)), g->dir, g->pat, g->d ? "" : " (no such folder)");
    if (!g->d || !find_next(g, ARG(1))) {
        if (g->d) closedir(g->d);
        free(g);
        rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE, 2);
    }
    RET(h_new(H_FIND, g), 2);
}
SHIM(FindNextFileA) {
    GFind *g = h_get(ARG(0), H_FIND);
    if (!g || !find_next(g, ARG(1))) { rt_set_last_error(c, ERROR_NO_MORE_FILES); RET(0, 2); }
    RET(1, 2);
}
SHIM(FindClose) {
    struct HObj *s = h_slot(ARG(0));
    if (s && s->type == H_FIND) {
        closedir(((GFind *)s->obj)->d);
        free(s->obj);
        s->type = H_FREE;
    }
    RET(1, 1);
}

/* ===================== heap / virtual memory ===================== */
#define HEAP_HANDLE 0x00150000u
SHIM(GetProcessHeap) { RET(HEAP_HANDLE, 0); }
/* Private heaps. D3DCompiler creates a heap per compile (HeapCreate), allocates
   from it and frees everything with HeapDestroy, so heaps created from its
   code are tracked: each block gets a 16-byte header {next, prev, heap, magic}
   linking it into its heap, and HeapDestroy frees whatever is left. Heaps
   created elsewhere keep the old behaviour (one shared handle, never freed). */
#define PHEAP_BASE  0x7FFE0000u   /* handle = PHEAP_BASE + id * 16, never dereferenced */
#define PHEAP_MAX   256
#define PHEAP_MAGIC 0x48504541u
static uint32_t g_pheap_head[PHEAP_MAX];
static uint8_t g_pheap_used[PHEAP_MAX];
static pthread_mutex_t g_pheap_mu = PTHREAD_MUTEX_INITIALIZER;
static int pheap_id(uint32_t h) {
    if (h < PHEAP_BASE || (h - PHEAP_BASE) % 16 || (h - PHEAP_BASE) / 16 >= PHEAP_MAX) return -1;
    int id = (int)((h - PHEAP_BASE) / 16);
    return g_pheap_used[id] ? id : -1;
}
static uint32_t pheap_alloc(int id, uint32_t n) {
    uint32_t b = gmalloc(n + 16);
    if (!b) return 0;
    pthread_mutex_lock(&g_pheap_mu);
    uint32_t head = g_pheap_head[id];
    M32(b) = head; M32(b + 4) = 0; M32(b + 8) = (uint32_t)id; M32(b + 12) = PHEAP_MAGIC ^ b;
    if (head) M32(head + 4) = b;
    g_pheap_head[id] = b;
    pthread_mutex_unlock(&g_pheap_mu);
    return b + 16;
}
static int pheap_owns(int id, uint32_t p) {
    return p >= 16 && M32(p - 16 + 12) == (PHEAP_MAGIC ^ (p - 16)) && M32(p - 16 + 8) == (uint32_t)id;
}
static void pheap_free(int id, uint32_t p) {
    uint32_t b = p - 16;
    pthread_mutex_lock(&g_pheap_mu);
    uint32_t nx = M32(b), pv = M32(b + 4);
    if (pv) M32(pv) = nx; else g_pheap_head[id] = nx;
    if (nx) M32(nx + 4) = pv;
    M32(b + 12) = 0;
    pthread_mutex_unlock(&g_pheap_mu);
    gfree(b);
}
SHIM(HeapCreate) {
    uint32_t ret = M32(c->r[4]);
    if (ret >= 0x09000000u && ret < 0x0A000000u) {  /* D3DCompiler_43 */
        pthread_mutex_lock(&g_pheap_mu);
        for (int id = 0; id < PHEAP_MAX; id++)
            if (!g_pheap_used[id]) {
                g_pheap_used[id] = 1;
                g_pheap_head[id] = 0;
                pthread_mutex_unlock(&g_pheap_mu);
                RET(PHEAP_BASE + (uint32_t)id * 16, 3);
            }
        pthread_mutex_unlock(&g_pheap_mu);
    }
    RET(HEAP_HANDLE + 0x10000, 3);
}
SHIM(HeapDestroy) {
    int id = pheap_id(ARG(0));
    if (id >= 0) {
        pthread_mutex_lock(&g_pheap_mu);
        uint32_t b = g_pheap_head[id];
        g_pheap_head[id] = 0;
        g_pheap_used[id] = 0;
        pthread_mutex_unlock(&g_pheap_mu);
        while (b) {
            uint32_t nx = M32(b);
            M32(b + 12) = 0;
            gfree(b);
            b = nx;
        }
    }
    RET(1, 1);
}
SHIM(HeapAlloc) {
    uint32_t n = ARG(2);
    int id = pheap_id(ARG(0));
    uint32_t p = id >= 0 ? pheap_alloc(id, n ? n : 1) : gmalloc(n ? n : 1);
    if (p && (ARG(1) & 8)) memset(MEM + p, 0, n);
    RET(p, 3);
}
SHIM(HeapFree) {
    uint32_t p = ARG(2);
    int id = pheap_id(ARG(0));
    if (p && id >= 0 && pheap_owns(id, p)) pheap_free(id, p);
    else gfree(p);
    RET(1, 3);
}
SHIM(HeapSize) {
    uint32_t p = ARG(2);
    int id = pheap_id(ARG(0));
    RET(id >= 0 && pheap_owns(id, p) ? gsize(p - 16) - 16 : gsize(p), 3);
}
SHIM(HeapReAlloc) {
    uint32_t old = ARG(2), n = ARG(3);
    int id = pheap_id(ARG(0));
    if (id >= 0 && pheap_owns(id, old)) {
        uint32_t osz = gsize(old - 16) - 16;
        if (n <= osz) RET(old, 4);
        if (ARG(1) & 0x10) RET(0, 4);  /* HEAP_REALLOC_IN_PLACE_ONLY */
        uint32_t p = pheap_alloc(id, n);
        if (p) {
            memcpy(MEM + p, MEM + old, osz);
            if (ARG(1) & 8) memset(MEM + p + osz, 0, n - osz);
            pheap_free(id, old);
        }
        RET(p, 4);
    }
    uint32_t osz = gsize(old);
    uint32_t p = grealloc(old, n);
    if (p && (ARG(1) & 8) && n > osz) memset(MEM + p + osz, 0, n - osz);
    RET(p, 4);
}
SHIM(VirtualAlloc) {
    uint32_t p = gvalloc(ARG(0), ARG(1));
    if (!p || ARG(1) >= (4u << 20)) LOG("VirtualAlloc(%08x, %u, %x) = %08x", ARG(0), ARG(1), ARG(2), p);
    RET(p, 4);
}
SHIM(VirtualFree) {
    if (ARG(2) & 0x8000) gvfree(ARG(0));
    RET(1, 3);
}

/* ===================== interlocked ===================== */
SHIM(InterlockedIncrement) { __atomic_thread_fence(__ATOMIC_SEQ_CST); uint32_t r_ = __atomic_add_fetch((uint32_t *)(MEM + ARG(0)), 1, __ATOMIC_SEQ_CST); __atomic_thread_fence(__ATOMIC_SEQ_CST); RET(r_, 1); }
SHIM(InterlockedDecrement) { __atomic_thread_fence(__ATOMIC_SEQ_CST); uint32_t r_ = __atomic_sub_fetch((uint32_t *)(MEM + ARG(0)), 1, __ATOMIC_SEQ_CST); __atomic_thread_fence(__ATOMIC_SEQ_CST); RET(r_, 1); }
SHIM(InterlockedExchange) { __atomic_thread_fence(__ATOMIC_SEQ_CST); uint32_t r_ = __atomic_exchange_n((uint32_t *)(MEM + ARG(0)), ARG(1), __ATOMIC_SEQ_CST); __atomic_thread_fence(__ATOMIC_SEQ_CST); RET(r_, 2); }

/* ===================== time ===================== */
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
SHIM(GetTickCount) { RET((uint32_t)(now_ns() / 1000000), 0); }
SHIM(QueryPerformanceCounter) {
    uint64_t t = now_ns() / 100;
    M32(ARG(0)) = (uint32_t)t;
    M32(ARG(0) + 4) = (uint32_t)(t >> 32);
    RET(1, 1);
}
SHIM(QueryPerformanceFrequency) {
    M32(ARG(0)) = 10000000;
    M32(ARG(0) + 4) = 0;
    RET(1, 1);
}
SHIM(GetSystemTimeAsFileTime) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ft = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;
    M32(ARG(0)) = (uint32_t)ft;
    M32(ARG(0) + 4) = (uint32_t)(ft >> 32);
    RET(0, 1);
}
SHIM(FileTimeToLocalFileTime) {
    M32(ARG(1)) = M32(ARG(0));
    M32(ARG(1) + 4) = M32(ARG(0) + 4);
    RET(1, 2);
}
SHIM(FileTimeToSystemTime) {
    uint64_t ft = M32(ARG(0)) | ((uint64_t)M32(ARG(0) + 4) << 32);
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    uint32_t o = ARG(1);
    M16(o) = (uint16_t)(tm.tm_year + 1900);
    M16(o + 2) = (uint16_t)(tm.tm_mon + 1);
    M16(o + 4) = (uint16_t)tm.tm_wday;
    M16(o + 6) = (uint16_t)tm.tm_mday;
    M16(o + 8) = (uint16_t)tm.tm_hour;
    M16(o + 10) = (uint16_t)tm.tm_min;
    M16(o + 12) = (uint16_t)tm.tm_sec;
    M16(o + 14) = (uint16_t)((ft / 10000) % 1000);
    RET(1, 2);
}
SHIM(GetTimeZoneInformation) {
    memset(MEM + ARG(0), 0, 172);
    RET(0, 1);
}

/* ===================== process / environment ===================== */
static uint32_t g_cmdline, g_env, g_envw;

SHIM(GetCommandLineA) { RET(g_cmdline, 0); }
SHIM(GetEnvironmentStrings) { RET(g_env, 0); }
SHIM(GetEnvironmentStringsW) { RET(g_envw, 0); }
SHIM(FreeEnvironmentStringsA) { RET(1, 1); }
SHIM(FreeEnvironmentStringsW) { RET(1, 1); }
SHIM(SetEnvironmentVariableA) { RET(1, 2); }
SHIM(ExpandEnvironmentStringsA) {
    const char *s = GSTR(ARG(0));
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (ARG(1) && ARG(2) >= n) memcpy(MEM + ARG(1), s, n);
    RET(n, 3);
}
SHIM(GetStartupInfoA) {
    memset(MEM + ARG(0), 0, 68);
    M32(ARG(0)) = 68;
    RET(0, 1);
}
SHIM(GetStdHandle) {
    int32_t n = (int32_t)ARG(0);
    RET(n == -10 ? STD_IN : n == -11 ? STD_OUT : STD_ERR, 1);
}
SHIM(SetStdHandle) { RET(1, 2); }
SHIM(SetHandleCount) { RET(ARG(0), 1); }
SHIM(GetVersionExA) {
    uint32_t v = ARG(0);
    uint32_t sz = M32(v);
    memset(MEM + v + 4, 0, sz - 4);
    M32(v + 4) = 5;    /* Windows XP */
    M32(v + 8) = 1;
    M32(v + 12) = 2600;
    M32(v + 16) = 2;   /* VER_PLATFORM_WIN32_NT */
    strcpy((char *)MEM + v + 20, "Service Pack 3");
    if (sz >= 156) { M16(v + 148) = 3; M16(v + 154) = 1; }
    RET(1, 1);
}
SHIM(VerifyVersionInfoA) { RET(1, 4); }
SHIM(VerSetConditionMask) {
    /* ULONGLONG ConditionMask passed as 2 dwords, then 2 more args; returns in edx:eax */
    c->r[2] = ARG(1);
    RET(ARG(0), 4);
}
SHIM(IsDebuggerPresent) { RET(0, 0); }
SHIM(GetLastError) { RET(rt_get_last_error(c), 0); }
SHIM(SetLastError) { rt_set_last_error(c, ARG(0)); RET(0, 1); }
SHIM(SetUnhandledExceptionFilter) { RET(0, 1); }
SHIM(UnhandledExceptionFilter) { RET(0, 1); }
SHIM(SetConsoleCtrlHandler) { RET(1, 2); }
SHIM(OutputDebugStringA) { LOG("[guest] %s", GSTR(ARG(0))); RET(0, 1); }
SHIM(OutputDebugStringW) {
    char buf[512];
    uint32_t s = ARG(0);
    int i = 0;
    for (; i < 511 && M16(s + 2 * i); i++) buf[i] = (char)M16(s + 2 * i);
    buf[i] = 0;
    LOG("[guest] %s", buf);
    RET(0, 1);
}
SHIM(DebugBreak) { rt_fatal("guest DebugBreak"); }
SHIM(FatalAppExitA) { rt_fatal("FatalAppExit: %s", GSTR(ARG(1))); }
SHIM(ExitProcess) { LOG("ExitProcess(%u)", ARG(0)); fflush(NULL); _exit((int)ARG(0)); }
SHIM(TerminateProcess) { LOG("TerminateProcess(%u)", ARG(1)); fflush(NULL); _exit((int)ARG(1)); }
SHIM(RaiseException) {
    uint32_t code = ARG(0);
    if (code == 0x406D1388u) RET(0, 4);  /* MS_VC_EXCEPTION: SetThreadName for a debugger */
    if (code == 0xE06D7363u) {
        uint32_t args = ARG(3);
        rt_fatal("C++ exception thrown (object %08x, type info %08x)", args ? M32(args + 4) : 0, args ? M32(args + 8) : 0);
    }
    rt_fatal("RaiseException %08x", code);
}
SHIM(RtlUnwind) { rt_fatal("RtlUnwind called"); }

/* ===================== modules ===================== */
typedef struct { char name[64]; } GModule;
static uint32_t module_base(const char *n);
static const char *module_name_of(uint32_t base);
static uint32_t pseudo_module(const char *n);

/* GPU vendor and desktop-compositor DLLs: absent, as on non-NVIDIA PCs
 * (the game parses a loaded nvapi.dll's PE headers). */
static int module_absent(const char *n) {
    static const char *const k_absent[] = {"nvapi", "nvpowerapi", "dwmapi", NULL};
    for (int i = 0; k_absent[i]; i++)
        if (!strncasecmp(n, k_absent[i], strlen(k_absent[i]))) return 1;
    return 0;
}
SHIM(GetModuleHandleA) {
    const char *n = GSTR(ARG(0));
    if (!n) RET(GUEST_IMAGE_BASE, 1);
    if (module_absent(n)) RET(0, 1);
    if (module_base(n)) RET(module_base(n), 1);
    RET(pseudo_module(n), 1);
}
/* One pseudo-module handle per name: FreeLibrary is a no-op, and D3DX
 * loads d3d9.dll on every call. */
static struct { char name[64]; uint32_t h; } g_libs[64];
static int g_nlibs;
static pthread_mutex_t g_liblock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t pseudo_module(const char *n) {
    uint32_t h = 0;
    pthread_mutex_lock(&g_liblock);
    for (int i = 0; i < g_nlibs && !h; i++)
        if (!strcasecmp(g_libs[i].name, n)) h = g_libs[i].h;
    if (!h) {
        GModule *m = calloc(1, sizeof *m);
        snprintf(m->name, sizeof m->name, "%s", n);
        h = h_new(H_MODULE, m);
        if (g_nlibs < 64) {
            snprintf(g_libs[g_nlibs].name, sizeof g_libs[0].name, "%s", n);
            g_libs[g_nlibs++].h = h;
        }
    }
    pthread_mutex_unlock(&g_liblock);
    return h;
}
static uint32_t load_library(const char *n) {
    if (module_base(n)) return module_base(n);
    TRACE("LoadLibrary(%s)", n);
    if (module_absent(n)) return 0;
    return pseudo_module(n);
}
SHIM(LoadLibraryA) { RET(load_library(GSTR(ARG(0))), 1); }
SHIM(LoadLibraryW) {
    char buf[260];
    uint32_t s = ARG(0);
    int i = 0;
    for (; i < 259 && M16(s + 2 * i); i++) buf[i] = (char)M16(s + 2 * i);
    buf[i] = 0;
    RET(load_library(buf), 1);
}
SHIM(FreeLibrary) { RET(1, 1); }
SHIM(GetProcAddress) {
    GModule *m = h_get(ARG(0), H_MODULE);
    uint32_t pn = ARG(1);
    char name[64];
    if (pn < 0x10000) snprintf(name, sizeof name, "#%u", pn);
    else snprintf(name, sizeof name, "%s", GSTR(pn));
    const char *dll = m ? m->name : module_name_of(ARG(0)) ? module_name_of(ARG(0)) : GAME_EXE;
    uint32_t r = rt_import_opt(dll, name);
    TRACE("GetProcAddress(%s, %s) = %08x", dll, name, r);
    RET(r, 2);
}
SHIM(GetModuleFileNameA) {
    const char *p = GAME_WIN_DIR "\\" GAME_EXE;
    uint32_t n = ARG(2);
    snprintf((char *)MEM + ARG(1), n, "%s", p);
    RET(strlen(p) < n ? strlen(p) : n, 3);
}

/* ===================== console ===================== */
SHIM(GetConsoleCP) { RET(437, 0); }
SHIM(GetConsoleOutputCP) { RET(437, 0); }
SHIM(GetConsoleMode) { RET(0, 2); }
SHIM(WriteConsoleA) {
    fwrite(MEM + ARG(1), 1, ARG(2), stderr);
    if (ARG(3)) M32(ARG(3)) = ARG(2);
    RET(1, 5);
}
SHIM(WriteConsoleW) {
    for (uint32_t i = 0; i < ARG(2); i++) fputc((char)M16(ARG(1) + 2 * i), stderr);
    if (ARG(3)) M32(ARG(3)) = ARG(2);
    RET(1, 5);
}

/* ===================== code pages / locale (Latin-1 / 1252 approximation) ===================== */
SHIM(GetACP) { RET(1252, 0); }
SHIM(GetOEMCP) { RET(437, 0); }
SHIM(IsValidCodePage) { RET(1, 1); }
SHIM(GetCPInfo) {
    uint32_t o = ARG(1);
    memset(MEM + o, 0, 20);
    M32(o) = 1;
    M8(o + 4) = '?';
    RET(1, 2);
}
SHIM(MultiByteToWideChar) {
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (!cap) RET(n, 6);
    int32_t k = n < (int32_t)cap ? n : (int32_t)cap;
    for (int32_t i = 0; i < k; i++) M16(dst + 2 * i) = M8(src + i);
    RET(k, 6);
}
SHIM(WideCharToMultiByte) {
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
    if (ARG(7)) M32(ARG(7)) = 0;
    if (!cap) RET(n, 8);
    int32_t k = n < (int32_t)cap ? n : (int32_t)cap;
    for (int32_t i = 0; i < k; i++) {
        uint16_t w = M16(src + 2 * i);
        M8(dst + i) = w < 256 ? (uint8_t)w : '?';
    }
    RET(k, 8);
}
SHIM(GetUserDefaultLCID) { RET(0x409, 0); }
SHIM(GetUserDefaultLangID) { RET(0x409, 0); }
SHIM(GetUserGeoID) { RET(244, 1); } /* United States */
SHIM(IsValidLocale) { RET(1, 2); }
SHIM(EnumSystemLocalesA) { RET(1, 2); }
static const char *locale_info(uint32_t type) {
    switch (type & 0xFFFF) {
    case 0x1: return "0409";               /* LOCALE_ILANGUAGE */
    case 0x3: return "ENU";                /* LOCALE_SABBREVLANGNAME */
    case 0x7: return "USA";
    case 0x5: return "1";                  /* LOCALE_ICOUNTRY */
    case 0xB: return "437";                /* LOCALE_IDEFAULTCODEPAGE */
    case 0x1004: return "1252";            /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x59: return "en";                /* LOCALE_SISO639LANGNAME */
    case 0x5A: return "US";                /* LOCALE_SISO3166CTRYNAME */
    case 0x1001: return "English";
    case 0x1002: return "United States";
    case 0xE: return ".";
    case 0xF: return ",";
    case 0x1F: return "M/d/yyyy";
    case 0x20: return "dddd, MMMM dd, yyyy";
    case 0x1003: return "h:mm:ss tt";
    default: return "";
    }
}
SHIM(GetLocaleInfoA) {
    const char *s = locale_info(ARG(1));
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (ARG(3)) { if (ARG(3) < n) RET(0, 4); memcpy(MEM + ARG(2), s, n); }
    RET(n, 4);
}
SHIM(GetLocaleInfoW) {
    const char *s = locale_info(ARG(1));
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (ARG(3)) {
        if (ARG(3) < n) RET(0, 4);
        for (uint32_t i = 0; i < n; i++) M16(ARG(2) + 2 * i) = (uint8_t)s[i];
    }
    RET(n, 4);
}
SHIM(GetStringTypeA) {
    /* (lcid, type, src, n, out) */
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    for (int32_t i = 0; i < n; i++) {
        uint8_t ch = M8(src + i);
        uint16_t t = 0;
        if (ch >= 'A' && ch <= 'Z') t |= 0x101;
        if (ch >= 'a' && ch <= 'z') t |= 0x102;
        if (ch >= '0' && ch <= '9') t |= 0x084;
        if (ch == ' ' || (ch >= 9 && ch <= 13)) t |= 0x008;
        if (ch == ' ') t |= 0x040;
        if (ch < 32 || ch == 127) t |= 0x020;
        if (ch > 32 && ch < 127 && !(t & 0x107)) t |= 0x010;
        M16(out + 2 * i) = t;
    }
    RET(1, 5);
}
SHIM(GetStringTypeW) {
    /* (type, src, n, out) */
    uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
    for (int32_t i = 0; i < n; i++) {
        uint16_t ch = M16(src + 2 * i);
        uint16_t t = 0;
        if (ch >= 'A' && ch <= 'Z') t |= 0x101;
        if (ch >= 'a' && ch <= 'z') t |= 0x102;
        if (ch >= '0' && ch <= '9') t |= 0x084;
        if (ch == ' ' || (ch >= 9 && ch <= 13)) t |= 0x008;
        if (ch == ' ') t |= 0x040;
        if (ch < 32 || ch == 127) t |= 0x020;
        if (ch > 32 && ch < 127 && !(t & 0x107)) t |= 0x010;
        M16(out + 2 * i) = t;
    }
    RET(1, 4);
}
SHIM(LCMapStringA) {
    /* (lcid, flags, src, n, dst, cap) */
    uint32_t fl = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (!cap) RET(n, 6);
    for (int32_t i = 0; i < n && i < (int32_t)cap; i++) {
        uint8_t ch = M8(src + i);
        if (fl & 0x100) ch = (uint8_t)tolower(ch);
        if (fl & 0x200) ch = (uint8_t)toupper(ch);
        M8(dst + i) = ch;
    }
    RET(n, 6);
}
SHIM(LCMapStringW) {
    uint32_t fl = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
    if (!cap) RET(n, 6);
    for (int32_t i = 0; i < n && i < (int32_t)cap; i++) {
        uint16_t ch = M16(src + 2 * i);
        if (ch < 128 && (fl & 0x100)) ch = (uint16_t)tolower(ch);
        if (ch < 128 && (fl & 0x200)) ch = (uint16_t)toupper(ch);
        M16(dst + 2 * i) = ch;
    }
    RET(n, 6);
}
SHIM(CompareStringA) {
    uint32_t fl = ARG(1);
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    const char *a = GSTR(ARG(2)), *b = GSTR(ARG(4));
    if (n1 < 0) n1 = (int32_t)strlen(a);
    if (n2 < 0) n2 = (int32_t)strlen(b);
    int32_t m = n1 < n2 ? n1 : n2;
    int r = (fl & 1) ? strncasecmp(a, b, (size_t)m) : strncmp(a, b, (size_t)m);
    if (!r) r = n1 - n2;
    RET(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}
SHIM(CompareStringW) {
    uint32_t fl = ARG(1), a = ARG(2), b = ARG(4);
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    if (n1 < 0) { n1 = 0; while (M16(a + 2 * n1)) n1++; }
    if (n2 < 0) { n2 = 0; while (M16(b + 2 * n2)) n2++; }
    int r = 0;
    for (int32_t i = 0; i < n1 && i < n2 && !r; i++) {
        int x = M16(a + 2 * i), y = M16(b + 2 * i);
        if (fl & 1) { x = x < 128 ? tolower(x) : x; y = y < 128 ? tolower(y) : y; }
        r = x - y;
    }
    if (!r) r = n1 - n2;
    RET(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}
SHIM(GetDateFormatA) {
    const char *s = "1/1/2008";
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (ARG(5)) snprintf((char *)MEM + ARG(4), ARG(5), "%s", s);
    RET(n, 6);
}
SHIM(GetTimeFormatA) {
    const char *s = "12:00:00 PM";
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (ARG(5)) snprintf((char *)MEM + ARG(4), ARG(5), "%s", s);
    RET(n, 6);
}

/* ===================== loaded modules ===================== */
#define MAX_MODS 16
static struct { char name[64]; uint32_t base; } g_mods[MAX_MODS];
static int g_nmods;
void rt_module_add(const char *name, uint32_t base) {
    if (g_nmods < MAX_MODS) {
        snprintf(g_mods[g_nmods].name, sizeof g_mods[0].name, "%s", name);
        g_mods[g_nmods++].base = base;
    }
}
/* Matches "d3dx9_35", "d3dx9_35.dll" or a path ending in either. */
static uint32_t module_base(const char *n) {
    if (!n) return 0;
    const char *s = strrchr(n, '\\');
    s = s ? s + 1 : n;
    for (int i = 0; i < g_nmods; i++) {
        size_t l = strlen(s);
        if (!strcasecmp(g_mods[i].name, s)) return g_mods[i].base;
        if (!strncasecmp(g_mods[i].name, s, l) && !strcasecmp(g_mods[i].name + l, ".dll")) return g_mods[i].base;
    }
    return 0;
}
static const char *module_name_of(uint32_t base) {
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].base == base) return g_mods[i].name;
    return NULL;
}

/* ===================== resources ===================== */
/* id is either an integer (<0x10000) or a guest string (wide if `wide`). */
static int res_name_eq(uint32_t rsrc, uint32_t ent, uint32_t id, int wide) {
    uint32_t nm = M32(ent);
    if (id < 0x10000) return !(nm & 0x80000000u) && (nm & 0xFFFF) == id;
    char want[128];
    int i = 0;
    for (; i < 127; i++) {
        uint32_t ch = wide ? M16(id + 2 * i) : M8(id + i);
        if (!ch) break;
        want[i] = (char)ch;
    }
    want[i] = 0;
    if (want[0] == '#') return !(nm & 0x80000000u) && (nm & 0xFFFF) == (uint32_t)atoi(want + 1);
    if (!(nm & 0x80000000u)) return 0;
    uint32_t s = rsrc + (nm & 0x7FFFFFFF);
    uint16_t len = M16(s);
    if (len != strlen(want)) return 0;
    for (int k = 0; k < len; k++)
        if (toupper(M16(s + 2 + 2 * k)) != toupper((uint8_t)want[k])) return 0;
    return 1;
}
static uint32_t res_find_in(uint32_t rsrc, uint32_t dir, uint32_t id, int wide) {
    uint32_t n = M16(dir + 12) + M16(dir + 14);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ent = dir + 16 + 8 * i;
        if (id == 0xFFFFFFFFu || res_name_eq(rsrc, ent, id, wide)) return M32(ent + 4);
    }
    return 0xFFFFFFFFu;
}
static uint32_t find_resource(uint32_t hmod, uint32_t name, uint32_t type, int wide) {
    uint32_t base = hmod && hmod >= 0x10000 && module_name_of(hmod) ? hmod : GUEST_IMAGE_BASE;
    uint32_t pe = base + M32(base + 0x3C);
    uint32_t rva = M32(pe + 24 + 112);
    if (!rva) return 0;
    uint32_t rsrc = base + rva;
    uint32_t e = res_find_in(rsrc, rsrc, type, wide);
    if (e == 0xFFFFFFFFu || !(e & 0x80000000u)) return 0;
    e = res_find_in(rsrc, rsrc + (e & 0x7FFFFFFF), name, wide);
    if (e == 0xFFFFFFFFu || !(e & 0x80000000u)) return 0;
    e = res_find_in(rsrc, rsrc + (e & 0x7FFFFFFF), 0xFFFFFFFFu, wide); /* first language */
    if (e == 0xFFFFFFFFu || (e & 0x80000000u)) return 0;
    return rsrc + e; /* IMAGE_RESOURCE_DATA_ENTRY */
}
SHIM(FindResourceA) {
    uint32_t r = find_resource(ARG(0), ARG(1), ARG(2), 0);
    if (!r) rt_set_last_error(c, 1814);
    RET(r, 3);
}
SHIM(FindResourceW) {
    uint32_t r = find_resource(ARG(0), ARG(1), ARG(2), 1);
    if (!r) rt_set_last_error(c, 1814);
    RET(r, 3);
}
SHIM(LoadResource) {
    uint32_t hmod = ARG(0), r = ARG(1);
    uint32_t base = hmod && module_name_of(hmod) ? hmod : GUEST_IMAGE_BASE;
    RET(r ? base + M32(r) : 0, 2);
}
SHIM(LockResource) { RET(ARG(0), 1); }
SHIM(SizeofResource) { RET(ARG(1) ? M32(ARG(1) + 4) : 0, 2); }
SHIM(FreeResource) { RET(0, 1); }

/* ===================== wide-path variants ===================== */
/* Narrows a guest UTF-16 string into a fresh guest buffer. */
static uint32_t narrow_dup(uint32_t w) {
    char buf[1024];
    int i = 0;
    for (; w && i < 1023 && M16(w + 2 * i); i++) buf[i] = (char)M16(w + 2 * i);
    buf[i] = 0;
    return gstrdup(buf);
}
/* Calls an A shim with its first `nstr` string args narrowed in place. */
static void call_narrowed(CPU *c, ShimFn fn, int nstr) {
    uint32_t tmp[2] = {0, 0};
    for (int i = 0; i < nstr; i++) { tmp[i] = narrow_dup(ARG(i)); ARG(i) = tmp[i]; }
    fn(c);
    for (int i = 0; i < nstr; i++) gfree(tmp[i]);
}
SHIM(CreateFileW) { call_narrowed(c, shim_CreateFileA, 1); }
SHIM(DeleteFileW) { call_narrowed(c, shim_DeleteFileA, 1); }
SHIM(MoveFileW) { call_narrowed(c, shim_MoveFileA, 2); }

/* ===================== file mapping ===================== */
typedef struct { int fd; uint64_t size; } GMapping;
SHIM(CreateFileMappingA) {
    GFile *f = h_get(ARG(0), H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0, 6);
    GMapping *m = calloc(1, sizeof *m);
    m->fd = f->fd;
    m->size = ARG(3) || ARG(4) ? ((uint64_t)ARG(3) << 32 | ARG(4)) : (uint64_t)st.st_size;
    RET(h_new(H_OTHER, m), 6);
}
SHIM(MapViewOfFile) {
    GMapping *m = h_get(ARG(0), H_OTHER);
    if (!m) RET(0, 5);
    uint64_t off = (uint64_t)ARG(2) << 32 | ARG(3);
    uint32_t n = ARG(4) ? ARG(4) : (uint32_t)(m->size - off);
    if (ARG(1) & 2) LOG("MapViewOfFile: writable view is a private copy");
    uint32_t p = gvalloc(0, n ? n : 1);
    if (!p) RET(0, 5);
    ssize_t r = pread(m->fd, MEM + p, n, (off_t)off);
    (void)r;
    RET(p, 5);
}
SHIM(UnmapViewOfFile) { gvfree(ARG(0)); RET(1, 1); }
SHIM(GetFileSizeEx) {
    GFile *f = h_get(ARG(0), H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0, 2);
    M32(ARG(1)) = (uint32_t)st.st_size;
    M32(ARG(1) + 4) = (uint32_t)((uint64_t)st.st_size >> 32);
    RET(1, 2);
}
SHIM(GetFullPathNameA) {
    const char *in = GSTR(ARG(0));
    uint32_t n = ARG(1), out = ARG(2), fp = ARG(3);
    char full[1024];
    if (in[0] && in[1] == ':') snprintf(full, sizeof full, "%s", in);
    else if (in[0] == '\\') snprintf(full, sizeof full, "C:%s", in);
    else snprintf(full, sizeof full, GAME_WIN_DIR "\\%s", in);
    for (char *q = full; *q; q++) if (*q == '/') *q = '\\';
    uint32_t len = (uint32_t)strlen(full);
    if (len + 1 > n) RET(len + 1, 4);
    memcpy(MEM + out, full, len + 1);
    if (fp) { char *s = strrchr(full, '\\'); M32(fp) = out + (uint32_t)(s ? s + 1 - full : 0); }
    RET(len, 4);
}
SHIM(GetTempPathA) {
    const char *p = "C:\\Users\\Player\\Temp\\";
    uint32_t len = (uint32_t)strlen(p);
    if (ARG(0) > len) memcpy(MEM + ARG(1), p, len + 1);
    RET(len, 2);
}
static uint32_t g_tmpctr = 1;
SHIM(GetTempFileNameA) {
    uint32_t u = ARG(2) ? ARG(2) : __atomic_fetch_add(&g_tmpctr, 1, __ATOMIC_SEQ_CST);
    char name[1024];
    snprintf(name, sizeof name, "%s%s%.3s%04X.tmp", GSTR(ARG(0)),
             GSTR(ARG(0))[0] && GSTR(ARG(0))[strlen(GSTR(ARG(0))) - 1] != '\\' ? "\\" : "", GSTR(ARG(1)), u & 0xFFFF);
    strcpy((char *)MEM + ARG(3), name);
    if (!ARG(2)) {
        char host[1024];
        rt_host_path(name, host, sizeof host);
        mkdirs_for(host);
        int fd = open(host, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
    }
    RET(u, 4);
}
SHIM(GetTempFileNameW) {
    uint32_t a = narrow_dup(ARG(0)), b = narrow_dup(ARG(1)), out = ARG(3);
    uint32_t tmp = gmalloc(1024);
    ARG(0) = a; ARG(1) = b; ARG(3) = tmp;
    shim_GetTempFileNameA(c);
    int i = 0;
    do M16(out + 2 * i) = M8(tmp + i); while (M8(tmp + i++));
    gfree(a); gfree(b); gfree(tmp);
}

/* ===================== misc ===================== */
SHIM(FormatMessageA) {
    uint32_t flags = ARG(0), id = ARG(2), buf = ARG(4), n = ARG(5);
    char msg[64];
    int len = snprintf(msg, sizeof msg, "Error %u.\r\n", id);
    if (flags & 0x100) {
        uint32_t p = gmalloc((uint32_t)len + 1);
        memcpy(MEM + p, msg, len + 1);
        M32(buf) = p;
    } else {
        if ((uint32_t)len + 1 > n) RET(0, 7);
        memcpy(MEM + buf, msg, len + 1);
    }
    RET(len, 7);
}
SHIM(LocalFree) { if (ARG(0)) gfree(ARG(0)); RET(0, 1); }
SHIM(InterlockedCompareExchange) {
    uint32_t exp = ARG(2);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_compare_exchange_n((uint32_t *)(MEM + ARG(0)), &exp, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    RET(exp, 3);
}
SHIM(lstrcmpiA) { RET((uint32_t)strcasecmp(GSTR(ARG(0)) ? GSTR(ARG(0)) : "", GSTR(ARG(1)) ? GSTR(ARG(1)) : ""), 2); }
SHIM(IsDBCSLeadByte) { RET(0, 1); }
SHIM(IsProcessorFeaturePresent) {
    uint32_t f = ARG(0);
    RET(f == 3 || f == 6 || f == 10, 1); /* MMX, SSE, SSE2 (matches rt_cpuid) */
}
SHIM(GetVersion) { RET(0x0A280105u, 0); } /* XP, build 2600 */
SHIM(GetSystemInfo) {
    uint32_t p = ARG(0);
    memset(MEM + p, 0, 36);
    M32(p + 4) = 0x1000;
    M32(p + 8) = 0x10000;
    M32(p + 12) = 0x7FFEFFFF;
    M32(p + 16) = (1u << guest_cpus()) - 1;  /* active processor mask */
    M32(p + 20) = guest_cpus();               /* number of processors */
    M32(p + 24) = 586;
    M32(p + 28) = 0x10000;
    M16(p + 32) = 6;
    RET(0, 1);
}
SHIM(GlobalMemoryStatus) {
    uint32_t p = ARG(0);
    M32(p) = 32;
    M32(p + 4) = 30;
    M32(p + 8) = 0x40000000u;  /* 1 GB physical */
    M32(p + 12) = 0x30000000u;
    M32(p + 16) = 0x80000000u;
    M32(p + 20) = 0x70000000u;
    M32(p + 24) = 0x7FFE0000u;
    M32(p + 28) = 0x60000000u;
    RET(0, 1);
}
SHIM(DisableThreadLibraryCalls) { RET(1, 1); }
SHIM(LocalAlloc) {
    uint32_t p = gmalloc(ARG(1) ? ARG(1) : 1);
    if (p && (ARG(0) & 0x40)) memset(MEM + p, 0, ARG(1));
    RET(p, 2);
}
static uint32_t put_dir(uint32_t buf, uint32_t n, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    if (len + 1 > n) return len + 1;
    memcpy(MEM + buf, s, len + 1);
    return len;
}
SHIM(GetWindowsDirectoryA) { RET(put_dir(ARG(0), ARG(1), "C:\\WINDOWS"), 2); }
SHIM(GetSystemDirectoryA) { RET(put_dir(ARG(0), ARG(1), "C:\\WINDOWS\\system32"), 2); }
SHIM(SetErrorMode) { RET(0, 1); }
SHIM(IsBadReadPtr) { RET(ARG(0) < 0x10000, 2); }
SHIM(IsBadWritePtr) { RET(ARG(0) < 0x10000, 2); }
SHIM(IsBadCodePtr) { RET(ARG(0) < 0x10000, 1); }


/* ===================== added for LEGO Marvel ===================== */
SHIM(GetFullPathNameW) {
    uint32_t in = narrow_dup(ARG(0)), n = ARG(1), out = ARG(2), fp = ARG(3);
    uint32_t tmp = gmalloc(1024);
    ARG(0) = in; ARG(1) = 1024; ARG(2) = tmp; ARG(3) = 0;
    uint32_t esp0 = c->r[4];
    shim_GetFullPathNameA(c);
    c->r[4] = esp0;
    uint32_t len = c->r[0];
    if (len && len + 1 <= n) {
        for (uint32_t i = 0; i <= len; i++) M16(out + 2 * i) = M8(tmp + i);
        if (fp) {
            uint32_t last = 0;
            for (uint32_t i = 0; i < len; i++) if (M8(tmp + i) == '\\') last = i + 1;
            M32(fp) = out + 2 * last;
        }
    } else if (len) len++;
    gfree(tmp);
    gfree(in);
    RET(len, 4);
}

SHIM(TryEnterCriticalSection) {
    uint32_t cs = ARG(0);
    if (pthread_mutex_trylock(cs_get(c, cs))) RET(0, 1);
    M32(cs + 8)++;
    M32(cs + 0xC) = M32(TEB(c) + 0x24);
    RET(1, 1);
}
SHIM(InitializeCriticalSectionAndSpinCount) {
    memset(MEM + ARG(0), 0, 24);
    cs_get(c, ARG(0));
    RET(1, 2);
}
SHIM(GetModuleFileNameW) {
    const char *p = GAME_WIN_DIR "\\" GAME_EXE;
    uint32_t n = ARG(2), o = ARG(1), i = 0;
    for (; p[i] && i + 1 < n; i++) M16(o + 2 * i) = (uint8_t)p[i];
    if (n) M16(o + 2 * i) = 0;
    RET(i, 3);
}
SHIM(GetModuleHandleW) {
    if (!ARG(0)) RET(GUEST_IMAGE_BASE, 1);
    call_narrowed(c, shim_GetModuleHandleA, 1);
}
SHIM(GetStartupInfoW) {
    memset(MEM + ARG(0), 0, 68);
    M32(ARG(0)) = 68;
    RET(0, 1);
}
SHIM(HeapSetInformation) { RET(1, 4); }
/* Pointer encoding is an identity here: nothing outside the guest decodes. */
SHIM(EncodePointer) { RET(ARG(0), 1); }
SHIM(DecodePointer) { RET(ARG(0), 1); }
SHIM(GetFileInformationByHandle) {
    GFile *f = h_get(ARG(0), H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0, 2);
    uint32_t o = ARG(1);
    memset(MEM + o, 0, 52);
    M32(o) = attrs_of(&st);
    uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
    for (int k = 0; k < 3; k++) { M32(o + 4 + 8 * k) = (uint32_t)ft; M32(o + 8 + 8 * k) = (uint32_t)(ft >> 32); }
    M32(o + 28) = 0x4C454731u;  /* volume serial */
    M32(o + 32) = (uint32_t)((uint64_t)st.st_size >> 32);
    M32(o + 36) = (uint32_t)st.st_size;
    M32(o + 40) = 1;
    M32(o + 44) = (uint32_t)((uint64_t)st.st_ino >> 32);
    M32(o + 48) = (uint32_t)st.st_ino;
    RET(1, 2);
}
SHIM(CreatePipe) { rt_set_last_error(c, 5); RET(0, 4); }
SHIM(CreateProcessA) {
    LOG("CreateProcessA(%s, %s) refused", GSTR(ARG(0)) ? GSTR(ARG(0)) : "", GSTR(ARG(1)) ? GSTR(ARG(1)) : "");
    rt_set_last_error(c, 5);
    RET(0, 10);
}
SHIM(GetExitCodeProcess) { M32(ARG(1)) = 1; RET(1, 2); }
SHIM(SetFileAttributesA) { RET(1, 2); }
SHIM(RemoveDirectoryA) {
    char host[1024];
    rt_host_path(GSTR(ARG(0)), host, sizeof host);
    RET(rmdir(host) == 0, 1);
}
SHIM(SetFileTime) { RET(1, 4); }
SHIM(ExitThread) {
    LOG("ExitThread(%u)", ARG(0));
    GThread *t = t_gthread;
    if (!t) { fflush(NULL); _exit((int)ARG(0)); }
    t->exit_code = ARG(0);
    thread_exit_slot(c);
    w_signal(&t->w);
    pthread_exit(NULL);
}
static void put_systemtime(uint32_t o, uint64_t ft) {
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    M16(o) = (uint16_t)(tm.tm_year + 1900);
    M16(o + 2) = (uint16_t)(tm.tm_mon + 1);
    M16(o + 4) = (uint16_t)tm.tm_wday;
    M16(o + 6) = (uint16_t)tm.tm_mday;
    M16(o + 8) = (uint16_t)tm.tm_hour;
    M16(o + 10) = (uint16_t)tm.tm_min;
    M16(o + 12) = (uint16_t)tm.tm_sec;
    M16(o + 14) = (uint16_t)((ft / 10000) % 1000);
}
SHIM(GetSystemTime) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    put_systemtime(ARG(0), ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10);
    RET(0, 1);
}
SHIM(SystemTimeToFileTime) {
    uint32_t i = ARG(0);
    struct tm tm = {0};
    tm.tm_year = M16(i) - 1900;
    tm.tm_mon = M16(i + 2) - 1;
    tm.tm_mday = M16(i + 6);
    tm.tm_hour = M16(i + 8);
    tm.tm_min = M16(i + 10);
    tm.tm_sec = M16(i + 12);
    uint64_t ft = ((uint64_t)timegm(&tm) + 11644473600ull) * 10000000ull + (uint64_t)M16(i + 14) * 10000;
    M32(ARG(1)) = (uint32_t)ft;
    M32(ARG(1) + 4) = (uint32_t)(ft >> 32);
    RET(1, 2);
}
SHIM(CopyFileA) {
    char a[1024], b[1024];
    rt_host_path(GSTR(ARG(0)), a, sizeof a);
    rt_host_path(GSTR(ARG(1)), b, sizeof b);
    if (ARG(2) && access(b, F_OK) == 0) RET(0, 3);
    int in = open(a, O_RDONLY);
    if (in < 0) RET(0, 3);
    mkdirs_for(b);
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); RET(0, 3); }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) break;
    close(in);
    close(out);
    RET(n == 0, 3);
}

static const ShimDef k32[] = {
#define S(n) {"KERNEL32.dll", #n, shim_##n},
    S(TryEnterCriticalSection) S(InitializeCriticalSectionAndSpinCount) S(GetModuleFileNameW)
    S(GetModuleHandleW) S(GetStartupInfoW) S(HeapSetInformation) S(EncodePointer) S(DecodePointer)
    S(GetFileInformationByHandle) S(CreatePipe) S(CreateProcessA) S(GetExitCodeProcess)
    S(SetFileAttributesA) S(RemoveDirectoryA) S(SetFileTime) S(ExitThread) S(GetSystemTime)
    S(SystemTimeToFileTime) S(CopyFileA) S(GetFullPathNameW)
    S(ReadFileEx) S(WriteFileEx) S(SleepEx) S(CreateThread) S(ResumeThread) S(GetExitCodeThread) S(GetCurrentThreadId) S(GetCurrentProcessId)
    S(GetCurrentThread) S(GetCurrentProcess) S(SetThreadPriority) S(SetThreadAffinityMask)
    S(SetProcessAffinityMask) S(GetProcessAffinityMask) S(SetThreadExecutionState) S(Sleep)
    S(TlsAlloc) S(TlsFree) S(TlsGetValue) S(TlsSetValue)
    S(InitializeCriticalSection) S(DeleteCriticalSection) S(EnterCriticalSection) S(LeaveCriticalSection)
    S(CreateEventA) S(SetEvent) S(ResetEvent) S(CreateMutexA) S(ReleaseMutex) S(CreateSemaphoreA)
    S(ReleaseSemaphore) S(WaitForSingleObject) S(WaitForMultipleObjects) S(WaitForMultipleObjectsEx)
    S(CreateFileA) S(ReadFile) S(WriteFile) S(SetFilePointer) S(SetFilePointerEx) S(GetFileSize)
    S(GetFileType) S(FlushFileBuffers) S(SetEndOfFile) S(CloseHandle) S(GetFileAttributesA)
    S(GetFileAttributesExA) S(DeleteFileA) S(MoveFileA) S(CreateDirectoryA) S(FindFirstFileA)
    S(FindNextFileA) S(FindClose)
    S(GetProcessHeap) S(HeapCreate) S(HeapDestroy) S(HeapAlloc) S(HeapFree) S(HeapSize) S(HeapReAlloc)
    S(VirtualAlloc) S(VirtualFree)
    S(InterlockedIncrement) S(InterlockedDecrement) S(InterlockedExchange)
    S(GetTickCount) S(QueryPerformanceCounter) S(QueryPerformanceFrequency) S(GetSystemTimeAsFileTime)
    S(FileTimeToLocalFileTime) S(FileTimeToSystemTime) S(GetTimeZoneInformation)
    S(GetCommandLineA) S(GetEnvironmentStrings) S(GetEnvironmentStringsW) S(FreeEnvironmentStringsA)
    S(FreeEnvironmentStringsW) S(SetEnvironmentVariableA) S(ExpandEnvironmentStringsA) S(GetStartupInfoA)
    S(GetStdHandle) S(SetStdHandle) S(SetHandleCount) S(GetVersionExA) S(VerifyVersionInfoA)
    S(VerSetConditionMask) S(IsDebuggerPresent) S(GetLastError) S(SetLastError)
    S(SetUnhandledExceptionFilter) S(UnhandledExceptionFilter) S(SetConsoleCtrlHandler)
    S(OutputDebugStringA) S(OutputDebugStringW) S(DebugBreak) S(FatalAppExitA) S(ExitProcess)
    S(TerminateProcess) S(RaiseException) S(RtlUnwind)
    S(GetModuleHandleA) S(LoadLibraryA) S(LoadLibraryW) S(FreeLibrary) S(GetProcAddress) S(GetModuleFileNameA)
    S(GetConsoleCP) S(GetConsoleOutputCP) S(GetConsoleMode) S(WriteConsoleA) S(WriteConsoleW)
    S(GetACP) S(GetOEMCP) S(IsValidCodePage) S(GetCPInfo) S(MultiByteToWideChar) S(WideCharToMultiByte)
    S(GetUserDefaultLCID) S(GetUserDefaultLangID) S(GetUserGeoID) S(IsValidLocale) S(EnumSystemLocalesA)
    S(GetLocaleInfoA) S(GetLocaleInfoW) S(GetStringTypeA) S(GetStringTypeW) S(LCMapStringA) S(LCMapStringW)
    S(CompareStringA) S(CompareStringW) S(GetDateFormatA) S(GetTimeFormatA)
    S(FindResourceA) S(FindResourceW) S(LoadResource) S(LockResource) S(SizeofResource) S(FreeResource) S(CreateFileW) S(DeleteFileW) S(MoveFileW) S(CreateFileMappingA) S(MapViewOfFile) S(UnmapViewOfFile) S(GetFileSizeEx) S(GetFullPathNameA) S(GetTempPathA) S(GetTempFileNameA) S(GetTempFileNameW) S(FormatMessageA) S(LocalFree) S(InterlockedCompareExchange) S(lstrcmpiA) S(IsDBCSLeadByte) S(IsProcessorFeaturePresent) S(GetVersion) S(GetSystemInfo) S(GlobalMemoryStatus) S(DisableThreadLibraryCalls) S(LocalAlloc) S(GetWindowsDirectoryA) S(GetSystemDirectoryA) S(SetErrorMode) S(IsBadReadPtr) S(IsBadWritePtr) S(IsBadCodePtr)
#undef S
    {NULL, NULL, NULL}
};

void k32_init(void) {
    rt_register_shims(k32);
    /* command line, environment, PEB */
    uint32_t m = GUEST_MISC_BASE;
    memset(MEM + m, 0, 0x10000);
    M32(m + 8) = GUEST_IMAGE_BASE;          /* PEB.ImageBaseAddress */
    g_cmdline = m + 0x1000;
    strcpy((char *)MEM + g_cmdline, "\"" GAME_WIN_DIR "\\" GAME_EXE "\"");
    g_env = m + 0x1100;                     /* "A=B\0\0" */
    strcpy((char *)MEM + g_env, "OS=Windows_NT");
    g_envw = m + 0x1200;
    const char *e = "OS=Windows_NT";
    for (int i = 0; e[i]; i++) M16(g_envw + 2 * i) = (uint8_t)e[i];
}

void k32_set_event(uint32_t h) {
    Waitable *w = h_get(h, H_EVENT);
    if (w) w_signal(w);
}
