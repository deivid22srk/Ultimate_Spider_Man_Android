# Phase 1 — Reconnaissance: Ultimate Spider-Man (PC, 2005)

All statements below come from the user's own copy of the game, examined with
`pefile`, `capstone`, `sha1sum` and hexdumps. Nothing here is assumed from
release notes or from the Skyrim reference port.

Game folder used: `Ultimate_Spider_Man_Win_Preinstalled_EN/Game Files`
(the "preinstalled" English release, 1.6 GB).

## 1. Module inventory

| File | Size | SHA-1 | Role (verified) |
|---|---:|---|---|
| `USM.exe` | 5,926,912 | `66c58c6e96ad96a1713f86fb903af1192e8bc8f5` | Main executable |
| `binkw32.dll` | 338,944 | `e7a2670c5e61d3645e39e7e9ba779eea743daee7` | RAD Bink 1.7d video/audio |
| `d3dx9_24.dll` | 2,222,800 | `51b323ea377f9dcd52946f5fe77ceb5673d1592c` | D3DX9 (June 2004) — **not imported by the game** |
| `dsound.dll` | 2,122,240 | `7f65a1a832f8ebcaf1251ecd65334e7deb1e05c1` | **Ultimate ASI Loader** (ThirteenAG), a mod loader proxy |
| `xerces-c_2_3_0.dll` | 1,552,384 | `d3ba680a54c03a61e5b33b9e6804f49e22c9a376` | Apache Xerces-C 2.3.0 — **never referenced by `USM.exe`** |
| `msvcr71.dll` | 348,160 | `d5502a1d00787d68f548ddeebbde1eca5e2b38ca` | MSVC 7.1 C runtime |
| `MSVCP71.DLL` | 499,712 | (not hashed) | MSVC 7.1 C++ runtime |
| `scripts/modupdater.asi` | — | — | Third-party mod updater (not part of the game) |
| `scripts/UltimateSpiderMan.WidescreenFix.asi` | — | — | Third-party widescreen fix (not part of the game) |

Notes:

* `USM.exe` imports **only** `binkw32.dll` among the game-local DLLs. It does
  not import `d3dx9_24.dll` or `xerces-c_2_3_0.dll` (verified by walking
  `IMAGE_DIRECTORY_ENTRY_IMPORT`, and by searching the whole image for the
  strings `d3dx9`, `xerces`: zero hits). Both are leftovers used by the
  third-party `.asi` mods or the installer. **They are not needed for the
  port and will not be recompiled.**
* `dsound.dll` in this release is not Microsoft's DirectSound: it is
  ThirteenAG's ASI loader (`FileDescription = Ultimate ASI Loader`,
  `OriginalFilename = Ultimate-ASI-Loader-x86.dll`). It exists so that
  `scripts/*.asi` get loaded. The game's `DirectSoundCreate8` import is
  satisfied by that proxy, which forwards to the system `dsound.dll`.
  For the port we ignore this DLL entirely and shim `DirectSoundCreate8`
  ourselves.

## 2. Executable facts

```
machine        0x014C (i386, 32-bit)
image base     0x00400000          (fixed: Characteristics = RELOCS_STRIPPED,
                                    no .reloc section, DllCharacteristics = 0)
entry point    0x008223D6  (RVA 0x4223D6)
size of image  0x005EA060
linker         MSVC 7.10 (Visual Studio .NET 2003)
version info   Treyarch Corp. / "Ultimate Spiderman" 1.0.0.1
               OriginalFilename = spiderman.exe, Copyright (c) 2004
PDB            C:\USM\Ultimate_final.pdb  (RSDS, debug dir at RVA 0x4EBB64)
timestamp      0x00564544  (not a valid date — see below)
```

Sections (RVA / virtual size / raw size / characteristics):

| Name | RVA | VSize | Raw | Characteristics |
|---|---:|---:|---:|---|
| `.text` | 0x00001000 | 0x46E000 | 0x46D023 | code, execute+read |
| `.rdata` | 0x0046F000 | 0x0AC000 | 0x0ABEAF | read |
| `.data` | 0x0051B000 | 0x06D000 | 0x03B290 | read/write |
| `.rsrc` | 0x00588000 | 0x009000 | 0x008D86 | read |
| (unnamed) | 0x00591000 | 0x059060 | 0x046A98 | read, discardable, entropy 6.72 |

* The fifth section has no name and holds high-entropy data (289 KB). It is
  mapped read-only and is not code; the game reads it as data. It does not
  look like a protection stub (see §3).
