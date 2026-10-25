/* DirectSound 8 shim: secondary buffers live in guest memory (so Lock can
 * hand out direct pointers) and are mixed in software on SDL's audio
 * thread. The primary buffer only exists to satisfy the API. */
#include "hcom.h"
#include <SDL2/SDL.h>

#define MIX_RATE 44100
#define MAX_BUFFERS 512

typedef struct {
    uint32_t data;      /* guest address */
    uint32_t size;      /* bytes */
    int refs;           /* shared between duplicates */
} DSData;

typedef struct {
    DSData *d;
    int primary;
    int channels, bits, rate;
    uint32_t flags;
    int32_t volume, pan;
    uint32_t freq;
    double pos;         /* in frames */
    int playing, looping;
    uint32_t guest;     /* guest object */
    /* IDirectSound3DListener / IDirectSound3DBuffer, handed out lazily when the
     * game queries them on a buffer. The parameters are stored faithfully but
     * not applied to the mix (all sources stay centred) -- the game needs the
     * calls to succeed, it never reads the result back. */
    uint32_t listener, buf3d;
    struct { uint32_t dwSize; float pos[3], vel[3], front[3], top[3], dist, dopp, rolloff; } lp;
    struct { uint32_t dwSize; float pos[3], vel[3]; uint32_t inside, outside; float cone[3];
             int32_t cone_vol; float min_dist, max_dist; uint32_t mode; } bp;
} DSBuf;

static HComClass k_ds8, k_dsb, k_ds3dl, k_ds3db;
static SDL_mutex *g_mix_lock;
static DSBuf *g_bufs[MAX_BUFFERS];
static SDL_AudioDeviceID g_dev;
static float g_master = 1.0f;

static float db_to_amp(int32_t v) {
    if (v <= -10000) return 0.0f;
    return powf(10.0f, (float)v / 2000.0f);
}

static inline float sample_at(DSBuf *b, uint32_t frame, int ch) {
    uint32_t bpf = (uint32_t)(b->channels * b->bits / 8);
    uint32_t nframes = b->d->size / bpf;
    if (frame >= nframes) return 0.0f;
    if (ch >= b->channels) ch = b->channels - 1;
    uint32_t a = b->d->data + frame * bpf + (uint32_t)ch * (uint32_t)(b->bits / 8);
    if (b->bits == 16) return (float)(int16_t)M16(a) / 32768.0f;
    return ((float)M8(a) - 128.0f) / 128.0f;
}

