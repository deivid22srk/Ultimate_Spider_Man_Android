/* Loads the port's HUD icon pack into one atlas texture.
 *
 * The pack is the artist's PNG set, unzipped next to the port's own folder
 * (<game>/port/icons by default). It is entirely optional: art_load() reports
 * how many icons it found, and the overlay keeps its plain discs for the rest.
 *
 * No dependency on the game or on Direct3D on purpose, so the packing can be
 * exercised by a host test (tools/art_test.c) instead of only on the phone.
 */
#include "art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* key -> file inside the icons folder. The eight buttons wear the icon of the
 * action they press (input.c's k_usm_keys, from the game's binding table). */
static const struct { const char *key, *file; } k_art[] = {
    {"jump", "hud_jump.png"},           {"on/jump", "on/hud_jump_on.png"},
    {"web", "hud_web.png"},             {"on/web", "on/hud_web_on.png"},
    {"punch", "hud_punch.png"},         {"on/punch", "on/hud_punch_on.png"},
    {"kick", "hud_kick.png"},           {"on/kick", "on/hud_kick_on.png"},
    {"wallcrawl", "hud_wallcrawl.png"}, {"on/wallcrawl", "on/hud_wallcrawl_on.png"},
    {"black", "hud_black.png"},         {"on/black", "on/hud_black_on.png"},
    {"pause", "hud_pause.png"},         {"on/pause", "on/hud_pause_on.png"},
    {"back", "hud_back.png"},           {"on/back", "on/hud_back_on.png"},
    {"ok", "hud_ok.png"},               /* confirm: the menus are keyboard only */
    {"stick", "hud_stick.png"},         {"knob", "hud_stick_knob.png"},
    {"campad", "hud_campad.png"},       {"cursor", "hud_cursor.png"},
    {"skip", "hud_skip.png"},
};
#define ART_NART ((int)(sizeof k_art / sizeof k_art[0]))

static uint8_t *read_png(const char *const *dirs, int ndirs, const char *file, int *w, int *h, char *found) {
    for (int i = 0; i < ndirs; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dirs[i], file);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (1 << 24)) { fclose(f); continue; }
        uint8_t *buf = malloc((size_t)sz);
        size_t got = buf ? fread(buf, 1, (size_t)sz, f) : 0;
        fclose(f);
        if (!buf || got != (size_t)sz) { free(buf); continue; }
        uint8_t *px = png_decode(buf, (size_t)sz, w, h);
        free(buf);
        if (!px) continue;
        if (found) snprintf(found, 1200, "%s", path);
        return px;
    }
    return NULL;
}

/* see art.h */
int art_load(const char *const *dirs, int ndirs, ArtEntry *out, int max, uint8_t **atlas, int *outw, int *outh) {
    if (max > ART_MAX) max = ART_MAX;
    const size_t cell = (size_t)ART_CELL * ART_CELL * 4;
    uint8_t *buf = calloc((size_t)max, cell);
    if (!buf) return 0;
    int n = 0;
    for (int i = 0; i < ART_NART && n < max; i++) {
        int iw = 0, ih = 0;
        uint8_t *px = read_png(dirs, ndirs, k_art[i].file, &iw, &ih, NULL);
        if (!px) continue;
        /* Cell (col,row) of the sheet, not the n-th contiguous 256K block: the
         * UVs below describe a grid, so the pixels have to land on it. Writing
         * them back to back left every cell holding slices of four neighbouring
         * icons, which is what put two actions (plain and pressed art) inside
         * one button on device. */
        const int col = n % ART_COLS, row = n / ART_COLS;
        uint8_t *dst = buf + ((size_t)row * ART_CELL * ART_W + (size_t)col * ART_CELL) * 4;
        for (int y = 0; y < ART_CELL; y++) {
            int sy = ih == ART_CELL ? y : (int)((int64_t)y * ih / ART_CELL);
            for (int x = 0; x < ART_CELL; x++) {
                int sx = iw == ART_CELL ? x : (int)((int64_t)x * iw / ART_CELL);
                const uint8_t *s = px + ((size_t)sy * iw + sx) * 4;
                uint8_t *o = dst + ((size_t)y * ART_W + x) * 4;
                o[0] = s[2];
                o[1] = s[1];
                o[2] = s[0];
                o[3] = s[3];
            }
        }
        free(px);
        ArtEntry *e = &out[n];
        snprintf(e->key, sizeof e->key, "%s", k_art[i].key);
        e->u0 = (float)(col * ART_CELL) / ART_W;
        e->v0 = (float)(row * ART_CELL) / ART_H;
        e->u1 = (float)((col + 1) * ART_CELL) / ART_W;
        e->v1 = (float)((row + 1) * ART_CELL) / ART_H;
        n++;
    }
    if (!n) {
        free(buf);
        return 0;
    }
    *atlas = buf;
    *outw = ART_W;
    *outh = ART_H;
    return n;
}