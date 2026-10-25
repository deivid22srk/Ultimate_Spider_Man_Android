// BC (DXT1-5) texture emulation for GPUs whose Vulkan driver lacks
// textureCompressionBC (common on Android). Such textures are created as
// A8R8G8B8 on the host; the guest still sees a DXT texture: descriptions
// report the DXT format, and locks hand out a compressed buffer that is
// decoded into the host texture on unlock.
//
// Enabled automatically when the device rejects DXT1, or with LB_BCEMU=1.
#include "d3d9_bridge.h"
#include <functional>
#include <map>
#include <mutex>
#include <tuple>

namespace {
bool g_decided, g_emulate;
std::mutex g_bc_lock;

struct BcTex { D3DFORMAT fmt; };
std::map<void *, BcTex> g_tex;                       // host texture -> real format
struct BcSurf { void *tex; uint32_t sub; };
std::map<void *, BcSurf> g_surf;                     // host level surface -> its texture level
struct BcLevel { uint32_t guest; uint32_t pitch, rows; };
std::map<std::pair<void *, uint32_t>, BcLevel> g_levels; // compressed copy per level
struct BcLock { uint8_t *host; INT host_pitch; RECT r; };
std::map<std::pair<void *, uint32_t>, BcLock> g_locks;

bool is_dxt(uint32_t f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

uint32_t block_bytes(D3DFORMAT f) { return f == D3DFMT_DXT1 ? 8 : 16; }

void rgb565(uint16_t v, uint8_t *o) {
    o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((v & 31) * 255 / 31);
}

// Decodes one 4x4 block into BGRA rows of `out` (pitch in bytes), clipped to w x h.
void decode_block(D3DFORMAT f, const uint8_t *b, uint8_t *out, int pitch, int w, int h) {
    uint8_t alpha[16];
    for (int i = 0; i < 16; i++) alpha[i] = 255;
    const uint8_t *cb = b;
    if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3) {
        for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
        cb = b + 8;
    } else if (f == D3DFMT_DXT4 || f == D3DFMT_DXT5) {
        uint8_t a[8];
        a[0] = b[0]; a[1] = b[1];
        if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
        else {
            for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5);
            a[6] = 0; a[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7];
        cb = b + 8;
    }
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint8_t col[4][4]; // r g b a
    rgb565(c0, col[0]); rgb565(c1, col[1]);
    col[0][3] = col[1][3] = col[2][3] = col[3][3] = 255;
    if (c0 > c1 || f != D3DFMT_DXT1) {
        for (int k = 0; k < 3; k++) {
            col[2][k] = (uint8_t)((2 * col[0][k] + col[1][k]) / 3);
            col[3][k] = (uint8_t)((col[0][k] + 2 * col[1][k]) / 3);
        }
    } else {
        for (int k = 0; k < 3; k++) { col[2][k] = (uint8_t)((col[0][k] + col[1][k]) / 2); col[3][k] = 0; }
        col[3][3] = 0;
    }
    uint32_t idx = (uint32_t)(cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24);
    for (int y = 0; y < h && y < 4; y++) {
        uint8_t *row = out + y * pitch;
        for (int x = 0; x < w && x < 4; x++) {
            int i = y * 4 + x;
            const uint8_t *cc = col[(idx >> (2 * i)) & 3];
            row[4 * x + 0] = cc[2];
            row[4 * x + 1] = cc[1];
            row[4 * x + 2] = cc[0];
            row[4 * x + 3] = (uint8_t)(cc[3] * alpha[i] / 255);
        }
    }
}

// Decode rect `r` (pixels, block aligned at the top-left) of a compressed level.
void decode_rect(D3DFORMAT f, const BcLevel &lv, const RECT &r, uint8_t *host, INT host_pitch) {
    uint32_t bb = block_bytes(f);
    for (LONG by = r.top; by < r.bottom; by += 4) {
        for (LONG bx = r.left; bx < r.right; bx += 4) {
            const uint8_t *blk = MEM + lv.guest + (by / 4) * lv.pitch + (bx / 4) * bb;
            uint8_t *o = host + (by - r.top) * host_pitch + (bx - r.left) * 4;
            decode_block(f, blk, o, host_pitch, (int)(r.right - bx), (int)(r.bottom - by));
        }
    }
}
} // namespace

bool bc_emulating(IDirect3D9 *d3d) {
    if (!g_decided && d3d) {
        g_decided = true;
        const char *e = getenv("LB_BCEMU");
        if (e) g_emulate = atoi(e) != 0;
        else g_emulate = FAILED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                                       D3DRTYPE_TEXTURE, D3DFMT_DXT1));
        LOG("BC texture emulation %s", g_emulate ? "on" : "off");
    }
    return g_emulate;
}

static bool bc_emulating_dev(IDirect3DDevice9 *d) {
    if (!g_decided) {
        IDirect3D9 *d3d = nullptr;
        d->GetDirect3D(&d3d);
        bc_emulating(d3d);
        if (d3d) d3d->Release();
    }
    return g_emulate;
}

