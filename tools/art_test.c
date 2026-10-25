/* Host test for runtime/art.c + runtime/png.c against a real icon pack.
 *
 *   clang -O1 -o arttest tools/art_test.c runtime/art.c runtime/png.c -lz
 *   ./arttest <icons-dir> [more dirs...]
 *
 * It packs the icons, then re-reads each source PNG and checks that the atlas
 * cell holds exactly that image (with B,G,R,A byte order), so the atlas a
 * phone would upload is verified without an APK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art.h"

static const char *k_files[] = {
    "hud_jump.png", "on/hud_jump_on.png", "hud_web.png", "on/hud_web_on.png",
    "hud_punch.png", "on/hud_punch_on.png", "hud_kick.png", "on/hud_kick_on.png",
    "hud_wallcrawl.png", "on/hud_wallcrawl_on.png", "hud_black.png", "on/hud_black_on.png",
    "hud_pause.png", "on/hud_pause_on.png", "hud_back.png", "on/hud_back_on.png",
    "hud_stick.png", "hud_stick_knob.png", "hud_campad.png", "hud_cursor.png",
    "hud_skip.png",
};

static uint8_t *load(const char *dir, const char *file, int *w, int *h) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    uint8_t *px = png_decode(b, (size_t)sz, w, h);
    free(b);
    return px;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: arttest <icons-dir> [...]\n"); return 2; }
    ArtEntry e[ART_MAX];
    uint8_t *atlas = NULL;
    int aw = 0, ah = 0;
    int n = art_load((const char *const *)argv + 1, argc - 1, e, ART_MAX, &atlas, &aw, &ah);
    printf("icons packed: %d (expected %d), atlas %dx%d\n", n, (int)(sizeof k_files / sizeof k_files[0]), aw, ah);
    if (n != (int)(sizeof k_files / sizeof k_files[0])) { printf("FAIL: wrong count\n"); return 1; }
    if (aw != ART_W || ah != ART_H) { printf("FAIL: wrong atlas size\n"); return 1; }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        int w = 0, h = 0;
        uint8_t *px = load(argv[1], k_files[i], &w, &h);
        if (!px || w != ART_CELL || h != ART_CELL) { printf("FAIL: cannot re-read %s\n", k_files[i]); bad++; continue; }
        const uint8_t *src = px;
        const uint8_t *cellpx = atlas + (size_t)i * ART_CELL * ART_CELL * 4;
        size_t diff = 0;
        for (int p = 0; p < ART_CELL * ART_CELL; p++) {
            if (cellpx[p * 4 + 0] != src[p * 4 + 2]) diff++;
            if (cellpx[p * 4 + 1] != src[p * 4 + 1]) diff++;
            if (cellpx[p * 4 + 2] != src[p * 4 + 0]) diff++;
            if (cellpx[p * 4 + 3] != src[p * 4 + 3]) diff++;
        }
        /* UVs must select exactly this cell */
        int col = (int)(e[i].u0 * ART_W) / ART_CELL, row = (int)(e[i].v0 * ART_H) / ART_CELL;
        int uvc = e[i].u1 - e[i].u0 > 0 && e[i].v1 - e[i].v0 > 0;
        if (diff || col != i % ART_COLS || row != i / ART_COLS || !uvc) {
            printf("FAIL %-22s key=%-12s diff=%zu cell=(%d,%d)\n", k_files[i], e[i].key, diff, col, row);
            bad++;
        } else {
            printf("ok   %-22s key=%-12s u=%.4f..%.4f v=%.4f..%.4f\n", k_files[i], e[i].key, e[i].u0, e[i].u1, e[i].v0, e[i].v1);
        }
        free(px);
    }
    free(atlas);
    printf(bad ? "FAILURES: %d\n" : "ALL GOOD (%d icons)\n", bad ? bad : n);
    return bad != 0;
}