/* USER32/GDI32 shims. The game window is owned by the host platform layer;
 * the guest only sees a fake HWND. Messages the guest needs (activation,
 * close) are delivered by calling its window procedure directly. */
#include "rt.h"
#include <string.h>
#include <pthread.h>
#include <SDL2/SDL.h>

#define FAKE_HWND 0x00010010u
static uint32_t g_wndproc;
static int g_screen_w = 1280, g_screen_h = 720;
static int g_quit;

uint32_t user32_wndproc(void) { return g_wndproc; }
void user32_set_screen(int w, int h) { g_screen_w = w; g_screen_h = h; }

SHIM(RegisterClassExA) {
    g_wndproc = M32(ARG(0) + 8);
    LOG("RegisterClassExA wndproc=%08x", g_wndproc);
    RET(0xC001, 1);
}
SHIM(CreateWindowExA) {
    LOG("CreateWindowExA '%s' %dx%d", GSTR(ARG(2)) ? GSTR(ARG(2)) : "", (int)ARG(6), (int)ARG(7));
    RET(FAKE_HWND, 12);
}
SHIM(DestroyWindow) { RET(1, 1); }
SHIM(ShowWindow) { RET(1, 2); }
SHIM(UpdateWindow) { RET(1, 1); }
SHIM(MoveWindow) { RET(1, 6); }
SHIM(SetForegroundWindow) { RET(1, 1); }
SHIM(GetForegroundWindow) { RET(FAKE_HWND, 0); }
SHIM(IsIconic) { RET(0, 1); }
SHIM(GetWindowLongA) { RET(0x10000000u | 0x00C00000u, 2); } /* WS_VISIBLE | WS_CAPTION */
SHIM(AdjustWindowRect) { RET(1, 3); }
SHIM(GetSystemMetrics) {
    int32_t i = (int32_t)ARG(0);
    RET(i == 0 ? g_screen_w : i == 1 ? g_screen_h : 0, 1);
}
SHIM(SystemParametersInfoA) { RET(1, 4); }
SHIM(LoadIconA) { RET(0x20001, 2); }
SHIM(LoadCursorA) { RET(0x20002, 2); }
SHIM(SetCursor) { RET(0, 1); }
SHIM(SetCursorPos) { RET(1, 2); }
/* real display counter: games loop "while (ShowCursor(FALSE) >= 0)" */
static volatile int g_cursor_count = 0;
SHIM(ShowCursor) { int n = ARG(0) ? __atomic_add_fetch(&g_cursor_count, 1, __ATOMIC_SEQ_CST) : __atomic_sub_fetch(&g_cursor_count, 1, __ATOMIC_SEQ_CST); RET((uint32_t)n, 1); }
SHIM(LoadKeyboardLayoutA) { RET(0x04090409, 2); }
/* US layout: virtual-key <-> PC set-1 scan code */
static const uint8_t k_vk_sc[][2] = {
    {0x08, 0x0E}, {0x09, 0x0F}, {0x0D, 0x1C}, {0x10, 0x2A}, {0x11, 0x1D}, {0x12, 0x38}, {0x1B, 0x01},
    {0x20, 0x39}, {0x21, 0x49}, {0x22, 0x51}, {0x23, 0x4F}, {0x24, 0x47}, {0x25, 0x4B}, {0x26, 0x48},
    {0x27, 0x4D}, {0x28, 0x50}, {0x2D, 0x52}, {0x2E, 0x53},
    {'0', 0x0B}, {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06}, {'6', 0x07}, {'7', 0x08},
    {'8', 0x09}, {'9', 0x0A},
    {'A', 0x1E}, {'B', 0x30}, {'C', 0x2E}, {'D', 0x20}, {'E', 0x12}, {'F', 0x21}, {'G', 0x22}, {'H', 0x23},
    {'I', 0x17}, {'J', 0x24}, {'K', 0x25}, {'L', 0x26}, {'M', 0x32}, {'N', 0x31}, {'O', 0x18}, {'P', 0x19},
    {'Q', 0x10}, {'R', 0x13}, {'S', 0x1F}, {'T', 0x14}, {'U', 0x16}, {'V', 0x2F}, {'W', 0x11}, {'X', 0x2D},
    {'Y', 0x15}, {'Z', 0x2C},
    {0x70, 0x3B}, {0x71, 0x3C}, {0x72, 0x3D}, {0x73, 0x3E}, {0x74, 0x3F}, {0x75, 0x40}, {0x76, 0x41},
    {0x77, 0x42}, {0x78, 0x43}, {0x79, 0x44}, {0x7A, 0x57}, {0x7B, 0x58},
    {0xA0, 0x2A}, {0xA1, 0x36}, {0xA2, 0x1D}, {0xA3, 0x1D}, {0xA4, 0x38}, {0xA5, 0x38},
};
SHIM(MapVirtualKeyExA) {
    uint32_t code = ARG(0), type = ARG(1);
    uint32_t r = 0;
    for (unsigned i = 0; i < sizeof k_vk_sc / sizeof k_vk_sc[0] && !r; i++) {
        if ((type == 0 || type == 4) && k_vk_sc[i][0] == code) r = k_vk_sc[i][1];
        else if ((type == 1 || type == 3) && k_vk_sc[i][1] == (code & 0xFF)) r = k_vk_sc[i][0];
    }
    if (type == 2) r = (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == ' ' ? code : code == 0x0D ? 0x0D : 0;
    RET(r, 3);
}
SHIM(MapVirtualKeyA) {
    /* same as the Ex variant without the layout argument */
    uint32_t code = ARG(0), type = ARG(1);
    uint32_t r = 0;
    for (unsigned i = 0; i < sizeof k_vk_sc / sizeof k_vk_sc[0] && !r; i++) {
        if ((type == 0 || type == 4) && k_vk_sc[i][0] == code) r = k_vk_sc[i][1];
        else if ((type == 1 || type == 3) && k_vk_sc[i][1] == (code & 0xFF)) r = k_vk_sc[i][0];
    }
    if (type == 2) r = (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == ' ' ? code : code == 0x0D ? 0x0D : 0;
    RET(r, 2);
}
/* Posted messages for the game window (keyboard input), filled by the
 * platform layer from SDL key events. */
typedef struct { uint32_t msg, wp, lp; } QMsg;
static QMsg g_q[256];
static unsigned g_qhead, g_qtail;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;

static void post_msg(uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&g_qlock);
    if (g_qtail - g_qhead < 256) g_q[g_qtail++ % 256] = (QMsg){msg, wp, lp};
    pthread_mutex_unlock(&g_qlock);
}

/* vk: Windows virtual-key code; sc: PC set-1 scan code */
void user32_key_event(uint32_t vk, uint32_t sc, int down) {
    if (!vk) return;
    uint32_t lp = 1 | (sc & 0xFF) << 16 | (down ? 0 : 0xC0000000u);
    post_msg(down ? 0x100 : 0x101, vk, lp); /* WM_KEYDOWN / WM_KEYUP */
}

SHIM(PeekMessageA) {
    uint32_t m = ARG(0), remove = ARG(4) & 1;
    if (g_quit == 1) {
        /* deliver WM_QUIT once */
        memset(MEM + m, 0, 28);
        M32(m + 4) = 0x12;
        g_quit = 2;
        RET(1, 5);
    }
    int got = 0;
    QMsg q;
    pthread_mutex_lock(&g_qlock);
    if (g_qhead != g_qtail) {
        q = g_q[g_qhead % 256];
        if (remove) g_qhead++;
        got = 1;
    }
    pthread_mutex_unlock(&g_qlock);
    if (!got) RET(0, 5);
    memset(MEM + m, 0, 28);
    M32(m) = FAKE_HWND;
    M32(m + 4) = q.msg;
    M32(m + 8) = q.wp;
    M32(m + 12) = q.lp;
    M32(m + 16) = SDL_GetTicks();
    RET(1, 5);
}
SHIM(TranslateMessage) {
    uint32_t m = ARG(0), vk = M32(m + 8);
    if (M32(m + 4) == 0x100) {
        uint32_t ch = 0;
        if (vk >= 'A' && vk <= 'Z') ch = vk + 32;
        else if ((vk >= '0' && vk <= '9') || vk == ' ' || vk == 0x0D || vk == 0x1B || vk == 0x08 || vk == 0x09) ch = vk;
        if (ch) post_msg(0x102, ch, M32(m + 12)); /* WM_CHAR */
    }
    RET(0, 1);
}
SHIM(DispatchMessageA) {
    uint32_t m = ARG(0);
    uint32_t r = 0;
    if (g_wndproc && M32(m + 4) != 0x12) {
        uint32_t args[4] = {M32(m), M32(m + 4), M32(m + 8), M32(m + 12)};
        r = rt_invoke(c, g_wndproc, 4, args);
    }
    RET(r, 1);
}
SHIM(SendMessageA) { RET(0, 4); }
SHIM(PostQuitMessage) { g_quit = 1; RET(0, 1); }
SHIM(DefWindowProcA) { RET(0, 4); }
SHIM(MessageBoxA) {
    LOG("MessageBoxA: [%s] %s", GSTR(ARG(2)) ? GSTR(ARG(2)) : "", GSTR(ARG(1)) ? GSTR(ARG(1)) : "");
    RET(1, 4);
}
SHIM(MessageBoxW) {
    char buf[512];
    uint32_t s = ARG(1);
    int i = 0;
    for (; s && i < 511 && M16(s + 2 * i); i++) buf[i] = (char)M16(s + 2 * i);
    buf[i] = 0;
    LOG("MessageBoxW: %s", buf);
    RET(1, 4);
}

SHIM(CreateDCA) { RET(0x30001, 4); }
SHIM(DeleteDC) { RET(1, 1); }
SHIM(DeleteObject) { RET(1, 1); }
SHIM(ExtEscape) { RET(0, 6); }

/* GDI text/font calls used by D3DX's font code; fonts are not rendered. */
SHIM(CreateDIBSection) { RET(0x0u, 6); }
SHIM(GetGlyphOutlineA) { RET(0xFFFFFFFFu, 7); }
SHIM(GetTextMetricsA) { RET(0x0u, 2); }
SHIM(GetObjectW) { RET(0x0u, 3); }
SHIM(SelectObject) { RET(0x0u, 2); }
SHIM(GetCharacterPlacementA) { RET(0x0u, 6); }
SHIM(GetCharacterPlacementW) { RET(0x0u, 6); }
SHIM(SetTextColor) { RET(0x0u, 2); }
SHIM(SetBkColor) { RET(0x0u, 2); }
SHIM(SetBkMode) { RET(0x1u, 2); }
SHIM(GetTextMetricsW) { RET(0x0u, 2); }
SHIM(GetFontLanguageInfo) { RET(0x0u, 1); }
SHIM(CreateFontIndirectA) { RET(0x30002u, 1); }
SHIM(CreateFontIndirectW) { RET(0x30002u, 1); }
SHIM(SetTextAlign) { RET(0x0u, 2); }
SHIM(SetMapMode) { RET(0x1u, 2); }
SHIM(CreateCompatibleDC) { RET(0x30001u, 1); }
SHIM(ExtTextOutA) { RET(0x0u, 8); }
SHIM(MoveToEx) { RET(0x1u, 4); }
SHIM(ExtTextOutW) { RET(0x0u, 8); }
SHIM(TranslateCharsetInfo) { RET(0x0u, 3); }
SHIM(GetCurrentObject) { RET(0x0u, 2); }
SHIM(GetOutlineTextMetricsA) { RET(0x0u, 3); }
SHIM(GetGlyphOutlineW) { RET(0xFFFFFFFFu, 7); }
SHIM(GetObjectA) { RET(0x0u, 3); }

/* Window/GDI calls made by binkw32; movies are drawn through D3D, not GDI. */
SHIM(GetTopWindow) { RET(0, 1); }
SHIM(GetActiveWindow) { RET(FAKE_HWND, 0); }
SHIM(GetWindow) { RET(0, 2); }
SHIM(UnregisterClassA) { RET(1, 2); }
SHIM(EndPaint) { RET(1, 2); }
SHIM(ReleaseDC) { RET(1, 2); }
SHIM(IsWindowVisible) { RET(1, 1); }
SHIM(ChangeDisplaySettingsA) { RET(0, 2); }
SHIM(RegisterClassA) { RET(0xC001, 1); }
SHIM(GetDC) { RET(0x30001, 1); }
SHIM(GetClassLongA) { RET(0, 2); }
SHIM(BeginPaint) { RET(0x30001, 2); }
SHIM(SetStretchBltMode) { RET(1, 2); }
SHIM(StretchBlt) { RET(1, 11); }
SHIM(GetDIBits) { RET(0, 7); }
SHIM(GetPixel) { RET(0, 3); }
SHIM(SetPixel) { RET(0, 4); }
SHIM(CreateCompatibleBitmap) { RET(0x30003, 3); }
SHIM(GetWindowThreadProcessId) { if (ARG(1)) M32(ARG(1)) = 0x42; RET(1, 2); }
SHIM(ClientToScreen) { RET(1, 2); }
SHIM(ScreenToClient) { RET(1, 2); }
SHIM(GetCursorPos) { M32(ARG(0)) = 0; M32(ARG(0) + 4) = 0; RET(1, 1); }
static void put_rect(uint32_t p) { M32(p) = 0; M32(p + 4) = 0; M32(p + 8) = (uint32_t)g_screen_w; M32(p + 12) = (uint32_t)g_screen_h; }
SHIM(GetWindowRect) { put_rect(ARG(1)); RET(1, 2); }
SHIM(GetClientRect) { put_rect(ARG(1)); RET(1, 2); }
SHIM(GetDeviceCaps) { uint32_t i = ARG(1); RET(i == 12 ? 32 : i == 8 ? g_screen_w : i == 10 ? g_screen_h : i == 116 ? 60 : 0, 2); }


/* ---- added for LEGO Marvel ---- */
SHIM(GetKeyboardLayout) { RET(0x04090409, 1); }
/* ToUnicodeEx(vk, scan, keystate, buf, cch, flags, hkl): letters, digits, space. */
SHIM(ToUnicodeEx) {
    uint32_t vk = ARG(0), buf = ARG(3);
    int shift = (M8(ARG(2) + 0x10) & 0x80) != 0;
    uint16_t ch = 0;
    if (vk >= 'A' && vk <= 'Z') ch = (uint16_t)(shift ? vk : vk + 32);
    else if ((vk >= '0' && vk <= '9') || vk == ' ') ch = (uint16_t)vk;
    if (!ch || !ARG(4)) RET(0, 7);
    M16(buf) = ch;
    RET(1, 7);
}
SHIM(TrackMouseEvent) { RET(1, 1); }
SHIM(SetWindowPos) { RET(1, 7); }
SHIM(SetWindowLongA) { RET(0, 3); }
/* One display mode, the host screen size (DEVMODEA fields by offset). */
SHIM(EnumDisplaySettingsA) {
    int32_t i = (int32_t)ARG(1);
    uint32_t d = ARG(2);
    if (i != -1 && i != -2 && i != 0) RET(0, 3);
    M16(d + 36) = 156;
    M32(d + 40) = 0x001C0000u;  /* DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT */
    M32(d + 104) = 32;
    M32(d + 108) = (uint32_t)g_screen_w;
    M32(d + 112) = (uint32_t)g_screen_h;
    M32(d + 120) = 60;
    RET(1, 3);
}
/* EnumDisplayMonitors(hdc, clip, proc, lparam): one monitor. */
SHIM(EnumDisplayMonitors) {
    uint32_t proc = ARG(2), lp = ARG(3);
    uint32_t r = gmalloc(16);
    M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = (uint32_t)g_screen_w; M32(r + 12) = (uint32_t)g_screen_h;
    c->r[4] += 4 + 16;  /* pop our frame first: proc runs on the caller's stack */
    uint32_t args[4] = {0x00010001u, 0, r, lp};
    rt_invoke(c, proc, 4, args);
    gfree(r);
    c->r[0] = 1;
}

static const ShimDef defs[] = {
#define S(n) {"USER32.dll", #n, shim_##n},
    S(RegisterClassExA) S(CreateWindowExA) S(DestroyWindow) S(ShowWindow) S(UpdateWindow) S(MoveWindow)
    S(SetForegroundWindow) S(GetForegroundWindow) S(IsIconic) S(GetWindowLongA) S(AdjustWindowRect)
    S(GetSystemMetrics) S(SystemParametersInfoA) S(LoadIconA) S(LoadCursorA) S(SetCursor) S(SetCursorPos)
    S(ShowCursor) S(LoadKeyboardLayoutA) S(MapVirtualKeyExA) S(MapVirtualKeyA) S(PeekMessageA) S(TranslateMessage)
    S(DispatchMessageA) S(SendMessageA) S(PostQuitMessage) S(DefWindowProcA) S(MessageBoxA) S(MessageBoxW)
    S(GetTopWindow) S(GetActiveWindow) S(GetWindow) S(UnregisterClassA) S(EndPaint) S(ReleaseDC) S(IsWindowVisible) S(ChangeDisplaySettingsA) S(RegisterClassA) S(GetDC) S(GetClassLongA) S(BeginPaint) S(GetWindowThreadProcessId) S(ClientToScreen) S(ScreenToClient) S(GetCursorPos) S(GetWindowRect) S(GetClientRect)
    S(GetKeyboardLayout) S(ToUnicodeEx) S(TrackMouseEvent) S(SetWindowPos) S(SetWindowLongA)
    S(EnumDisplaySettingsA) S(EnumDisplayMonitors)
#undef S
#define S(n) {"GDI32.dll", #n, shim_##n},
    S(CreateDCA) S(DeleteDC) S(DeleteObject) S(ExtEscape)
    S(SetStretchBltMode) S(StretchBlt) S(GetDIBits) S(GetPixel) S(SetPixel) S(CreateCompatibleBitmap) S(GetDeviceCaps)
    S(CreateDIBSection) S(GetGlyphOutlineA) S(GetTextMetricsA) S(GetObjectW) S(SelectObject) S(GetCharacterPlacementA) S(GetCharacterPlacementW) S(SetTextColor) S(SetBkColor) S(SetBkMode) S(GetTextMetricsW) S(GetFontLanguageInfo) S(CreateFontIndirectA) S(CreateFontIndirectW) S(SetTextAlign) S(SetMapMode) S(CreateCompatibleDC) S(ExtTextOutA) S(MoveToEx) S(ExtTextOutW) S(TranslateCharsetInfo) S(GetCurrentObject) S(GetOutlineTextMetricsA) S(GetGlyphOutlineW) S(GetObjectA)
#undef S
    {NULL, NULL, NULL}
};

void user32_init(void) { rt_register_shims(defs); }
