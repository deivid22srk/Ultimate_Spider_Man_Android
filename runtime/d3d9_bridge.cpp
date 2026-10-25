// Guest<->host Direct3D 9 bridge: COM object registry and the methods
// that need hand-written marshalling (structures with pointers, locks,
// presentation). Everything else is generated into d3d9_bridge_gen.cpp.
#include <SDL2/SDL.h>
#include <ctype.h>
#include <functional>
#include <vector>
void shadow_flush(void *obj);
void shadow_forget(void *obj);
#include "d3d9_bridge.h"
void bc_forget(void *obj);
bool bc_lock(void *tex, uint32_t sub, uint32_t w, uint32_t h, const RECT *r, DWORD flags, uint32_t out,
             const std::function<HRESULT(D3DLOCKED_RECT *, const RECT *)> &lock, HRESULT *hr);
bool bc_unlock(void *tex, uint32_t sub);
bool bc_surface_target(void *surf, void **tex, uint32_t *sub);
#include <mutex>
#include <unordered_map>
#include <vector>
#include <map>

// ---------------------------------------------------------------- registry
namespace {
struct IfaceInfo { const char *name = nullptr; uint32_t vtbl = 0; };
IfaceInfo g_ifaces[64];
std::recursive_mutex g_lock;
std::unordered_map<void *, uint32_t> g_host_to_guest;
std::vector<void *> g_objs{nullptr};
std::vector<uint32_t> g_free_ids;
}

void com_register_iface(int iface, const char *name, const ShimFn *fns, int n) {
    uint32_t vt = gmalloc(4 * n);
    for (int i = 0; i < n; i++) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s#%d", name, i);
        M32(vt + 4 * i) = rt_thunk(fns[i], strdup(buf));
    }
    g_ifaces[iface].name = name;
    g_ifaces[iface].vtbl = vt;
}

void *com_host(uint32_t g) {
    if (!g) return nullptr;
    uint32_t id = M32(g + 4);
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (id >= g_objs.size()) rt_fatal("bad guest COM object %08x", g);
    return g_objs[id];
}

uint32_t com_wrap(void *host, int iface) {
    if (!host) return 0;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_host_to_guest.find(host);
    // A texture level's surface shares its container's refcount, so its
    // wrapper is never forgotten; when the host address is reused by an
    // object of another type, the stale mapping must not be returned.
    if (it != g_host_to_guest.end()) {
        if ((int)M32(it->second + 8) == iface) return it->second;
        g_objs[M32(it->second + 4)] = nullptr;
        g_free_ids.push_back(M32(it->second + 4));
        M32(it->second) = 0xDEAD0000u;
        g_host_to_guest.erase(it);
    }
    uint32_t id;
    if (!g_free_ids.empty()) { id = g_free_ids.back(); g_free_ids.pop_back(); g_objs[id] = host; }
    else { id = (uint32_t)g_objs.size(); g_objs.push_back(host); }
    uint32_t g = gmalloc(16);
    M32(g) = g_ifaces[iface].vtbl;
    M32(g + 4) = id;
    M32(g + 8) = (uint32_t)iface;
    M32(g + 12) = 0;
    g_host_to_guest[host] = g;
    return g;
}

static void com_forget(uint32_t g) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    uint32_t id = M32(g + 4);
    void *h = g_objs[id];
    shadow_forget(h);
    bc_forget(h);
    g_host_to_guest.erase(h);
    g_objs[id] = nullptr;
    g_free_ids.push_back(id);
    M32(g) = 0xDEAD0000u;
    gfree(g);
}

void com_AddRef(CPU *c) {
    auto *u = (IUnknown *)com_host(ARG(0));
    RET(u->AddRef(), 1);
}

void com_Release(CPU *c) {
    uint32_t g = ARG(0);
    auto *u = (IUnknown *)com_host(g);
    ULONG r = u->Release();
    if (r == 0) com_forget(g);
    RET(r, 1);
}

void com_QueryInterface(CPU *c) {
    auto *u = (IUnknown *)com_host(ARG(0));
    GUID iid;
    memcpy(&iid, GPTR(ARG(1)), sizeof iid);
    void *out = nullptr;
    HRESULT hr = u->QueryInterface(iid, &out);
    if (ARG(2)) M32(ARG(2)) = 0;
    if (SUCCEEDED(hr) && out) {
        int iface = d3d9_iface_from_iid(iid);
        if (iface < 0) iface = (int)M32(ARG(0) + 8); // IUnknown: same object
        M32(ARG(2)) = d3d9_wrap(out, iface);
    }
    RET(hr, 3);
}