void bc_forget(void *obj) {
    std::lock_guard<std::mutex> l(g_bc_lock);
    if (g_tex.erase(obj)) {
        for (auto it = g_levels.begin(); it != g_levels.end();) {
            if (it->first.first == obj) { gfree(it->second.guest); it = g_levels.erase(it); }
            else ++it;
        }
        for (auto it = g_surf.begin(); it != g_surf.end();)
            it = it->second.tex == obj ? g_surf.erase(it) : std::next(it);
    }
    g_surf.erase(obj);
}

void B_IDirect3D9_CheckDeviceFormat(CPU *c) {
    auto *d3d = (IDirect3D9 *)com_host(ARG(0));
    uint32_t check = ARG(6);
    if (is_dxt(check) && ARG(4) == 0 && bc_emulating(d3d)) RET(D3D_OK, 7);
    RET(d3d->CheckDeviceFormat(ARG(1), (D3DDEVTYPE)ARG(2), (D3DFORMAT)ARG(3), ARG(4),
                               (D3DRESOURCETYPE)ARG(5), (D3DFORMAT)check), 7);
}

// Logs every failed creation and the first texture of each format/pool/usage,
// so a log shows which kinds of texture a missing image could be.
static void tex_census(const char *kind, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage,
                       uint32_t fmt, uint32_t pool, HRESULT hr) {
    static std::mutex m;
    static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int> seen;
    std::lock_guard<std::mutex> l(m);
    bool first = seen[{fmt, pool, usage}]++ == 0;
    if (FAILED(hr) || first)
        LOG("%s %ux%u levels %u usage %x fmt %x pool %u -> %08x%s", kind, w, h, levels, usage, fmt, pool,
            (uint32_t)hr, FAILED(hr) ? " FAILED" : "");
}

void B_IDirect3DDevice9_CreateTexture(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    D3DFORMAT fmt = (D3DFORMAT)ARG(5);
    bool emu = is_dxt(fmt) && bc_emulating_dev(d);
    IDirect3DTexture9 *t = nullptr;
    HRESULT hr = d->CreateTexture(ARG(1), ARG(2), ARG(3), ARG(4), emu ? D3DFMT_A8R8G8B8 : fmt,
                                  (D3DPOOL)ARG(6), &t, nullptr);
    tex_census("CreateTexture", ARG(1), ARG(2), ARG(3), ARG(4), fmt, ARG(6), hr);
    if (SUCCEEDED(hr) && emu) {
        std::lock_guard<std::mutex> l(g_bc_lock);
        g_tex[t] = BcTex{fmt};
    }
    if (ARG(7)) M32(ARG(7)) = SUCCEEDED(hr) ? d3d9_wrap(t, IFACE_IDirect3DTexture9) : 0;
    RET(hr, 9);
}

void B_IDirect3DDevice9_CreateCubeTexture(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    D3DFORMAT fmt = (D3DFORMAT)ARG(4);
    bool emu = is_dxt(fmt) && bc_emulating_dev(d);
    IDirect3DCubeTexture9 *t = nullptr;
    HRESULT hr = d->CreateCubeTexture(ARG(1), ARG(2), ARG(3), emu ? D3DFMT_A8R8G8B8 : fmt,
                                      (D3DPOOL)ARG(5), &t, nullptr);
    tex_census("CreateCubeTexture", ARG(1), ARG(1), ARG(2), ARG(3), fmt, ARG(5), hr);
    if (SUCCEEDED(hr) && emu) {
        std::lock_guard<std::mutex> l(g_bc_lock);
        g_tex[t] = BcTex{fmt};
    }
    if (ARG(6)) M32(ARG(6)) = SUCCEEDED(hr) ? d3d9_wrap(t, IFACE_IDirect3DCubeTexture9) : 0;
    RET(hr, 8);
}

static D3DFORMAT bc_format(void *tex) {
    std::lock_guard<std::mutex> l(g_bc_lock);
    auto it = g_tex.find(tex);
    return it == g_tex.end() ? D3DFMT_UNKNOWN : it->second.fmt;
}

static void put_desc(uint32_t g, const D3DSURFACE_DESC &d, D3DFORMAT fmt) {
    M32(g + 0) = fmt ? (uint32_t)fmt : (uint32_t)d.Format;
    M32(g + 4) = d.Type;
    M32(g + 8) = d.Usage;
    M32(g + 12) = d.Pool;
    M32(g + 16) = d.MultiSampleType;
    M32(g + 20) = d.MultiSampleQuality;
    M32(g + 24) = d.Width;
    M32(g + 28) = d.Height;
}

void B_IDirect3DTexture9_GetLevelDesc(CPU *c) {
    auto *t = (IDirect3DTexture9 *)com_host(ARG(0));
    D3DSURFACE_DESC d;
    HRESULT hr = t->GetLevelDesc(ARG(1), &d);
    if (SUCCEEDED(hr)) put_desc(ARG(2), d, bc_format(t));
    RET(hr, 3);
}