static void mix_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    float *out = (float *)stream;
    int frames = len / (int)(2 * sizeof(float));
    memset(stream, 0, (size_t)len);
    SDL_LockMutex(g_mix_lock);
    for (int k = 0; k < MAX_BUFFERS; k++) {
        DSBuf *b = g_bufs[k];
        if (!b || b->primary || !b->playing || !b->d->size) continue;
        uint32_t bpf = (uint32_t)(b->channels * b->bits / 8);
        double nframes = (double)(b->d->size / bpf);
        double step = (double)(b->freq ? b->freq : (uint32_t)b->rate) / MIX_RATE;
        float vol = db_to_amp(b->volume) * g_master;
        float lv = vol * (b->pan > 0 ? db_to_amp(-b->pan) : 1.0f);
        float rv = vol * (b->pan < 0 ? db_to_amp(b->pan) : 1.0f);
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)b->pos;
            float l = sample_at(b, f, 0), r = sample_at(b, f, 1);
            out[2 * i] += l * lv;
            out[2 * i + 1] += r * rv;
            b->pos += step;
            if (b->pos >= nframes) {
                if (b->looping) b->pos -= nframes;
                else { b->pos = 0; b->playing = 0; break; }
            }
        }
    }
    SDL_UnlockMutex(g_mix_lock);
    for (int i = 0; i < 2 * frames; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

static void audio_open(void) {
    if (g_dev) return;
    g_mix_lock = SDL_CreateMutex();
    SDL_AudioSpec want = {0}, have;
    want.freq = MIX_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix_cb;
    rt_host_affinity_begin();  /* SDL's audio thread runs no guest code */
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    rt_host_affinity_end();
    if (!g_dev) LOG("audio: SDL_OpenAudioDevice failed: %s (continuing silently)", SDL_GetError());
    else SDL_PauseAudioDevice(g_dev, 0);
}

static uint32_t buf_register(DSBuf *b) {
    SDL_LockMutex(g_mix_lock);
    for (int k = 0; k < MAX_BUFFERS; k++)
        if (!g_bufs[k]) { g_bufs[k] = b; break; }
    SDL_UnlockMutex(g_mix_lock);
    b->guest = hcom_new(&k_dsb, b);
    return b->guest;
}

static void buf_destroy(DSBuf *b) {
    SDL_LockMutex(g_mix_lock);
    for (int k = 0; k < MAX_BUFFERS; k++)
        if (g_bufs[k] == b) g_bufs[k] = NULL;
    SDL_UnlockMutex(g_mix_lock);
    if (--b->d->refs == 0) {
        if (b->d->data) gfree(b->d->data);
        free(b->d);
    }
    free(b);
}

/* ---------------- IDirectSoundBuffer8 ---------------- */
#define B ((DSBuf *)hcom_host(ARG(0)))
SHIM(dsb_QueryInterface) {
    /* IDirectSoundBuffer8 / IDirectSoundBuffer: same object. 3D/notify: not supported yet. */
    uint8_t iid[16];
    memcpy(iid, MEM + ARG(1), 16);
    uint32_t d1 = iid[0] | iid[1] << 8 | iid[2] << 16 | (uint32_t)iid[3] << 24;
    if (d1 == 0x279AFA85u || d1 == 0x6825A449u || d1 == 0) { /* IDirectSoundBuffer, IDirectSoundBuffer8, IUnknown */
        hcom_addref(ARG(0));
        M32(ARG(2)) = ARG(0);
        RET(0, 3);
    }
    if (d1 == 0x279AFA84u) { /* IDirectSound3DListener (279afa84-4981-11ce-a521-0020af0be560) */
        DSBuf *b = B;
        if (!b->listener) b->listener = hcom_new(&k_ds3dl, b);
        hcom_addref(b->listener);
        M32(ARG(2)) = b->listener;
        RET(0, 3);
    }
    if (d1 == 0x279AFA86u) { /* IDirectSound3DBuffer (279afa86-4981-11ce-a521-0020af0be560) */
        DSBuf *b = B;
        if (!b->buf3d) b->buf3d = hcom_new(&k_ds3db, b);
        hcom_addref(b->buf3d);
        M32(ARG(2)) = b->buf3d;
        RET(0, 3);
    }
    LOG("dsound: QueryInterface for unsupported %08x", d1);
    M32(ARG(2)) = 0;
    RET(E_NOINTERFACE_, 3);
}
SHIM(dsb_AddRef) { RET(hcom_addref(ARG(0)), 1); }
SHIM(dsb_Release) {
    uint32_t left;
    DSBuf *b = B;
    if (hcom_release(ARG(0), &left)) buf_destroy(b);
    RET(left, 1);
}
SHIM(dsb_GetCaps) {
    uint32_t p = ARG(1);
    M32(p + 4) = B->flags;
    M32(p + 8) = B->d->size;
    M32(p + 12) = 0;
    M32(p + 16) = 0;
    RET(0, 2);
}
SHIM(dsb_GetCurrentPosition) {
    DSBuf *b = B;
    SDL_LockMutex(g_mix_lock);
    uint32_t bpf = (uint32_t)(b->channels * b->bits / 8);
    uint32_t play = b->primary || !b->d->size ? 0 : ((uint32_t)b->pos * bpf) % b->d->size;
    SDL_UnlockMutex(g_mix_lock);
    uint32_t ahead = b->rate * bpf / 50; /* 20 ms */
    if (ARG(1)) M32(ARG(1)) = play;
    if (ARG(2)) M32(ARG(2)) = b->d->size ? (play + ahead) % b->d->size : 0;
    RET(0, 3);
}
SHIM(dsb_GetFormat) {
    DSBuf *b = B;
    uint32_t p = ARG(1);
    if (p) {
        M16(p) = 1;
        M16(p + 2) = (uint16_t)b->channels;
        M32(p + 4) = (uint32_t)b->rate;
        M16(p + 12) = (uint16_t)(b->channels * b->bits / 8);
        M32(p + 8) = (uint32_t)b->rate * M16(p + 12);
        M16(p + 14) = (uint16_t)b->bits;
        if (ARG(2) >= 18) M16(p + 16) = 0;
    }
    if (ARG(3)) M32(ARG(3)) = 18;
    RET(0, 4);
}
SHIM(dsb_GetVolume) { M32(ARG(1)) = (uint32_t)B->volume; RET(0, 2); }
SHIM(dsb_GetPan) { M32(ARG(1)) = (uint32_t)B->pan; RET(0, 2); }
SHIM(dsb_GetFrequency) { M32(ARG(1)) = B->freq ? B->freq : (uint32_t)B->rate; RET(0, 2); }
SHIM(dsb_GetStatus) {
    DSBuf *b = B;
    M32(ARG(1)) = (b->playing ? 1u : 0u) | (b->playing && b->looping ? 4u : 0u);
    RET(0, 2);
}
SHIM(dsb_Initialize) { RET(0x88780082u, 3); } /* DSERR_ALREADYINITIALIZED */
SHIM(dsb_Lock) {
    /* (this, offset, bytes, p1, n1, p2, n2, flags) */
    DSBuf *b = B;
    uint32_t size = b->d->size;
    uint32_t off = ARG(1), n = ARG(2), fl = ARG(7);
    if (!size) { RET(0x88780032u, 8); }
    if (fl & 1) { /* DSBLOCK_FROMWRITECURSOR */
        uint32_t bpf = (uint32_t)(b->channels * b->bits / 8);
        off = (((uint32_t)b->pos * bpf) + (uint32_t)b->rate * bpf / 50) % size;
    }
    if (fl & 2) n = size; /* DSBLOCK_ENTIREBUFFER */
    if (n > size) n = size;
    off %= size;
    uint32_t n1 = n <= size - off ? n : size - off;
    M32(ARG(3)) = b->d->data + off;
    M32(ARG(4)) = n1;
    if (ARG(5)) M32(ARG(5)) = n1 < n ? b->d->data : 0;
    if (ARG(6)) M32(ARG(6)) = n - n1;
    RET(0, 8);
}
SHIM(dsb_Play) {
    DSBuf *b = B;
    SDL_LockMutex(g_mix_lock);
    b->playing = 1;
    b->looping = (ARG(3) & 1) != 0;
    SDL_UnlockMutex(g_mix_lock);
    RET(0, 4);
}
SHIM(dsb_SetCurrentPosition) {
    DSBuf *b = B;
    uint32_t bpf = (uint32_t)(b->channels * b->bits / 8);
    SDL_LockMutex(g_mix_lock);
    b->pos = bpf ? (double)(ARG(1) / bpf) : 0;
    SDL_UnlockMutex(g_mix_lock);
    RET(0, 2);
}
SHIM(dsb_SetFormat) {
    DSBuf *b = B;
    uint32_t w = ARG(1);
    if (w) { b->channels = M16(w + 2); b->rate = (int)M32(w + 4); b->bits = M16(w + 14); }
    RET(0, 2);
}
SHIM(dsb_SetVolume) { B->volume = (int32_t)ARG(1); RET(0, 2); }
SHIM(dsb_SetPan) { B->pan = (int32_t)ARG(1); RET(0, 2); }
SHIM(dsb_SetFrequency) { B->freq = ARG(1); RET(0, 2); }
SHIM(dsb_Stop) {
    SDL_LockMutex(g_mix_lock);
    B->playing = 0;
    SDL_UnlockMutex(g_mix_lock);
    RET(0, 1);
}
SHIM(dsb_Unlock) { RET(0, 5); }
SHIM(dsb_Restore) { RET(0, 1); }
SHIM(dsb_SetFX) { RET(E_NOTIMPL_, 4); }
SHIM(dsb_AcquireResources) { RET(0, 4); }
SHIM(dsb_GetObjectInPath) { RET(E_NOTIMPL_, 5); }
#undef B

/* ---------------- IDirectSound3DListener / IDirectSound3DBuffer ---------------- */
/* The game asks the primary buffer for a listener and every 3D buffer for a 3D
 * buffer interface, then calls methods on the result *without checking the
 * HRESULT*: returning E_NOINTERFACE crashed it on a NULL vtable. Both objects
 * keep their parameters (DS3DLISTENER / DS3DBUFFER layout) and return DS_OK. */
#define B3 ((DSBuf *)hcom_host(ARG(0)))
SHIM(ds3d_QueryInterface) { hcom_addref(ARG(0)); M32(ARG(2)) = ARG(0); RET(0, 3); }
SHIM(ds3d_AddRef) { RET(hcom_addref(ARG(0)), 1); }
SHIM(ds3d_Release) { uint32_t left; hcom_release(ARG(0), &left); RET(left, 1); }

SHIM(ds3dl_GetAllParameters) {
    DSBuf *b = B3;
    uint32_t p = ARG(1);
    if (p) { uint32_t n = M32(p); if (n > sizeof b->lp) n = sizeof b->lp; memcpy(MEM + p, &b->lp, n); M32(p) = sizeof b->lp; }
    RET(0, 2);
}
SHIM(ds3dl_SetAllParameters) {
    DSBuf *b = B3;
    uint32_t p = ARG(1);
    if (p) { uint32_t n = M32(p); if (n > sizeof b->lp) n = sizeof b->lp; memcpy(&b->lp, MEM + p, n); b->lp.dwSize = sizeof b->lp; }
    RET(0, 3);
}
SHIM(ds3dl_GetDistanceFactor) { if (ARG(1)) MF32(ARG(1)) = B3->lp.dist; RET(0, 2); }
SHIM(ds3dl_GetDopplerFactor) { if (ARG(1)) MF32(ARG(1)) = B3->lp.dopp; RET(0, 2); }
SHIM(ds3dl_GetRolloffFactor) { if (ARG(1)) MF32(ARG(1)) = B3->lp.rolloff; RET(0, 2); }
SHIM(ds3dl_GetOrientation) {
    DSBuf *b = B3;
    if (ARG(1)) { MF32(ARG(1)) = b->lp.front[0]; MF32(ARG(1) + 4) = b->lp.front[1]; MF32(ARG(1) + 8) = b->lp.front[2]; }
    if (ARG(2)) { MF32(ARG(2)) = b->lp.top[0]; MF32(ARG(2) + 4) = b->lp.top[1]; MF32(ARG(2) + 8) = b->lp.top[2]; }
    RET(0, 3);
}
SHIM(ds3dl_GetPosition) {
    DSBuf *b = B3;
    if (ARG(1)) { MF32(ARG(1)) = b->lp.pos[0]; MF32(ARG(1) + 4) = b->lp.pos[1]; MF32(ARG(1) + 8) = b->lp.pos[2]; }
    RET(0, 2);
}
SHIM(ds3dl_GetVelocity) {
    DSBuf *b = B3;
    if (ARG(1)) { MF32(ARG(1)) = b->lp.vel[0]; MF32(ARG(1) + 4) = b->lp.vel[1]; MF32(ARG(1) + 8) = b->lp.vel[2]; }
    RET(0, 2);
}
SHIM(ds3dl_SetDistanceFactor) { B3->lp.dist = ARGF(1); RET(0, 3); }
SHIM(ds3dl_SetDopplerFactor) { B3->lp.dopp = ARGF(1); RET(0, 3); }
SHIM(ds3dl_SetRolloffFactor) { B3->lp.rolloff = ARGF(1); RET(0, 3); }
SHIM(ds3dl_SetOrientation) {
    DSBuf *b = B3;
    b->lp.front[0] = ARGF(1); b->lp.front[1] = ARGF(2); b->lp.front[2] = ARGF(3);
    b->lp.top[0] = ARGF(4); b->lp.top[1] = ARGF(5); b->lp.top[2] = ARGF(6);
    RET(0, 8);
}
SHIM(ds3dl_SetPosition) {
    DSBuf *b = B3;
    b->lp.pos[0] = ARGF(1); b->lp.pos[1] = ARGF(2); b->lp.pos[2] = ARGF(3);
    RET(0, 5);
}
SHIM(ds3dl_SetVelocity) {
    DSBuf *b = B3;
    b->lp.vel[0] = ARGF(1); b->lp.vel[1] = ARGF(2); b->lp.vel[2] = ARGF(3);
    RET(0, 5);
}
SHIM(ds3dl_CommitDeferredSettings) { RET(0, 1); }

SHIM(ds3db_GetAllParameters) {
    DSBuf *b = B3;
    uint32_t p = ARG(1);
    if (p) { uint32_t n = M32(p); if (n > sizeof b->bp) n = sizeof b->bp; memcpy(MEM + p, &b->bp, n); M32(p) = sizeof b->bp; }
    RET(0, 2);
}
SHIM(ds3db_SetAllParameters) {
    DSBuf *b = B3;
    uint32_t p = ARG(1);
    if (p) { uint32_t n = M32(p); if (n > sizeof b->bp) n = sizeof b->bp; memcpy(&b->bp, MEM + p, n); b->bp.dwSize = sizeof b->bp; }
    RET(0, 3);
}
SHIM(ds3db_GetConeAngles) {
    DSBuf *b = B3;
    if (ARG(1)) M32(ARG(1)) = b->bp.inside;
    if (ARG(2)) M32(ARG(2)) = b->bp.outside;
    RET(0, 3);
}
SHIM(ds3db_SetConeAngles) { B3->bp.inside = ARG(1); B3->bp.outside = ARG(2); RET(0, 4); }
SHIM(ds3db_GetConeOrientation) {
    DSBuf *b = B3;
    if (ARG(1)) { MF32(ARG(1)) = b->bp.cone[0]; MF32(ARG(1) + 4) = b->bp.cone[1]; MF32(ARG(1) + 8) = b->bp.cone[2]; }
    RET(0, 2);
}
SHIM(ds3db_SetConeOrientation) {
    DSBuf *b = B3;
    b->bp.cone[0] = ARGF(1); b->bp.cone[1] = ARGF(2); b->bp.cone[2] = ARGF(3);
    RET(0, 5);
}
SHIM(ds3db_GetConeOutsideVolume) { if (ARG(1)) M32(ARG(1)) = (uint32_t)B3->bp.cone_vol; RET(0, 2); }
SHIM(ds3db_SetConeOutsideVolume) { B3->bp.cone_vol = (int32_t)ARG(1); RET(0, 3); }
SHIM(ds3db_GetMaxDistance) { if (ARG(1)) MF32(ARG(1)) = B3->bp.max_dist; RET(0, 2); }
SHIM(ds3db_SetMaxDistance) { B3->bp.max_dist = ARGF(1); RET(0, 3); }
SHIM(ds3db_GetMinDistance) { if (ARG(1)) MF32(ARG(1)) = B3->bp.min_dist; RET(0, 2); }
SHIM(ds3db_SetMinDistance) { B3->bp.min_dist = ARGF(1); RET(0, 3); }
SHIM(ds3db_GetMode) { if (ARG(1)) M32(ARG(1)) = B3->bp.mode; RET(0, 2); }
SHIM(ds3db_SetMode) { B3->bp.mode = ARG(1); RET(0, 3); }
SHIM(ds3db_GetPosition) {
    DSBuf *b = B3;
    if (ARG(1)) { MF32(ARG(1)) = b->bp.pos[0]; MF32(ARG(1) + 4) = b->bp.pos[1]; MF32(ARG(1) + 8) = b->bp.pos[2]; }
    RET(0, 2);
}
SHIM(ds3db_SetPosition) {
    DSBuf *b = B3;
    b->bp.pos[0] = ARGF(1); b->bp.pos[1] = ARGF(2); b->bp.pos[2] = ARGF(3);
    RET(0, 5);
}
SHIM(ds3db_GetVelocity) {
    DSBuf *b = B3;
    if (ARG(1)) { MF32(ARG(1)) = b->bp.vel[0]; MF32(ARG(1) + 4) = b->bp.vel[1]; MF32(ARG(1) + 8) = b->bp.vel[2]; }
    RET(0, 2);
}
SHIM(ds3db_SetVelocity) {
    DSBuf *b = B3;
    b->bp.vel[0] = ARGF(1); b->bp.vel[1] = ARGF(2); b->bp.vel[2] = ARGF(3);
    RET(0, 5);
}
#undef B3

static const ShimFn k_ds3dl_fns[] = {
    shim_ds3d_QueryInterface, shim_ds3d_AddRef, shim_ds3d_Release,
    shim_ds3dl_GetAllParameters, shim_ds3dl_GetDistanceFactor, shim_ds3dl_GetDopplerFactor,
    shim_ds3dl_GetOrientation, shim_ds3dl_GetPosition, shim_ds3dl_GetRolloffFactor,
    shim_ds3dl_GetVelocity, shim_ds3dl_SetAllParameters, shim_ds3dl_SetDistanceFactor,
    shim_ds3dl_SetDopplerFactor, shim_ds3dl_SetOrientation, shim_ds3dl_SetPosition,
    shim_ds3dl_SetRolloffFactor, shim_ds3dl_SetVelocity, shim_ds3dl_CommitDeferredSettings,
};

static const ShimFn k_ds3db_fns[] = {
    shim_ds3d_QueryInterface, shim_ds3d_AddRef, shim_ds3d_Release,
    shim_ds3db_GetAllParameters, shim_ds3db_GetConeAngles, shim_ds3db_GetConeOrientation,
    shim_ds3db_GetConeOutsideVolume, shim_ds3db_GetMaxDistance, shim_ds3db_GetMinDistance,
    shim_ds3db_GetMode, shim_ds3db_GetPosition, shim_ds3db_GetVelocity,
    shim_ds3db_SetAllParameters, shim_ds3db_SetConeAngles, shim_ds3db_SetConeOrientation,
    shim_ds3db_SetConeOutsideVolume, shim_ds3db_SetMaxDistance, shim_ds3db_SetMinDistance,
    shim_ds3db_SetMode, shim_ds3db_SetPosition, shim_ds3db_SetVelocity,
};

static const ShimFn k_dsb_fns[] = {
    shim_dsb_QueryInterface, shim_dsb_AddRef, shim_dsb_Release, shim_dsb_GetCaps, shim_dsb_GetCurrentPosition,
    shim_dsb_GetFormat, shim_dsb_GetVolume, shim_dsb_GetPan, shim_dsb_GetFrequency, shim_dsb_GetStatus,
    shim_dsb_Initialize, shim_dsb_Lock, shim_dsb_Play, shim_dsb_SetCurrentPosition, shim_dsb_SetFormat,
    shim_dsb_SetVolume, shim_dsb_SetPan, shim_dsb_SetFrequency, shim_dsb_Stop, shim_dsb_Unlock,
    shim_dsb_Restore, shim_dsb_SetFX, shim_dsb_AcquireResources, shim_dsb_GetObjectInPath,
};

/* ---------------- IDirectSound8 ---------------- */
SHIM(ds_QueryInterface) { hcom_addref(ARG(0)); M32(ARG(2)) = ARG(0); RET(0, 3); }
SHIM(ds_AddRef) { RET(hcom_addref(ARG(0)), 1); }
SHIM(ds_Release) { uint32_t left; hcom_release(ARG(0), &left); RET(left, 1); }
SHIM(ds_CreateSoundBuffer) {
    uint32_t desc = ARG(1);
    DSBuf *b = calloc(1, sizeof *b);
    b->d = calloc(1, sizeof *b->d);
    b->d->refs = 1;
    b->flags = M32(desc + 4);
    b->primary = (b->flags & 1) != 0;
    b->channels = 2; b->bits = 16; b->rate = 44100;
    /* DS3DLISTENER / DS3DBUFFER defaults */
    b->lp.dwSize = sizeof b->lp;
    b->lp.front[2] = 1.0f; b->lp.top[1] = 1.0f;
    b->lp.dist = b->lp.dopp = b->lp.rolloff = 1.0f;
    b->bp.dwSize = sizeof b->bp;
    b->bp.inside = b->bp.outside = 360;
    b->bp.cone[2] = 1.0f;
    b->bp.min_dist = 1.0f; b->bp.max_dist = 1.0e9f;
    uint32_t wfx = M32(desc + 16);
    if (wfx) { b->channels = M16(wfx + 2); b->rate = (int)M32(wfx + 4); b->bits = M16(wfx + 14); }
    if (!b->primary) {
        b->d->size = M32(desc + 8);
        b->d->data = gmalloc(b->d->size ? b->d->size : 16);
        memset(MEM + b->d->data, b->bits == 8 ? 0x80 : 0, b->d->size);
    }
    TRACE("dsound: buffer %u bytes %dch %dHz %dbit flags %x", b->d->size, b->channels, b->rate, b->bits, b->flags);
    M32(ARG(2)) = buf_register(b);
    RET(0, 4);
}
SHIM(ds_GetCaps) {
    uint32_t p = ARG(1);
    uint32_t sz = M32(p);
    memset(MEM + p + 4, 0, sz - 4);
    M32(p + 4) = 0x00000F5F; /* continuous rate, 8/16 bit, mono/stereo, secondary */
    M32(p + 8) = 100;
    M32(p + 12) = 200000;
    RET(0, 2);
}
SHIM(ds_DuplicateSoundBuffer) {
    DSBuf *src = hcom_host(ARG(1));
    DSBuf *b = calloc(1, sizeof *b);
    *b = *src;
    b->d->refs++;
    b->playing = 0;
    b->pos = 0;
    b->listener = 0;
    b->buf3d = 0;
    M32(ARG(2)) = buf_register(b);
    RET(0, 3);
}
SHIM(ds_SetCooperativeLevel) { RET(0, 3); }
SHIM(ds_Compact) { RET(0, 1); }
SHIM(ds_GetSpeakerConfig) { M32(ARG(1)) = 4; RET(0, 2); } /* DSSPEAKER_STEREO */
SHIM(ds_SetSpeakerConfig) { RET(0, 2); }
SHIM(ds_Initialize) { RET(0, 2); }
SHIM(ds_VerifyCertification) { M32(ARG(1)) = 0; RET(0, 2); }

static const ShimFn k_ds_fns[] = {
    shim_ds_QueryInterface, shim_ds_AddRef, shim_ds_Release, shim_ds_CreateSoundBuffer, shim_ds_GetCaps,
    shim_ds_DuplicateSoundBuffer, shim_ds_SetCooperativeLevel, shim_ds_Compact, shim_ds_GetSpeakerConfig,
    shim_ds_SetSpeakerConfig, shim_ds_Initialize, shim_ds_VerifyCertification,
};

SHIM(DirectSoundCreate8) {
    static int init;
    if (!init) {
        hcom_class_init(&k_ds8, "IDirectSound8", k_ds_fns, sizeof k_ds_fns / sizeof k_ds_fns[0]);
        hcom_class_init(&k_dsb, "IDirectSoundBuffer8", k_dsb_fns, sizeof k_dsb_fns / sizeof k_dsb_fns[0]);
        hcom_class_init(&k_ds3dl, "IDirectSound3DListener", k_ds3dl_fns, sizeof k_ds3dl_fns / sizeof k_ds3dl_fns[0]);
        hcom_class_init(&k_ds3db, "IDirectSound3DBuffer", k_ds3db_fns, sizeof k_ds3db_fns / sizeof k_ds3db_fns[0]);
        audio_open();
        init = 1;
    }
    M32(ARG(1)) = hcom_new(&k_ds8, NULL);
    LOG("DirectSoundCreate8");
    RET(0, 3);
}

static const ShimDef defs[] = {
    {"DSOUND.dll", "#11", shim_DirectSoundCreate8},
    {"DSOUND.dll", "DirectSoundCreate8", shim_DirectSoundCreate8},
    {NULL, NULL, NULL}
};

void dsound_init(void) { rt_register_shims(defs); }
