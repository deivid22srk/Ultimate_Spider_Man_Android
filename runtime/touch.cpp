// On-screen touch controls: a virtual Xbox pad (left stick, A/B/X/Y,
// shoulders, Start/Back) that feeds the game's XInput input, drawn over
// each frame with D3D9 just before Present.
//
// Available on Android; on desktop enable with LB_TOUCH=1 (mouse clicks
// then act as touches for testing). Where available, the PC-only "Vertical
// Sync" item in Options > Video Settings becomes "Touch Controls" and shows
// or hides the overlay (vsync itself is then always on, see d3d9_bridge.cpp).
#include "d3d9_bridge.h"
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <cmath>
#include <string>

extern "C" void input_set_virtual(uint16_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                                  uint8_t lt, uint8_t rt);

namespace {
// Layout in units of screen height, anchored to a corner (ax, ay in {0,1}).
struct Button { const char *name; uint16_t bit; float ax, ay, dx, dy, r; uint32_t color; };
const Button k_buttons[] = {
    {"A", 0x1000, 1, 1, -0.20f, -0.14f, 0.075f, 0xFF40C040},
    {"B", 0x2000, 1, 1, -0.08f, -0.27f, 0.075f, 0xFFE04040},
    {"X", 0x4000, 1, 1, -0.32f, -0.27f, 0.075f, 0xFF4080F0},
    {"Y", 0x8000, 1, 1, -0.20f, -0.40f, 0.075f, 0xFFF0D040},
    {"LB", 0x0100, 0, 0, 0.12f, 0.30f, 0.06f, 0xFFB0B0B0},
    {"RB", 0x0200, 1, 0, -0.12f, 0.30f, 0.06f, 0xFFB0B0B0},
    {"Back", 0x0020, 0, 0, 0.36f, 0.07f, 0.045f, 0xFFD0D0D0},
    {"Start", 0x0010, 1, 0, -0.36f, 0.07f, 0.045f, 0xFFFFFFFF},
};
const int NB = sizeof k_buttons / sizeof k_buttons[0];
const float STICK_X = 0.24f, STICK_Y = -0.26f, STICK_R = 0.17f; // from bottom-left

struct Finger { SDL_FingerID id; int button; bool stick; bool look; bool ptr; float x, y; bool used; };
Finger g_fingers[10];
float g_stick_x, g_stick_y;   // knob offset, -1..1
bool g_available, g_decided, g_seen, g_was_on;
bool g_touch_on = true;       // the player's choice, kept in <save>/touch.cfg
uint16_t g_buttons;
float g_aspect = 16.f / 9.f;  // width / height of the screen
// Touch pointer (GAME_USM): where the last pointer finger landed, for the
// on-screen marker.
float g_ptr_x = 0.5f, g_ptr_y = 0.5f;
uint32_t g_ptr_ms;

// The game's VSync option byte, which the menu toggles and the settings save.
const uint32_t G_VSYNC_OPTION = 0x9D0F3C;
// Aspect ratio: the setting (pcconfig "Widescreen") and the menu's copy of it.
// Its menu row is Skip Cutscenes now, so it stays at 3, "From Screen-Res"
// (0 = 4:3, the game's default, 1 = 16:9, 2 = 16:10).
const uint32_t G_ASPECT = 0x94C070, G_ASPECT_MENU = 0xAD7294, ASPECT_FROM_RES = 3;

bool available() {
    if (!g_decided) {
        g_decided = true;
#ifdef __ANDROID__
        g_available = true;
#else
        const char *e = getenv("LB_TOUCH");
        g_available = e && atoi(e);
#endif
    }
    return g_available;
}

bool enabled() {
    if (!available()) return false;
    // The game loads and resets its own VSync value; ours always wins, and only
    // the menu changes it (f_6C42C0 below).
#ifdef GAME_BATMAN
    M8(G_VSYNC_OPTION) = g_touch_on;
    M32(G_ASPECT_MENU) = ASPECT_FROM_RES;
#endif
    bool on = g_touch_on;
    if (g_was_on && !on) {
        // Switched off in the menu: release everything the overlay was holding.
        for (auto &g : g_fingers) g.used = false;
        g_stick_x = g_stick_y = 0;
        g_buttons = 0;
        input_set_virtual(0, 0, 0, 0, 0, 0, 0);
#ifdef GAME_USM
        { extern void user32_pointer_fingers(int); user32_pointer_fingers(0); } // let go of the left button
#endif
    }
    g_was_on = on;
    return on;
}

// Centre of a button in normalised screen coordinates (0..1 both axes).
void button_pos(const Button &b, float *x, float *y) {
    *x = b.ax + b.dx / g_aspect;
    *y = b.ay + b.dy;
}

void stick_pos(float *x, float *y) {
    *x = STICK_X / g_aspect;
    *y = 1 + STICK_Y;
}

int hit_button(float x, float y) {
    for (int i = 0; i < NB; i++) {
        float bx, by;
        button_pos(k_buttons[i], &bx, &by);
        float dx = (x - bx) * g_aspect, dy = y - by, r = k_buttons[i].r * 1.3f;
        if (dx * dx + dy * dy < r * r) return i;
    }
    return -1;
}

void update() {
    uint16_t b = 0;
    float sx = 0, sy = 0;
    for (auto &f : g_fingers) {
        if (!f.used) continue;
        if (f.stick) {
            float cx, cy;
            stick_pos(&cx, &cy);
            sx = (f.x - cx) * g_aspect / STICK_R;
            sy = (f.y - cy) / STICK_R;
            float l = std::sqrt(sx * sx + sy * sy);
            if (l > 1) { sx /= l; sy /= l; }
        } else {
            // a finger may slide between buttons
            int h = hit_button(f.x, f.y);
            if (h >= 0) f.button = h;
            if (f.button >= 0) b |= k_buttons[f.button].bit;
        }
    }
    g_buttons = b;
    g_stick_x = sx;
    g_stick_y = sy;
    input_set_virtual(b, (int16_t)(sx * 32767), (int16_t)(-sy * 32767), 0, 0, 0, 0);
}
} // namespace

