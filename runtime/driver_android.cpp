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
 * Loading the driver is only half of a correct integration. Every import log
 * so far shows hook_impl picking the custom libvulkan.so up just fine and the
 * process still dying moments later, somewhere between driver code and guest
 * code. Handing an unvalidated driver to the game means a bad pack closes the
 * app instead of falling back, and the report carries no clue. So before the
 * driver is used for anything, it is probed end to end through the very same
 * vkGetInstanceProcAddr DXVK will use:
 *
 *   1. the file is a 64-bit ARM ELF (packs for another ABI previously failed
 *      deep inside linkernsbypass with nothing usable in the log),
 *   2. instance extension enumeration (the step that makes the system loader
 *      actually load the driver through the adrenotools hook),
 *   3. a throwaway VkInstance with the same WSI extensions DXVK asks for,
 *   4. physical device enumeration + properties + memory heaps (the adapter
 *      identity lands in the log, so a report says which driver it ran on),
 *   5. a VkDevice, and a host-visible allocation mapped, written and unmapped
 *      (exercises the KGSL memory path, the one that differs most between
 *      Qualcomm's blob and Turnip).
 *
 * The whole probe runs under a SIGSEGV/SIGBUS/SIGILL guard: a driver that
 * crashes during validation longjmps back out, the port's own crash handler
 * goes back in place, and the app falls back to the system driver with the
 * reason in the log. With no driver selected none of this runs and everything
 * stays on the system driver, which is the default.
 */
#include "rt.h"

#ifdef __ANDROID__

#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vulkan/vulkan.h>

extern "C" void dxvk_vulkan_set_get_instance_proc_addr(void *pfn);
extern "C" void SDL_Vulkan_SetExternalLoader(void *handle, void *gipa);

namespace {

/* ---- crash guard -------------------------------------------------------- */

sigjmp_buf g_probe_jmp;
volatile sig_atomic_t g_probe_armed = 0;
struct sigaction g_prev_sa[3];                      /* SEGV, BUS, ILL */
const int g_probe_sigs[3] = {SIGSEGV, SIGBUS, SIGILL};

void probe_signal_handler(int sig, siginfo_t *info, void *ucv) {
    if (g_probe_armed) {
        g_probe_armed = 0;
        siglongjmp(g_probe_jmp, 0x1000 + sig);
        return;                                     /* unreachable */
    }
    /* Not during the probe: hand the fault back to whoever owned the signal
     * (usually the port's crash reporter, installed from main()). */
    struct sigaction *prev = nullptr;
    for (int i = 0; i < 3; i++)
        if (g_probe_sigs[i] == sig) prev = &g_prev_sa[i];
    if (!prev) return;
    if (prev->sa_flags & SA_SIGINFO) prev->sa_sigaction(sig, info, ucv);
    else if (prev->sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else if (prev->sa_handler != SIG_IGN) prev->sa_handler(sig);
}

void probe_guard_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = probe_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < 3; i++)
        sigaction(g_probe_sigs[i], &sa, &g_prev_sa[i]);
}

void probe_guard_remove(void) {
    for (int i = 0; i < 3; i++)
        sigaction(g_probe_sigs[i], &g_prev_sa[i], NULL);
}

/* ---- probe -------------------------------------------------------------- */

#define PROBE_PFN(inst, name) reinterpret_cast<PFN_##name>(gipa(inst, #name))

/* Runs every step with the guard armed. Returns a static string naming the
 * first failed step, or nullptr when the driver passed. `detail` receives a
 * short extra note for the log. */
const char *probe_driver(void *gipa_, std::string &detail) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa_);

    if (sigsetjmp(g_probe_jmp, 1) != 0) {
        g_probe_armed = 0;
        detail = "the driver crashed during the probe";
        return "driver crash";
    }
    g_probe_armed = 1;

    /* 1. instance extension enumeration: this is the call that makes the
     *    isolated system loader scan ICDs and run the adrenotools hook, so it
     *    is also where a driver that cannot even load shows itself. */
    auto enumInstanceExts = PROBE_PFN(nullptr, vkEnumerateInstanceExtensionProperties);
    if (!enumInstanceExts) {
        g_probe_armed = 0;
        detail = "vkEnumerateInstanceExtensionProperties missing";
        return "loader symbol";
    }
    uint32_t next = 0;
    VkResult vr = enumInstanceExts(nullptr, &next, nullptr);
    if (vr != VK_SUCCESS || next == 0) {
        g_probe_armed = 0;
        detail = std::string("VkResult ") + std::to_string((int)vr);
        return "extension enumeration";
    }
    std::vector<VkExtensionProperties> iexts(next);
    vr = enumInstanceExts(nullptr, &next, iexts.data());
    bool has_surface = false, has_android = false;
    for (const auto &e : iexts) {
        if (!strcmp(e.extensionName, "VK_KHR_surface")) has_surface = true;
        if (!strcmp(e.extensionName, "VK_KHR_android_surface")) has_android = true;
    }
    if (!has_surface || !has_android) {
        g_probe_armed = 0;
        detail = std::string("surface ") + (has_surface ? "ok" : "missing") + ", android " +
                 (has_android ? "ok" : "missing");
        return "WSI extensions";
    }

    /* 2. throwaway instance, same shape as the one DXVK creates. */
    auto createInstance = PROBE_PFN(nullptr, vkCreateInstance);
    auto destroyInstance = PROBE_PFN(nullptr, vkDestroyInstance);
    if (!createInstance || !destroyInstance) {
        g_probe_armed = 0;
        detail = "vkCreateInstance/vkDestroyInstance missing";
        return "loader symbol";
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "usm-driver-probe";
    app.apiVersion = VK_API_VERSION_1_1;
    const char *wantExts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = wantExts;
    VkInstance inst = VK_NULL_HANDLE;
    vr = createInstance(&ici, nullptr, &inst);
    if (vr != VK_SUCCESS || !inst) {
        g_probe_armed = 0;
        detail = std::string("VkResult ") + std::to_string((int)vr);
        return "vkCreateInstance";
    }

    /* 3. physical devices: identity + memory heaps go to the log. */
    auto enumPDs = PROBE_PFN(inst, vkEnumeratePhysicalDevices);
    auto pdProps = PROBE_PFN(inst, vkGetPhysicalDeviceProperties);
    auto pdMem = PROBE_PFN(inst, vkGetPhysicalDeviceMemoryProperties);
    auto pdQueues = PROBE_PFN(inst, vkGetPhysicalDeviceQueueFamilyProperties);
    auto createDevice = PROBE_PFN(inst, vkCreateDevice);
    auto destroyDevice = PROBE_PFN(inst, vkDestroyDevice);
    const char *step = "loader symbol";
    VkDevice dev = VK_NULL_HANDLE;
    do {
        if (!enumPDs || !pdProps || !pdMem || !pdQueues || !createDevice || !destroyDevice) {
            detail = "instance symbols missing";
            break;
        }
        uint32_t npd = 0;
        vr = enumPDs(inst, &npd, nullptr);
        if (vr != VK_SUCCESS || npd == 0) {
            detail = std::string("VkResult ") + std::to_string((int)vr) + ", " +
                     std::to_string(npd) + " devices";
            step = "physical device enumeration";
            break;
        }
        std::vector<VkPhysicalDevice> pds(npd);
        vr = enumPDs(inst, &npd, pds.data());
        if (vr != VK_SUCCESS || npd == 0) {
            detail = "second enumeration failed";
            step = "physical device enumeration";
            break;
        }
        VkPhysicalDeviceProperties props{};
        pdProps(pds[0], &props);
        char dv[64];
        snprintf(dv, sizeof dv, "%u.%u.%u",
                 (unsigned)VK_API_VERSION_MAJOR(props.driverVersion),
                 (unsigned)VK_API_VERSION_MINOR(props.driverVersion),
                 (unsigned)VK_API_VERSION_PATCH(props.driverVersion));
        LOG("driver: probe %s, vendor %04x device %04x, driver %s, api %u.%u.%u",
            props.deviceName, props.vendorID, props.deviceID, dv,
            (unsigned)VK_API_VERSION_MAJOR(props.apiVersion),
            (unsigned)VK_API_VERSION_MINOR(props.apiVersion),
            (unsigned)VK_API_VERSION_PATCH(props.apiVersion));
        VkPhysicalDeviceMemoryProperties mem{};
        pdMem(pds[0], &mem);
        for (uint32_t h = 0; h < mem.memoryHeapCount; h++)
            LOG("driver: probe heap %u: %llu MiB", h,
                (unsigned long long)(mem.memoryHeaps[h].size >> 20));

        /* 4. a device, then the KGSL-sensitive path: host-visible memory
         *    mapped and touched. */
        uint32_t qfam = 0xFFFFFFFFu;
        {   uint32_t nq = 0;
            pdQueues(pds[0], &nq, nullptr);
            if (nq) {
                std::vector<VkQueueFamilyProperties> qs(nq);
                pdQueues(pds[0], &nq, qs.data());
                for (uint32_t q = 0; q < nq; q++)
                    if (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = q; break; }
            }
        }
        if (qfam == 0xFFFFFFFFu) { detail = "no graphics queue"; step = "queue enumeration"; break; }

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        vr = createDevice(pds[0], &dci, nullptr, &dev);
        if (vr != VK_SUCCESS || !dev) {
            detail = std::string("VkResult ") + std::to_string((int)vr);
            step = "vkCreateDevice";
            break;
        }

        int32_t hvt = -1;
        for (uint32_t t = 0; t < mem.memoryTypeCount; t++) {
            VkMemoryType mt = mem.memoryTypes[t];
            if ((mt.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (mt.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { hvt = (int32_t)t; break; }
        }
        if (hvt < 0) { detail = "no host visible memory type"; step = "memory type"; break; }

        auto allocMem = PROBE_PFN(inst, vkAllocateMemory);
        auto freeMem = PROBE_PFN(inst, vkFreeMemory);
        auto mapMem = PROBE_PFN(inst, vkMapMemory);
        auto unmapMem = PROBE_PFN(inst, vkUnmapMemory);
        if (!allocMem || !freeMem || !mapMem || !unmapMem) { detail = "memory symbols missing"; step = "loader symbol"; break; }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = 1u << 20;
        mai.memoryTypeIndex = (uint32_t)hvt;
        VkDeviceMemory vm = VK_NULL_HANDLE;
        vr = allocMem(dev, &mai, nullptr, &vm);
        if (vr != VK_SUCCESS || !vm) {
            detail = std::string("VkResult ") + std::to_string((int)vr);
            step = "vkAllocateMemory";
            break;
        }
        void *host = nullptr;
        vr = mapMem(dev, vm, 0, mai.allocationSize, 0, &host);
        if (vr != VK_SUCCESS || !host) {
            detail = std::string("VkResult ") + std::to_string((int)vr);
            step = "vkMapMemory";
            freeMem(dev, vm, nullptr);
            break;
        }
        memset(host, 0xA5, 4096);                   /* write: coherency + real backing */
        unmapMem(dev, vm);
        freeMem(dev, vm, nullptr);
        step = nullptr;                             /* everything passed */
    } while (0);

    if (dev) destroyDevice(dev, nullptr);
    destroyInstance(inst, nullptr);
    g_probe_armed = 0;
    return step;
}

/* Reads just enough of the ELF header to reject packs for another ABI: those
 * used to fail deep inside linkernsbypass with nothing usable in the log. */
bool elf_is_aarch64(const char *path, std::string &why) {
    unsigned char h[20];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { why = "cannot open"; return false; }
    ssize_t n = read(fd, h, sizeof h);
    close(fd);
    if (n != (ssize_t)sizeof h) { why = "too short"; return false; }
    if (memcmp(h, "\x7f""ELF", 4) != 0) { why = "not an ELF"; return false; }
    if (h[4] != 2) { why = "not 64-bit"; return false; }
    uint16_t machine = (uint16_t)(h[18] | (h[19] << 8));
    if (machine != 183 /* EM_AARCH64 */) { why = "not arm64"; return false; }
    return true;
}

} /* namespace */

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

    std::string why;
    if (!elf_is_aarch64(lib.c_str(), why)) {
        LOG("driver: %s is %s, keeping the system driver (is the pack for arm64?)",
            lib.c_str(), why.c_str());
        return 0;
    }

    void *handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                              nullptr, native_lib_dir, dir.c_str(),
                                              "libvulkan.so", nullptr, nullptr);
    if (!handle) {
        /* Missing hook libraries, a namespace the kernel refuses, or a driver
         * built for another ABI all land here. */
        const char *err = dlerror();
        LOG("driver: libadrenotools could not open %s%s%s, keeping the system driver",
            lib.c_str(), err ? ": " : "", err ? err : "");
        return 0;
    }

    void *gipa = dlsym(handle, "vkGetInstanceProcAddr");
    if (!gipa) {
        LOG("driver: %s has no vkGetInstanceProcAddr, keeping the system driver", lib.c_str());
        return 0;
    }
    LOG("driver: %s opened, probing before use", lib.c_str());

    /* Validate before anything else touches the driver. A failure here used to
     * be a mid-init SIGSEGV with the game half started; now it is a fallback
     * with the failing step in the log. */
    probe_guard_install();
    std::string detail;
    const char *failed = probe_driver(gipa, detail);
    probe_guard_remove();

    if (failed) {
        LOG("driver: probe failed at %s (%s), keeping the system driver", failed, detail.c_str());
        return 0;
    }
    LOG("driver: probe passed, using %s", dir.c_str());

    *out_handle = handle;
    *out_gipa = gipa;
    return 1;
}

#else

extern "C" int usm_driver_open(const char *, const char *, void **out_handle, void **out_gipa) {
    *out_handle = nullptr;
    *out_gipa = nullptr;
    return 0;
}

#endif
