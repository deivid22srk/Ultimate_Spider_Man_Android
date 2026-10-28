# Custom GPU drivers (libadrenotools)

The launcher can install a user supplied Vulkan driver (a Turnip pack, for
example) and the runtime will run the whole renderer on it through
[libadrenotools](https://github.com/bylaws/libadrenotools) instead of the
Qualcomm driver that ships with the phone. This file documents what the feature
accepts, how the runtime validates a pack, and what every `driver:` line in
`port/log.txt` means.

## How it works

The app links against Android's system Vulkan loader, which only ever loads the
vendor driver. libadrenotools opens that loader inside an isolated linker
namespace with a hook in place, so when the loader goes to pick up
`vulkan.adreno.so` it is handed the pack's `libvulkan.so` instead. Everything
the renderer does with Vulkan (DXVK's instance/device/surface) goes through the
one `vkGetInstanceProcAddr` that comes out of that loader, so the surface DXVK
presents to and the driver that renders into it always agree.

## What pack works

The launcher's picker accepts a `.zip` or a bare `.so`, four levels deep,
skipping `__MACOSX`, and installs whatever it finds as `libvulkan.so` in the
app's internal `files/drivers/<name>/` folder (internal storage is mandatory:
Android refuses to `dlopen` a library anyone else can write). But installing is
not the same as working — the pack has to be:

- **arm64-v8a.** A 32-bit or x86 build is rejected by an ELF header check
  before anything loads it.
- **A Vulkan ICD** that exports `vkGetInstanceProcAddr` (every Vulkan driver
  does).
- **Built for the KGSL backend.** This is the one that bites. On stock Android
  an app has no `/dev/dri/renderD*` render nodes, so a Turnip built for DRM
  enumerates zero physical devices and reports `VK_SUCCESS` while doing it.
  KGSL builds (Mesa `-Dfreedreno-kgsl=true`, what the adrenotools ecosystem
  ships) talk to `/dev/kgsl-3d0` directly and find the GPU. Known-good
  sources of KGSL/adrenotools packs:
  - [K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers)
    (Turnip revisions, patched and unpatched, labelled "compatible with
    Adrenotools")
  - [The412Banner/Banners-Turnip](https://github.com/The412Banner/Banners-Turnip)
    (automated bleeding-edge Mesa Turnip, packaged for AdrenoTools)
  - the driver packs yuzu/Skyline/Vita3K communities distribute

  Packs built for Termux/glibc or Winlator's proot environment are the usual
  DRM-only offenders.

## The probe

Handing an unvalidated driver to the game is how a bad pack used to close the
app mid-init (SIGSEGV between driver code and guest code, with no clue in the
log). So before anything uses the pack, `runtime/driver_android.cpp` probes it
end to end through the very same `vkGetInstanceProcAddr` DXVK will use, under a
SIGSEGV/SIGBUS/SIGILL guard:

1. the file is a 64-bit ARM ELF;
2. instance extension enumeration — the call that makes the isolated loader
   run the adrenotools hook and actually load the pack;
3. a throwaway `VkInstance` with the WSI extensions DXVK asks for;
4. physical device enumeration, properties (including the driver's own name
   via `VK_KHR_driver_properties`, e.g. `turnip`) and memory heaps;
5. a `VkDevice`, then a host-visible allocation mapped, written and unmapped —
   the KGSL memory path that differs most between Qualcomm's blob and Turnip.

Any failure (including a crash during the probe) falls back to the system
driver; the app keeps working and the log says what happened.

## The direct-load fallback

The adrenotools hook is the only part of the integration that fails silently:
its errors go to logcat (`hook_impl`), not to `port/log.txt`. When the hook
path does not produce a working driver, the runtime loads the same pack a
second time — directly into the app's own linker namespace, with no system
loader copy, no isolated namespace and no interposition. KGSL Turnip packs
work that way on their own (the ICD exports `vkGetInstanceProcAddr` and opens
`/dev/kgsl-3d0`, which apps may open), and unlike the hook path every failure
ends in a `dlerror` that lands in the log:

| Line | Meaning |
|---|---|
| `driver: hook environment: libhook_impl.so=… libmain_hook.so=… system-loader=…` | presence of the pieces the hook needs, before anything runs |
| `driver: falling back to a direct load of <path>` | the hook path did not yield a working driver |
| `driver: direct load resolved vkGetInstanceProcAddr` (or `vk_icdGetInstanceProcAddr`) | the pack loaded directly and exports the ICD entry point |
| `driver: direct load failed: <dlerror>` | the pack cannot be loaded by bionic at all (missing dependency, wrong build — the dlerror says which) |
| `driver: direct-load probe passed, using <path>` | the pack drives the renderer, hook skipped entirely |
| `driver: direct-load probe failed at <step> (<detail>)` | the pack loads but still cannot drive the GPU (a DRM-only pack, for example) |

## Reading the log

| Line | Meaning |
|---|---|
| `driver: system` | no pack selected, nothing ran |
| `driver: <path> is not readable / is <why> (…arm64?)` | the pack failed the file checks |
| `driver: libadrenotools could not open <path>: <dlerror>` | the hook libraries or the namespace failed (rare) |
| `driver: <path> opened, probing before use` | the pack is loaded, the probe starts |
| `driver: probe <device>, vendor…, driver…` | probe success so far: adapter identity |
| `driver: probe driver name 'turnip', …` | the pack's own driver name (`VK_KHR_driver_properties`) |
| `driver: probe failed at <step> (<detail>)` | the pack was rejected at `<step>` (followed by `keeping the system driver` if nothing else works) |
| `driver: <verdict sentence>` | who failed, decided by scanning `/proc/self/maps` (below) |
| `driver: probe passed (hook path), using <dir>` | the hook loaded the pack and the probe passed |

The verdict line is the one that says whose fault a rejection is:

- **"the pack is loaded in the process but reported no GPU"** — the hook ran,
  the pack linked, and it still returned zero adapters: the pack cannot drive
  this GPU. Nine times out of ten this is a DRM-only build (see above); the fix
  is a KGSL/adrenotools Turnip pack for the phone's GPU family.
- **"a system ICD was mapped instead of the pack"** — the hook did not take
  effect and the loader picked the vendor driver; logcat tag `hook_impl` names
  the `dlerror`.
- **"the hook's private loader is mapped but no ICD loaded"** — the machinery
  ran, but the ICD open failed inside the hook namespace; logcat tag
  `hook_impl` has the reason. The direct-load fallback below runs right after.
- **"no driver library was mapped at all"** — the hook never ran; that is an
  integration problem, and logcat tag `hook_impl` has the reason.

Every verdict line ends with raw flags (`maps: pack=N vendor-icd=N
hook-loader=N`) so a report stays interpretable even when no category matches.

## Reporting a problem with a pack

Attach `port/log.txt` (the `driver:` lines above) and, if the verdict points at
the hook, a logcat capture filtered to `hook_impl` while starting the game. The
DXVK lines in the same log show the driver that actually ended up rendering.