extern "C" void touch_handle_event(const SDL_Event *e) {
    if (!enabled()) return;
    float x, y;
    SDL_FingerID id;
    bool down = false, up = false;
    switch (e->type) {
    case SDL_FINGERDOWN: down = true; /* fallthrough */
    case SDL_FINGERMOTION:
        x = e->tfinger.x; y = e->tfinger.y; id = e->tfinger.fingerId; break;
    case SDL_FINGERUP:
        up = true; x = e->tfinger.x; y = e->tfinger.y; id = e->tfinger.fingerId; break;
#ifndef __ANDROID__
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: {
        int w, h;
        SDL_GetWindowSize(SDL_GetWindowFromID(e->button.windowID), &w, &h);
        if (e->type == SDL_MOUSEMOTION && !(e->motion.state & SDL_BUTTON_LMASK)) return;
        x = (float)(e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x) / w;
        y = (float)(e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y) / h;
        id = 1;
        down = e->type == SDL_MOUSEBUTTONDOWN;
        up = e->type == SDL_MOUSEBUTTONUP;
        break;
    }
#endif
    default:
        return;
    }
    g_seen = true;
    Finger *f = nullptr;
    for (auto &g : g_fingers) if (g.used && g.id == id) f = &g;
    if (down && !f) {
        for (auto &g : g_fingers) if (!g.used) { f = &g; break; }
        if (!f) return;
        f->used = true;
        f->id = id;
        f->button = hit_button(x, y);
        f->ptr = false;
        // anything on the left half that is not a button drives the stick
        f->stick = f->button < 0 && x < 0.5f;
#ifdef GAME_USM
        /* USM has no pad support: the overlay presses keys, and a finger on
         * the right half drags the camera (the game reads the mouse). */
        f->stick = f->button < 0 && x < 0.45f;
        f->look = f->button < 0 && x >= 0.45f;
        f->ptr = f->button < 0;
#endif
    }
    if (!f) return;
#ifdef GAME_USM
    /* USM's menus hit-test widgets against the *system cursor* (GetCursorPos)
     * and its window procedure ignores every mouse message, so a touch that is
     * not on an overlay button warps the virtual cursor and holds the left
     * button: the menus become tappable and the in-game prompts follow the
     * finger. The left button is bound to nothing in game, so the same gesture
     * is harmless there. */
    extern void user32_screen(int *, int *);
    extern void user32_cursor_set(int, int);
    extern void user32_mouse_button(int, int);
    extern void user32_pointer_fingers(int);
    if (f->ptr && down) {
        int w = 1280, h = 720;
        user32_screen(&w, &h);
        user32_cursor_set((int)(x * w), (int)(y * h));
        g_ptr_x = x;
        g_ptr_y = y;
        g_ptr_ms = SDL_GetTicks();
        user32_mouse_button(0, 1);
    }
    if (!down && !up && f->look) {
        int w = 0, h = 0;
        extern void platform_window_size(int *, int *);
        extern void platform_mouse_add(int, int);
        platform_window_size(&w, &h);
        platform_mouse_add((int)((x - f->x) * w * 2.f), (int)((y - f->y) * h * 2.f));
        g_ptr_x = x;
        g_ptr_y = y;
        g_ptr_ms = SDL_GetTicks();
    }
#endif
    f->x = x;
    f->y = y;
    if (up) f->used = false;
#ifdef GAME_USM
    {   /* the button follows the fingers that are actually down (see user32.c) */
        int n = 0;
        for (auto &g : g_fingers)
            if (g.used && g.ptr) n++;
        user32_pointer_fingers(n);
    }
#endif
    update();
}