int d3d9_iface_from_iid(const GUID &iid) {
#define I(T) if (iid == __uuidof(T)) return IFACE_##T;
    I(IDirect3D9) I(IDirect3DDevice9) I(IDirect3DSwapChain9) I(IDirect3DTexture9) I(IDirect3DCubeTexture9)
    I(IDirect3DVolumeTexture9) I(IDirect3DVolume9) I(IDirect3DSurface9) I(IDirect3DVertexBuffer9)
    I(IDirect3DIndexBuffer9) I(IDirect3DVertexDeclaration9) I(IDirect3DVertexShader9) I(IDirect3DPixelShader9)
    I(IDirect3DStateBlock9) I(IDirect3DQuery9)
#undef I
    if (iid == __uuidof(IDirect3DBaseTexture9)) return IFACE_IDirect3DBaseTexture9;
    return -1;
}

uint32_t d3d9_wrap(void *host, int iface) {
    if (!host) return 0;
    if (iface == IFACE_IDirect3DBaseTexture9) {
        switch (((IDirect3DBaseTexture9 *)host)->GetType()) {
        case D3DRTYPE_CUBETEXTURE: iface = IFACE_IDirect3DCubeTexture9; break;
        case D3DRTYPE_VOLUMETEXTURE: iface = IFACE_IDirect3DVolumeTexture9; break;
        default: iface = IFACE_IDirect3DTexture9; break;
        }
    }
    return com_wrap(host, iface);
}

// ---------------------------------------------------------------- structs
extern "C" int touch_available(void);

static void pp_from_guest(uint32_t g, D3DPRESENT_PARAMETERS *pp) {
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = M32(g + 0);
    pp->BackBufferHeight = M32(g + 4);
    pp->BackBufferFormat = (D3DFORMAT)M32(g + 8);
    pp->BackBufferCount = M32(g + 12);
    pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)M32(g + 16);
    pp->MultiSampleQuality = M32(g + 20);
    pp->SwapEffect = (D3DSWAPEFFECT)M32(g + 24);
    pp->hDeviceWindow = (HWND)platform_window();
    pp->Windowed = TRUE; // the platform layer owns the (fullscreen) window
    pp->EnableAutoDepthStencil = M32(g + 36);
    pp->AutoDepthStencilFormat = (D3DFORMAT)M32(g + 40);
    pp->Flags = M32(g + 44);
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = M32(g + 52);
    // The VSync menu item controls the touch overlay instead (touch.cpp).
    // Phones always present with vsync; desktop test runs never wait for it.
    if (touch_available())
#ifdef __ANDROID__
        pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
#else
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
#endif
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
}

static void pp_to_guest(const D3DPRESENT_PARAMETERS *pp, uint32_t g) {
    M32(g + 0) = pp->BackBufferWidth;
    M32(g + 4) = pp->BackBufferHeight;
    M32(g + 8) = pp->BackBufferFormat;
    M32(g + 12) = pp->BackBufferCount;
}

// ---------------------------------------------------------------- IDirect3D9
void B_IDirect3D9_GetAdapterMonitor(CPU *c) { RET(0x00010001u, 2); }

// The game picks its shaders and default quality by GPU vendor. On a vendor
// it does not know (Qualcomm on phones) it compiles shaders at runtime with
// D3DCompiler; as NVIDIA/Intel/AMD it creates ~1700 prebuilt shaders up front,
// which takes minutes on a phone. So the real adapter is reported by default
// (0.1.5 behaviour). save/gpu_vendor.txt or LB_GPU_VENDOR: keep (default),
// intel, nvidia, amd.
static const char *gpu_vendor_choice() {
    static char v[16];
    if (v[0]) return v;
    snprintf(v, sizeof v, "keep");
    const char *e = getenv("LB_GPU_VENDOR");
    if (e && *e) snprintf(v, sizeof v, "%s", e);
    else {
        char path[1100];
        snprintf(path, sizeof path, "%s/gpu_vendor.txt", g_save_dir);
        if (FILE *f = fopen(path, "r")) {
            char w[16] = "";
            if (fscanf(f, "%15s", w) == 1) snprintf(v, sizeof v, "%s", w);
            fclose(f);
        }
    }
    for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p);
    return v;
}

void B_IDirect3D9_GetAdapterIdentifier(CPU *c) {
    auto *d3d = (IDirect3D9 *)com_host(ARG(0));
    if (g_d3d_trace) d3d_trace(c, "IDirect3D9::GetAdapterIdentifier", 3);
    auto *id = (D3DADAPTER_IDENTIFIER9 *)GPTR(ARG(3));
    HRESULT hr = d3d->GetAdapterIdentifier(ARG(1), ARG(2), id);
    const char *v = gpu_vendor_choice();
    if (SUCCEEDED(hr) && id && strcmp(v, "keep")) {
        uint32_t ven = 0x8086, dev = 0x5912;                     // Intel HD 630
        if (!strcmp(v, "nvidia")) ven = 0x10DE, dev = 0x1C82;   // GTX 1050 Ti
        else if (!strcmp(v, "amd")) ven = 0x1002, dev = 0x67DF; // RX 480
        LOG("GPU vendor %04x device %04x reported as %s %04x:%04x", (unsigned)id->VendorId,
            (unsigned)id->DeviceId, v, ven, dev);
        id->VendorId = ven;
        id->DeviceId = dev;
    }
    RET((uint32_t)hr, 4);
}

