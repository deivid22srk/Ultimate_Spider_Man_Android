/* Host platform layer on SDL2: window (Vulkan surface for DXVK), event pump.
 * The same code runs on desktop Linux and on Android. */
#include "game.h"
#include "rt.h"
#include <SDL2/SDL.h>
#ifndef __ANDROID__
#include <fcntl.h>
#endif

static SDL_Window *g_window;
static int g_frames;
static void load_key_script(void);

void platform_init(void) {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
        rt_fatal("SDL_Init: %s", SDL_GetError());
#ifdef __ANDROID__
    /* Custom GPU driver, if the launcher picked one. Before this point SDL has
     * no video device to hand the loader to; after it, nothing has touched
     * Vulkan yet, which is the window where both the renderer and the surface
     * can still be pointed at the same loader. */
    {
        extern int usm_driver_open(const char *, const char *, void **, void **);
        extern void dxvk_vulkan_set_get_instance_proc_addr(void *);
        extern void SDL_Vulkan_SetExternalLoader(void *, void *);
        extern char g_driver_dir[1024], g_native_lib_dir[1024];
        void *drv = NULL, *gipa = NULL;
        if (usm_driver_open(g_driver_dir, g_native_lib_dir, &drv, &gipa)) {
            dxvk_vulkan_set_get_instance_proc_addr(gipa);
            SDL_Vulkan_SetExternalLoader(drv, gipa);
        }
    }
#endif
    int w = 1280, h = 720;
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (getenv("LB_WINDOW")) sscanf(getenv("LB_WINDOW"), "%dx%d", &w, &h);  /* desktop testing */
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { w = dm.w; h = dm.h; }
#endif
    g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!g_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    extern void user32_set_screen(int, int);
    user32_set_screen(w, h);
    LOG("platform: window %dx%d", w, h);
    load_key_script();
}

void *platform_window(void) { return g_window; }

/* Scripted input for automated runs: LB_KEYS="frame:Key[:frames],..." holds
 * the SDL-named key (e.g. Return, Space, Left) from that frame on. */
typedef struct { int frame, len; SDL_Scancode sc; } KeyEv;
static KeyEv g_script[256];
static int g_nscript;
static Uint8 g_keys[SDL_NUM_SCANCODES];
static int g_padonly;  /* LB_PADONLY=1: scripted keys drive only the virtual XInput pad */

static void load_key_script(void) {
    const char *s = getenv("LB_KEYS");
    g_padonly = getenv("LB_PADONLY") && *getenv("LB_PADONLY") == '1';
    while (s && *s && g_nscript < 256) {
        char name[32] = "";
        int fr = 0, len = 10, n = 0;
        if (sscanf(s, "%d:%31[^:,]%n", &fr, name, &n) < 2) break;
        s += n;
        if (*s == ':') { s++; len = (int)strtol(s, (char **)&s, 10); }
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        if (sc != SDL_SCANCODE_UNKNOWN) g_script[g_nscript++] = (KeyEv){fr, len, sc};
        else LOG("LB_KEYS: unknown key %s", name);
        if (*s == ',') s++;
    }
}

/* SDL scancode -> Windows virtual-key code */
static uint32_t vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    default: return 0;
    }
}

/* Virtual keyboard/mouse state fed by the touch overlay and the physical
 * pad (games that only read DirectInput's keyboard, like USM). */
static uint8_t g_vdik[256];
static int g_mdx, g_mdy;
void platform_vk_set(uint32_t dik, int down) {
    if (dik < 256) g_vdik[dik] = down ? 0x80 : 0;
}
const uint8_t *platform_virtual_dik(void) { return g_vdik; }
void platform_mouse_add(int dx, int dy) { g_mdx += dx; g_mdy += dy; }
void platform_mouse_take(int *dx, int *dy) {
    *dx = g_mdx; *dy = g_mdy;
    g_mdx = g_mdy = 0;
}

static void key_event(SDL_Scancode sc, int down) {
    extern void user32_key_event(uint32_t vk, uint32_t sc, int down);
    uint32_t vk = vk_of(sc);
    /* PC scan code: approximate with the DirectInput code, which matches set 1 */
    extern uint32_t dinput_dik_of(int sdl_scancode);
    user32_key_event(vk, dinput_dik_of(sc), down);
}

/* Emits key events for scripted keys whose state changed this frame. */
static void script_events(void) {
    static Uint8 prev[SDL_NUM_SCANCODES];
    if (!g_nscript) return;
    Uint8 cur[SDL_NUM_SCANCODES] = {0};
    for (int i = 0; i < g_nscript; i++)
        if (g_frames >= g_script[i].frame && g_frames < g_script[i].frame + g_script[i].len) cur[g_script[i].sc] = 1;
    for (int i = 0; i < g_nscript; i++) {
        SDL_Scancode sc = g_script[i].sc;
        if (cur[sc] != prev[sc]) {
            if (!g_padonly) key_event(sc, cur[sc]);
            SDL_Event e = {0};
            e.type = cur[sc] ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.keysym.scancode = sc;
            e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
            extern void input_handle_event(const SDL_Event *e);
            input_handle_event(&e);
            prev[sc] = cur[sc];
        }
    }
}