// ---------------------------------------------------------------- drawing
namespace {
IDirect3DTexture9 *g_tex;      // soft-edged disc
IDirect3DStateBlock9 *g_state;

struct Vtx { float x, y, z, rhw; uint32_t color; float u, v; };

void make_texture(IDirect3DDevice9 *d) {
    const int N = 64;
    if (FAILED(d->CreateTexture(N, N, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(g_tex->LockRect(0, &lr, nullptr, 0))) return;
    for (int y = 0; y < N; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < N; x++) {
            float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1;
            float r = std::sqrt(dx * dx + dy * dy);
            // translucent fill with a brighter rim
            float a = r > 1 ? 0 : r > 0.86f ? 1 - (r - 0.86f) / 0.14f * 0.3f : 0.45f;
            if (r > 0.97f) a *= (1 - r) / 0.03f;
            row[x] = (uint32_t)(a * 255) << 24 | 0xFFFFFF;
        }
    }
    g_tex->UnlockRect(0);
}

void quad(Vtx *v, float cx, float cy, float r, float w, float h, uint32_t color) {
    float x0 = (cx - r / g_aspect) * w, x1 = (cx + r / g_aspect) * w;
    float y0 = (cy - r) * h, y1 = (cy + r) * h;
    Vtx q[4] = {{x0, y0, 0, 1, color, 0, 0}, {x1, y0, 0, 1, color, 1, 0},
                {x0, y1, 0, 1, color, 0, 1}, {x1, y1, 0, 1, color, 1, 1}};
    v[0] = q[0]; v[1] = q[1]; v[2] = q[2];
    v[3] = q[2]; v[4] = q[1]; v[5] = q[3];
}
} // namespace

void touch_draw(IDirect3DDevice9 *d) {
    if (!enabled()) return;
    IDirect3DSurface9 *bb = nullptr, *old = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    float w = (float)desc.Width, h = (float)desc.Height;
    g_aspect = w / h;
    if (!g_tex) make_texture(d);
    if (!g_state) d->CreateStateBlock(D3DSBT_ALL, &g_state);
    if (!g_tex || !g_state) { bb->Release(); return; }
    g_state->Capture();
    d->GetRenderTarget(0, &old);
    d->SetRenderTarget(0, bb);
    D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0, 1};
    d->SetViewport(&vp);
    d->BeginScene();
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    d->SetTexture(0, g_tex);
    for (int s = 1; s < 8; s++) d->SetTexture(s, nullptr);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    Vtx v[6 * (NB + 4)];
    int n = 0;
    float sx, sy;
    stick_pos(&sx, &sy);
    quad(v + 6 * n++, sx, sy, STICK_R, w, h, 0x90FFFFFF);
    quad(v + 6 * n++, sx + g_stick_x * STICK_R / g_aspect, sy + g_stick_y * STICK_R, STICK_R * 0.45f, w, h,
         0xD0FFFFFF);
    for (int i = 0; i < NB; i++) {
        float bx, by;
        button_pos(k_buttons[i], &bx, &by);
        uint32_t col = k_buttons[i].color;
        uint32_t a = (g_buttons & k_buttons[i].bit) ? 0xF0 : 0x90;
        quad(v + 6 * n++, bx, by, k_buttons[i].r, w, h, (col & 0xFFFFFF) | a << 24);
    }
#ifdef GAME_USM
    /* Where the finger last landed: the menus are clicked through the virtual
     * system cursor, so this tells the player where that cursor now is (the
     * game's own pointer, when it draws one, sits at the same spot). */
    uint32_t age = SDL_GetTicks() - g_ptr_ms;
    if (g_ptr_ms && age < 700) {
        uint32_t a = (uint32_t)((700 - age) * 0xC0 / 700);
        quad(v + 6 * n++, g_ptr_x, g_ptr_y, 0.032f, w, h, 0xFFFFFF | a << 24);
        quad(v + 6 * n++, g_ptr_x, g_ptr_y, 0.013f, w, h, 0xFFFFFF | a << 24);
    }
#endif
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2 * n, v, sizeof(Vtx));
    d->EndScene();
    d->SetRenderTarget(0, old);
    if (old) old->Release();
    bb->Release();
    g_state->Apply();
}

extern "C" int touch_available(void) { return available(); }

static std::string cfg_path() { return std::string(g_save_dir) + "/touch.cfg"; }

// Port settings kept beside the saves: "<touch controls> <nintendo layout> <skip cutscenes>".
static bool g_nintendo, g_skip_cutscenes;
extern "C" void input_set_nintendo(int on);
extern "C" void input_set_skip_cutscenes(int on);
static void save_cfg() {
    if (FILE *f = fopen(cfg_path().c_str(), "w")) {
        fprintf(f, "%d %d %d\n", g_touch_on, g_nintendo, g_skip_cutscenes);
        fclose(f);
    }
}
static uint32_t guest_str(const char *s) {
    uint32_t g = gmalloc((uint32_t)strlen(s) + 1);
    memcpy(MEM + g, s, strlen(s) + 1);
    return g;
}