// Display modes: a fixed list of heights (never above the window's) at the
// window's aspect ratio; the launcher offers the same list as "Resolution"
// and writes the chosen one to SkyrimPrefs.ini (the game only accepts a size
// that is in this list). The chosen mode becomes the back buffer, which dxvk
// scales up to the window.
static const uint32_t k_mode_heights[] = {1080, 900, 720, 648, 576, 540, 480, 432, 360, 288, 270, 216, 180, 144, 108};
static uint32_t mode_count(int *ww, int *wh) {
    *ww = 1280; *wh = 720;
    SDL_GetWindowSize((SDL_Window *)platform_window(), ww, wh);
    uint32_t n = 1;  // the window size itself
    for (uint32_t h : k_mode_heights) if ((int)h < *wh) n++;
    return n;
}
extern "C" void bridge_scaled_mode(uint32_t i, uint32_t *w, uint32_t *h) {
    int ww, wh;
    mode_count(&ww, &wh);
    uint32_t k = 0;
    *w = (uint32_t)ww; *h = (uint32_t)wh;
    if (i == 0) return;
    for (uint32_t mh : k_mode_heights) {
        if ((int)mh >= wh) continue;
        if (++k == i) { *h = mh; *w = (uint32_t)((uint64_t)mh * ww / wh) & ~1u; return; }
    }
}
void B_IDirect3D9_GetAdapterModeCount(CPU *c) { int ww, wh; RET(mode_count(&ww, &wh), 3); }
void B_IDirect3D9_EnumAdapterModes(CPU *c) {
    /* (this, adapter, format, mode, D3DDISPLAYMODE *) */
    uint32_t i = ARG(3), p = ARG(4);
    int ww, wh;
    if (i >= mode_count(&ww, &wh)) RET(D3DERR_INVALIDCALL, 5);
    uint32_t w, h;
    bridge_scaled_mode(i, &w, &h);
    M32(p) = w;
    M32(p + 4) = h;
    M32(p + 8) = 60;
    M32(p + 12) = ARG(2) ? ARG(2) : D3DFMT_X8R8G8B8;
    RET(0, 5);
}

// The game sizes its first-run default config from the desktop mode, so
// never let this fail: fall back to the platform window's size.
void B_IDirect3D9_GetAdapterDisplayMode(CPU *c) {
    auto *d3d = (IDirect3D9 *)com_host(ARG(0));
    D3DDISPLAYMODE m = {};
    HRESULT hr = d3d->GetAdapterDisplayMode(ARG(1), &m);
    if (FAILED(hr) || !m.Width || !m.Height) {
        int w = 1280, h = 720;
        SDL_GetWindowSize((SDL_Window *)platform_window(), &w, &h);
        m.Width = (UINT)w;
        m.Height = (UINT)h;
        m.RefreshRate = 60;
        m.Format = D3DFMT_X8R8G8B8;
        LOG("GetAdapterDisplayMode failed (%08x); using %ux%u", (unsigned)hr, m.Width, m.Height);
    }
    if (!m.Format) m.Format = D3DFMT_X8R8G8B8;
    uint32_t p = ARG(2);
    M32(p) = m.Width;
    M32(p + 4) = m.Height;
    M32(p + 8) = m.RefreshRate;
    M32(p + 12) = m.Format;
    RET(0, 3);
}

void B_IDirect3D9_CreateDevice(CPU *c) {
    auto *d3d = (IDirect3D9 *)com_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_from_guest(ARG(5), &pp);
    LOG("CreateDevice %ux%u fmt=%d ds=%d/%d windowed(guest)=%u flags=%x interval=%x",
        pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat, pp.EnableAutoDepthStencil,
        pp.AutoDepthStencilFormat, M32(ARG(5) + 32), ARG(4), pp.PresentationInterval);
    IDirect3DDevice9 *dev = nullptr;
    DWORD flags = ARG(4) | D3DCREATE_MULTITHREADED;
    rt_host_affinity_begin();
    HRESULT hr = d3d->CreateDevice(ARG(1), (D3DDEVTYPE)ARG(2), (HWND)platform_window(), flags, &pp, &dev);
    rt_host_affinity_end();
    pp_to_guest(&pp, ARG(5));
    if (ARG(6)) M32(ARG(6)) = SUCCEEDED(hr) ? d3d9_wrap(dev, IFACE_IDirect3DDevice9) : 0;
    LOG("CreateDevice -> %08x", (uint32_t)hr);
    RET(hr, 7);
}

// ---------------------------------------------------------------- device
void B_IDirect3DDevice9_GetCreationParameters(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = d->GetCreationParameters(&p);
    uint32_t g = ARG(1);
    M32(g) = p.AdapterOrdinal;
    M32(g + 4) = p.DeviceType;
    M32(g + 8) = 0x00010010u;
    M32(g + 12) = p.BehaviorFlags;
    RET(hr, 2);
}

