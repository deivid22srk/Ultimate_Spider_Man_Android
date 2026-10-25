/* Recompiled modules linked into this build. */
#include "game.h"
#include "rt_table.h"

extern const FnEntry g_fn_table_exe[];
extern const unsigned g_fn_count_exe;
extern const FnEntry g_fn_table_d3dx9[];
extern const unsigned g_fn_count_d3dx9;
#ifdef GAME_USM
/* Ultimate Spider-Man: binkw32.dll only. Loaded at 0x09000000, like the
 * reference port loads its Bink DLL, so the fixed addresses stay distinct
 * from the exe's own base (0x00400000). */
extern const FnEntry g_fn_table_bink[];
extern const unsigned g_fn_count_bink;
const FnModule g_modules[] = {
    {GAME_EXE, g_fn_table_exe, 0},
    {"binkw32.dll", g_fn_table_bink, 0},
};
const unsigned g_nmodules = sizeof g_modules / sizeof g_modules[0];
unsigned rt_module_count(unsigned m) {
    switch (m) {
    case 0: return g_fn_count_exe;
    case 1: return g_fn_count_bink;
    default: return 0;
    }
}
const char *const g_dll_modules[] = {"binkw32.dll", NULL};
const uint32_t g_dll_bases[] = {0x09000000u, 0};
#else
#ifdef GAME_SKYRIM
/* Skyrim: d3dx9_42, binkw32 (logo video) and X3DAudio1_7 (3D panning). */
extern const FnEntry g_fn_table_bink[];
extern const unsigned g_fn_count_bink;
extern const FnEntry g_fn_table_x3da[];
extern const unsigned g_fn_count_x3da;
const FnModule g_modules[] = {
    {GAME_EXE, g_fn_table_exe, 0},
    {"d3dx9_42.dll", g_fn_table_d3dx9, 0},
    {"binkw32.dll", g_fn_table_bink, 0},
    {"X3DAudio1_7.dll", g_fn_table_x3da, 0},
};
const unsigned g_nmodules = sizeof g_modules / sizeof g_modules[0];
unsigned rt_module_count(unsigned m) {
    switch (m) {
    case 0: return g_fn_count_exe;
    case 1: return g_fn_count_d3dx9;
    case 2: return g_fn_count_bink;
    case 3: return g_fn_count_x3da;
    default: return 0;
    }
}
const char *const g_dll_modules[] = {"d3dx9_42.dll", "binkw32.dll", "X3DAudio1_7.dll", NULL};
const uint32_t g_dll_bases[] = {0x08000000u, 0x09000000u, 0x0A000000u, 0};
#else
#ifdef GAME_MARVEL
/* LEGO Marvel: D3DX9_43 and D3DCompiler_43 (runtime HLSL compiles). */
#define g_fn_table_bink g_fn_table_d3dc
#define g_fn_count_bink g_fn_count_d3dc
#define DLL1 "d3dx9_43.dll"
#define DLL2 "D3DCompiler_43.dll"
#else
#define DLL1 "d3dx9_35.dll"
#define DLL2 "binkw32.dll"
#endif
extern const FnEntry g_fn_table_bink[];
extern const unsigned g_fn_count_bink;

const FnModule g_modules[] = {
    {GAME_EXE, g_fn_table_exe, 0},
    {DLL1, g_fn_table_d3dx9, 0},
    {DLL2, g_fn_table_bink, 0},
};
const unsigned g_nmodules = sizeof g_modules / sizeof g_modules[0];

/* Counts are link-time constants in other translation units. */
unsigned rt_module_count(unsigned m) {
    switch (m) {
    case 0: return g_fn_count_exe;
    case 1: return g_fn_count_d3dx9;
    case 2: return g_fn_count_bink;
    default: return 0;
    }
}

/* Recompiled DLLs loaded at startup, from the game directory. */
const char *const g_dll_modules[] = {DLL1, DLL2, NULL};
const uint32_t g_dll_bases[] = {0x08000000u, 0x09000000u, 0};
#endif /* GAME_SKYRIM */
#endif /* GAME_USM */