#ifdef GAME_MARVEL
// Video Settings in LEGO Marvel (widget menu at 0x8E9F60 / input 0x8E31B0).
// The display mode object at 0x1952470 holds the mode list (+0x1B8, 16-byte
// {w, h, refresh, ?} entries), its count (+0x1B4), the current mode (+0x1D4)
// and VSync (+0x1D8: 0 off, 1 on, 2 adaptive). Rows: 0 Screen Resolution
// (shown as Resolution Scale), 1 Refresh Rate (now Nintendo Button Layout),
// 2 VSync (now Touch Controls), 3 Aspect Ratio (held at From Screen-Res).
// Labels come from the text lookup 0x584500 by key; ours replace four keys.
const uint32_t M_MODES = 0x1952628, M_MODE = 0x1952644, M_VSYNC = 0x1952648;
const uint32_t M_ASPECT_MENU = 0x196C97C, M_TEXT_ON = 0x19937E8, M_TEXT_OFF = 0x19937EC;
const uint32_t M_SET_TEXT = 0x90D230;  // thiscall widget_text::set(const char *)
static uint32_t g_scale_text[11];      // "0.0" .. "1.0"
static int g_in_refresh_step;          // refresh prev/next pressed on the Nintendo row

static uint32_t guest_text(const char *s);
static void set_widget_text(CPU *c, uint32_t widget, uint32_t text) {
    if (!widget || !text) return;
    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2];
    c->r[1] = widget + 0x16C;
    rt_invoke(c, M_SET_TEXT, 1, &text);
    c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx;
}

// Text lookup by key (cdecl key, flag -> char *).
extern "C" void f_584500_guest(CPU *c);
extern "C" void f_584500(CPU *c) {
    static const struct { const char *key, *text; } k_relabel[] = {
        {"PCSTRING_VIDEOSETTINGS", "Video and Port Settings"},
        {"PC_SETTINGS_VIDEO", "Video and Port Settings"},
        {"PCSTRING_SCREEN_RES", "Resolution Scale"},
        {"PCSTRING_REFRESHRATE", "Nintendo Button Layout"},
        {"PCSTRING_VERTICALSYNC", "Touch Controls"},
    };
    static uint32_t s_text[5];
    uint32_t key = ARG(0);
    f_584500_guest(c);
    if (!key || !available()) return;
    const char *k = (const char *)MEM + key;
    for (int i = 0; i < 5; i++)
        if (!strcmp(k, k_relabel[i].key)) {
            if (!s_text[i]) s_text[i] = guest_text(k_relabel[i].text);
            c->r[0] = s_text[i];
        }
}

// Values: the game writes each row's text, then ours overwrite rows 0-2.
extern "C" void f_8E9F60_guest(CPU *c);
extern "C" void f_8E9F60(CPU *c) {
    uint32_t self = c->r[1];
    if (available()) M32(M_ASPECT_MENU) = 3;
    f_8E9F60_guest(c);
    if (!available() || !self || !M32(M_MODES)) return;
    uint32_t rows = M32(self + 0x40);
    if (!rows) return;
    uint32_t w = M32(M32(M_MODES) + M32(M_MODE) * 16);
    int ww = 1280, wh = 720;
    SDL_GetWindowSize((SDL_Window *)platform_window(), &ww, &wh);
    int tenths = ww > 0 ? (int)((w * 10 + ww / 2) / ww) : 10;
    if (tenths < 0) tenths = 0;
    if (tenths > 10) tenths = 10;
    if (!g_scale_text[tenths]) {
        char b[8];
        snprintf(b, sizeof b, "%d.%d", tenths / 10, tenths % 10);
        g_scale_text[tenths] = guest_text(b);
    }
    uint32_t on = M32(M_TEXT_ON), off = M32(M_TEXT_OFF);
    set_widget_text(c, M32(rows + 0), g_scale_text[tenths]);
    set_widget_text(c, M32(rows + 4), g_nintendo ? on : off);
    set_widget_text(c, M32(rows + 8), g_touch_on ? on : off);
}

