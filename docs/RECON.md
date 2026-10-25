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

## 11. Discovery and lifting results (measured on this machine)

Machine: Qualcomm SM8350 (Snapdragon 888), 8 cores, 7.3 GB RAM, Android/Termux
with `python3.14` + `capstone 5.0.7` + `pefile`.

`binkw32.dll` (283,277 bytes of code, image base rebased to `0x09000000`):

```
tail-call starts 252
text relocs 303, 151 in jump tables skipped
gap round 0 added 7 ... gaps 196 gap bytes 76454
functions 787  bad data roots 75308  jump tables 46  unresolved indirect jmps 5
text bytes 283277 covered 203743 cc/nop bytes 4632
discovery 13.7 s
lifted 780, rejected 7 (junk opcodes)
gen: 24 C files, 6.4 MB, 31 s
```

`USM.exe` (4,644,864 bytes of code, image base `0x00400000`):

```
text relocs 0, 0 in jump tables skipped
gap round 0 added 5486
call closure round 0 added 14
gaps 1179  gap bytes 54236
functions 44032  bad data roots 17966  jump tables 360  unresolved indirect jmps 345
text bytes 4644864 covered 4334101  cc/nop bytes 271598
coverage 93.3 %  (gap bytes 1.2 %, alignment/padding 5.8 %)
discovery ~20 min, peak RSS ~0.5 GB (DISCOVER_CACHE=60000)
```

Lifting `USM.exe` (`tools/lift.py analysis/functions_usm.json gen exe`):

```
lifted 43825  rejected 207 (0.47 %)
rejections: MMX/pf* instructions 83, junk/privileged opcodes 97, segment regs 27
gen: 90 C files, 120 MB, 43825 entries in g_fn_table_exe, ~5.5 min
```

The 207 rejected functions contain instructions the lifter does not implement
(mostly MMX SIMD inside CRT/string helpers and stray data decoded as code). They
are logged by name at lift time and are the first candidates for
`HOST_OVERRIDES_USM` if one is ever called.

`clang -fsyntax-only` on the lifted output (representative files, including the
17 MB translation unit) is clean; the only diagnostics are the lifter's
`-Wparentheses-equality` idiom, silenced by `-w` in the build.

## 11b. Input model (verified in the binary, `llvm-objdump` of the whole image)

This is what the game actually does, not what the SDK usually implies. Every
claim below comes from a call-site scan of `USM.exe` (`66c58c6e...`).

| Fact | Evidence |
|---|---|
| No static DirectInput/XInput imports | the import table has neither `dinput8.dll` nor `xinput*.dll`; `winmm.dll` only brings the time functions |
| DirectInput is loaded dynamically | `f_81F9A0`: `LoadLibraryA("dinput8.dll")` + `GetProcAddress(..., "DirectInput8Create")`, called from the input-manager constructor `f_5B0D00` (object 0x129E8 bytes, `IDirectInput8` at +0x2894, manager pointer `[0x987948]`) |
| Keyboard and mouse come from DInput devices | the manager polls `IDirectInputDevice8::GetDeviceState`; our shim is reached there (`runtime/dinput.c`) |
| The window procedure sees **no** mouse messages | `f_5941A0` handles only 2..0x100, 0x102..0x112, 0x1C, ...; `0x200`/`0x201` fall through to `DefWindowProcA` |
| Keyboard-by-window-message is dead code | the `WM_KEYDOWN`/`WM_KEYUP` broadcast `f_5BCA60` is invoked only through the sink pointers `[0x9680A0]`/`[0x9680A4]`, and the two functions that would set them (`f_5BCA20`, `f_5BCA40`) are **never called** anywhere in the image |
| The UI is driven by the **system cursor** | widget hit-test `f_589EF0`/`f_589FA0` calls `GetCursorPos` + `ScreenToClient` + `GetClientRect`; `f_581C60` maps the cursor into widget coordinates and is called by the widget classes; the game draws its own pointer (`f_594DF0`, texture `data\packs\mouse.dat`) |
| `ClipCursor` is only ever called with `NULL` | all four call sites (wndproc `0x5942BB`, `f_5A4F40` `0x5A5005`, main loop `0x5AD533`, `0x5AD59C`) `pushl $0x0` first, so there is **no** "mouse captured for mouse-look" signal to read |
| `SetCursorPos` is not used for mouse look | only three call sites, all in window/UI classes (`f_589DB0`, `f_589E50`, `f_8203F0`); the camera comes from the mouse device's relative axes |
| A 256-byte table maps VK → the game's own "logical key" codes | `0x88CC68`, used by the wndproc (letters 1..26) — a different space from the DirectInput bindings |

