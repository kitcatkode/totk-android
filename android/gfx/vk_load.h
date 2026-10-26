/* Vulkan driver loading for the NVN -> Vulkan backend and android/vkprobe: the system driver through the loader, or a
   driver file picked by the player (TOTK_VK_DRIVER=/path/to/driver.so, e.g. a Turnip build), loaded directly.

   A picked file may be any of the shapes drivers are shipped in:
     - a desktop / Mesa ICD               (vk_icdGetInstanceProcAddr, after vk_icdNegotiateLoaderICDInterfaceVersion)
     - a library exporting the API itself (vkGetInstanceProcAddr)
     - an Android Vulkan HAL module       (symbol HMI, a hwvulkan_module_t; what Android Turnip builds and the driver
                                           packs emulators use are: the device it opens holds GetInstanceProcAddr)
   Without the loader there are no layers, and every call goes straight to the driver. */
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* path NULL: the system driver. Returns vkGetInstanceProcAddr, or NULL with *why set. *kind names what was found. */
PFN_vkGetInstanceProcAddr vk_load_driver(const char *path, const char **kind, const char **why);