// Input: left/right/select on the Refresh row step the refresh rate
// (0x6D3600 / 0x6D3670, thiscall on the mode object); on the VSync row they
// cycle the VSync value. Both become toggles of our settings instead.
extern "C" void f_6D3600_guest(CPU *c);
// a swallowed call still pops its return address, as the guest's ret would
extern "C" void f_6D3600(CPU *c) { if (!g_in_refresh_step) { f_6D3600_guest(c); return; } g_in_refresh_step = 2; c->r[4] += 4; }
extern "C" void f_6D3670_guest(CPU *c);
extern "C" void f_6D3670(CPU *c) { if (!g_in_refresh_step) { f_6D3670_guest(c); return; } g_in_refresh_step = 2; c->r[4] += 4; }
extern "C" void f_8E31B0_guest(CPU *c);
extern "C" void f_8E31B0(CPU *c) {
    if (!available()) { f_8E31B0_guest(c); return; }
    uint32_t vsync = M32(M_VSYNC);
    g_in_refresh_step = 1;
    f_8E31B0_guest(c);
    bool changed = false;
    if (g_in_refresh_step == 2) {
        g_nintendo = !g_nintendo;
        input_set_nintendo(g_nintendo);
        changed = true;
    }
    g_in_refresh_step = 0;
    if (M32(M_VSYNC) != vsync) {
        M32(M_VSYNC) = vsync;
        g_touch_on = !g_touch_on;
        changed = true;
    }
    M32(M_ASPECT_MENU) = 3;
    if (changed) save_cfg();
}

// pcconfig "Widescreen"/"AspectRatio" setter (cdecl, pointer to the config
// value): the aspect lands in 0x1470FCC; 3 (From Screen-Res) applies the
// screen's aspect 0x1952638 through thiscall 0x5A8320 on 0x183C850.
const uint32_t M_ASPECT = 0x1470FCC, M_SCREEN_ASPECT = 0x1952638;
const uint32_t M_APPLY_ASPECT = 0x5A8320, M_ASPECT_OBJ = 0x183C850;
extern "C" void f_6D8F00_guest(CPU *c);
extern "C" void f_6D8F00(CPU *c) {
    f_6D8F00_guest(c);
    if (!available() || M32(M_ASPECT) == 3) return;
    uint32_t eax = c->r[0], ecx = c->r[1], a = M32(M_SCREEN_ASPECT);
    M32(M_ASPECT) = 3;
    c->r[1] = M_ASPECT_OBJ;
    rt_invoke(c, M_APPLY_ASPECT, 1, &a);
    c->r[0] = eax; c->r[1] = ecx;
}

// Graphics quality starts at its lowest on every launch (the phone can't load
// the high-quality shader set in reasonable time). pcconfig setters and the
// vendor-based defaults picker (0x6DAF30) run as usual, then these win.
// Shader 0x1470FD0/0x1470FD8 (+0x138C4E6, 0x183D6E0 bit 0x4000000),
// shadow 0x1470FD4 (setter clamps to 1..3), texture 0x1470FDC, and in
// 0x183D6E4: FSAA/EdgeAA 0x200000, soft particles 0x80000 and the effects
// (bloom 0x2, process colour 0x4, DoF 0x8, exposure 0x10, soft shadows 0x100,
// SSR 0x200, SSAO 0x400, speed blur 0x1000, motion blur 0x2000, ring 0x4000,
// lens filter 0x8000, water distortion 0x10000, edge highlight 0x20000000).
// AllowVendorExtensions (0x1471010) is off too.
// Low is the default: on the first launch (no save/low_defaults_v3_done) every
// setting is forced low as pcconfig.txt loads, and pcconfig.txt itself is
// rewritten to low, so later launches start low and keep menu changes. The
// vendor presets picker (0x6DAF30, the game's own "defaults") is always low.
static const struct { const char *key; int value; } k_low_cfg[] = {
    {"ShaderQuality", 0}, {"ShadowQuality", 1}, {"TextureQuality", 0}, {"FSAA", 0}, {"EdgeAA", 0},
    {"EdgeAAEnabled", 0}, {"SoftParticles", 0}, {"BloomEnabled", 0}, {"ExposureControlEnabled", 0},
    {"DoFEnabled", 0}, {"SoftShadowsEnabled", 0}, {"SpeedBlurEnabled", 0}, {"MotionBlurEnabled", 0},
    {"ProcessColourEnabled", 0}, {"WaterDistortionEnabled", 0}, {"SSAOEnabled", 0},
    {"LensFilterEnabled", 0}, {"RingEffectEnabled", 0}, {"ScreenSpaceReflectionsEnabled", 0},
    {"EdgeHightlightEnabled", 0}, {"AllowVendorExtensions", 0}, {"IgnoreVendorPresets", 1},
};

static bool find_ci(const std::string &dir, const char *name, std::string &out, int depth) {
    DIR *d = opendir(dir.c_str());
    if (!d) return false;
    bool hit = false;
    while (struct dirent *de = readdir(d)) {
        if (de->d_name[0] == '.') continue;
        std::string p = dir + "/" + de->d_name;
        if (!strcasecmp(de->d_name, name)) { out = p; hit = true; break; }
        struct stat st;
        if (depth > 0 && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
            strcasecmp(de->d_name, "CachedShaders") && find_ci(p, name, out, depth - 1)) { hit = true; break; }
    }
    closedir(d);
    return hit;
}

