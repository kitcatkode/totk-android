/* What the device's Vulkan driver offers the NVN -> Vulkan backend (android/README.md, "Vulkan backend").

     vkprobe                          the system driver (libvulkan.so through the Android loader)
     vkprobe /path/libvulkan_freedreno.so   a Mesa driver loaded directly (Turnip), no loader involved

   Prints the device, the extensions and features the backend uses (each marked NEED / WANT), the format support it
   depends on, the memory heaps, and whether a host allocation can be imported as device memory (VK_EXT_external_memory_host:
   the guest's memory pools stay zero-copy). Exit status 0 when every NEED item is there. */
#include "../gfx/vk_load.c"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
static int missing_need;

static void item(const char *kind, const char *what, int have) {
    printf("  %-4s %-3s %s\n", kind, have ? "yes" : "NO", what);
    if (!have && !strcmp(kind, "NEED")) missing_need++;
}

static VkExtensionProperties *exts; static uint32_t next;
static int has_ext(const char *n) { for (uint32_t i = 0; i < next; i++) if (!strcmp(exts[i].extensionName, n)) return 1; return 0; }

/* ---------- guest memory pools on the GPU ----------
   Guest memory is shared anonymous memory at fixed addresses. The GPU must see a pool where the game wrote it, and the
   game must see what the GPU wrote, with no copies. Three ways, tried in turn; each is checked by a GPU round trip
   (CPU writes, GPU fills and copies, CPU reads back through the guest-style address):
     A  import the guest's own pages               (VK_EXT_external_memory_host)
     B  device memory exported as a file           (VK_KHR_external_memory_fd), mmap'd over the guest's address range
     C  a dma-buf from /dev/dma_heap/system        (VK_EXT_external_memory_dma_buf), imported, mmap'd the same way
   B and C also map a second view of the same pages at another address (virtual pools alias pages this way). */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

static VkDevice dev; static VkQueue queue; static VkCommandPool cpool;
static PFN_vkGetDeviceProcAddr gdpa;
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
static const VkPhysicalDeviceMemoryProperties *g_mp;

