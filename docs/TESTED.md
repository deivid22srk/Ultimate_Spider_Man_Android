# What was actually tested

This file is the honest record. "Tested" means a command was run and its output
observed; "not tested" means exactly that.

Machine used for everything local: Motorola Edge 30 Fusion (Qualcomm SM8350,
8 cores, 7.3 GB RAM), Android, Termux with root, Python 3.14 + capstone 5.0.7 +
pefile, clang 21.1.8 (aarch64). The APK itself is only ever built by GitHub
Actions (NDK 27.2.12479018, clang 18).

## Tested and working

| What | How | Result |
|---|---|---|
| PE analysis of `USM.exe` and the DLLs | `pefile`, `capstone`, `sha1sum`, hexdump | all facts in `docs/RECON.md` §2–§8 |
| DRM / packing check | section names, entropy, imports, overlay, entry point | no packer, no disc check |
| Import table extraction | `pefile` walk of `IMAGE_DIRECTORY_ENTRY_IMPORT` | 17 DLLs, 337 functions |
| D3D9 / DirectSound / Bink identification | imports + strings | D3D9, DirectSound8, Bink 1.7d |
| `discover.py` on `binkw32.dll` | 283,277 bytes of code | 787 functions, 46 jump tables, 13.7 s |
| `lift.py` on `binkw32.dll` | 780 functions | 24 C files, 6.4 MB, 31 s |
| `discover.py` on `USM.exe` | 4,644,864 bytes of code | 44,032 functions, 360 jump tables, 93.3 % coverage, ~20 min |
| `lift.py` on `USM.exe` | 44,032 functions discovered | 43,825 lifted, 207 rejected, 90 C files, 120 MB, ~5.5 min |
| lifted C is valid C | `clang -fsyntax-only` on `gen/bink_00.c`, `gen/exe_00.c` and `gen/exe_33.c` (17 MB TU) | exit 0 |
| every `USM.exe` import has a shim | script diffing `IMAGE_DIRECTORY_ENTRY_IMPORT` against the runtime's shim tables (`msvcr71` aliased to the `msvcrt` table) | 0 missing (binkw32/d3d9 excluded: recompiled, not shimmed) |
| APK builds in CI | Actions runs for `34f0ab4`, `e08b5ae`, `a3dd136` | 3/3 success; `apk-16-20261025-1819`, 18,510,944 bytes |
| the game reaches the front end on the phone | install + `am start`, screenshots pulled from the device | menus render (`docs/RECON.md` §1 for the SHA-1 pin); see the run log for `port/log.txt` |
| the `.fdf` parser crash is gone | `msvcr71` `strtol`/`strtoul` now write the end pointer (heap corruption) | no crash on the boot path after `34f0ab4` |
| background/resume | `platform_pump_events()` keeps the SDL pump alive while the game idles in `PeekMessageA` | no freeze after `e08b5ae` |
| keyboard injection reaches the game | `key down/up vk=.. sc=..` + `dinput: keyboard poll #N` lines in `port/log.txt` | game reacts to scripted keys after `a3dd136` |
| action binding table and input model | `llvm-objdump` call-site scan of the whole image | `docs/RECON.md` §11b, every row has a call site |
| PNG reader (`runtime/png.c`) | host build with `clang`, decoding all 58 files of the icon pack | dimensions and an FNV-1a of the RGBA bytes match an independent Python decoder byte for byte |
| icon packing (`runtime/art.c`) | `clang -o arttest tools/art_test.c runtime/art.c runtime/png.c -lz` against the real pack | 21/21 icons, 1024x1536 atlas, every atlas cell byte-identical to its source PNG (B,G,R,A order), UVs select the right cell |
| Option A builds | Actions run `40234915350` | success; the earlier run `40233580512` had failed to link (`(anonymous namespace)::user32_pointer_fingers`), which `tools/check_c_linkage.sh` now catches locally |

## Implemented but not tested

| What | Why not |
|---|---|
| Virtual system cursor (`user32_cursor_*`, `GetCursorPos`/`SetCursorPos`/`ClipCursor`) | compiles and links; needs an APK run. Local check: `clang -DGAME_USM -fsyntax-only` on the runtime and `llvm-nm` on `touch.o` showing the unmangled `user32_*` references |
| Touch pointer: tap warps the cursor and holds `VK_LBUTTON`/DInput button 0 for ≥120 ms | same; the click has not been observed in a real menu yet |
| DInput mouse buttons from the touch layer (`dev_GetDeviceState`) | same |
| `VK_LBUTTON`/`VK_RBUTTON`/`VK_MBUTTON` mirrored into `user32_vk_states()` | same |
| Right-stick mouse deltas now gated to one per presented frame (were injected ~1 kHz) | the over-injection was a code-reading finding, not a measured one; needs a run with a real pad |
| Touch marker drawn at the last touch point (fades after 700 ms) | needs a screenshot |
| HUD drawn from the artist's icon pack (`runtime/touch.cpp` + `runtime/art.c`) | compiles and packs offline, but no APK has drawn it yet; the run writes `touch: N HUD icons loaded from ...` to `port/log.txt`, which is how it will be confirmed |
| Icons pressed state (`on/hud_*_on.png`) and the camera-zone hint | same run |
| "Contextual" part of the HUD (show/hide and swap icons by game state) | not implemented: none of the game-state signals (level pointer, Bink playback, ShowCursor count) is wired yet |
| Android launcher (folder validation, SHA-1 check, options) | only exercised by hand so far |
| `runtime/usm_shims.c` CRT/dialog/image-list shims beyond the exercised paths | compiles, but not every shim has been hit by real game code |
| `tools/difftest.py` (lifter self-test) | `unicorn` does not build in this Termux/Android environment; run it on a normal Linux host |

## Not implemented / blocked

| What | Reason |
|---|---|
| SEH / C++ exceptions | the reference runtime aborts on `_CxxThrowException` / `__CxxFrameHandler`; the game imports them, so this is still the top risk |
| Gameplay input without the touch overlay | needs a real DInput *joystick* device in the shim: the game asks for one, our shim answers `DIERR_DEVICENOTREG`, and the keyboard path (window messages) is dead code (§11b) |
| Physical gamepad | same reason as above; the pad currently works by pressing keys through the keyboard device |
| On-device verification of the cursor/clicks | `adb connect 192.168.0.112:39587` → `Connection refused`; the phone is not reachable from here |
| Audio data | the user's copy has no `data/packs/sounds_PC.PAK` and no `STREAMS_*` wave banks; the game boots and renders without them |
| `xerces-c_2_3_0.dll`, `d3dx9_24.dll` | not imported by `USM.exe`; deliberately not recompiled |
| `.asi` mods | third-party, not loaded |