* `TimeDateStamp = 0x00564544` is not a plausible build date (it decodes to
  1970-03-07). The file was re-stamped when the preinstalled release was
  packaged. It has no effect on recompilation.
* File overlay: 1,384 bytes, **all zero**. No crack signature, no appended
  archive.

## 3. DRM / protection

Evidence gathered:

| Check | Result |
|---|---|
| Known protection section names (`.sdata`, `.stext`, `.securom`, `.perplex`, …) | none |
| Strings `SecuROM`, `SafeDisc`, `StarForce`, `Secur` in the whole image | 0 hits |
| Section entropy | 6.51 / 5.38 / 2.95 / 3.46 — normal for MSVC code and data |
| Entry point | standard MSVC 7.1 CRT startup (`push 0x74; push __except_handler3; call __security_init_cookie`, PEB/`MZ`/`PE` probe, `_heap_init`, `_ioinit`, `GetCommandLineA`, `main`) |
| Imports | plain system DLLs only; no `Nt*`/`Zw*` native calls, no `LoadLibrary` of a protection DLL |
| Overlay | zeros only |
| `Rich` header | present (normal MSVC link) |
| Relocations | stripped; image is not ASLR — the retail exe's normal layout |

**Conclusion:** this `USM.exe` is not packed and carries no disc check. It is
the 1.0.0.1 retail build already prepared for the preinstalled release (the
disc check was removed before packaging; the release ships an ASI loader as
`dsound.dll`). There is **no SafeDisc/StarForce/SecuROM stub to remove**, so
static lifting can start from this file directly.

Consequence for the port: we do not need Steamless-like unpacking, and we do
not need to reproduce any protection behaviour. It also means the port is
tied to *this* build (SHA-1 `66c58c6e…`); another release will have different
addresses.

## 4. Complete import table of `USM.exe`

17 DLLs, 337 imported functions. This is the list the runtime must satisfy
(either shimmed or recompiled).