Action binding table: `0x88D648`, 18 entries of 0x48 bytes each
(`name[0x20]`, `id`, `inverse`, then three 8-byte bindings encoded as
`type << 16 | code`). The left mouse button appears in **no** binding:

| # | Action | Keyboard | Alt | Pad / mouse |
|---|---|---|---|---|
| 01 | Forward | W | Up | Pad 4 |
| 02 | Backward | S | Down | Pad 3 |
| 03 | TurnLeft | A | Left | Pad 2 |
| 04 | TurnRight | D | Right | Pad 1 |
| 05 | CameraUp | Numpad 8 | PgUp | — |
| 06 | CameraDown | Numpad 2 | PgDn | — |
| 07 | CameraLeft | Numpad 4 | — | — |
| 08 | CameraRight | Numpad 6 | — | — |
| 09 | Jump | Space | — | Pad 21 |
| 0A | StickToWalls | F | LShift | Pad 22 |
| 0B | Punch | Q | LCtrl | Pad 23 |
| 0C | Kick | E | LAlt | Pad 24 |
| 0D | ThrowWeb | C | — | Mouse 10 |
| 0E | BlackButton | V | — | Mouse 9 |
| 0F | Pause | Esc | — | — |
| 10 | BackButton | M | — | — |
| 11 | CameraCenter | B | — | — |
| 12 | ScreenShot | F11 | — | — |

`Pad 21..24` are pad buttons 0..3, i.e. A = Jump, B = StickToWalls, X = Punch,
Y = Kick — exactly the order `runtime/input.c` presses them in.

**Consequences for the port** (implemented in `runtime/`):

1. Keys must be injected as DInput device state (and as real Win32 key
   messages for the `.fdf` parser, which polls `GetKeyboardState`); the window
   message path alone does nothing.
2. The menus need a **virtual system cursor**: our `GetCursorPos` used to
   return `(0,0)` for ever, which is why no menu widget could ever be hit.
   `runtime/user32.c` now owns a cursor that the touch layer warps (absolute on
   touch-down) and that the mouse deltas also move, exactly like Windows.
3. Clicks are DInput mouse buttons. A tap holds button 0 for at least 120 ms
   (a one-frame tap would be missed by a polled `GetDeviceState`), and the same
   state is mirrored into `VK_LBUTTON` for `GetKeyState`.
4. The right-half drag still drives the camera through the mouse's relative
   axes; the same deltas keep the virtual cursor in step, so the same gesture
   moves the pointer in menus.

## 11c. Touch overlay and the icon pack

The overlay presses **keys of the game's own bindings**, not pad buttons the
game does not read (there is no XInput in `USM.exe`, §11b). `runtime/input.c`
translates the virtual pad to those keys, and `runtime/touch.cpp` draws the
buttons. Which icon each button wears follows from that table:

| Overlay button | Key it sends | Action in `USM.exe` | Icon (`port/icons/`) |
|---|---|---|---|
| A | Space | `Jump` (id 09) | `hud_jump.png` |
| B | C | `ThrowWeb` (id 0D) | `hud_web.png` |
| X | Q | `Punch` (id 0B) | `hud_punch.png` |
| Y | E | `Kick` (id 0C) | `hud_kick.png` |
| LB | F | `StickToWalls` (id 0A) | `hud_wallcrawl.png` |
| RB | V | `BlackButton` (id 0E) | `hud_black.png` |
| Start | Esc | `Pause` (id 0F) | `hud_pause.png` |
| Back | M | `BackButton` (id 10) | `hud_back.png` |