static void rewrite_pcconfig_low() {
    std::string path;
    if (!find_ci(g_save_dir, "pcconfig.txt", path, 6)) { LOG("low defaults: no pcconfig.txt yet"); return; }
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::string text, line, out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    bool seen[sizeof k_low_cfg / sizeof k_low_cfg[0]] = {};
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        size_t k0 = line.find_first_not_of(" \t"), k1 = line.find_first_of(" \t=", k0);
        if (k0 != std::string::npos && k1 != std::string::npos) {
            std::string key = line.substr(k0, k1 - k0);
            for (size_t i = 0; i < sizeof k_low_cfg / sizeof k_low_cfg[0]; i++)
                if (!strcasecmp(key.c_str(), k_low_cfg[i].key)) {
                    size_t v0 = line.find_first_not_of(" \t=", k1);
                    size_t v1 = v0 == std::string::npos ? std::string::npos : line.find_first_of(" \t\r", v0);
                    std::string tail = v1 == std::string::npos ? (line.size() && line.back() == '\r' ? "\r" : "") : line.substr(v1);
                    line = line.substr(0, v0 == std::string::npos ? line.size() : v0) + std::to_string(k_low_cfg[i].value) + tail;
                    seen[i] = true;
                }
        }
        out += line;
        if (e != std::string::npos) out += '\n';
    }
    if ((f = fopen(path.c_str(), "wb"))) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
    int missing = 0;
    for (bool s : seen) missing += !s;
    LOG("low defaults: rewrote %s (%d keys not present)", path.c_str(), missing);
}

static void restore_pcconfig_defaults() {
    std::string path;
    if (!find_ci(g_save_dir, "pcconfig.txt", path, 6)) { LOG("defaults: no pcconfig.txt yet"); return; }
    std::string bak = path.substr(0, path.size() - 12) + "pcconfig_low.bak";
    LOG("defaults: moved %s aside (%s) so the game uses its own defaults",
        path.c_str(), rename(path.c_str(), bak.c_str()) == 0 ? "ok" : strerror(errno));
}

static bool first_launch_low() {
    static int v = -1;
    if (v < 0) {
        std::string marker = std::string(g_save_dir) + "/v015_defaults_done";
        struct stat st;
        v = stat(marker.c_str(), &st) != 0;
        if (v) {
            restore_pcconfig_defaults();
            if (FILE *f = fopen(marker.c_str(), "w")) fclose(f);
        }
    }
    return v;
}
static void force_low_quality_now() {
    M32(0x1470FD0) = 0; M32(0x1470FD8) = 0; M8(0x138C4E6) = 0;
    M32(0x183D6E0) &= ~0x4000000u;
    M32(0x1470FD4) = 1; M32(0x183D6E4) |= 0x80u;
    M32(0x1470FDC) = 0;
    M32(0x183D6E4) &= ~(0x200000u | 0x80000u | 0x2u | 0x4u | 0x8u | 0x10u | 0x100u | 0x200u |
                        0x400u | 0x1000u | 0x2000u | 0x4000u | 0x8000u | 0x10000u | 0x20000000u);
    M8(0x1471010) = 0;
}
// 0.2.6 (TJ: back to 0.1.5): nothing is forced; the game's own defaults apply.
// The first launch of this version moves the low pcconfig.txt aside once
// (pcconfig_low.bak) so the game recreates it with its defaults.
static void force_low_quality() {}
extern "C" void port_restore_defaults(void) { (void)first_launch_low(); }
#define LOW_QUALITY_HOOK(addr) \
    extern "C" void f_##addr##_guest(CPU *c); \
    extern "C" void f_##addr(CPU *c) { f_##addr##_guest(c); force_low_quality(); }
LOW_QUALITY_HOOK(6D8ED0)  // FSAA / EdgeAA
LOW_QUALITY_HOOK(6D8F70)  // ShaderQuality
LOW_QUALITY_HOOK(6D8FB0)  // ShadowQuality
LOW_QUALITY_HOOK(6D3BD0)  // TextureQuality
LOW_QUALITY_HOOK(6D9000)  // SoftParticles
extern "C" void f_6DAF30_guest(CPU *c);   // defaults by GPU vendor/device: always low
extern "C" void f_6DAF30(CPU *c) { f_6DAF30_guest(c); }
LOW_QUALITY_HOOK(6D9040)  // BloomEnabled
LOW_QUALITY_HOOK(6D9070)  // ExposureControlEnabled
LOW_QUALITY_HOOK(6D90A0)  // DoFEnabled
LOW_QUALITY_HOOK(6D90D0)  // SoftShadowsEnabled
LOW_QUALITY_HOOK(6D9100)  // SpeedBlurEnabled
LOW_QUALITY_HOOK(6D9130)  // MotionBlurEnabled
LOW_QUALITY_HOOK(6D9160)  // ProcessColourEnabled
LOW_QUALITY_HOOK(6D9190)  // WaterDistortionEnabled
LOW_QUALITY_HOOK(6D91C0)  // SSAOEnabled
LOW_QUALITY_HOOK(6D9280)  // LensFilterEnabled
LOW_QUALITY_HOOK(6D91F0)  // RingEffectEnabled
LOW_QUALITY_HOOK(6D9220)  // ScreenSpaceReflectionsEnabled
LOW_QUALITY_HOOK(6D9250)  // EdgeHightlightEnabled
LOW_QUALITY_HOOK(6D3DC0)  // AllowVendorExtensions