| DLL | # | Functions |
|---|---:|---|
| `advapi32` | 7 | `RegCloseKey`, `RegOpenKeyA`, `RegCreateKeyA`, `RegQueryValueExA`, `RegSetValueExA`, `RegOpenKeyExA`, `RegCreateKeyExA` |
| `comctl32` | 5 | `ImageList_Remove`, `ImageList_Add`, `InitCommonControlsEx`, `ImageList_Destroy`, `ImageList_Create` |
| `comdlg32` | 5 | `ChooseFontA`, `ChooseColorA`, `GetSaveFileNameA`, `CommDlgExtendedError`, `GetOpenFileNameA` |
| `dsound` | 1 | `DirectSoundCreate8` |
| `gdi32` | 4 | `GetTextExtentPoint32A`, `DeleteObject`, `GetStockObject`, `CreateBitmap` |
| `kernel32` | 93 | `ExitThread`, `SetThreadPriority`, `CreateThread`, `CreateEventA`, `LocalFree`, `FormatMessageA`, `GetLastError`, `GetModuleHandleA`, `GetCurrentDirectoryA`, `GetModuleFileNameA`, `GetProfileIntA`, `GetPrivateProfileIntA`, `GetProfileStringA`, `GetPrivateProfileStringA`, `WriteProfileStringA`, `WritePrivateProfileStringA`, `ReadFile`, `WriteFile`, `MapViewOfFile`, `CreateFileMappingA`, `UnmapViewOfFile`, `SetFilePointer`, `Sleep`, `VirtualQuery`, `GlobalFree`, `GlobalUnlock`, `GlobalLock`, `GlobalAlloc`, `OutputDebugStringA`, `GetFileAttributesA`, `GetFileSize`, `CreateFileA`, `SetErrorMode`, `SetUnhandledExceptionFilter`, `InitializeCriticalSection`, `GetDiskFreeSpaceExA`, `CreateMutexA`, `OpenProcess`, `GetCurrentProcessId`, `GetCurrentProcess`, `GlobalMemoryStatus`, `QueryPerformanceFrequency`, `ReleaseMutex`, `SwitchToThread`, `SetEndOfFile`, `SetFilePointerEx`, `SleepEx`, `SetEvent`, `FindNextFileA`, `FileTimeToSystemTime`, `FileTimeToLocalFileTime`, `FindFirstFileA`, `FindFirstFileExA`, `CreateDirectoryA`, `RemoveDirectoryA`, `DeleteFileA`, `GetSystemTimeAsFileTime`, `GetCurrentThreadId`, `ExitProcess`, `GetStartupInfoA`, `ResumeThread`, `GetOverlappedResult`, `CancelIo`, `WriteFileEx`, `ReadFileEx`, `GetSystemDirectoryA`, `FreeLibrary`, `HeapFree`, `GetProcessHeap`, `HeapAlloc`, `VirtualAlloc`, `VirtualFree`, `lstrcmpi`, `GetFullPathNameA`, `DeleteCriticalSection`, `InterlockedCompareExchange`, `InterlockedExchange`, `GetVersionExA`, `WideCharToMultiByte`, `CreateFileW`, `GetProcAddress`, `LoadLibraryA`, `IsProcessorFeaturePresent`, `GetSystemInfo`, `WaitForSingleObject`, `CloseHandle`, `EnterCriticalSection`, `GetLocalTime`, `LeaveCriticalSection`, `QueryPerformanceCounter`, `FindClose`, `GetTickCount`, `SetLastError` |
| `msvcp71` | 10 | `basic_ofstream` ctor/`open`/`close`/dtor, `operator<<(ostream&, const char*)`, `basic_string` ctor/copy-ctor/dtor, `std::_Nomemory` |
| `msvcr71` | 106 | C runtime: `malloc`/`free`/`realloc`, `fopen`/`fread`/`fwrite`/`fseek`/`ftell`/`fclose`/`fflush`, `printf`/`fprintf`/`sprintf`/`_snprintf`/`vsprintf`/`sscanf`, `str*`/`mem*`/`is*`/`to*`, `qsort`/`bsearch`, `strtod`/`atoi`/`atof`, `time`/`clock`/`srand`/`rand`, `_setjmp3`/`longjmp`, `_except_handler3`, `__CxxFrameHandler`, `_CxxThrowException`, `_initterm`, `__getmainargs`, `__set_app_type`, `_adjust_fdiv`, `_controlfp`, `_aligned_malloc/free`, `_msize`, `_fstat`, `_mkdir`, `_chdir`, `_getcwd`, `_iob`, `_onexit`, `_purecall`, `?terminate@@YAXXZ`, `??0/1exception`, `??1type_info`, `??3@YAXPAX@Z` (operator delete), `??_V@YAXPAX@Z` (delete[]) |
| `ole32` | 3 | `CoCreateInstance`, `CoInitialize`, `CoUninitialize` |
| `oleaut32` | 3 | `SysStringLen`, `VariantInit`, `VariantClear` |
| `psapi` | 1 | `GetProcessMemoryInfo` |
| `shell32` | 3 | `SHGetMalloc`, `SHBrowseForFolder`, `SHGetPathFromIDList` |
| `user32` | 75 | window class/creation, message loop (`PeekMessageA`, `DispatchMessageA`, `TranslateMessage`), `CreateWindowExA`, `DefWindowProcA`, dialogs (`CreateDialogParamA`, `IsDialogMessage`, `GetDlgItem*`, `SetDlgItem*`), menus, clipboard, cursor, `GetAsyncKeyState`, `GetKeyboardState`, `MapVirtualKeyA`, `SystemParametersInfoA`, `GetSystemMetrics`, … |
| `winmm` | 3 | `timeBeginPeriod`, `timeGetTime`, `timeKillEvent` |
| `ws2_32` | 1 | `gethostbyname` (one call site at 0x5789CD) |
| `binkw32` | 8 | `_BinkOpen@8`, `_BinkClose@4`, `_BinkDoFrame@4`, `_BinkNextFrame@4`, `_BinkWait@4`, `_BinkCopyToBuffer@28`, `_BinkSetSoundSystem@8`, `_BinkOpenDirectSound@4` |
| `d3d9` | 1 | `Direct3DCreate9` |

Implications:

* The API surface is a classic 2005 Win32 game: no XAudio2, no XInput, no
  DirectInput (input goes through `user32` messages + `GetAsyncKeyState`),
  no D3DX, no DirectDraw, no OpenAL, no Steam.
* 93 `kernel32` functions + 75 `user32` + 106 `msvcr71` means the Win32 shim
  layer is the bulk of the runtime work, but every one of them is already in
  the reference runtime's vocabulary (Skyrim imports a superset).

## 5. Graphics API — Direct3D 9

`d3d9.dll` is imported for exactly one symbol, `Direct3DCreate9`. The game then
uses the D3D9 COM interfaces (`IDirect3D9`, `IDirect3DDevice9`, …) through
vtables obtained at runtime, which is why no further imports appear.