static int pick_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mp->memoryTypeCount; i++) if ((bits >> i & 1) && (g_mp->memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
/* CPU writes a pattern at cpu[0..4K); GPU fills [1M, 1M+4K) with a word and copies [0, 4K) to [2M, 2M+4K); CPU checks. */
static const char *round_trip(VkDeviceMemory mem, VkBuffer buf, volatile uint32_t *cpu) {
    DFN(vkBindBufferMemory); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdFillBuffer); DFN(vkCmdCopyBuffer);
    DFN(vkEndCommandBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle); DFN(vkFreeCommandBuffers); DFN(vkCmdPipelineBarrier);
    if (mem && vkBindBufferMemory(dev, buf, mem, 0)) return "vkBindBufferMemory failed";
    for (uint32_t i = 0; i < 1024; i++) cpu[i] = 0x5eed0000u + i;
    for (uint32_t i = 0; i < 1024; i++) cpu[(1u << 18) + i] = cpu[(2u << 18) + i] = 0;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(dev, &cai, &cb)) return "vkAllocateCommandBuffers failed";
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    vkCmdFillBuffer(cb, buf, 1u << 20, 4096, 0xabcd1234u);
    VkBufferCopy c = {0, 2u << 20, 4096}; vkCmdCopyBuffer(cb, buf, buf, 1, &c);
    VkMemoryBarrier tb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &tb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)) return "vkQueueSubmit failed";
    if (vkQueueWaitIdle(queue)) return "vkQueueWaitIdle failed (device lost?)";
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
    for (uint32_t i = 0; i < 1024; i++) if (cpu[(1u << 18) + i] != 0xabcd1234u) return "CPU does not see the GPU fill";
    for (uint32_t i = 0; i < 1024; i++) if (cpu[(2u << 18) + i] != 0x5eed0000u + i) return "GPU did not read what the CPU wrote";
    return NULL;
}
static VkBuffer make_buffer(VkDeviceSize sz, VkExternalMemoryHandleTypeFlagBits ht, VkMemoryRequirements *req) {
    DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements);
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = ht};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, ht ? &eb : NULL, 0, sz,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer b; if (vkCreateBuffer(dev, &bci, NULL, &b)) return VK_NULL_HANDLE;
    vkGetBufferMemoryRequirements(dev, b, req);
    return b;
}
/* A reserved range like the guest's, with fd mapped over its start; and a second view of fd at offset 1M elsewhere. */
static const char *map_fixed(int fd, size_t sz, void **at, void **alias) {
    void *res = mmap(NULL, 2 * sz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) return "reserve failed";
    void *p = mmap(res, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (p == MAP_FAILED) { static char e[96]; snprintf(e, sizeof e, "mmap of the exported memory at a fixed address: %s", strerror(errno)); return e; }
    void *q = mmap((char *)res + sz, sz - (1u << 20), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 1u << 20);
    if (q == MAP_FAILED) { static char e[96]; snprintf(e, sizeof e, "second view at offset 1M: %s", strerror(errno)); return e; }
    *at = p; *alias = q; return NULL;
}
static const char *check_alias(volatile uint32_t *alias) {
    for (uint32_t i = 0; i < 1024; i++) if (alias[i] != 0xabcd1234u) return "the second view does not show the same pages";
    return NULL;
}

static const char *path_a(size_t sz) {
    DFN(vkGetMemoryHostPointerPropertiesEXT); DFN(vkAllocateMemory);
    void *g = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, g, &hp) || !hp.memoryTypeBits) return "the driver will not import shared anonymous pages";
    VkMemoryRequirements req; VkBuffer b = make_buffer(sz, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, &req);
    if (!b) return "vkCreateBuffer failed";
    int t = pick_type(hp.memoryTypeBits & req.memoryTypeBits, 0); if (t < 0) return "no memory type fits both";
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = g};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, sz, (uint32_t)t};
    VkDeviceMemory m; VkResult r = vkAllocateMemory(dev, &mai, NULL, &m);
    if (r) { static char e[64]; snprintf(e, sizeof e, "vkAllocateMemory import: %d", r); return e; }
    return round_trip(m, b, g);
}
static const char *path_b(size_t sz) {
    DFN(vkAllocateMemory); DFN(vkGetMemoryFdKHR);
    PFN_vkGetPhysicalDeviceExternalBufferProperties eprops = (PFN_vkGetPhysicalDeviceExternalBufferProperties)gipa(inst, "vkGetPhysicalDeviceExternalBufferProperties");
    extern VkPhysicalDevice g_pd;
    static const struct { VkExternalMemoryHandleTypeFlagBits t; const char *n; } hts[] = {
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "dma-buf"}, {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "opaque fd"}};
    static char why[512]; why[0] = 0;
    for (int h = 0; h < 2; h++) {
        VkPhysicalDeviceExternalBufferInfo q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .handleType = hts[h].t};
        VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        eprops(g_pd, &q, &ep);
        VkExternalMemoryFeatureFlags ef = ep.externalMemoryProperties.externalMemoryFeatures;
        printf("    %-9s export %s%s\n", hts[h].n, ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? "yes" : "no", ef & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ? " (dedicated only)" : "");
        if (!(ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) continue;
        VkMemoryRequirements req; VkBuffer b = make_buffer(sz, hts[h].t, &req);
        if (!b) { snprintf(why + strlen(why), 100, "[%s] vkCreateBuffer failed; ", hts[h].n); continue; }
        int t = pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (t < 0) t = pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (t < 0) t = pick_type(req.memoryTypeBits, 0);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = b};
        VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, ef & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ? &ded : NULL, hts[h].t};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, req.size, (uint32_t)t};
        VkDeviceMemory m; VkResult r = vkAllocateMemory(dev, &mai, NULL, &m);
        if (r) { snprintf(why + strlen(why), 100, "[%s] vkAllocateMemory export: %d; ", hts[h].n, r); continue; }
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = m, .handleType = hts[h].t};
        int fd = -1; r = vkGetMemoryFdKHR(dev, &gi, &fd);
        if (r || fd < 0) { snprintf(why + strlen(why), 100, "[%s] vkGetMemoryFdKHR: %d; ", hts[h].n, r); continue; }
        printf("    %-9s memory type %d, fd %d\n", hts[h].n, t, fd);
        void *at, *alias; const char *e = map_fixed(fd, sz, &at, &alias);
        if (!e) e = round_trip(m, b, at);
        if (!e) e = check_alias(alias);
        if (!e) { printf("    %-9s works (fixed-address mapping, GPU round trip, second view)\n", hts[h].n); return NULL; }
        snprintf(why + strlen(why), 200, "[%s] %s; ", hts[h].n, e);
    }
    return why[0] ? why : "no exportable handle type";
}
struct dma_heap_alloc { uint64_t len; uint32_t fd, fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_alloc)
static const char *path_c(size_t sz) {
    DFN(vkAllocateMemory); DFN(vkGetMemoryFdPropertiesKHR);
    static const char *heaps[] = {"/dev/dma_heap/system", "/dev/dma_heap/qcom,system", "/dev/dma_heap/system-uncached"};
    int hfd = -1; const char *hn = NULL;
    for (unsigned i = 0; i < 3 && hfd < 0; i++) { hfd = open(heaps[i], O_RDONLY | O_CLOEXEC); hn = heaps[i]; }
    if (hfd < 0) { static char e[96]; snprintf(e, sizeof e, "no dma-buf heap can be opened (%s)", strerror(errno)); return e; }
    struct dma_heap_alloc a = {.len = sz, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a)) { static char e[96]; snprintf(e, sizeof e, "%s allocation: %s", hn, strerror(errno)); return e; }
    printf("    heap %s\n", hn);
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, (int)a.fd, &fp);
    if (r || !fp.memoryTypeBits) { static char e[64]; snprintf(e, sizeof e, "vkGetMemoryFdPropertiesKHR: %d", r); return e; }
    VkMemoryRequirements req; VkBuffer b = make_buffer(sz, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &req);
    if (!b) return "vkCreateBuffer failed";
    int t = pick_type(fp.memoryTypeBits & req.memoryTypeBits, 0); if (t < 0) return "no memory type fits both";
    void *at, *alias; const char *e = map_fixed((int)a.fd, sz, &at, &alias); if (e) return e;
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup((int)a.fd)};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, sz, (uint32_t)t};
    VkDeviceMemory m; r = vkAllocateMemory(dev, &mai, NULL, &m);
    if (r) { static char e[64]; snprintf(e, sizeof e, "vkAllocateMemory import: %d", r); return e; }
    e = round_trip(m, b, at); if (!e) e = check_alias(alias);
    return e;
}

