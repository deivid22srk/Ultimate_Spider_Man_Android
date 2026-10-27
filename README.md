# Ultimate Spider-Man (PC, 2005) for Android

An unofficial native Android port of the 2005 PC version of *Ultimate
Spider-Man* (Treyarch), built by **static recompilation**: the x86 machine code
of the game's own executable is translated to C at build time and compiled for
ARM64 with the Android NDK. No Wine, no Box64/FEX, no x86 emulator at runtime.

This repository contains no game code and no game data. You need your own copy
of the PC game, which is used as the input to the recompilation.

Architecture and tooling follow the Skyrim Android port
([ttom795/Skyrim_Android](https://github.com/ttom795/Skyrim_Android)), which is
the same recompilation runtime generalised for LEGO Batman, LEGO Marvel and
Skyrim. What is specific to this game is in `docs/RECON.md`.

> **Status: work in progress, not a working port yet.** Read
> [Status](#status) before anything else.

## Status

Honest state of this repository, updated as work progresses:

| Part | State |
|---|---|
| Phase 1 reconnaissance (`docs/RECON.md`) | **done**, evidence from the binaries |
| Toolchain adapted to `USM.exe` + `binkw32.dll` | done (`tools/`, `LIFT_GAME=usm`) |
| `gen/` (recompiled C) | generated locally from the user's copy; **never committed** |
| Runtime (Win32/D3D9/DirectSound shims, app, launcher) | written, **not yet run** |
| APK | built only by `.github/workflows/build.yml` |
| Reaches the main menu | **not verified** |

Everything that has actually been compiled and tested is listed in
[docs/TESTED.md](docs/TESTED.md). Nothing in this README should be read as a
claim that the game runs.

## What the port will be

- **Static recompilation.** `USM.exe` (1.0.0.1) and `binkw32.dll` (Bink 1.7d)
  are translated to C and compiled for arm64. The exe keeps its original
  load address (0x00400000); Bink is relocated to 0x09000000.
- **Graphics:** Direct3D 9 → [dxvk-native](https://github.com/Joshua-Ashton/dxvk-native)
  (native-1.9.2b) → Vulkan 1.1. The game imports exactly one D3D9 symbol,
  `Direct3DCreate9`.
- **Audio:** DirectSound 8 → SDL2. Bink audio is decoded by the recompiled
  `binkw32.dll`.
- **Video:** the original Bink decoder (`binkw32.dll`, recompiled) plays the
  `.dat` cutscenes, which are Bink files (`BIKi`).
- **Input:** physical controller (Xbox layout by default, optional Nintendo
  layout) and an on-screen touch overlay.
- **Threads:** the game's threads run on all cores; shared memory uses ARM
  load-acquire/store-release to keep x86 ordering.
- **Storage:** everything the port writes goes to `port/` inside the game
  folder (`port/log.txt`, `port/save/`). Nothing in `Android/data`.

## Game files you need

From your own PC install of *Ultimate Spider-Man* (2005), the contents of the
`Game Files` folder:

| File | Required | SHA-1 (this build) |
|---|---|---|
| `USM.exe` | yes | `66c58c6e96ad96a1713f86fb903af1192e8bc8f5` |
| `binkw32.dll` | yes | `e7a2670c5e61d3645e39e7e9ba779eea743daee7` |
| `data/` (packs, Sound, movies, …) | yes | — |
| `d3dx9_24.dll`, `xerces-c_2_3_0.dll` | no | not imported by `USM.exe` |
| `dsound.dll` | no | in this release it is an ASI mod loader; ignore it |
| `scripts/*.asi` | no | third-party mods; ignore them |

The recompiled code uses fixed addresses, so it only works with `USM.exe`
1.0.0.1 as listed above. The launcher checks the size and SHA-1 and warns when
the executable is a different build.

Check your copy on Windows with `certutil -hashfile USM.exe SHA1`, or on
Linux/macOS/Android with `sha1sum USM.exe`.

## 1. Generate the recompiled code

Requirements: Python 3.10+, `pip install capstone==5.0.9 pefile==2024.8.26`
(`unicorn==2.0.1.post1` only for the lifter self-test `tools/difftest.py`).

```sh
tools/regen_gen.sh "/path/to/Game Files"
```

This writes `gen/` (git-ignored, derived from your game — never redistribute it)
and `analysis/` (discovery output). Measured on a Snapdragon 888 phone:

| Step | Time | Peak RAM |
|---|---|---|
| discovery of `USM.exe` | see `docs/RECON.md` §11 | ~0.5 GB |
| discovery of `binkw32.dll` | 14 s | <0.2 GB |
| lifting both | see `docs/RECON.md` §11 | ~0.4 GB |
| `gen/` size | see `docs/RECON.md` §11 | — |

## 2. Build the APK

**The APK is built by GitHub Actions only** (`.github/workflows/build.yml`).

This repository is **public** and contains no game code and no code generated
from the game, so the public workflow builds the **app-only** APK: launcher +
runtime against `ci/gen_stub` (empty recompiled tables). It installs, the
launcher validates your folder and the exe build, and **Play** stops with a
clear message. It exists to prove that the Android/runtime build works on every
push.

### Building the full APK

`gen/` is derived from your own copy of the game and must never be published
(it is git-ignored), so the full build keeps it in a **private repository** and
lets this public repository's CI (free on public repos) fetch it:

1. `tools/regen_gen.sh` produces `gen/` from your copy of the game.
2. Pack it and upload it to a private repository as a release asset:
   ```sh
   tar -I zstd -cf gen.tar.zst gen
   gh release create gen-$(date +%Y%m%d) gen.tar.zst --repo <you>/USM_gen
   ```
3. In this repository set the variables and the secret the workflow reads:
   ```sh
   gh variable set GEN_REPO --body <you>/USM_gen
   gh variable set GEN_TAG  --body gen-$(date +%Y%m%d)
   gh secret   set GEN_TOKEN --body "<a token with repo scope on <you>/USM_gen>"
   ```
4. `gh workflow run build.yml` (or push). The run downloads `gen.tar.zst`,
   builds the APK and publishes it as an asset of a **private** release in
   `GEN_REPO` (`apk-<run>-<date>`). Nothing game-derived is uploaded here.

Without `GEN_REPO` the workflow builds the app-only APK and uploads it as a
public artifact, which is safe because it contains no game code.

Locally the same build is `cd android && ./gradlew assembleDebug -PndkVersion=27.2.12479018 -PlbGenJobs=2`
with a real `gen/` in place.

`LB_GEN_JOBS` caps how many recompiled files are compiled at once (each needs
~1.5 GB of RAM). The APK is signed with the debug key (fine for personal use,
not for distribution).

## 3. Install

Requirements: arm64 Android 9+, Vulkan 1.1, 3 GB RAM, ~7 GB free space.

1. Install the APK from the workflow artifact.
2. Copy the contents of your `Game Files` folder to one folder outside
   `Android/data`, e.g. `Internal storage/USM/` (`USM.exe`, `binkw32.dll`,
   `data/` side by side).
3. Start the app, tap **Choose Ultimate Spider-Man folder**, allow file access.
   The launcher says **Ready** and shows the executable it found, or lists what
   is missing.
4. Set the options and tap **Play**.

## Custom GPU driver

The launcher can install a user supplied Vulkan driver pack (Turnip and the
like) and run the renderer on it through libadrenotools, falling back to the
system driver if the pack does not pass validation. What packs work, how a pack
is probed and what the `driver:` lines in `port/log.txt` mean:
[docs/GPU_DRIVERS.md](docs/GPU_DRIVERS.md).

## Controls

- Controllers: Xbox layout by default; the launcher can swap A/B and X/Y.
- Touch overlay: stick, A/B/X/Y, shoulders, Start and Back.

## Troubleshooting

- `port/log.txt` in the game folder is rewritten on every launch; attach it
  when reporting a problem. `port/last_exit.txt` records how the previous run
  ended.
- `port/save/singlecore.txt` runs the game's threads on one core (rules out
  threading problems, slower).
- `port/save/noprof.txt` turns off the sampling profiler in the log.
- Desktop build for debugging: `desktop/build.sh` then `desktop/run.sh <game dir> [save dir]`
  (needs clang, cmake, ninja, SDL2 and Vulkan development files).
  Useful variables: `LB_WINDOW=WxH`, `LB_TOUCH=1`, `LB_LOG=n`,
  `LB_D3DTRACE=from:to`, `LB_BCEMU=1`.

## Known issues

- Not yet run: see [Status](#status).
- Only `USM.exe` 1.0.0.1 (`66c58c6e…`) is supported.
- The `dsound.dll` shipped by the "preinstalled" release is an ASI mod loader,
  not DirectSound; it is ignored, and `scripts/*.asi` mods are not loaded.
- The app targets API 29 with legacy storage, like the reference port: the
  folder is accessed with plain file calls, so it must be on shared storage
  and readable (the launcher asks for all-files access if needed).

## Legal

Ultimate Spider-Man, Spider-Man, Marvel and all related names and assets belong
to their respective owners. This project is not affiliated with or endorsed by
them. It ships no game code, no game data and no code generated from them.
`gen/` and anything built from it are derived from your own copy of the game and
must not be redistributed. Third-party code keeps its own licenses: SDL2 (zlib)
in `android/app/jni/SDL2`, dxvk-native (zlib) in `android/app/jni/dxvk`.
