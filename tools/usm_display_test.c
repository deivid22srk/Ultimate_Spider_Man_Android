/* Host test for the USM display-sync byte-pattern scanner installed in
 * runtime/d3d9_bridge.cpp (usm_display::pat_find and the offsets it reads).
 *
 *   cc -O1 -o usm_display_test tools/usm_display_test.c
 *   ./usm_display_test
 *
 * It builds a synthetic guest image with the instruction sequences of
 * USM.exe 1.0.0.1 that the patterns must find — placed at the addresses the
 * WidescreenFix for this exe comments (ThirteenAG/WidescreenFixesPack,
 * source/UltimateSpiderMan.WidescreenFix) — and checks that each pattern
 * matches exactly once and yields the right global. It also checks that a
 * second copy of a pattern is reported as ambiguous (the runtime disables the
 * sync unless every pattern is unique). The matcher here is a copy of the
 * runtime one; if the two drift, the address/offset expectations below stop
 * describing what the port actually does.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static uint8_t *MEM;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
#define M32(a) (*(u32u *)(MEM + (uint32_t)(a)))

/* ---- copy of usm_display::pat_find (runtime/d3d9_bridge.cpp) -------------- */
static int pat_find(const char *pat, uint32_t lo, uint32_t hi, uint32_t *out) {
    uint8_t b[64];
    bool wild[64];
    int n = 0;
    for (const char *p = pat; *p && n < 64; ) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { wild[n] = true; b[n] = 0; n++; p += 2; continue; }
        int h4 = *p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10;
        int l4 = p[1] <= '9' ? p[1] - '0' : (p[1] | 32) - 'a' + 10;
        wild[n] = false; b[n] = (uint8_t)(h4 * 16 + l4);
        n++; p += 2;
    }
    int hits = 0;
    uint32_t hit = 0;
    for (uint32_t a = lo; a + (uint32_t)n <= hi; a++) {
        const uint8_t *s = MEM + a;
        bool m = true;
        for (int i = 0; i < n && m; i++) m = wild[i] || s[i] == b[i];
        if (m) { hits++; hit = a; }
    }
    *out = hit;
    return hits;
}

/* ---- the synthetic image: guest addresses, like the runtime's MEM -------- */
static const uint32_t TEXT_LO = 0x401000, TEXT_HI = 0x86F000;

/* where each pattern lands on USM.exe 1.0.0.1 (mod comments, §11e) */
#define A_WIDTH_PAT   0x5AC3F9u
#define A_HEIGHT_PAT  0x5AC41Eu
#define A_DIMS_PAT    0x5AC42Eu
#define A_WINDOWED    0x5AC4A0u
#define A_ASPECT_CALL 0x515290u
#define A_DEFRES      0x5AC3C8u

static uint32_t G_W1, G_H1, G_W2, G_H2, G_W3, G_H3, G_PARAMS, G_WINFLAG, G_ASPECT;

static void put(uint32_t a, const uint8_t *b, size_t n) { memcpy(MEM + a, b, n); }
static void put4(uint32_t a, uint32_t v) { memcpy(MEM + a, &v, 4); }