VkPhysicalDevice g_pd;
static void pool_tests(VkPhysicalDevice pd, PFN_vkCreateDevice vkCreateDevice, PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr, const VkPhysicalDeviceMemoryProperties *mp) {
    g_pd = pd; g_mp = mp; gdpa = vkGetDeviceProcAddr;
    printf("\nguest memory pools on the GPU (zero-copy)\n");
    const char *want[] = {"VK_EXT_external_memory_host", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_KHR_dedicated_allocation"};
    const char *on[4]; uint32_t non = 0;
    for (int i = 0; i < 4; i++) if (has_ext(want[i])) on[non++] = want[i];
    float prio = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = non, .ppEnabledExtensionNames = on};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) { printf("  vkCreateDevice: %d\n", r); missing_need++; return; }
    DFN(vkGetDeviceQueue); DFN(vkCreateCommandPool);
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    size_t sz = 16u << 20;
    const char *ra = has_ext(want[0]) ? path_a(sz) : "extension missing";
    printf("  A host-pointer import:   %s\n", ra ? ra : "works");
    printf("  B export + fixed mmap:\n");
    const char *rb = has_ext(want[1]) ? path_b(sz) : "VK_KHR_external_memory_fd missing";
    if (rb) printf("    FAILED: %s\n", rb);
    printf("  C dma-buf heap import:\n");
    const char *rc = has_ext(want[2]) ? path_c(sz) : "VK_EXT_external_memory_dma_buf missing";
    printf("    %s\n", rc ? rc : "works (fixed-address mapping, GPU round trip, second view)");
    int any = !ra || !rb || !rc;
    printf("  zero-copy pools: %s\n", any ? (!ra ? "path A" : !rb ? "path B" : "path C") : "NO PATH WORKS");
    if (!any) missing_need++;
}

