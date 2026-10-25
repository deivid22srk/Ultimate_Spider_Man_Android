# What was actually tested

This file is the honest record. "Tested" means a command was run and its output
observed; "not tested" means exactly that.

Machine used for everything local: Motorola Edge 30 Fusion (Qualcomm SM8350,
8 cores, 7.3 GB RAM), Android, Termux with root, Python 3.14 + capstone 5.0.7 +
pefile, clang 20.

## Tested and working

| What | How | Result |
|---|---|---|
| PE analysis of `USM.exe` and the DLLs | `pefile`, `capstone`, `sha1sum`, hexdump | all facts in `docs/RECON.md` §2–§8 |
| DRM / packing check | section names, entropy, imports, overlay, entry point | no packer, no disc check |
| Import table extraction | `pefile` walk of `IMAGE_DIRECTORY_ENTRY_IMPORT` | 17 DLLs, 337 functions |
| D3D9 / DirectSound / Bink identification | imports + strings | D3D9, DirectSound8, Bink 1.7d |
| `discover.py` on `binkw32.dll` | 283,277 bytes of code | 787 functions, 46 jump tables, 13.7 s |
| `lift.py` on `binkw32.dll` | 780 functions | 24 C files, 6.4 MB, 31 s |
| lifted C is valid C | `clang -fsyntax-only` on `gen/bink_00.c` | exit 0 |
| runtime compiles for this game | `clang -fsyntax-only -DGAME_USM` on `runtime/modules.c`, `main.c`, `shims.c`, `platform_sdl.c`, `touch.cpp` | exit 0 |

## Implemented but not tested

| What | Why not |
|---|---|
| Runtime Win32/D3D9/DirectSound shims for this game | needs the full `gen/` and an APK run |
| Android launcher (folder validation, SHA-1 check, options) | needs an APK |
| Touch overlay / controller mapping | needs an APK |
| CI workflow (`.github/workflows/build.yml`) | see the Actions run referenced in the final report |
| `tools/difftest.py` (lifter self-test) | `unicorn` does not build in this Termux/Android environment; run it on a normal Linux host |

## Not implemented / blocked

| What | Reason |
|---|---|
| Reaching the main menu | the runtime has not been run against the recompiled game |
| SEH / C++ exceptions | the reference runtime aborts on `_CxxThrowException` / `__CxxFrameHandler`; the game imports them, so this is the top known risk |
| `xerces-c_2_3_0.dll`, `d3dx9_24.dll` | not imported by `USM.exe`; deliberately not recompiled |
| `.asi` mods | third-party, not loaded |