void B_IDirect3DCubeTexture9_GetLevelDesc(CPU *c) {
    auto *t = (IDirect3DCubeTexture9 *)com_host(ARG(0));
    D3DSURFACE_DESC d;
    HRESULT hr = t->GetLevelDesc(ARG(1), &d);
    if (SUCCEEDED(hr)) put_desc(ARG(2), d, bc_format(t));
    RET(hr, 3);
}

void B_IDirect3DSurface9_GetDesc(CPU *c) {
    auto *s = (IDirect3DSurface9 *)com_host(ARG(0));
    D3DSURFACE_DESC d;
    HRESULT hr = s->GetDesc(&d);
    D3DFORMAT f = D3DFMT_UNKNOWN;
    {
        std::lock_guard<std::mutex> l(g_bc_lock);
        auto it = g_surf.find(s);
        if (it != g_surf.end()) { auto t = g_tex.find(it->second.tex); if (t != g_tex.end()) f = t->second.fmt; }
    }
    if (SUCCEEDED(hr)) put_desc(ARG(1), d, f);
    RET(hr, 2);
}

void B_IDirect3DTexture9_GetSurfaceLevel(CPU *c) {
    auto *t = (IDirect3DTexture9 *)com_host(ARG(0));
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = t->GetSurfaceLevel(ARG(1), &s);
    if (SUCCEEDED(hr) && bc_format(t)) {
        std::lock_guard<std::mutex> l(g_bc_lock);
        g_surf[s] = BcSurf{t, ARG(1)};
    }
    if (ARG(2)) M32(ARG(2)) = SUCCEEDED(hr) ? d3d9_wrap(s, IFACE_IDirect3DSurface9) : 0;
    RET(hr, 3);
}

void B_IDirect3DCubeTexture9_GetCubeMapSurface(CPU *c) {
    auto *t = (IDirect3DCubeTexture9 *)com_host(ARG(0));
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = t->GetCubeMapSurface((D3DCUBEMAP_FACES)ARG(1), ARG(2), &s);
    if (SUCCEEDED(hr) && bc_format(t)) {
        std::lock_guard<std::mutex> l(g_bc_lock);
        g_surf[s] = BcSurf{t, ARG(1) << 16 | ARG(2)};
    }
    if (ARG(3)) M32(ARG(3)) = SUCCEEDED(hr) ? d3d9_wrap(s, IFACE_IDirect3DSurface9) : 0;
    RET(hr, 4);
}

// If `tex`/`sub` is an emulated level, locks it: the host level is locked
// (as RGBA) and the guest gets a pointer into the level's compressed copy.
// `lock` locks the host rect. Returns false if the object is not emulated.
bool bc_lock(void *tex, uint32_t sub, uint32_t w, uint32_t h, const RECT *r, DWORD flags, uint32_t out,
             const std::function<HRESULT(D3DLOCKED_RECT *, const RECT *)> &lock, HRESULT *hr) {
    D3DFORMAT f = bc_format(tex);
    if (!f) return false;
    RECT rr = r ? *r : RECT{0, 0, (LONG)w, (LONG)h};
    rr.left &= ~3; rr.top &= ~3;
    D3DLOCKED_RECT lr;
    *hr = lock(&lr, &rr);
    if (FAILED(*hr)) { M32(out) = 0; M32(out + 4) = 0; return true; }
    std::lock_guard<std::mutex> l(g_bc_lock);
    auto key = std::make_pair(tex, sub);
    auto it = g_levels.find(key);
    if (it == g_levels.end()) {
        BcLevel lv;
        lv.pitch = ((w + 3) / 4) * block_bytes(f);
        lv.rows = (h + 3) / 4;
        lv.guest = gmalloc(lv.pitch * lv.rows + 16);
        if (!lv.guest) rt_fatal("out of guest memory for a BC level");
        memset(MEM + lv.guest, 0, lv.pitch * lv.rows);
        it = g_levels.emplace(key, lv).first;
    }
    g_locks[key] = BcLock{(uint8_t *)lr.pBits, lr.Pitch, rr};
    M32(out) = it->second.pitch;
    M32(out + 4) = it->second.guest + (rr.top / 4) * it->second.pitch + (rr.left / 4) * block_bytes(f);
    (void)flags;
    return true;
}

bool bc_unlock(void *tex, uint32_t sub) {
    D3DFORMAT f = bc_format(tex);
    if (!f) return false;
    std::lock_guard<std::mutex> l(g_bc_lock);
    auto key = std::make_pair(tex, sub);
    auto lk = g_locks.find(key);
    if (lk == g_locks.end()) return true;
    decode_rect(f, g_levels[key], lk->second.r, lk->second.host, lk->second.host_pitch);
    g_locks.erase(lk);
    return true;
}

// A level surface of an emulated texture: route to its texture's level.
bool bc_surface_target(void *surf, void **tex, uint32_t *sub) {
    std::lock_guard<std::mutex> l(g_bc_lock);
    auto it = g_surf.find(surf);
    if (it == g_surf.end()) return false;
    *tex = it->second.tex;
    *sub = it->second.sub;
    return true;
}