Strings in the image confirm it: `d3d9.dll` (4 occurrences), `Direct3D` (4),
and D3D9-specific error strings around RVA 0x4D9xxx.

**Decision:** D3D9 → `dxvk-native` (native-1.9.2b, Vulkan 1.1), the same
layer the reference port uses. No D3D8/DirectDraw fallback is needed.
`d3dx9_24.dll` is not imported, so the D3DX half of the reference's
`d3d9_bridge_gen.cpp` is not exercised by this game.

## 6. Audio

* `dsound.dll!DirectSoundCreate8` — DirectSound 8 for sound effects and music.
* `binkw32.dll!_BinkOpenDirectSound@4` / `_BinkSetSoundSystem@8` — Bink
  decodes its own audio and hands PCM to DirectSound.
* `winmm` timer (`timeBeginPeriod` / `timeGetTime` / `timeKillEvent`).

**Plan:** `DirectSoundCreate8` → SDL2 audio (the reference's `runtime/dsound.c`
already does this: primary/secondary buffers, 3D listener, volume, panning).
`binkw32.dll` is recompiled (it is a normal x86 DLL with relocations and
separate codec sections `BINK*`), so Bink audio and video decode with the
original code. No external codec is needed.

## 7. Video / cutscenes

`binkw32.dll` 1.7d (RAD Game Tools), 8 imports as listed above. The movies are
in `data/movies/pc/*.dat` and start with `BIKi` (Bink video + Bink audio),
e.g. `Treyarch_USM_logo.dat`, `S01_FaPr_IGC1.dat` (48 MB),
`S13_FnBt_IGC1.dat` (82 MB). Total 279 MB.

The `.dat` extension is only a rename: the container is Bink. Since
`binkw32.dll` is recompiled, playback uses the game's original decoder.

## 8. Assets and loading

| Path | Size | Format (verified by header) |
|---|---:|---|
| `data/packs/*.PAK` | 510 MB, 42 files | custom Treyarch archive (`SCNANIMS_PC.PAK`, `amalga_PC.PAK`, …) |
| `data/packs/*.dat` | — | loose assets, many are `DDS ` textures (e.g. `celshading.dat`) |
| `data/Sound/pc/**/*.WBK` | 781 MB | `WAVEBK11` wave banks with embedded names (`ARACHNO_MAN_COSTUME`, …) |
| `data/movies/pc/*.dat` | 279 MB | Bink (`BIKi`) |
| `data/game.ini` | 4 KB | INI (`[Strings]`, `[Ints]`, `[Flags]`, `FORCE_PROGRESSION=1`) |
| `data/ump.dat` | 8 KB | PNG (`\x89PNG`, 64×64) |
| `data/usm_lte.usm` | 8 KB | text localisation (`QUIT=QUIT`, `NEW_GAME=NEW GAME`, …) |

The game opens files with `CreateFileA`/`CreateFileW`/`ReadFile` and also uses
`CreateFileMappingA`/`MapViewOfFile` (2 call sites: 0x58DFBC, 0x7DF272), so the
shim must provide real shared-memory mapping semantics, not just `read()`.
No middleware (no Miles, no FMOD, no PhysX, no GameSpy SDK) appears in the
import table. `.WBK` decoding is done by the game's own code, so it comes for
free with static recompilation.

## 9. Signals that could break static recompilation

| Signal | Finding | Consequence |
|---|---|---|
| Packing / encryption | none (§3) | lift the file as-is |
| Self-modifying code | `VirtualAlloc` has 3 call sites (0x7F040B, 0x7F0433, 0x7F0495); all pass `flProtect = 4` (`PAGE_READWRITE`) and `MEM_COMMIT|MEM_RESERVE`. `VirtualProtect` is **not imported**. No `PAGE_EXECUTE_READWRITE` anywhere. | no runtime code generation expected |
| Base relocations | stripped; image fixed at 0x400000 | fixed addresses, ideal for static recompilation |
| Jump tables | MSVC 7.1 switch tables (`jmp [reg*4+table]` bounded by `cmp`) — counted by `discover.py` (see §11) | handled by the existing discovery/lifting heuristics |
| SEH / C++ exceptions | `_except_handler3`, `__CxxFrameHandler`, `_CxxThrowException`, `_setjmp3`, `longjmp`, `_purecall` imported; `SetUnhandledExceptionFilter` called once (0x5AD1D3); entry prologue installs an SEH frame | the runtime needs a working SEH/C++ unwinder over lifted frames, or those paths must be avoided. **Highest-risk item.** |
| Threads | `CreateThread` + `CreateEventA`/`SetEvent`/`WaitForSingleObject`/`CreateMutexA`/critical sections; `SetThreadPriority`, `SwitchToThread`, `SleepEx` | multi-threaded engine; needs the reference's acquire/release memory model |
| Network | one `gethostbyname` call (0x5789CD) | stub: return NULL, log |
| Timers | `winmm` multimedia timer + `QueryPerformanceCounter` | shim over `clock_gettime`/SDL |
| Dialogs / shell | `CreateDialogParamA`, `SHBrowseForFolder`, `ChooseFontA`, clipboard | needed for the options/save screens; may be stubbed initially |
| Font/text | `CreateBitmap`, `GetTextExtentPoint32A`, `GetStockObject` | GDI text metrics shim |