int main(void) {
    MEM = calloc(1, 0x9EA060u);
    /* mov [G_W1], eax ; call esi ; push eax */
    { uint8_t s[] = {0xA3,0,0,0,0, 0xFF,0xD6, 0x50};
      G_W1 = 0x971F2C; put(A_WIDTH_PAT, s, sizeof s); put4(A_WIDTH_PAT + 1, G_W1); }
    /* fild [G_H1] ; add esp,0x18 ; push 0 ; fmul [x] */
    { uint8_t s[] = {0xDB,0x05,0,0,0,0, 0x83,0xC4,0x18, 0x6A,0x00, 0xD8,0x0D};
      G_H1 = 0x971F30; put(A_HEIGHT_PAT, s, sizeof s); put4(A_HEIGHT_PAT + 2, G_H1); }
    /* mov eax,[G_W2]; mov ecx,[G_H2]; mov [G_W3],eax; mov [G_PARAMS],eax;
     * mov eax,[G_WINFLAG]; xor edx,edx; test eax,eax; push 0x29; setne dl;
     * push 3; push 0; push 0x16; mov [G_H3],ecx; mov [x],ecx; mov ecx,[y] */
    { uint8_t s[] = {0xA1,0,0,0,0, 0x8B,0x0D,0,0,0,0, 0xA3,0,0,0,0, 0xA3,0,0,0,0,
                     0xA1,0,0,0,0, 0x33,0xD2, 0x85,0xC0, 0x6A,0x29, 0x0F,0x95,0xC2,
                     0x6A,0x03, 0x6A,0x00, 0x6A,0x16, 0x89,0x0D,0,0,0,0, 0x89,0x0D,0,0,0,0, 0x8B,0x0D};
      G_W2 = 0x971F34; G_H2 = 0x971F38; G_W3 = 0x971F3C; G_PARAMS = 0x9D0F40;
      G_WINFLAG = 0x971F40; G_H3 = 0x971F44;
      put(A_DIMS_PAT, s, sizeof s);
      put4(A_DIMS_PAT + 1, G_W2);
      put4(A_DIMS_PAT + 7, G_H2);
      put4(A_DIMS_PAT + 12, G_W3);
      put4(A_DIMS_PAT + 17, G_PARAMS);
      put4(A_DIMS_PAT + 22, G_WINFLAG);
      put4(A_DIMS_PAT + 43, G_H3); }
    /* mov dword [G_WINFLAG], 0 ; call [x] ; mov eax,[y] ; mov ecx,[z] ; push 4 */
    { uint8_t s[] = {0xC7,0x05,0,0,0,0, 0,0,0,0, 0xFF,0x15,0,0,0,0, 0xA1,0,0,0,0,
                     0x8B,0x0D,0,0,0,0, 0x6A,0x04};
      put(A_WINDOWED, s, sizeof s); put4(A_WINDOWED + 2, G_WINFLAG); }
    /* call f ; fdiv [G_ASPECT], with f at 0x515280 */
    { uint8_t s[] = {0xE8,0,0,0,0, 0xD8,0x3D};
      int32_t rel = (int32_t)(0x515280u - (A_ASPECT_CALL + 5));
      G_ASPECT = 0x971F48;
      put(A_ASPECT_CALL, s, sizeof s);
      memcpy(MEM + A_ASPECT_CALL + 1, &rel, 4);
      put4(0x515280 + 2, G_ASPECT); }
    /* push str ; push 0x800 ; call f ; mov esi,[x], with "800x600" at 0x971F50 */
    { uint8_t s[] = {0x68,0,0,0,0, 0x68,0x00,0x08,0x00,0x00, 0xE8,0,0,0,0, 0x8B,0x35};
      put(A_DEFRES, s, sizeof s);
      put4(A_DEFRES + 1, 0x971F50);
      memcpy(MEM + 0x971F50, "800x600", 8); }

    int fails = 0;
    uint32_t m;
    if (pat_find("A3 ?? ?? ?? ?? FF D6 50", TEXT_LO, TEXT_HI, &m) != 1 || M32(m + 1) != G_W1)
        { printf("FAIL width (m=%x)\n", m); fails++; }
    if (pat_find("DB 05 ?? ?? ?? ?? 83 C4 18 6A 00 D8 0D", TEXT_LO, TEXT_HI, &m) != 1 || M32(m + 2) != G_H1)
        { printf("FAIL height (m=%x)\n", m); fails++; }
    if (pat_find("A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? A3 ?? ?? ?? ?? A3 ?? ?? ?? ?? A1 ?? ?? ?? ?? "
                 "33 D2 85 C0 6A 29 0F 95 C2 6A 03 6A 00 6A 16 89 0D ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 8B 0D",
                 TEXT_LO, TEXT_HI, &m) != 1 ||
        M32(m + 1) != G_W2 || M32(m + 7) != G_H2 || M32(m + 12) != G_W3 ||
        M32(m + 17) != G_PARAMS || M32(m + 22) != G_WINFLAG || M32(m + 43) != G_H3)
        { printf("FAIL dimensions (m=%x)\n", m); fails++; }
    if (pat_find("C7 05 ?? ?? ?? ?? 00 00 00 00 FF 15 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 6A 04",
                 TEXT_LO, TEXT_HI, &m) != 1 || M32(m + 2) != G_WINFLAG)
        { printf("FAIL windowed (m=%x)\n", m); fails++; }
    {   /* the call chase: target = e8 + 5 + rel32, the pointer sits at +2 */
        if (pat_find("E8 ?? ?? ?? ?? D8 3D", TEXT_LO, TEXT_HI, &m) != 1) { printf("FAIL aspect (no match)\n"); fails++; }
        else {
            uint32_t target = m + 5 + (uint32_t)(int32_t)M32(m + 1);
            if (M32(target + 2) != G_ASPECT) { printf("FAIL aspect (target %x)\n", target); fails++; }
        }
    }
    if (pat_find("68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 35", TEXT_LO, TEXT_HI, &m) != 1 ||
        M32(m + 1) != 0x971F50 || strcmp((char *)MEM + M32(m + 1), "800x600") != 0)
        { printf("FAIL default resolution (m=%x)\n", m); fails++; }
    {   /* a second copy anywhere in .text must surface as a second hit */
        uint8_t s[] = {0xA3,0,0,0,0, 0xFF,0xD6, 0x50};
        put(0x600000, s, sizeof s);
        if (pat_find("A3 ?? ?? ?? ?? FF D6 50", TEXT_LO, TEXT_HI, &m) != 2)
            { printf("FAIL ambiguity count\n"); fails++; }
    }
    printf(fails ? "usm_display_test: FAILED (%d)\n" : "usm_display_test: all patterns match\n", fails);
    return fails != 0;
}
