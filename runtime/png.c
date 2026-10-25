/* Minimal PNG reader for the port's own HUD art (the icons folder's PNGs) and
 * launcher's icons. The icons the artist generates are 8-bit RGBA,
 * non-interlaced; the other colour types are handled anyway because they cost
 * twenty lines and save a confusing failure later.
 *
 * Inflate comes from zlib: the NDK ships libz and the desktop build links the
 * system one. Nothing here is used by the game itself -- the game's own art
 * (the game's packed art) is decoded by the game's recompiled code. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* buf/n: whole file. Returns malloc'd w*h RGBA, or NULL. */
uint8_t *png_decode(const uint8_t *buf, size_t n, int *ow, int *oh) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8 || memcmp(buf, sig, 8)) return NULL;
    int w = 0, h = 0, depth = 0, type = 0, interlace = 0;
    uint8_t pal[256 * 3], trns[256];
    int npal = 0, ntrns = 0;
    size_t idat_len = 0, idat_cap = 0;
    uint8_t *idat = NULL;
    for (size_t off = 8; off + 8 <= n;) {
        uint32_t ln = be32(buf + off);
        const uint8_t *ty = buf + off + 4;
        if (off + 12 + ln > n) break;
        const uint8_t *d = buf + off + 8;
        if (!memcmp(ty, "IHDR", 4) && ln >= 13) {
            w = (int)be32(d);
            h = (int)be32(d + 4);
            depth = d[8];
            type = d[9];
            interlace = d[12];
        } else if (!memcmp(ty, "PLTE", 4) && ln <= 768) {
            memcpy(pal, d, ln);
            npal = (int)ln / 3;
        } else if (!memcmp(ty, "tRNS", 4)) {
            ntrns = ln > 256 ? 256 : (int)ln;
            memcpy(trns, d, (size_t)ntrns);
        } else if (!memcmp(ty, "IDAT", 4)) {
            if (idat_len + ln > idat_cap) {
                idat_cap = (idat_len + ln) * 2 + 1024;
                uint8_t *p = realloc(idat, idat_cap);
                if (!p) { free(idat); return NULL; }
                idat = p;
            }
            memcpy(idat + idat_len, d, ln);
            idat_len += ln;
        } else if (!memcmp(ty, "IEND", 4)) {
            break;
        }
        off += 12 + ln;
    }
    int ch = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    if (!w || !h || !ch || depth != 8 || interlace != 0 || !idat) { free(idat); return NULL; }
    size_t stride = (size_t)w * ch, rawlen = (size_t)h * (stride + 1);
    uint8_t *raw = malloc(rawlen);
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!raw || !rgba) { free(raw); free(rgba); free(idat); return NULL; }
    uLongf outlen = (uLongf)rawlen;
    if (uncompress(raw, &outlen, idat, (uLong)idat_len) != Z_OK || outlen != rawlen) {
        free(raw); free(rgba); free(idat);
        return NULL;
    }
    free(idat);
    /* per-scanline filters */
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1);
        uint8_t *cur = row + 1;
        const uint8_t *prev = y ? row - (stride + 1) + 1 : NULL; /* previous row's pixels */
        int ft = row[0];
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)ch ? cur[i - ch] : 0, b = prev ? prev[i] : 0;
            int c = (prev && i >= (size_t)ch) ? prev[i - ch] : 0;
            switch (ft) {
            case 0: break;
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            default: free(raw); free(rgba); return NULL;
            }
        }
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *src = raw + (size_t)y * (stride + 1) + 1;
        uint8_t *dst = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b, a = 255;
            switch (type) {
            case 0: r = g = b = src[x]; break;
            case 4: r = g = b = src[x * 2]; a = src[x * 2 + 1]; break;
            case 2: r = src[x * 3]; g = src[x * 3 + 1]; b = src[x * 3 + 2]; break;
            case 6: r = src[x * 4]; g = src[x * 4 + 1]; b = src[x * 4 + 2]; a = src[x * 4 + 3]; break;
            default: { /* palette */
                int i = src[x];
                if (i >= npal) { free(raw); free(rgba); return NULL; }
                r = pal[i * 3]; g = pal[i * 3 + 1]; b = pal[i * 3 + 2];
                a = i < ntrns ? trns[i] : 255;
                break;
            }
            }
            dst[x * 4] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }
    free(raw);
    *ow = w;
    *oh = h;
    return rgba;
}