## 10. Decisions taken from this reconnaissance

| Module | Decision | Load address |
|---|---|---|
| `USM.exe` | recompile | 0x00400000 (its own base) |
| `binkw32.dll` | recompile (video + Bink audio) | 0x09000000 |
| `d3dx9_24.dll` | **not used** — not imported | — |
| `xerces-c_2_3_0.dll` | **not used** — not imported | — |
| `msvcr71.dll`, `MSVCP71.DLL` | shim on the host (`msvcrt.c` + `msvcp71` additions) | — |
| `dsound.dll` (ASI loader) | ignore; shim `DirectSoundCreate8` | — |
| `d3d9.dll` | `dxvk-native` | — |
| `scripts/*.asi` | ignore (third-party mods) | — |

Because the game imports no D3DX and no Xerces, the recompiled-code surface is
`USM.exe` (4.6 MB of `.text`) plus `binkw32.dll` (283 KB of code). That is
about 40% of the Skyrim reference's code volume.

## 11. Discovery results (measured on this machine)

Machine: Qualcomm SM8350 (Snapdragon 888), 8 cores, 7.3 GB RAM, Android/Termux
with `python3.14` + `capstone 5.0.7` + `pefile`.

`binkw32.dll` (reference run, 283,277 bytes of code):

```
tail-call starts 252
text relocs 303, 151 in jump tables skipped
gap round 0 added 7 ... gaps 196 gap bytes 76454
functions 787  bad data roots 75308  jump tables 46  unresolved indirect jmps 5
text bytes 283277 covered 203743 cc/nop bytes 4632
real 0m13.7s
```

`USM.exe` discovery: see `docs/RECON_USM_DISCOVERY.md` (generated when the
run finishes; the numbers — function count, coverage, jump tables, unresolved
indirect jumps — are the input to the risk assessment in §12).

## 12. Risk plan

| Risk | Likelihood | Mitigation |
|---|---|---|
| SEH-based C++ exceptions in engine code (`__CxxFrameHandler`) | high | implement `_except_handler3`/`__CxxFrameHandler`/`_CxxThrowException` over lifted frames (the reference already shims them for Skyrim); if a path cannot be supported, `HOST_OVERRIDES` + a host replacement at a fixed address |
| D3D9 state-block/behaviour gaps in dxvk on mobile | medium | the reference's `d3d9_bridge` + `LB_` debug flags; trace calls per frame |
| 93 `kernel32` + 75 `user32` shims | medium | the reference already implements this vocabulary; missing entries stop with a log line, never silently |
| Wave-bank / PAK formats | low | decoded by the game's own recompiled code; no host work |
| Performance on mobile GPU (2005 D3D9 title, 60 fps target) | medium | resolution scale option; dxvk settings |
| Build size/RAM (gen/ C is large, 1–2 GB per file to compile) | medium | `LB_GEN_JOBS`, split files, CI-only build |
| Another game build (different SHA-1) | — | port is pinned to `66c58c6e…`; hooks use fixed addresses |

## 13. Open questions

1. Does the engine hit `_CxxThrowException` on a normal boot path (menu)? This
   decides whether exception support is on the critical path.
2. Which `IDirect3D9`/`IDirect3DDevice9` methods are actually used (needs the
   call-site scan after lifting, not an import scan).
3. Whether the `WAVEBK11` decoder needs `IDirectSound8::CreateSoundBuffer`
   with 3D interfaces or only 2D buffers.
4. Whether `SHBrowseForFolder` (save-folder picker) is on the boot path.