static uint32_t guest_text(const char *s) {
    uint32_t g = gmalloc((uint32_t)strlen(s) + 1);
    memcpy(MEM + g, s, strlen(s) + 1);
    return g;
}

// Port settings come from save/touch.cfg ("<touch controls> <nintendo layout>"),
// touch on by default.
static void load_cfg_marvel() {
    if (FILE *f = fopen(cfg_path().c_str(), "r")) {
        int t = 1, n = 0;
        if (fscanf(f, "%d %d", &t, &n) >= 1) { g_touch_on = t != 0; g_nintendo = n != 0; }
        fclose(f);
    }
    input_set_nintendo(g_nintendo);
}
#endif
#ifdef GAME_BATMAN
// Video (and Port) Settings item input handler. Items: 0 Screen Resolution
// (shown as Resolution Scale, see d3d9_bridge.cpp), 1 Refresh Rate (now
// Nintendo Button Layout), 2 VSync (now Touch Controls), 3 Aspect Ratio (now
// Skip Cutscenes). Runs the game's code for the others, then keeps any change
// it made to the VSync byte as the touch-controls choice.
extern "C" void f_6C42C0_guest(CPU *c);
extern "C" void f_6C42C0(CPU *c) {
    // arg: menu input state; +0x0A item index, +0x5C/+0x60 left/right,
    // +0x68 Back pressed, which reverts the menu's values (ours is restored
    // by enabled()).
    uint32_t in = ARG(0);
    int item = (int16_t)M16(in + 0xA);
    bool back = M32(in + 0x68) != 0;
    if (available() && !back && (item == 1 || item == 3)) {
        if (M32(in + 0x5C) || M32(in + 0x60)) {
            if (item == 1) {
                g_nintendo = !g_nintendo;
                input_set_nintendo(g_nintendo);
                LOG("touch: nintendo layout %s", g_nintendo ? "on" : "off");
            } else {
                g_skip_cutscenes = !g_skip_cutscenes;
                input_set_skip_cutscenes(g_skip_cutscenes);
                LOG("touch: skip cutscenes %s", g_skip_cutscenes ? "on" : "off");
            }
            save_cfg();
            M32(0x99E380) = M32(0x99E390);  // as the game does on a change (menu sound)
        }
        RETC(0);
    }
    uint8_t before = M8(G_VSYNC_OPTION);
    f_6C42C0_guest(c);
    uint8_t after = M8(G_VSYNC_OPTION);
    if (!available() || back || item != 2 || after == before) return;
    g_touch_on = after != 0;
    save_cfg();
    LOG("touch: controls %s", g_touch_on ? "on" : "off");
}

// Video Settings draw: values for the repurposed items. The value formats are
// patched in touch_setup_guest ("%s: %d.%d" and "%s: %s"); the current mode
// entry is swapped for the scale digits and On/Off text just while it draws.
extern "C" void f_6C80D0_guest(CPU *c);
extern "C" void f_6C80D0(CPU *c) {
    if (!available() || !M32(0x9D0EEC)) { f_6C80D0_guest(c); return; }  // no mode list yet
    uint32_t mode = M32(0x9D0EEC) + M32(0x9D0F38) * 16;
    uint32_t w = M32(mode), h = M32(mode + 4), hz = M32(mode + 8);
    int ww = 1280, wh = 720;
    SDL_GetWindowSize((SDL_Window *)platform_window(), &ww, &wh);
    int tenths = ww > 0 ? (int)((w * 10 + ww / 2) / ww) : 10;
    if (tenths < 2) tenths = 2;
    if (tenths > 10) tenths = 10;
    M32(mode) = tenths / 10;
    M32(mode + 4) = tenths % 10;
    M32(mode + 8) = M32(g_nintendo ? 0x9D588C : 0x9D5890);  // the game's "On"/"Off"
    // Aspect Ratio value: its case 3 prints "%s: %s" with the string at 0x9D58B0.
    uint32_t aspect = M32(0xAD7294), aspect_text = M32(0x9D58B0);
    M32(0xAD7294) = 3;
    M32(0x9D58B0) = M32(g_skip_cutscenes ? 0x9D588C : 0x9D5890);
    f_6C80D0_guest(c);
    M32(mode) = w;
    M32(mode + 4) = h;
    M32(mode + 8) = hz;
    M32(0xAD7294) = aspect;
    M32(0x9D58B0) = aspect_text;
}