void B_IDirect3DDevice9_CreateAdditionalSwapChain(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_from_guest(ARG(1), &pp);
    IDirect3DSwapChain9 *sc = nullptr;
    HRESULT hr = d->CreateAdditionalSwapChain(&pp, &sc);
    pp_to_guest(&pp, ARG(1));
    if (ARG(2)) M32(ARG(2)) = d3d9_wrap(sc, IFACE_IDirect3DSwapChain9);
    RET(hr, 3);
}

void B_IDirect3DDevice9_Reset(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_from_guest(ARG(1), &pp);
    rt_host_affinity_begin();
    HRESULT hr = d->Reset(&pp);
    rt_host_affinity_end();
    pp_to_guest(&pp, ARG(1));
    LOG("Reset %ux%u -> %08x", pp.BackBufferWidth, pp.BackBufferHeight, (uint32_t)hr);
    RET(hr, 2);
}

// LB_D3DTRACE=from:to logs every generated bridge call made during those
// frames (counted by Present) to stderr.
int g_d3d_trace;
static int g_frame, g_trace_from = -1, g_trace_to = -1;

void d3d_trace(CPU *c, const char *name, int nargs) {
    fprintf(stderr, "D3D %d %s", g_frame, name);
    for (int i = 1; i <= nargs; i++) fprintf(stderr, " %x", ARG(i));
    if (strstr(name, "ConstantF") && ARG(2))
        for (uint32_t i = 0; i < ARG(3) * 4 && i < 64; i++)
            fprintf(stderr, "%s%g", i % 4 ? " " : " | ", *(float *)GPTR(ARG(2) + 4 * i));
    if (strstr(name, "SetTransform") && ARG(2))
        for (uint32_t i = 0; i < 16; i++)
            fprintf(stderr, "%s%g", i % 4 ? " " : " | ", *(float *)GPTR(ARG(2) + 4 * i));
    fprintf(stderr, "\n");
}

// LB_BISECT=F: on frame F+2k, skip the k-th draw call of the frame and save
// a screenshot after presenting it (finds which draw produces an artefact).
int g_bisect;
static int g_bisect_from = -1, g_draw_no;

bool d3d_skip_draw(void) {
    int k = g_frame - g_bisect_from;
    return k >= 0 && !(k & 1) && g_draw_no++ == k / 2;
}

static void bisect_present(void) {
    if (g_bisect_from < 0) {
        const char *e = getenv("LB_BISECT");
        g_bisect_from = e ? atoi(e) : 1 << 30;
    }
    int k = g_frame - 1 - g_bisect_from;
    if (k >= 0 && !(k & 1) && g_bisect) {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "import -window root %s/skip_%03d.png",
                 getenv("LB_BISECT_DIR") ? getenv("LB_BISECT_DIR") : ".", k / 2);
        if (system(cmd)) {}
    }
    g_draw_no = 0;
    g_bisect = g_frame >= g_bisect_from && g_frame < g_bisect_from + 160;
}

void B_IDirect3DDevice9_Present(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    if (g_trace_from < 0) {
        const char *e = getenv("LB_D3DTRACE");
        g_trace_from = 1 << 30;
        if (e) sscanf(e, "%d:%d", &g_trace_from, &g_trace_to);
    }
    g_frame++;
    g_d3d_trace = g_frame >= g_trace_from && g_frame <= g_trace_to;
    if (g_d3d_trace) fprintf(stderr, "D3D %d Present\n", g_frame);
    extern void touch_draw(IDirect3DDevice9 *d);
    touch_draw(d);
    HRESULT hr = d->Present((const RECT *)GPTR(ARG(1)), (const RECT *)GPTR(ARG(2)), nullptr,
                            (const RGNDATA *)GPTR(ARG(4)));
    platform_present_hook();
    bisect_present();
    RET(hr, 5);
}

void B_IDirect3DDevice9_GetNPatchMode(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    fpush(c, d->GetNPatchMode());
    RET(0, 1);
}

void B_IDirect3DSwapChain9_Present(CPU *c) {
    auto *s = (IDirect3DSwapChain9 *)com_host(ARG(0));
    HRESULT hr = s->Present((const RECT *)GPTR(ARG(1)), (const RECT *)GPTR(ARG(2)), nullptr,
                            (const RGNDATA *)GPTR(ARG(4)), ARG(5));
    platform_present_hook();
    RET(hr, 6);
}

