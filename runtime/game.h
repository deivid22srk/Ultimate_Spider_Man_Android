/* Per-game names: this runtime is shared by the LEGO Batman, LEGO Marvel,
 * Skyrim and Ultimate Spider-Man ports. Build with -DGAME_MARVEL,
 * -DGAME_SKYRIM or -DGAME_USM (default is Batman). */
#pragma once
#if defined(GAME_USM)
/* Ultimate Spider-Man (PC, 2005). USM.exe imports exactly one game-local DLL:
 * binkw32.dll (Bink 1.7d, video + audio). d3dx9_24.dll and xerces-c_2_3_0.dll
 * sit in the game folder but are never imported (see docs/RECON.md), so they
 * are neither recompiled nor required. The d3dx9 slot is used by the runtime
 * as "the second recompiled module"; here it holds binkw32. */
#define GAME_EXE       "USM.exe"
#define DLL_D3DX9      "binkw32.dll"
#define DLL_EXTRA      ""
#define GAME_WIN_DIR   "C:\\USM"
#define GAME_WIN_PREFIX "\\usm\\"
#define GAME_TITLE     "Ultimate Spider-Man"
#define GAME_PKG_DIR   "com.deivid22srk.usm"
#define GAME_USM 1
#elif defined(GAME_SKYRIM)
#define GAME_EXE       "TESV.exe"
#define DLL_D3DX9      "d3dx9_42.dll"
#define DLL_EXTRA      "binkw32.dll"
#define GAME_WIN_DIR   "C:\\Skyrim"
#define GAME_WIN_PREFIX "\\skyrim\\"
#define GAME_TITLE     "Skyrim"
#define GAME_PKG_DIR   "com.tj.skyrim"
#elif defined(GAME_MARVEL)
#define GAME_EXE       "LEGOMARVEL.exe"
#define DLL_D3DX9      "d3dx9_43.dll"
#define DLL_EXTRA      "D3DCompiler_43.dll"
#define GAME_WIN_DIR   "C:\\LEGOMarvel"
#define GAME_WIN_PREFIX "\\legomarvel\\"
#define GAME_TITLE     "LEGO Marvel Super Heroes"
#define GAME_PKG_DIR   "com.tj.legomarvel"
#else
#define GAME_EXE       "LEGOBatman.exe"
#define DLL_D3DX9      "d3dx9_35.dll"
#define DLL_EXTRA      "binkw32.dll"
#define GAME_WIN_DIR   "C:\\LEGOBatman"
#define GAME_WIN_PREFIX "\\legobatman\\"
#define GAME_TITLE     "LEGO Batman"
#define GAME_PKG_DIR   "com.tj.legobatman"
#define GAME_BATMAN 1
#endif
