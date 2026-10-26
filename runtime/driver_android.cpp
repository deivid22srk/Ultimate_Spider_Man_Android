/* Custom GPU driver support (libadrenotools, BSD-2-Clause, by Billy Laws).
 *
 * The app links against the system Vulkan loader, so a Turnip (or any other)
 * driver cannot simply be dropped in: the loader only ever loads Qualcomm's
 * vendor driver. libadrenotools opens the system loader inside an isolated
 * linker namespace with a hook in place that makes the loader pick up the
 * user's libvulkan.so instead, and hands back a handle to that patched loader.
 *
 * From there two things have to be told to use it:
 *   - DXVK, which resolves Vulkan through one vkGetInstanceProcAddr, and
 *   - SDL, which would otherwise dlopen a second, stock loader just to create
 *     the Android surface -- a surface that then would not belong to the
 *     instance DXVK created.
 *
 * With no driver selected none of this runs and everything stays on the
 * system driver, which is the default.
 */
#include "rt.h"

#ifdef __ANDROID__

#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <string>
#include <unistd.h>

extern "C" void dxvk_vulkan_set_get_instance_proc_addr(void *pfn);
extern "C" void SDL_Vulkan_SetExternalLoader(void *handle, void *gipa);

/* Opens the loader with `driver_dir` swapped in.
 * Returns 1 and fills the handle + vkGetInstanceProcAddr on success; 0 means
 * "stay on the system driver", which is always a safe fallback. */
extern "C" int usm_driver_open(const char *driver_dir, const char *native_lib_dir,
                               void **out_handle, void **out_gipa) {
    *out_handle = nullptr;
    *out_gipa = nullptr;

    if (!driver_dir || !driver_dir[0]) {
        LOG("driver: system");
        return 0;
    }
    if (!native_lib_dir || !native_lib_dir[0]) {
        LOG("driver: no native lib dir, keeping the system driver");
        return 0;
    }

    /* libadrenotools stats customDriverDir + customDriverName, so the trailing
     * slash is not optional. */
    std::string dir(driver_dir);
    if (dir.back() != '/') dir += '/';
    std::string lib = dir + "libvulkan.so";

    if (access(lib.c_str(), R_OK) != 0) {
        LOG("driver: %s is not readable, keeping the system driver", lib.c_str());
        return 0;
    }

    void *handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                              nullptr, native_lib_dir, dir.c_str(),
                                              "libvulkan.so", nullptr, nullptr);
    if (!handle) {
        /* Missing hook libraries, a namespace the kernel refuses, or a driver
         * built for another ABI all land here. */
        LOG("driver: libadrenotools could not open %s, keeping the system driver", lib.c_str());
        return 0;
    }

    void *gipa = dlsym(handle, "vkGetInstanceProcAddr");
    if (!gipa) {
        LOG("driver: %s has no vkGetInstanceProcAddr, keeping the system driver", lib.c_str());
        return 0;
    }

    *out_handle = handle;
    *out_gipa = gipa;
    LOG("driver: loaded %s", lib.c_str());
    return 1;
}

#else

extern "C" int usm_driver_open(const char *, const char *, void **out_handle, void **out_gipa) {
    *out_handle = nullptr;
    *out_gipa = nullptr;
    return 0;
}

#endif
