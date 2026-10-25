/* HUD art: the port's own icon pack (PNG) and the atlas it is packed into.
 *
 * Deliberately self-contained -- no game headers -- so that both the runtime
 * and the host test (tools/art_test.c) can use it. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ART_CELL 256
#define ART_COLS 4
#define ART_ROWS 6
#define ART_MAX (ART_COLS * ART_ROWS)
#define ART_W (ART_COLS * ART_CELL)
#define ART_H (ART_ROWS * ART_CELL)

/* One packed icon: the key the overlay asks for, and its rectangle in the
 * atlas (normalised texture coordinates). */
typedef struct {
    char key[24];
    float u0, v0, u1, v1;
} ArtEntry;

/* Packs the icon files found in dirs[] into a malloc'd ART_W*ART_H*4 buffer in
 * D3DFMT_A8R8G8B8 order (bytes B,G,R,A). Returns the number of icons packed
 * (0 when the pack is absent; *atlas then stays NULL) and the atlas size. */
int art_load(const char *const *dirs, int ndirs, ArtEntry *out, int max, uint8_t **atlas, int *outw, int *outh);

/* Decodes an 8-bit non-interlaced PNG (colour types 0/2/3/4/6) to malloc'd
 * RGBA8. Returns NULL on anything it does not understand. */
uint8_t *png_decode(const uint8_t *buf, size_t n, int *ow, int *oh);

#ifdef __cplusplus
}
#endif