// The display mode list builder (0x52E2D0) drops modes under 640x480, which
// hides the low resolution scales. It sorts the surviving modes with qsort
// right after filtering (return address 0x52E3BD); refill the array with every
// scaled mode there, so the rest of the builder sees them all.
extern "C" void f_7401A0_guest(CPU *c);
extern "C" void bridge_scaled_mode(uint32_t i, uint32_t *w, uint32_t *h);
extern "C" void f_7401A0(CPU *c) {
    if (available() && M32(c->r[4]) == 0x52E3BD && ARG(2) == 16) {
        uint32_t base = ARG(0), n = ARG(1), obj = c->r[7];  // edi = display object
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w, h;
            bridge_scaled_mode(i, &w, &h);
            M32(base + i * 16) = w;
            M32(base + i * 16 + 4) = h;
            M32(base + i * 16 + 8) = 60;
            M32(base + i * 16 + 12) = 0x16;  // D3DFMT_X8R8G8B8, as enumerated
        }
        M32(obj + 0x618) = n;
    }
    f_7401A0_guest(c);
}

// pcconfig "Widescreen" setter, used at load and by the menu's Accept.
extern "C" void f_524860_guest(CPU *c);
extern "C" void f_524860(CPU *c) {
    f_524860_guest(c);
    if (available()) M32(G_ASPECT) = ASPECT_FROM_RES;
}

#endif
extern "C" void touch_setup_guest(void) {
#ifdef GAME_MARVEL
    load_cfg_marvel();
    return;
#endif
#ifdef GAME_SKYRIM
    /* No Skyrim menu hooks: the Android launcher writes touch.cfg ("<touch> <nintendo>"). */
    if (FILE *f = fopen(cfg_path().c_str(), "r")) {
        int t = 1, n = 0;
        if (fscanf(f, "%d %d", &t, &n) >= 1) { g_touch_on = t != 0; g_nintendo = n != 0; }
        fclose(f);
    }
    input_set_nintendo(g_nintendo);
    return;
#endif
#ifdef GAME_USM
    /* Ultimate Spider-Man: no menu hooks yet. The Android launcher writes
     * touch.cfg ("<touch> <nintendo>"), exactly as for Skyrim. */
    if (FILE *f = fopen(cfg_path().c_str(), "r")) {
        int t = 1, n = 0;
        if (fscanf(f, "%d %d", &t, &n) >= 1) { g_touch_on = t != 0; g_nintendo = n != 0; }
        fclose(f);
    }
    input_set_nintendo(g_nintendo);
    return;
#endif
    if (!available()) return;
    if (FILE *f = fopen(cfg_path().c_str(), "r")) {
        int t = 1, n = 0, k = 0;
        if (fscanf(f, "%d %d %d", &t, &n, &k) >= 1) { g_touch_on = t != 0; g_nintendo = n != 0; g_skip_cutscenes = k != 0; }
        fclose(f);
    }
    input_set_nintendo(g_nintendo);
    input_set_skip_cutscenes(g_skip_cutscenes);
    M8(G_VSYNC_OPTION) = g_touch_on;
    M32(G_ASPECT) = M32(G_ASPECT_MENU) = ASPECT_FROM_RES;
    uint32_t touch = guest_str("Touch Controls"), quit = guest_str("Quit Game"),
             scale = guest_str("Resolution Scale"), layout = guest_str("Nintendo Button Layout"),
             skip = guest_str("Skip Cutscenes"), title = guest_str("Video and Port Settings");
    strcpy((char *)MEM + 0x872A10, "%s: %d.%d");  // was "%s: %d x %d"
    strcpy((char *)MEM + 0x872A04, "%s: %s");     // was "%s: %dHz"
    // PC menu text tables (one per language): { id, char *text } pairs in .data.
    // Ids: 1 Quit to Windows, 0x12 Video Settings, 0x13 Screen Resolution,
    // 0x14 Refresh Rate, 0x15 Vertical Sync, 0x16 Aspect Ratio.
    int n = 0;
    for (uint32_t a = 0x934008; a < 0x934000 + 0x8A000 - 12; a += 4) {
        if (M32(a) != 0x15 || M32(a - 8) != 0x14 || M32(a + 8) != 0x16) continue;
        M32(a + 4) = touch;
        M32(a - 4) = layout;
        M32(a + 12) = skip;
        if (M32(a - 16) == 0x13) M32(a - 12) = scale;
        if (M32(a - 24) == 0x12) M32(a - 20) = title;
        n++;
        // "Quit to Windows" (id 1) sits 0x14 entries earlier in the same table.
        uint32_t q = a - 0x14 * 8;
        if (M32(q) == 1 && !strcmp((const char *)MEM + M32(q + 4), "Quit to Windows")) M32(q + 4) = quit;
    }
    LOG("touch: relabelled %d menu tables", n);
}