const Uint8 *platform_keys(int *n) {
    int nk;
    const Uint8 *ks = SDL_GetKeyboardState(&nk);
    if (!g_nscript || g_padonly) { if (n) *n = nk; return ks; }
    memcpy(g_keys, ks, nk < SDL_NUM_SCANCODES ? nk : SDL_NUM_SCANCODES);
    for (int i = 0; i < g_nscript; i++)
        if (g_frames >= g_script[i].frame && g_frames < g_script[i].frame + g_script[i].len) g_keys[g_script[i].sc] = 1;
    if (n) *n = SDL_NUM_SCANCODES;
    return g_keys;
}

/* Interactive test input: LB_KEYFIFO=path, lines "Key[:frames]" are queued
 * at the current frame (desktop testing only). */
static void poll_key_fifo(void) {
#ifndef __ANDROID__
    static FILE *fifo;
    static int tried;
    if (!tried) {
        tried = 1;
        const char *p = getenv("LB_KEYFIFO");
        if (p) {
            int fd = open(p, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) fifo = fdopen(fd, "r");
        }
    }
    if (!fifo) return;
    char line[64];
    while (fgets(line, sizeof line, fifo)) {
        char name[32] = "";
        int len = 8;
        if (!strncmp(line, "dump ", 5)) {  /* "dump <file>": save the exe's data sections */
            line[strcspn(line, "\n")] = 0;
            FILE *d = fopen(line + 5, "wb");
            if (d) { fwrite(MEM + 0x934000, 1, 0x20E2490, d); fclose(d); }
            LOG("keyfifo: dumped data to %s at frame %d", line + 5, g_frames);
            continue;
        }
        if (sscanf(line, "%31[^:\n]:%d", name, &len) < 1) continue;
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        if (sc == SDL_SCANCODE_UNKNOWN || g_nscript >= 256) continue;
        g_script[g_nscript++] = (KeyEv){g_frames + 1, len, sc};
        LOG("keyfifo: %s at frame %d", name, g_frames + 1);
    }
    clearerr(fifo);
#endif
}

/* Pumps and dispatches SDL events. Called from Present *and* from
 * PeekMessageA: when the game loses activation it stops presenting and spins
 * on PeekMessageA, so pumping only from Present meant it never saw the
 * "back in the foreground" event and stayed frozen on the last frame. */
void platform_pump_events(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint32_t last_ms;
    uint32_t now = SDL_GetTicks();
    if (now == last_ms) return;            /* at most once a millisecond */
    if (pthread_mutex_trylock(&mu)) return;
    last_ms = now;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            LOG("quit requested");
            fflush(NULL);
            exit(0);
        }
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
            key_event(e.key.keysym.scancode, e.type == SDL_KEYDOWN);
        if (e.type == SDL_WINDOWEVENT) {
            extern void user32_focus(int);
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) user32_focus(1);
            else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) user32_focus(0);
        }
        /* Android pause/resume: SDL reports these even when the window never
         * sees a focus event (the app is paused, so nothing presents). */
        if (e.type == SDL_APP_DIDENTERFOREGROUND) {
            extern void user32_focus(int);
            user32_focus(1);
        }
        if (e.type == SDL_APP_WILLENTERBACKGROUND) {
            extern void user32_focus(int);
            user32_focus(0);
        }
        extern void input_handle_event(const SDL_Event *e);
        input_handle_event(&e);
        extern void touch_handle_event(const SDL_Event *e);
        touch_handle_event(&e);
    }
#ifdef GAME_USM
    { extern void usm_input_update(void); usm_input_update(); }
#endif
    pthread_mutex_unlock(&mu);
}

void platform_present_hook(void) {
    poll_key_fifo();
    platform_pump_events();
    {   /* The window only gets its real size once it is fullscreen, and the
         * game's client space -- GetClientRect, and the cursor the UI divides
         * by it -- must follow it (user32.c owns that space). */
        static int lw, lh;
        int ww = 0, wh = 0;
        if (g_window) SDL_GetWindowSize(g_window, &ww, &wh);
        if (ww > 0 && wh > 0 && (ww != lw || wh != lh)) {
            lw = ww; lh = wh;
            user32_set_screen(ww, wh);
            LOG("platform: client %dx%d", ww, wh);
        }
    }
    script_events();
    g_frames++;
#ifdef GAME_SKYRIM
    /* bGamepadEnable (Controls menu toggle, not read from the INI at boot):
     * always on, the phone's pad and touch overlay are XInput pads. */
    M8(0x12C3650) = 1;
#endif
    if (g_frames % 300 == 0) LOG("frame %d", g_frames);
    {   /* canary: the main thread's static TLS pointer (TEB slot 0 +0x2C) */
        static uint32_t last;
        uint32_t v = M32(GUEST_TEB_BASE + 0x2C);
        if (last && v != last)
            fprintf(stderr, "frame %d: main TEB+0x2C changed %08x -> %08x (TEB+0x24 tid %u, +0x1000 %08x)\n",
                    g_frames, last, v, M32(GUEST_TEB_BASE + 0x24), M32(GUEST_TEB_BASE + 0x1000));
        last = v;
    }
}
void platform_window_size(int *w, int *h) { if (g_window) SDL_GetWindowSize(g_window, w, h); }
int platform_frame_count(void) { return g_frames; }