void B_IDirect3DSwapChain9_GetPresentParameters(CPU *c) {
    auto *s = (IDirect3DSwapChain9 *)com_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = s->GetPresentParameters(&pp);
    uint32_t g = ARG(1);
    M32(g + 0) = pp.BackBufferWidth;
    M32(g + 4) = pp.BackBufferHeight;
    M32(g + 8) = pp.BackBufferFormat;
    M32(g + 12) = pp.BackBufferCount;
    M32(g + 16) = pp.MultiSampleType;
    M32(g + 20) = pp.MultiSampleQuality;
    M32(g + 24) = pp.SwapEffect;
    M32(g + 28) = 0x00010010u;
    M32(g + 32) = pp.Windowed;
    M32(g + 36) = pp.EnableAutoDepthStencil;
    M32(g + 40) = pp.AutoDepthStencilFormat;
    M32(g + 44) = pp.Flags;
    M32(g + 48) = pp.FullScreen_RefreshRateInHz;
    M32(g + 52) = pp.PresentationInterval;
    RET(hr, 2);
}

// Sources may be system-memory textures written through a persistent lock
// pointer; push those writes to the host first.
void B_IDirect3DDevice9_UpdateTexture(CPU *c) {
    auto *dev = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *src = (IDirect3DBaseTexture9 *)com_host(ARG(1));
    auto *dst = (IDirect3DBaseTexture9 *)com_host(ARG(2));
    shadow_flush(src);
    RET(dev->UpdateTexture(src, dst), 3);
}

void B_IDirect3DDevice9_UpdateSurface(CPU *c) {
    auto *dev = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *src = (IDirect3DSurface9 *)com_host(ARG(1));
    auto *dst = (IDirect3DSurface9 *)com_host(ARG(3));
    shadow_flush(src);
    RET(dev->UpdateSurface(src, (const RECT *)GPTR(ARG(2)), dst, (const POINT *)GPTR(ARG(4))), 5);
}

// ---------------------------------------------------------------- locks
// Guest code cannot address host memory, so every lock hands the guest a
// shadow copy in guest memory and copies it back on unlock.
namespace {
struct Shadow { uint32_t guest; void *host; size_t size; DWORD flags; bool persist; pthread_t th; };
// A resource can be locked several times at once (nested locks of buffer
// ranges, or locks from several threads): every lock keeps its own shadow,
// and an unlock ends the newest one taken by the same thread.
std::map<std::pair<void *, uint32_t>, std::vector<Shadow>> g_shadows;
std::mutex g_shadow_lock;

// System-memory textures keep one guest buffer for their whole life: games
// (and Bink's frame buffers) keep writing through the pointer after unlock,
// which is valid on Windows for D3DPOOL_SYSTEMMEM. The buffer is copied to
// the host object on unlock and again before it is used as an update source.
struct Persist { uint32_t guest; size_t size; std::function<void(const uint8_t *)> upload; };
std::map<std::pair<void *, uint32_t>, Persist> g_persist;

uint32_t shadow_begin(void *obj, uint32_t sub, void *hostbits, size_t size, DWORD flags,
                      std::function<void(const uint8_t *)> upload = nullptr) {
    uint32_t g = 0;
    bool persist = upload != nullptr;
    std::lock_guard<std::mutex> l(g_shadow_lock);
    if (persist) {
        auto it = g_persist.find({obj, sub});
        if (it != g_persist.end() && it->second.size == size) {
            g = it->second.guest; // guest copy is authoritative
        } else {
            if (it != g_persist.end()) gfree(it->second.guest);
            g = gmalloc((uint32_t)size + 16);
            if (!g) rt_fatal("out of guest memory for a %zu byte lock", size);
            memcpy(MEM + g, hostbits, size);
            g_persist[{obj, sub}] = Persist{g, size, upload};
        }
    } else {
        g = gmalloc((uint32_t)size + 16);
        if (!g) rt_fatal("out of guest memory for a %zu byte lock", size);
        if (!(flags & D3DLOCK_DISCARD)) memcpy(MEM + g, hostbits, size);
    }
    g_shadows[{obj, sub}].push_back(Shadow{g, hostbits, size, flags, persist, pthread_self()});
    return g;
}

bool shadow_end(void *obj, uint32_t sub) {
    Shadow s;
    {
        std::lock_guard<std::mutex> l(g_shadow_lock);
        auto it = g_shadows.find({obj, sub});
        if (it == g_shadows.end() || it->second.empty()) return false;
        auto &v = it->second;
        size_t k = v.size() - 1;
        for (size_t j = v.size(); j-- > 0;)
            if (pthread_equal(v[j].th, pthread_self())) { k = j; break; }
        s = v[k];
        v.erase(v.begin() + k);
        if (v.empty()) g_shadows.erase(it);
    }
    if (!(s.flags & D3DLOCK_READONLY)) memcpy(s.host, MEM + s.guest, s.size);
    if (!s.persist) gfree(s.guest);
    return true;
}

// Vertex/index buffers keep a guest mirror of their whole contents. A lock
// hands out the mirror at the locked offset and the unlock copies the locked
// range back, so a NOOVERWRITE lock of a big range that writes only part of
// it leaves the rest intact, without reading the (write-combined) mapping.
std::map<void *, std::pair<uint32_t, uint32_t>> g_bufmirror;  // obj -> guest, size

uint32_t buffer_shadow(void *obj, uint32_t total, uint32_t off, void *hostbits, uint32_t size, DWORD flags) {
    std::lock_guard<std::mutex> l(g_shadow_lock);
    auto it = g_bufmirror.find(obj);
    if (it == g_bufmirror.end() || it->second.second != total) {
        if (it != g_bufmirror.end()) gfree(it->second.first);
        uint32_t g = gmalloc(total + 16);
        if (!g) rt_fatal("out of guest memory for a %u byte buffer", total);
        memset(MEM + g, 0, total);
        // the first lock sees what the buffer holds (zero for a new buffer)
        if (!(flags & D3DLOCK_DISCARD)) memcpy(MEM + g + off, hostbits, size);
        it = g_bufmirror.insert_or_assign(obj, std::make_pair(g, total)).first;
    }
    uint32_t g = it->second.first + off;
    g_shadows[{obj, 0}].push_back(Shadow{g, hostbits, size, flags, true, pthread_self()});
    return g;
}
}

