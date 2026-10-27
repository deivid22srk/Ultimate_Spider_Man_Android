#include "../wsi_presenter.h"

#include "../../util/util_string.h"
#include "../../util/log/log.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <wsi/native_wsi.h>

namespace dxvk::wsi {

  VkResult createSurface(
          HWND                hWindow,
    const Rc<vk::InstanceFn>& vki,
          VkSurfaceKHR*       pSurface) {
    SDL_Window* window = fromHwnd(hWindow);

    if (!SDL_Vulkan_CreateSurface(window, vki->instance(), pSurface)) {
      /* Port: returning VK_ERROR_OUT_OF_HOST_MEMORY here used to swallow the
       * reason (loader/instance mismatch, no VK_KHR_android_surface, no native
       * window, ...). Log it, it is the only clue a report will carry. */
      Logger::err(str::format("SDL_Vulkan_CreateSurface: ", SDL_GetError()));
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    return VK_SUCCESS;
  }

}