The art is the port's own (generated for this port, never extracted from the
game); it is unzipped next to the port's folder as `<game>/port/icons/`, with
the pressed variants in `icons/on/`. `runtime/art.c` packs whatever it finds
into one 1024×1536 atlas uploaded as a `D3DFMT_A8R8G8B8` texture, so the whole
overlay is two `DrawPrimitiveUP` calls (plain discs + icons); **every icon is
optional** and a missing file falls back to the disc. `runtime/touch.cpp` logs
`touch: N HUD icons loaded from <dir>` on the first frame, which is how a run
is confirmed from `port/log.txt`.

Context-dependent behaviour implemented so far, and no more: the camera-zone
hint (`hud_campad`) is drawn only while a finger is dragging in the camera
zone, and the pointer art (`hud_cursor`) only for 1.5 s after a pointer
gesture. Swapping the whole HUD by game state (menus vs gameplay vs cutscene)
is **not** implemented — none of the signals it needs (the front-end level
pointer, Bink playback, a meaningful `ClipCursor`/`ShowCursor` change) is
wired up yet.

## 11d. The present path stretched the whole frame (measured, fixed)

The first runs on the phone showed the HUD stretched and pushed to one side,
and the game's own menu text in the same state: wide, squat glyphs. That is
not an overlay bug — the frame itself is stretched.

`USM.exe` keeps its own resolution in `pcconfig.txt` and on a fresh install
that is its 4:3 default, so it called `CreateDevice` with
`BackBufferWidth/Height = 800x600`. DXVK sizes its D3D9 back buffer from those
present parameters (`D3D9SwapChainEx::CreateBackBuffers`) but builds the
presenter images at `wsi::getWindowSize()` — the SDL window, 2285x1080 here —
and `UpdatePresentRegion` then blits `m_srcRect = (0,0,800,600)` onto
`m_dstRect = (0,0,2285,1080)`. Non-uniform scale, and the swap chain
composited at an x offset of 115 px (the 2400-px display is wider than the
2285-px surface).

Measured on `Screenshot_20261025-190014.png` by fitting seven known overlay
elements (`runtime/touch.cpp` puts them at fixed normalised positions):

    x_screen = 2.857 * x_800 + 114.5        y_screen = 1.801 * y_600
    2.857 = 2285/800, 1.801 = 1080/600, 114.5 = the compositor's x offset

Two consequences, both visible on the device:

* the game is drawn 1.587x too wide (the menu glyphs confirm it), and
* `touch_draw` reads its aspect from `GetBackBuffer()->GetDesc()`, i.e. 4:3,
  so every overlay quad was laid out 1.587x too wide and every button an
  ellipse instead of a disc.

Fix, in `runtime/d3d9_bridge.cpp`: `pp_fix_size` replaces a requested size
whose aspect is not the window's with the window's own size — entry 0 of the
list `EnumAdapterModes` already offers the game's "Resolution Scale" menu — so
the blit is 1:1. A size the player picks from that menu (or a scaled mode like
1142x540) is already at the window's aspect and is left alone. It also parks
`user32_set_screen` on the same window size, because that is the space
`GetClientRect` reports and the space the menus divide the cursor by (§11b);
`platform_present_hook` keeps it in step, since the window only gets its real
size once it is fullscreen.

`touch_draw` no longer trusts the back buffer for its aspect: it lays the
overlay out in back-buffer pixels but corrects the aspect with the size the
frame is *presented* at, so a smaller back buffer can never distort the
overlay again. It also fades the overlay to 30 % after 2.5 s without a touch
(back to full on any touch), so it stops covering the game's own menus, which
are tapped through the virtual cursor — a stopgap until the contextual HUD.

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