// Pushes guest-side writes made since the last unlock into the host object.
void shadow_flush(void *obj) {
    std::vector<std::pair<const uint8_t *, std::function<void(const uint8_t *)>>> todo;
    {
        std::lock_guard<std::mutex> l(g_shadow_lock);
        for (auto &kv : g_persist)
            if (kv.first.first == obj && !g_shadows.count(kv.first)) todo.push_back({MEM + kv.second.guest, kv.second.upload});
    }
    for (auto &t : todo) t.second(t.first);
}

void shadow_forget(void *obj) {
    std::lock_guard<std::mutex> l(g_shadow_lock);
    auto bm = g_bufmirror.find(obj);
    if (bm != g_bufmirror.end()) { gfree(bm->second.first); g_bufmirror.erase(bm); }
    for (auto it = g_persist.begin(); it != g_persist.end();) {
        if (it->first.first == obj) { gfree(it->second.guest); it = g_persist.erase(it); }
        else ++it;
    }
}

namespace {
bool is_block_format(D3DFORMAT f) {
    switch ((uint32_t)f) {
    case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
    case MAKEFOURCC('A', 'T', 'I', '1'): case MAKEFOURCC('A', 'T', 'I', '2'):
        return true;
    default:
        return false;
    }
}

uint32_t format_bits(D3DFORMAT f) {
    switch ((uint32_t)f) {
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2: return 8;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_V8U8: case D3DFMT_A8R3G3B2:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_L6V5U5: return 16;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16: return 64;
    case D3DFMT_A32B32G32R32F: return 128;
    default: return 32;
    }
}

// Bytes from the first locked byte to the last: with a rect, pBits points at
// its top-left corner, so the last row ends at its right edge, not a full
// pitch later (copying pitch * h would run past the end of the surface).
size_t rect_bytes(const D3DSURFACE_DESC &desc, const RECT *r, INT pitch) {
    uint32_t h = r ? (uint32_t)(r->bottom - r->top) : desc.Height;
    uint32_t w = r ? (uint32_t)(r->right - r->left) : desc.Width;
    size_t row;
    if (is_block_format(desc.Format)) {
        h = (h + 3) / 4;
        bool b8 = desc.Format == D3DFMT_DXT1 || desc.Format == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1');
        row = (size_t)((w + 3) / 4) * (b8 ? 8 : 16);
    } else {
        row = ((size_t)w * format_bits(desc.Format) + 7) / 8;
    }
    if (!h) return 0;
    if (row > (size_t)pitch) row = (size_t)pitch;
    return (size_t)pitch * (h - 1) + row;
}

void lock_rect_common(CPU *c, void *obj, uint32_t sub, const D3DSURFACE_DESC &desc, HRESULT hr,
                      const D3DLOCKED_RECT &lr, const RECT *r, DWORD flags, uint32_t out,
                      std::function<void(const uint8_t *)> upload = nullptr) {
    if (FAILED(hr)) { M32(out) = 0; M32(out + 4) = 0; return; }
    if (desc.Pool != D3DPOOL_SYSTEMMEM || r) upload = nullptr;
    uint32_t g = shadow_begin(obj, sub, lr.pBits, rect_bytes(desc, r, lr.Pitch), flags, upload);
    M32(out) = (uint32_t)lr.Pitch;
    M32(out + 4) = g;
}
}