int main(int argc, char **argv) {
    const char *kind, *why;
    gipa = vk_load_driver(argc > 1 ? argv[1] : NULL, &kind, &why);
    if (!gipa) { fprintf(stderr, "driver %s: %s\n", argc > 1 ? argv[1] : "(system)", why); return 2; }
    printf("loaded   %s (%s)\n", argc > 1 ? argv[1] : "system driver", kind);

    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "totk-vkprobe", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r) { fprintf(stderr, "vkCreateInstance: %d\n", r); return 2; }
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties2); IFN(vkGetPhysicalDeviceFeatures2);
    IFN(vkEnumerateDeviceExtensionProperties); IFN(vkGetPhysicalDeviceFormatProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
    IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr);

    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (!n) { fprintf(stderr, "no Vulkan device\n"); return 2; }
    VkPhysicalDevice pds[8]; if (n > 8) n = 8; vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];

    vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
    exts = calloc(next, sizeof *exts); vkEnumerateDeviceExtensionProperties(pd, NULL, &next, exts);

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &hostp};
    VkPhysicalDeviceSubgroupProperties sub = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, &drv};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sub};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    VkPhysicalDeviceProperties *p = &p2.properties;
    printf("device   %s (vendor %#x, device %#x)\n", p->deviceName, p->vendorID, p->deviceID);
    printf("driver   %s %s, Vulkan %u.%u.%u\n", drv.driverName, drv.driverInfo,
           VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion));
    printf("subgroup size %u\n", sub.subgroupSize);

    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT rob2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &cbc};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, &rob2};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, &eds3};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, &eds2};
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &xfb};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &dcc};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, &u8};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &div};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceFeatures *f = &f2.features;
    int v13 = p->apiVersion >= VK_API_VERSION_1_3;

    printf("\nextensions and features\n");
    item("NEED", "Vulkan 1.3", v13);
    item("WANT", "VK_EXT_external_memory_host (pool path A)", has_ext("VK_EXT_external_memory_host"));
    if (has_ext("VK_EXT_external_memory_host")) printf("            minImportedHostPointerAlignment %#llx\n", (unsigned long long)hostp.minImportedHostPointerAlignment);
    item("NEED", "dynamicRendering", f13.dynamicRendering);
    item("NEED", "synchronization2", f13.synchronization2);
    item("NEED", "timelineSemaphore", f12.timelineSemaphore);
    item("NEED", "bufferDeviceAddress", f12.bufferDeviceAddress);
    item("NEED", "independentBlend", f->independentBlend);
    item("NEED", "fragmentStoresAndAtomics", f->fragmentStoresAndAtomics);
    item("NEED", "shaderClipDistance", f->shaderClipDistance);
    item("NEED", "shaderStorageImageWriteWithoutFormat", f->shaderStorageImageWriteWithoutFormat);
    item("WANT", "shaderStorageImageReadWithoutFormat", f->shaderStorageImageReadWithoutFormat);
    item("WANT", "textureCompressionBC (else BC -> ASTC / RGBA on upload)", f->textureCompressionBC);
    item("WANT", "textureCompressionASTC_LDR", f->textureCompressionASTC_LDR);
    item("WANT", "geometryShader (layered render targets)", f->geometryShader);
    item("WANT", "shaderOutputLayer / shaderOutputViewportIndex", f12.shaderOutputLayer && f12.shaderOutputViewportIndex);
    item("WANT", "vertexPipelineStoresAndAtomics", f->vertexPipelineStoresAndAtomics);
    item("WANT", "multiViewport", f->multiViewport);
    item("WANT", "depthClamp", f->depthClamp);
    item("WANT", "depthBiasClamp", f->depthBiasClamp);
    item("WANT", "dualSrcBlend", f->dualSrcBlend);
    item("WANT", "logicOp", f->logicOp);
    item("WANT", "samplerAnisotropy", f->samplerAnisotropy);
    item("WANT", "shaderInt64", f->shaderInt64);
    item("WANT", "shaderInt16", f->shaderInt16);
    item("WANT", "shaderFloat64", f->shaderFloat64);
    item("WANT", "shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation);
    item("WANT", "drawIndirectCount", f12.drawIndirectCount);
    item("WANT", "multiDrawIndirect", f->multiDrawIndirect);
    item("WANT", "descriptorIndexing (update after bind, partially bound)", f12.descriptorBindingPartiallyBound && f12.descriptorBindingUpdateUnusedWhilePending);
    item("WANT", "VK_KHR_push_descriptor", has_ext("VK_KHR_push_descriptor"));
    item("WANT", "VK_EXT_robustness2 nullDescriptor", has_ext("VK_EXT_robustness2") && rob2.nullDescriptor);
    item("WANT", "VK_EXT_custom_border_color", has_ext("VK_EXT_custom_border_color") && cbc.customBorderColors);
    item("WANT", "VK_EXT_depth_clip_control (NVN -1..1 depth)", has_ext("VK_EXT_depth_clip_control") && dcc.depthClipControl);
    item("WANT", "VK_EXT_index_type_uint8", has_ext("VK_EXT_index_type_uint8") && u8.indexTypeUint8);
    item("WANT", "VK_EXT_vertex_attribute_divisor", has_ext("VK_EXT_vertex_attribute_divisor") && div.vertexAttributeInstanceRateDivisor);
    item("WANT", "VK_EXT_transform_feedback", has_ext("VK_EXT_transform_feedback") && xfb.transformFeedback);
    item("WANT", "VK_EXT_extended_dynamic_state2", has_ext("VK_EXT_extended_dynamic_state2") && eds2.extendedDynamicState2);
    item("WANT", "VK_EXT_extended_dynamic_state3", has_ext("VK_EXT_extended_dynamic_state3"));
    item("WANT", "VK_EXT_conditional_rendering", has_ext("VK_EXT_conditional_rendering"));
    item("WANT", "VK_EXT_provoking_vertex", has_ext("VK_EXT_provoking_vertex"));
    item("WANT", "VK_EXT_shader_stencil_export", has_ext("VK_EXT_shader_stencil_export"));
    item("WANT", "VK_EXT_shader_viewport_index_layer", has_ext("VK_EXT_shader_viewport_index_layer"));
    item("WANT", "VK_KHR_shader_float_controls", has_ext("VK_KHR_shader_float_controls") || v13);
    item("WANT", "VK_EXT_4444_formats", has_ext("VK_EXT_4444_formats") || v13);

    printf("\nlimits\n");
    printf("  maxUniformBufferRange %u, maxStorageBufferRange %u, maxPushConstantsSize %u\n", p->limits.maxUniformBufferRange, p->limits.maxStorageBufferRange, p->limits.maxPushConstantsSize);
    printf("  maxBoundDescriptorSets %u, per stage: samplers %u, sampled images %u, storage images %u, UBOs %u, SSBOs %u\n", p->limits.maxBoundDescriptorSets,
           p->limits.maxPerStageDescriptorSamplers, p->limits.maxPerStageDescriptorSampledImages, p->limits.maxPerStageDescriptorStorageImages,
           p->limits.maxPerStageDescriptorUniformBuffers, p->limits.maxPerStageDescriptorStorageBuffers);
    printf("  maxColorAttachments %u, maxViewports %u, maxImageDimension2D %u, maxImageArrayLayers %u, timestampPeriod %.3f ns\n",
           p->limits.maxColorAttachments, p->limits.maxViewports, p->limits.maxImageDimension2D, p->limits.maxImageArrayLayers, p->limits.timestampPeriod);
    printf("  minUniformBufferOffsetAlignment %llu, minStorageBufferOffsetAlignment %llu, minTexelBufferOffsetAlignment %llu\n",
           (unsigned long long)p->limits.minUniformBufferOffsetAlignment, (unsigned long long)p->limits.minStorageBufferOffsetAlignment, (unsigned long long)p->limits.minTexelBufferOffsetAlignment);

    printf("\nformats (S sampled, L linear filter, C color attachment, B blend, D depth, W storage)\n");
    static const struct { VkFormat f; const char *n; } fmts[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"}, {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"}, {VK_FORMAT_B8G8R8A8_UNORM, "BGRA8"},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "RGB10A2"}, {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "R11G11B10F"}, {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "RGB9E5"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"}, {VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32F"}, {VK_FORMAT_R16G16_SFLOAT, "RG16F"},
        {VK_FORMAT_R32_SFLOAT, "R32F"}, {VK_FORMAT_R32_UINT, "R32UI"}, {VK_FORMAT_R16_UNORM, "R16"}, {VK_FORMAT_R16G16B16A16_UNORM, "RGBA16"},
        {VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5"}, {VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1R5G5B5"}, {VK_FORMAT_R4G4B4A4_UNORM_PACK16, "RGBA4"},
        {VK_FORMAT_D16_UNORM, "D16"}, {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8"}, {VK_FORMAT_D32_SFLOAT, "D32F"}, {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32FS8"}, {VK_FORMAT_S8_UINT, "S8"},
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1"}, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1 sRGB"}, {VK_FORMAT_BC2_UNORM_BLOCK, "BC2"}, {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"},
        {VK_FORMAT_BC3_SRGB_BLOCK, "BC3 sRGB"}, {VK_FORMAT_BC4_UNORM_BLOCK, "BC4"}, {VK_FORMAT_BC5_UNORM_BLOCK, "BC5"}, {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H"},
        {VK_FORMAT_BC7_UNORM_BLOCK, "BC7"}, {VK_FORMAT_BC7_SRGB_BLOCK, "BC7 sRGB"}, {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC 4x4"}, {VK_FORMAT_ASTC_8x8_SRGB_BLOCK, "ASTC 8x8 sRGB"},
    };
    for (unsigned i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, fmts[i].f, &fp);
        VkFormatFeatureFlags o = fp.optimalTilingFeatures;
        printf("  %-14s %c%c%c%c%c%c\n", fmts[i].n, o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? 'S' : '-', o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? 'L' : '-',
               o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? 'C' : '-', o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT ? 'B' : '-',
               o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? 'D' : '-', o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? 'W' : '-');
    }

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    printf("\nmemory\n");
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) printf("  heap %u: %llu MiB%s\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device-local" : "");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags fl = mp.memoryTypes[i].propertyFlags;
        printf("  type %u heap %u:%s%s%s%s\n", i, mp.memoryTypes[i].heapIndex, fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
               fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "", fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : "", fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "");
    }

    pool_tests(pd, vkCreateDevice, vkGetDeviceProcAddr, &mp);
    printf("\n%s\n", missing_need ? "MISSING required features (see NO under NEED)" : "all required features present");
    return missing_need ? 1 : 0;
}