void B_IDirect3DSurface9_LockRect(CPU *c) {
    auto *s = (IDirect3DSurface9 *)com_host(ARG(0));
    const RECT *r = (const RECT *)GPTR(ARG(2));
    D3DSURFACE_DESC desc;
    s->GetDesc(&desc);
    void *bt; uint32_t bsub; HRESULT hr;
    if (bc_surface_target(s, &bt, &bsub) &&
        bc_lock(bt, bsub, desc.Width, desc.Height, r, ARG(3), ARG(1),
                [&](D3DLOCKED_RECT *l, const RECT *rr) { return s->LockRect(l, rr, ARG(3)); }, &hr))
        RET(hr, 4);
    D3DLOCKED_RECT lr;
    hr = s->LockRect(&lr, r, ARG(3));
    size_t bytes = rect_bytes(desc, nullptr, lr.Pitch);
    lock_rect_common(c, s, 0, desc, hr, lr, r, ARG(3), ARG(1), [s, bytes](const uint8_t *src) {
        D3DLOCKED_RECT l2;
        if (SUCCEEDED(s->LockRect(&l2, nullptr, 0))) {
            memcpy(l2.pBits, src, bytes);
            s->UnlockRect();
        }
    });
    RET(hr, 4);
}

void B_IDirect3DSurface9_UnlockRect(CPU *c) {
    auto *s = (IDirect3DSurface9 *)com_host(ARG(0));
    void *bt; uint32_t bsub;
    if (bc_surface_target(s, &bt, &bsub)) bc_unlock(bt, bsub);
    shadow_end(s, 0);
    RET(s->UnlockRect(), 1);
}

void B_IDirect3DTexture9_LockRect(CPU *c) {
    auto *t = (IDirect3DTexture9 *)com_host(ARG(0));
    uint32_t level = ARG(1);
    const RECT *r = (const RECT *)GPTR(ARG(3));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    HRESULT hr;
    if (bc_lock(t, level, desc.Width, desc.Height, r, ARG(4), ARG(2),
                [&](D3DLOCKED_RECT *l, const RECT *rr) { return t->LockRect(level, l, rr, ARG(4)); }, &hr))
        RET(hr, 5);
    D3DLOCKED_RECT lr;
    hr = t->LockRect(level, &lr, r, ARG(4));
    size_t bytes = rect_bytes(desc, nullptr, lr.Pitch);
    lock_rect_common(c, t, level, desc, hr, lr, r, ARG(4), ARG(2), [t, level, bytes](const uint8_t *src) {
        D3DLOCKED_RECT l2;
        if (SUCCEEDED(t->LockRect(level, &l2, nullptr, 0))) {
            memcpy(l2.pBits, src, bytes);
            t->UnlockRect(level);
        }
    });
    RET(hr, 5);
}

void B_IDirect3DTexture9_UnlockRect(CPU *c) {
    auto *t = (IDirect3DTexture9 *)com_host(ARG(0));
    bc_unlock(t, ARG(1));
    shadow_end(t, ARG(1));
    RET(t->UnlockRect(ARG(1)), 2);
}

void B_IDirect3DCubeTexture9_LockRect(CPU *c) {
    auto *t = (IDirect3DCubeTexture9 *)com_host(ARG(0));
    uint32_t face = ARG(1), level = ARG(2);
    const RECT *r = (const RECT *)GPTR(ARG(4));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    HRESULT hr;
    if (bc_lock(t, face << 16 | level, desc.Width, desc.Height, r, ARG(5), ARG(3),
                [&](D3DLOCKED_RECT *l, const RECT *rr) { return t->LockRect((D3DCUBEMAP_FACES)face, level, l, rr, ARG(5)); }, &hr))
        RET(hr, 6);
    D3DLOCKED_RECT lr;
    hr = t->LockRect((D3DCUBEMAP_FACES)face, level, &lr, r, ARG(5));
    lock_rect_common(c, t, face << 16 | level, desc, hr, lr, r, ARG(5), ARG(3));
    RET(hr, 6);
}

void B_IDirect3DCubeTexture9_UnlockRect(CPU *c) {
    auto *t = (IDirect3DCubeTexture9 *)com_host(ARG(0));
    bc_unlock(t, ARG(1) << 16 | ARG(2));
    shadow_end(t, ARG(1) << 16 | ARG(2));
    RET(t->UnlockRect((D3DCUBEMAP_FACES)ARG(1), ARG(2)), 3);
}

static void lock_box_common(void *obj, uint32_t sub, const D3DVOLUME_DESC &desc, HRESULT hr,
                            const D3DLOCKED_BOX &lb, const D3DBOX *b, DWORD flags, uint32_t out) {
    if (FAILED(hr)) { M32(out) = M32(out + 4) = M32(out + 8) = 0; return; }
    uint32_t depth = b ? b->Back - b->Front : desc.Depth;
    size_t size = (size_t)lb.SlicePitch * depth;
    uint32_t g = shadow_begin(obj, sub, lb.pBits, size, flags);
    M32(out) = (uint32_t)lb.RowPitch;
    M32(out + 4) = (uint32_t)lb.SlicePitch;
    M32(out + 8) = g;
}

void B_IDirect3DVolumeTexture9_LockBox(CPU *c) {
    auto *t = (IDirect3DVolumeTexture9 *)com_host(ARG(0));
    D3DVOLUME_DESC desc;
    t->GetLevelDesc(ARG(1), &desc);
    D3DLOCKED_BOX lb;
    const D3DBOX *b = (const D3DBOX *)GPTR(ARG(3));
    HRESULT hr = t->LockBox(ARG(1), &lb, b, ARG(4));
    lock_box_common(t, ARG(1), desc, hr, lb, b, ARG(4), ARG(2));
    RET(hr, 5);
}

void B_IDirect3DVolumeTexture9_UnlockBox(CPU *c) {
    auto *t = (IDirect3DVolumeTexture9 *)com_host(ARG(0));
    shadow_end(t, ARG(1));
    RET(t->UnlockBox(ARG(1)), 2);
}

void B_IDirect3DVolume9_LockBox(CPU *c) {
    auto *v = (IDirect3DVolume9 *)com_host(ARG(0));
    D3DVOLUME_DESC desc;
    v->GetDesc(&desc);
    D3DLOCKED_BOX lb;
    const D3DBOX *b = (const D3DBOX *)GPTR(ARG(2));
    HRESULT hr = v->LockBox(&lb, b, ARG(3));
    lock_box_common(v, 0, desc, hr, lb, b, ARG(3), ARG(1));
    RET(hr, 4);
}

void B_IDirect3DVolume9_UnlockBox(CPU *c) {
    auto *v = (IDirect3DVolume9 *)com_host(ARG(0));
    shadow_end(v, 0);
    RET(v->UnlockBox(), 1);
}

template <class B, class D>
static void buffer_lock(CPU *c) {
    auto *b = (B *)com_host(ARG(0));
    uint32_t off = ARG(1), size = ARG(2), flags = ARG(4);
    D desc;
    b->GetDesc(&desc);
    if (off > desc.Size) off = desc.Size;
    if (size == 0 || (uint64_t)off + size > desc.Size) size = desc.Size - off;
    void *p = nullptr;
    HRESULT hr = b->Lock(off, size, &p, flags);
    if (FAILED(hr) || !p) { M32(ARG(3)) = 0; RET(hr, 5); }
    M32(ARG(3)) = buffer_shadow(b, desc.Size, off, p, size, flags);
    RET(hr, 5);
}

template <class B>
static void buffer_unlock(CPU *c) {
    auto *b = (B *)com_host(ARG(0));
    shadow_end(b, 0);
    RET(b->Unlock(), 1);
}

void B_IDirect3DVertexBuffer9_Lock(CPU *c) { buffer_lock<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(c); }
void B_IDirect3DVertexBuffer9_Unlock(CPU *c) { buffer_unlock<IDirect3DVertexBuffer9>(c); }
void B_IDirect3DIndexBuffer9_Lock(CPU *c) { buffer_lock<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(c); }
void B_IDirect3DIndexBuffer9_Unlock(CPU *c) { buffer_unlock<IDirect3DIndexBuffer9>(c); }

// ---------------------------------------------------------------- misc

void B_IDirect3DSurface9_GetContainer(CPU *c) {
    auto *s = (IDirect3DSurface9 *)com_host(ARG(0));
    GUID iid;
    memcpy(&iid, GPTR(ARG(1)), sizeof iid);
    void *out = nullptr;
    HRESULT hr = s->GetContainer(iid, &out);
    int iface = d3d9_iface_from_iid(iid);
    M32(ARG(2)) = SUCCEEDED(hr) && out && iface >= 0 ? d3d9_wrap(out, iface) : 0;
    RET(hr, 3);
}

void B_IDirect3DVolume9_GetContainer(CPU *c) {
    auto *v = (IDirect3DVolume9 *)com_host(ARG(0));
    GUID iid;
    memcpy(&iid, GPTR(ARG(1)), sizeof iid);
    void *out = nullptr;
    HRESULT hr = v->GetContainer(iid, &out);
    int iface = d3d9_iface_from_iid(iid);
    M32(ARG(2)) = SUCCEEDED(hr) && out && iface >= 0 ? d3d9_wrap(out, iface) : 0;
    RET(hr, 3);
}

void B_IDirect3DSurface9_GetDC(CPU *c) { RET(D3DERR_INVALIDCALL, 2); }
void B_IDirect3DSurface9_ReleaseDC(CPU *c) { RET(D3DERR_INVALIDCALL, 2); }

// ---------------------------------------------------------------- entry point
void d3d9_register_vtables(void);

static void shim_Direct3DCreate9(CPU *c) {
    static bool init = false;
    if (!init) { d3d9_register_vtables(); init = true; }
    LOG("Direct3DCreate9(%u)", ARG(0));
    rt_host_affinity_begin();
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    rt_host_affinity_end();
    RET(d3d9_wrap(d3d, IFACE_IDirect3D9), 1);
}

static const ShimDef defs[] = {
    {"d3d9.dll", "Direct3DCreate9", shim_Direct3DCreate9},
    {nullptr, nullptr, nullptr},
};

extern "C" void d3d9_init(void) { rt_register_shims(defs); }
