/* The NVN replay interface (third_party/d2r_gfx/gfx/nvn/nvn_mtl.h) on Vulkan: Linux / Android. This file: the device,
   memory pools, submissions and their completion, and the command walker; textures, shaders and drawing are in
   vk_res.c, vk_shader.c and vk_draw.c (vk_int.h). android/VULKAN.md has the plan and the reasons.

   Every effect the game can observe of GPU completion (event values, counter reports, fences) happens only after the
   GPU has really finished the submission, as nvn_mtl.m does on Metal.

   Memory pools stay zero-copy: the game and the GPU use the same pages. Three ways, chosen at start
   (TOTK_VK_POOLS=host|export|heap forces one):
     host    import the guest's pages as device memory           (VK_EXT_external_memory_host; not on Adreno drivers)
     export  device memory exported as a dma-buf, mmap'd over the pool's guest address range (Turnip)
     heap    a dma-buf from /dev/dma_heap, imported, mmap'd the same way
   With export / heap, registering a pool moves its contents into the dma-buf and maps the dma-buf at the same address,
   so the game keeps its pointers; removing the pool moves the contents back into ordinary shared memory. Virtual pools
   (totk_vpool.c) alias physical pool pages with a second mmap of the dma-buf (mtl_pool_alias).

   The driver is the system one, or the file named by TOTK_VK_DRIVER (a Turnip build, android/gfx/vk_load.h). */
#include <malloc.h>
#include "vk_int.h"
#include "totk_timeline.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define VK_DEF(name) PFN_##name name;
VK_DEVICE_FUNCS(VK_DEF)
#undef VK_DEF
vk_dev V;
#define g_dev V.dev

/* ---------- device ---------- */
static PFN_vkGetInstanceProcAddr gipa;
PFN_vkGetInstanceProcAddr vk_gipa(void) { return gipa; }
static VkSemaphore g_timeline;
enum { POOLS_HOST = 1, POOLS_EXPORT, POOLS_HEAP };
static int g_pool_mode; static const char *g_pool_mode_name;
static VkDeviceSize g_host_align = 4096;
static PFN_vkGetPhysicalDeviceExternalBufferProperties vkGetPhysicalDeviceExternalBufferProperties;

static VkExtensionProperties *g_ext; static uint32_t g_next;
static int has_ext(const char *n) { for (uint32_t i = 0; i < g_next; i++) if (!strcmp(g_ext[i].extensionName, n)) return 1; return 0; }

int vk_pick_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < V.mp.memoryTypeCount; i++) if ((bits >> i & 1) && (V.mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static int pick_host_type(uint32_t bits) {   /* CPU reads guest memory constantly: cached + coherent first */
    int t = vk_pick_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = vk_pick_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(bits, 0);
    return t;
}

static const VkBufferUsageFlags POOL_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

static void *completion_thread(void *);
static int export_ok(VkExternalMemoryHandleTypeFlagBits ht, int *dedicated) {
    VkPhysicalDeviceExternalBufferInfo q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = POOL_USAGE, .handleType = ht};
    VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(V.pd, &q, &ep);
    VkExternalMemoryFeatureFlags f = ep.externalMemoryProperties.externalMemoryFeatures;
    if (dedicated) *dedicated = (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    return (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
}
static int g_export_dedicated;
static int heap_open(void);

/* Device features: everything the driver has, except robust buffer access (costly; NVN has none) -- the translated
   shaders use what Ryujinx's Vulkan output needs (storage image formats, clip distances, int64, ...). */
static VkPhysicalDeviceCustomBorderColorFeaturesEXT f_cbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
static VkPhysicalDeviceRobustness2FeaturesEXT f_rob2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
static VkPhysicalDeviceIndexTypeUint8FeaturesEXT f_u8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
static VkPhysicalDeviceDepthClipControlFeaturesEXT f_dcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
static VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT f_div = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
static VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
static VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
static VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
static VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

static VkFormat depth_choice(VkFormat want, VkFormat fallback) {
    VkFormatProperties fp; V.get_format_props(V.pd, want, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? want : fallback;
}

static void vk_init_once(void) {
    const char *kind, *why, *drv = getenv("TOTK_VK_DRIVER");
    if (drv && (!*drv || !strcmp(drv, "system"))) drv = NULL;
    gipa = vk_load_driver(drv, &kind, &why);
    if (!gipa) DIE("Vulkan driver %s: %s", drv ? drv : "(system)", why);
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    /* What the driver's per-app rules (driconf) see. Default "yuzu Emulator" for both, as Eden reports: Balemuni's Turnip
       applies its TOTK fixes only to emulators it recognises, and with this name the grey squares are gone on the Thor
       (user-confirmed 2026-10-23). TOTK_VK_APP_NAME / TOTK_VK_ENGINE_NAME override (e.g. =totk-native for the old name). */
    const char *an = getenv("TOTK_VK_APP_NAME"), *en = getenv("TOTK_VK_ENGINE_NAME");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = an && *an ? an : "yuzu Emulator", .applicationVersion = VK_MAKE_VERSION(1, 3, 0),
                             .pEngineName = en && *en ? en : "yuzu Emulator", .engineVersion = VK_MAKE_VERSION(1, 3, 0), .apiVersion = VK_API_VERSION_1_3};
    LOGV("Vulkan application name \"%s\", engine \"%s\" (TOTK_VK_APP_NAME / TOTK_VK_ENGINE_NAME)", app.pApplicationName, app.pEngineName ? app.pEngineName : "");
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VKCHECK(vkCreateInstance(&ici, NULL, &V.inst));
#define IL(name) PFN_##name name = (PFN_##name)gipa(V.inst, #name)
    IL(vkEnumeratePhysicalDevices); IL(vkCreateDevice); IL(vkGetDeviceProcAddr); IL(vkEnumerateDeviceExtensionProperties);
    IL(vkGetPhysicalDeviceProperties2); IL(vkGetPhysicalDeviceFeatures2); IL(vkGetPhysicalDeviceMemoryProperties);
    IL(vkGetPhysicalDeviceQueueFamilyProperties);
#undef IL
    vkGetPhysicalDeviceExternalBufferProperties = (PFN_vkGetPhysicalDeviceExternalBufferProperties)gipa(V.inst, "vkGetPhysicalDeviceExternalBufferProperties");
    V.get_format_props = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(V.inst, "vkGetPhysicalDeviceFormatProperties");
    V.get_format_props2 = (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(V.inst, "vkGetPhysicalDeviceFormatProperties2");
    V.get_image_format_props = (PFN_vkGetPhysicalDeviceImageFormatProperties)gipa(V.inst, "vkGetPhysicalDeviceImageFormatProperties");
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(V.inst, &n, &V.pd);
    if ((r && r != VK_INCOMPLETE) || !n) DIE("no Vulkan device");
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDevicePushDescriptorPropertiesKHR pushp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, &hostp};
    VkPhysicalDeviceDriverProperties drvp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &pushp};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drvp};
    vkGetPhysicalDeviceProperties2(V.pd, &p2);
    V.props = p2.properties; V.max_push = pushp.maxPushDescriptors;
    if (V.props.apiVersion < VK_API_VERSION_1_3) DIE("%s offers Vulkan %u.%u; 1.3 is needed", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion));
    LOGV("device %s, driver %s %s (%s%s%s)", V.props.deviceName, drvp.driverName, drvp.driverInfo, kind, drv ? " " : "", drv ? drv : "");
    LOGV("buffer limits: storage range %u, uniform range %u, storage offset alignment %llu", V.props.limits.maxStorageBufferRange, V.props.limits.maxUniformBufferRange,
         (unsigned long long)V.props.limits.minStorageBufferOffsetAlignment);
    vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mp);
    vkEnumerateDeviceExtensionProperties(V.pd, NULL, &g_next, NULL);
    g_ext = calloc(g_next, sizeof *g_ext); vkEnumerateDeviceExtensionProperties(V.pd, NULL, &g_next, g_ext);
    if (getenv("TOTK_VK_EXTLIST")) for (uint32_t i = 0; i < g_next; i++) LOGV("device extension %s", g_ext[i].extensionName);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, qf);
    V.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { V.qfam = i; break; }
    free(qf);
    if (V.qfam == UINT32_MAX) DIE("no graphics + compute queue");

    /* Pool path. */
    const char *want = getenv("TOTK_VK_POOLS");
    int can_host = has_ext("VK_EXT_external_memory_host");
    int can_export = has_ext("VK_KHR_external_memory_fd") && has_ext("VK_EXT_external_memory_dma_buf") && export_ok(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &g_export_dedicated);
    int can_heap = has_ext("VK_KHR_external_memory_fd") && has_ext("VK_EXT_external_memory_dma_buf") && heap_open() >= 0;
    if (want && !strcmp(want, "host")) { if (!can_host) DIE("TOTK_VK_POOLS=host: the driver has no VK_EXT_external_memory_host"); g_pool_mode = POOLS_HOST; }
    else if (want && !strcmp(want, "export")) { if (!can_export) DIE("TOTK_VK_POOLS=export: the driver cannot export dma-bufs"); g_pool_mode = POOLS_EXPORT; }
    else if (want && !strcmp(want, "heap")) { if (!can_heap) DIE("TOTK_VK_POOLS=heap: no dma-buf heap or no dma-buf import"); g_pool_mode = POOLS_HEAP; }
    else g_pool_mode = can_host ? POOLS_HOST : can_export ? POOLS_EXPORT : can_heap ? POOLS_HEAP : 0;
    if (!g_pool_mode) DIE("this driver cannot share memory with the game (no host import, dma-buf export or dma-buf heap import): try a Turnip driver with TOTK_VK_DRIVER");
    g_pool_mode_name = g_pool_mode == POOLS_HOST ? "host-pointer import" : g_pool_mode == POOLS_EXPORT ? "dma-buf export mapped at the guest address" : "dma-buf heap mapped at the guest address";
    if (g_pool_mode == POOLS_HOST) g_host_align = hostp.minImportedHostPointerAlignment ? hostp.minImportedHostPointerAlignment : 4096;

    /* Features: query the whole chain the backend knows, then enable what is there. */
    void *chain = NULL;
#define LINK(s, ext) if (!(ext) || has_ext(ext)) { (s).pNext = chain; chain = &(s); }
    LINK(f_cbc, "VK_EXT_custom_border_color") LINK(f_rob2, "VK_EXT_robustness2") LINK(f_u8, "VK_EXT_index_type_uint8")
    LINK(f_dcc, "VK_EXT_depth_clip_control") LINK(f_div, "VK_EXT_vertex_attribute_divisor")
    LINK(f13, NULL) LINK(f12, NULL) LINK(f11, NULL)
#undef LINK
    f2.pNext = chain;
    vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    if (!f12.timelineSemaphore || !f13.synchronization2 || !f13.dynamicRendering) DIE("the driver lacks timeline semaphores, synchronization2 or dynamic rendering");
    f2.features.robustBufferAccess = VK_FALSE; f_rob2.robustBufferAccess2 = VK_FALSE; f_rob2.robustImageAccess2 = VK_FALSE;
    f13.robustImageAccess = VK_FALSE; f13.pipelineCreationCacheControl = VK_FALSE;
    f12.bufferDeviceAddressCaptureReplay = VK_FALSE; f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    f_cbc.customBorderColorWithoutFormat = f_cbc.customBorderColorWithoutFormat && f_cbc.customBorderColors;
    V.has_custom_border = has_ext("VK_EXT_custom_border_color") && f_cbc.customBorderColors && f_cbc.customBorderColorWithoutFormat;
    V.has_null_descriptor = has_ext("VK_EXT_robustness2") && f_rob2.nullDescriptor;
    V.has_index_u8 = has_ext("VK_EXT_index_type_uint8") && f_u8.indexTypeUint8;
    V.has_depth_clip_control = has_ext("VK_EXT_depth_clip_control") && f_dcc.depthClipControl;
    V.has_divisor = has_ext("VK_EXT_vertex_attribute_divisor") && f_div.vertexAttributeInstanceRateDivisor;
    V.has_bc = f2.features.textureCompressionBC; V.has_geometry = f2.features.geometryShader;
    V.has_layer_output = f12.shaderOutputLayer; V.has_logic_op = f2.features.logicOp; V.has_dual_src = f2.features.dualSrcBlend;
    V.samples = V.props.limits.framebufferColorSampleCounts & V.props.limits.framebufferDepthSampleCounts;

    static const char *want_ext[] = {"VK_EXT_external_memory_host", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_KHR_external_semaphore_fd", "VK_KHR_push_descriptor",
        "VK_EXT_custom_border_color", "VK_EXT_robustness2", "VK_EXT_index_type_uint8", "VK_EXT_depth_clip_control",
        "VK_EXT_vertex_attribute_divisor", "VK_EXT_shader_stencil_export", "VK_EXT_shader_viewport_index_layer",
        "VK_KHR_shader_float_controls", "VK_EXT_provoking_vertex", "VK_EXT_primitive_topology_list_restart"};
    const char *exts[32]; uint32_t ne = 0;
    for (unsigned i = 0; i < sizeof want_ext / sizeof *want_ext; i++) {
        if (!has_ext(want_ext[i])) continue;
        if (!strcmp(want_ext[i], "VK_EXT_external_memory_host") && g_pool_mode != POOLS_HOST) continue;
        exts[ne++] = want_ext[i];
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = ne, .ppEnabledExtensionNames = exts};
    VKCHECK(vkCreateDevice(V.pd, &dci, NULL, &V.dev));
#define DL(name) name = (PFN_##name)vkGetDeviceProcAddr(V.dev, #name);
    VK_DEVICE_FUNCS(DL)
#undef DL
    vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue);
    VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &st};
    VKCHECK(vkCreateSemaphore(V.dev, &sci, NULL, &g_timeline));
    const char *pcache_path(void);
    /* The pipeline cache is kept across runs (TOTK, 2026-10-22: every start compiled 1,100+ pipelines again while
       playing - hitches). The driver checks the header (device, driver version) and ignores a stale file.
       TOTK_VK_PCACHE=<path> (default pipeline_cache.bin in the game folder); TOTK_VK_OFF=pcache: not kept. */
    VkPipelineCacheCreateInfo pcc = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    void *pcd = NULL; size_t pcn = 0;
    if (!vk_off("pcache")) { FILE *f = fopen(pcache_path(), "rb");
        if (f) { fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET); if (l > 0 && (pcd = malloc((size_t)l)) && fread(pcd, 1, (size_t)l, f) == (size_t)l) pcn = (size_t)l; fclose(f); } }
    pcc.initialDataSize = pcn; pcc.pInitialData = pcn ? pcd : NULL;
    if (vkCreatePipelineCache(V.dev, &pcc, NULL, &V.pcache)) { pcc.initialDataSize = 0; pcc.pInitialData = NULL; VKCHECK(vkCreatePipelineCache(V.dev, &pcc, NULL, &V.pcache)); pcn = 0; }
    if (pcn) LOGV("pipeline cache: %zu KiB from %s", pcn >> 10, pcache_path());
    free(pcd);
    vk_fmt_set_depth(depth_choice(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT), depth_choice(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT));
    pthread_t t; if (pthread_create(&t, NULL, completion_thread, NULL)) DIE("completion thread");
    pthread_detach(t);
    LOGV("memory pools: %s; %u extensions, BC %s, custom border colours %s, 8-bit indices %s, null descriptors %s", g_pool_mode_name, ne,
         V.has_bc ? "yes" : "no", V.has_custom_border ? "yes" : "no", V.has_index_u8 ? "yes" : "no", V.has_null_descriptor ? "yes" : "no");
}
const char *pcache_path(void) { const char *e = getenv("TOTK_VK_PCACHE"); return e && *e ? e : "pipeline_cache.bin"; }
static _Atomic int g_pcache_saving; static size_t g_pcache_saved;
static void *pcache_save_thread(void *arg) {
    (void)arg; size_t n = 0;
    pthread_mutex_lock(&g_pipecache_mu); VkResult r0 = vkGetPipelineCacheData(V.dev, V.pcache, &n, NULL); pthread_mutex_unlock(&g_pipecache_mu);
    if (!r0 && n && n != g_pcache_saved) {
        void *d = malloc(n); VkResult r1 = VK_ERROR_UNKNOWN;
        if (d) { pthread_mutex_lock(&g_pipecache_mu); r1 = vkGetPipelineCacheData(V.dev, V.pcache, &n, d); pthread_mutex_unlock(&g_pipecache_mu); }   /* V.pcache: externally synchronised */
        if (d && (r1 == VK_SUCCESS || r1 == VK_INCOMPLETE)) {
            char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", pcache_path());
            FILE *f = fopen(tmp, "wb");
            if (f) { int ok = fwrite(d, 1, n, f) == n; ok &= !fclose(f); if (ok && !rename(tmp, pcache_path())) { g_pcache_saved = n; LOGV("pipeline cache saved (%zu KiB)", n >> 10); } }
        }
        free(d);
    }
    atomic_store(&g_pcache_saving, 0);
    return NULL;
}
void vk_pcache_save_async(void) {   /* every 300 frames (vk_draw.c): on its own thread, so the frame does not wait for it */
    if (!V.pcache || vk_off("pcache")) return;
    int z = 0; if (!atomic_compare_exchange_strong(&g_pcache_saving, &z, 1)) return;
    pthread_t t; if (pthread_create(&t, NULL, pcache_save_thread, NULL)) { atomic_store(&g_pcache_saving, 0); return; }
    pthread_detach(t);
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
void mtl_init(void) { pthread_once(&g_once, vk_init_once); }

/* ---------- dma-buf heap ---------- */
struct dma_heap_alloc { uint64_t len; uint32_t fd, fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_alloc)
static int g_heap_fd = -1;
static int heap_open(void) {
    static const char *heaps[] = {"/dev/dma_heap/system", "/dev/dma_heap/qcom,system"};
    for (unsigned i = 0; i < 2 && g_heap_fd < 0; i++) g_heap_fd = open(heaps[i], O_RDONLY | O_CLOEXEC);
    return g_heap_fd;
}

/* ---------- memory pools ----------
   kind OWN: the pool has its own device memory (and, for export / heap, the dma-buf mapped at its address).
   kind VIEW: the pool lies inside another pool's pages (pools may be carved from a bigger pool): same buffer, offset.
   kind VIRT: a virtual pool; addresses resolve through the alias table (physical pool pages mapped into it).
   kind CPU: no GPU backing (partial overlap with another pool): GPU-ordered work on it runs on the CPU in order. */
enum { K_OWN = 1, K_VIEW, K_VIRT, K_CPU };
typedef struct pool_ent {
    uintptr_t lo, hi, base, end;    /* registered range; page-rounded range */
    int kind, fd;
    VkBuffer buf; VkDeviceMemory mem;
    struct pool_ent *owner; uint64_t off;   /* VIEW: owner pool, offset of base in owner's buffer */
} pool_ent;
typedef struct { uintptr_t va, end; pool_ent *owner; uint64_t off; } alias_ent;   /* virtual pool pages -> owner's buffer offset */
static pool_ent **g_pools; static int g_npool, g_cappool;
static alias_ent *g_alias; static int g_nalias, g_capalias;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pool_bytes, g_pool_cpu;
/* Address index for resolve(): every GPU-backed range (aliases, OWN and VIEW pools) sorted by start, with the largest
   end so far, rebuilt after a change. The game has ~1700 pools and each draw resolves several buffers. */
typedef struct { uintptr_t lo, end; VkBuffer buf; uint64_t off; int iso; } span_t;   /* off: buffer offset of lo; iso: overlaps no other span */
static span_t *g_span; static uintptr_t *g_span_maxend; static int g_nspan, g_capspan, g_span_dirty = 1;
static _Atomic uint64_t g_span_gen = 1;   /* bumped with every pool-table change: the per-thread resolve caches (vk_resolve*) */

static uintptr_t page_sz(void) { static uintptr_t p; if (!p) p = (uintptr_t)getpagesize(); return p; }
static pool_ent *own_containing(uintptr_t a, uintptr_t b) {   /* caller holds g_pool_mu */
    for (int i = 0; i < g_npool; i++) { pool_ent *p = g_pools[i]; if (p->kind == K_OWN && a >= p->base && b <= p->end) return p; }
    return NULL;
}
static int own_overlapping(uintptr_t a, uintptr_t b) {
    for (int i = 0; i < g_npool; i++) { pool_ent *p = g_pools[i]; if (p->kind == K_OWN && a < p->end && b > p->base) return 1; }
    return 0;
}
static VkBuffer make_buffer(VkDeviceSize size, VkExternalMemoryHandleTypeFlags ht, VkMemoryRequirements *req) {
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = ht};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, ht ? &eb : NULL, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; VKCHECK(vkCreateBuffer(g_dev, &bci, NULL, &b));
    vkGetBufferMemoryRequirements(g_dev, b, req);
    return b;
}
/* Give [base, end) its own device memory. 0 on failure (the pool then runs on the CPU path). */
static int back_pool(pool_ent *e) {
    size_t size = e->end - e->base;
    VkMemoryRequirements req;
    if (g_pool_mode == POOLS_HOST) {
        VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)e->base, &hp) || !hp.memoryTypeBits) return 0;
        e->buf = make_buffer(size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, &req);
        int t = pick_host_type(hp.memoryTypeBits & req.memoryTypeBits);
        VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)e->base};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, (uint32_t)t};
        if (t < 0 || vkAllocateMemory(g_dev, &mai, NULL, &e->mem)) { vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        VKCHECK(vkBindBufferMemory(g_dev, e->buf, e->mem, 0));
        e->fd = -1;
        return 1;
    }
    int fd = -1;
    e->buf = make_buffer(size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &req);
    if (g_pool_mode == POOLS_EXPORT) {
        int t = pick_host_type(req.memoryTypeBits);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = e->buf};
        VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, g_export_dedicated ? &ded : NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, req.size, (uint32_t)t};
        VkResult r = t < 0 ? VK_ERROR_UNKNOWN : vkAllocateMemory(g_dev, &mai, NULL, &e->mem);
        if (r) { LOGV("pool %#llx+%#zx: export allocation failed: %d", (unsigned long long)e->base, size, r); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = e->mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        if (vkGetMemoryFdKHR(g_dev, &gi, &fd) || fd < 0) { vkFreeMemory(g_dev, e->mem, NULL); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
    } else {
        struct dma_heap_alloc a = {.len = req.size > size ? req.size : size, .fd_flags = O_RDWR | O_CLOEXEC};
        if (ioctl(g_heap_fd, DMA_HEAP_IOCTL_ALLOC, &a)) { LOGV("pool %#llx+%#zx: dma-buf heap allocation: %s", (unsigned long long)e->base, size, strerror(errno)); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        fd = (int)a.fd;
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        int t = vkGetMemoryFdPropertiesKHR(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) ? -1 : pick_host_type(fp.memoryTypeBits & req.memoryTypeBits);
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd)};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, a.len, (uint32_t)t};
        if (t < 0 || vkAllocateMemory(g_dev, &mai, NULL, &e->mem)) { close(imp.fd); close(fd); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
    }
    VKCHECK(vkBindBufferMemory(g_dev, e->buf, e->mem, 0));
    /* Move the contents in, then put the dma-buf where they were: one mmap replaces the pages at once. */
    void *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (tmp == MAP_FAILED) DIE("pool %#llx+%#zx: mapping the dma-buf: %s", (unsigned long long)e->base, size, strerror(errno));
    memcpy(tmp, (void *)e->base, size);
    if (mmap((void *)e->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != (void *)e->base)
        DIE("pool %#llx+%#zx: mapping the dma-buf at the guest address: %s", (unsigned long long)e->base, size, strerror(errno));
    munmap(tmp, size);
    e->fd = fd;
    return 1;
}
static void unback_pool(pool_ent *e) {
    size_t size = e->end - e->base;
    if (e->fd >= 0) {   /* contents back into ordinary shared memory, moved into place in one step */
        void *fresh = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (fresh == MAP_FAILED) DIE("pool %#llx: fresh pages: %s", (unsigned long long)e->base, strerror(errno));
        memcpy(fresh, (void *)e->base, size);
        if (mremap(fresh, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)e->base) != (void *)e->base)
            DIE("pool %#llx: restoring guest pages: %s", (unsigned long long)e->base, strerror(errno));
        close(e->fd); e->fd = -1;
    }
    vkDestroyBuffer(g_dev, e->buf, NULL); vkFreeMemory(g_dev, e->mem, NULL);
}
static void pool_insert(pool_ent *e) {
    g_span_dirty = 1; atomic_fetch_add(&g_span_gen, 1);
    if (g_npool == g_cappool) { g_cappool = g_cappool ? 2 * g_cappool : 64; g_pools = realloc(g_pools, (size_t)g_cappool * sizeof *g_pools); }
    g_pools[g_npool++] = e;
}
static void add_range(const void *mem, uint64_t size, int virt) {
    mtl_init();
    uintptr_t pg = page_sz(), lo = (uintptr_t)mem, hi = lo + size;
    if (g_pool_mode == POOLS_HOST && pg < g_host_align) pg = (uintptr_t)g_host_align;
    pool_ent *e = calloc(1, sizeof *e);
    e->lo = lo; e->hi = hi; e->base = lo & ~(pg - 1); e->end = (hi + pg - 1) & ~(pg - 1); e->fd = -1;
    pthread_mutex_lock(&g_pool_mu);
    pool_ent *owner = own_containing(e->base, e->end);
    if (virt && g_pool_mode != POOLS_HOST) e->kind = K_VIRT;
    else if (owner) { e->kind = K_VIEW; e->owner = owner; e->off = e->base - owner->base; }
    else if (own_overlapping(e->base, e->end)) e->kind = K_CPU;
    else e->kind = back_pool(e) ? K_OWN : K_CPU;
    if (e->kind == K_CPU) { static int said; if (said++ < 8) LOGV("【红】pool %p+%#llx gets no GPU memory (overlaps another pool or allocation failed): its GPU work runs on the CPU", mem, (unsigned long long)size); g_pool_cpu++; }
    if (e->kind == K_OWN) g_pool_bytes += e->end - e->base;
    pool_insert(e);
    pthread_mutex_unlock(&g_pool_mu);
}
static void del_range(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    int idx = -1;
    for (int i = g_npool - 1; i >= 0; i--) if (g_pools[i]->lo == (uintptr_t)mem) { idx = i; break; }
    if (idx < 0) { pthread_mutex_unlock(&g_pool_mu); return; }
    pool_ent *e = g_pools[idx]; g_pools[idx] = g_pools[--g_npool]; g_span_dirty = 1; atomic_fetch_add(&g_span_gen, 1);
    if (e->kind == K_VIRT) {   /* its aliases go with it (MapVirtual re-adds the pool after remapping) */
        for (int i = 0; i < g_nalias; ) { if (g_alias[i].va >= e->base && g_alias[i].end <= e->end) g_alias[i] = g_alias[--g_nalias]; else i++; }
    }
    if (e->kind == K_OWN) {
        /* Pools inside it and virtual pages mapped from it lose their GPU backing; the CPU still sees the same bytes. */
        for (int i = 0; i < g_npool; i++) if (g_pools[i]->owner == e) { g_pools[i]->kind = K_CPU; g_pools[i]->owner = NULL; }
        for (int i = 0; i < g_nalias; ) { if (g_alias[i].owner == e) g_alias[i] = g_alias[--g_nalias]; else i++; }
        HK_ADD(idle_pool, 1);   /* a pool teardown drains the whole GPU: if these land mid-play, they are the hitches */
        vk_gpu_idle();
        unback_pool(e);
        g_pool_bytes -= e->end - e->base;
    }
    pthread_mutex_unlock(&g_pool_mu);
    vk_wb_range_gone(e->base, e->end);
    if (e->kind == K_OWN) vk_tex_pool_gone(e->base, e->end);   /* after the pool lock: texture code takes the locks the other way round */
    free(e);
}
void mtl_pool_add(const void *mem, uint64_t size) { add_range(mem, size, 0); }
void mtl_pool_del(const void *mem) { del_range(mem); }
/* totk_vpool.c: a virtual pool's range, and its page aliases. */
void mtl_vpool_add(const void *mem, uint64_t size) { add_range(mem, size, 1); }
void mtl_vpool_del(const void *mem) { del_range(mem); }
int mtl_pool_alias(void *dst, const void *src, uint64_t size) {
    mtl_init();
    if (g_pool_mode == POOLS_HOST) return 0;   /* plain shared pages: totk_vpool.c's mremap aliases them */
    pthread_mutex_lock(&g_pool_mu);
    pool_ent *o = own_containing((uintptr_t)src, (uintptr_t)src + size);
    int ok = 0;
    if (o && o->fd >= 0) {
        uint64_t off = (uintptr_t)src - o->base;
        if (mmap(dst, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, o->fd, (off_t)off) == dst) {
            if (g_nalias == g_capalias) { g_capalias = g_capalias ? 2 * g_capalias : 256; g_alias = realloc(g_alias, (size_t)g_capalias * sizeof *g_alias); }
            g_alias[g_nalias++] = (alias_ent){(uintptr_t)dst, (uintptr_t)dst + size, o, off}; g_span_dirty = 1; atomic_fetch_add(&g_span_gen, 1);
            ok = 1;
        } else LOGV("【红】virtual pool alias at %p (+%#llx): %s", dst, (unsigned long long)size, strerror(errno));
    }
    pthread_mutex_unlock(&g_pool_mu);
    return ok;
}
static int span_cmp(const void *x, const void *y) { const span_t *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
static void span_rebuild(void) {   /* caller holds g_pool_mu */
    int n = g_nalias + g_npool;
    if (n > g_capspan) { g_capspan = n + 256; g_span = realloc(g_span, (size_t)g_capspan * sizeof *g_span); g_span_maxend = realloc(g_span_maxend, (size_t)g_capspan * sizeof *g_span_maxend); }
    g_nspan = 0;
    for (int i = 0; i < g_nalias; i++) { const alias_ent *x = &g_alias[i]; g_span[g_nspan++] = (span_t){x->va, x->end, x->owner->buf, x->off}; }
    for (int i = 0; i < g_npool; i++) {
        const pool_ent *p = g_pools[i];
        if (p->kind == K_OWN) g_span[g_nspan++] = (span_t){p->base, p->end, p->buf, 0};
        else if (p->kind == K_VIEW) g_span[g_nspan++] = (span_t){p->base, p->end, p->owner->buf, p->off};
    }
    qsort(g_span, (size_t)g_nspan, sizeof *g_span, span_cmp);
    uintptr_t m = 0; for (int i = 0; i < g_nspan; i++) { if (g_span[i].end > m) m = g_span[i].end; g_span_maxend[i] = m; }
    for (int i = 0; i < g_nspan; i++) g_span[i].iso = (i == 0 || g_span_maxend[i - 1] <= g_span[i].lo) && (i == g_nspan - 1 || g_span[i + 1].lo >= g_span[i].end);
    g_span_dirty = 0;
}
/* Which buffer holds [addr, addr + len), and how many bytes from addr it holds (*avail): caller holds g_pool_mu. */
static int resolve_span_hit(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail, span_t *hit);
static int resolve_span(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail) { return resolve_span_hit(addr, len, buf, off, avail, NULL); }
static int resolve_span_hit(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail, span_t *hit) {   /* hit: the span found (iso = 0 if none or the old scan) */
    uintptr_t a = (uintptr_t)addr, b = a + len;
    if (hit) hit->iso = 0;
    if (b < a) return 0;
    static int linear = -1; if (linear < 0) linear = vk_off("span");
    if (linear) {   /* the old scan (TOTK_VK_OFF=span) */
        for (int i = 0; i < g_nalias; i++) { const alias_ent *x = &g_alias[i]; if (a >= x->va && b <= x->end) { *buf = x->owner->buf; *off = x->off + (a - x->va); if (avail) *avail = x->end - a; return 1; } }
        for (int i = 0; i < g_npool; i++) {
            const pool_ent *p = g_pools[i];
            if (a < p->base || b > p->end) continue;
            if (p->kind == K_OWN) { *buf = p->buf; *off = a - p->base; if (avail) *avail = p->end - a; return 1; }
            if (p->kind == K_VIEW) { *buf = p->owner->buf; *off = p->off + (a - p->base); if (avail) *avail = p->end - a; return 1; }
        }
        return 0;
    }
    if (g_span_dirty) span_rebuild();
    int lo = 0, hi = g_nspan; while (lo < hi) { int m = (lo + hi) >> 1; if (g_span[m].lo <= a) lo = m + 1; else hi = m; }   /* [0, lo) start at or below a */
    const span_t *best = NULL;
    for (int i = lo - 1; i >= 0 && g_span_maxend[i] > a; i--) if (g_span[i].end >= b && g_span[i].end > a && (!best || g_span[i].end > best->end)) best = &g_span[i];
    if (!best) return 0;
    *buf = best->buf; *off = best->off + (a - best->lo); if (avail) *avail = best->end - a;
    if (hit) *hit = *best;
    return 1;
}
static int resolve(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) { return resolve_span(addr, len, buf, off, NULL); }
/* Per-thread resolve cache (TOTK, 2026-10-23: the pool lock taken for every buffer binding of every draw, shared with
   the game threads, was ~10% of the replay thread in mutex traffic). Valid while the pool table's generation holds.
   TOTK_VK_OFF=rcache: off. */
typedef struct { uint64_t addr, len, gen, off, avail; VkBuffer buf; int ok; } rc_ent;
enum { RC_N = 2048 };
static __thread rc_ent *tl_rc;
static rc_ent *rc_slot(uint64_t addr, uint64_t len) {
    if (!tl_rc) tl_rc = calloc(RC_N, sizeof *tl_rc);
    return tl_rc ? &tl_rc[(unsigned)(((addr >> 6) ^ (addr >> 17) ^ (len * 0x9E3779B1u)) & (RC_N - 1))] : NULL;
}
static int rc_off(void) { static int off = -1; if (off < 0) off = vk_off("rcache"); return off; }
/* Per-thread span cache (2026-10-26): the address cache above keys on the exact address (and length), and every draw's
   vertex / index ranges are new offsets, so it missed and took the pool lock for nearly every binding (a DWARF profile at
   Lookout Landing: pthread_mutex_unlock under vk_resolve / vk_resolve_span was ~10% of the replay thread). This keeps the
   last spans found that overlap no other span: any address inside such a span resolves to it alone, as the locked
   search would. Valid while the pool table's generation holds. TOTK_VK_OFF=spancache: off. */
enum { SC_N = 8 };
static __thread struct { uint64_t gen; int n, next; span_t s[SC_N]; } tl_sc;
static int sc_off(void) { static int off = -1; if (off < 0) off = vk_off("spancache"); return off; }
static int sc_get(uint64_t gen, uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail) {
    if (tl_sc.gen != gen) return 0;
    uintptr_t a = (uintptr_t)addr, b = a + (len ? len : 1);
    if (b < a) return 0;
    for (int i = 0; i < tl_sc.n; i++) { const span_t *x = &tl_sc.s[i];
        if (a >= x->lo && b <= x->end) { *buf = x->buf; *off = x->off + (a - x->lo); if (avail) *avail = x->end - a; return 1; } }
    return 0;
}
static void sc_put(uint64_t gen, const span_t *x) {
    if (!x->iso) return;
    if (tl_sc.gen != gen) { tl_sc.gen = gen; tl_sc.n = tl_sc.next = 0; }
    tl_sc.s[tl_sc.next] = *x; tl_sc.next = (tl_sc.next + 1) % SC_N; if (tl_sc.n < SC_N) tl_sc.n++;
}
int vk_resolve_span(uint64_t addr, VkBuffer *buf, uint64_t *off, uint64_t *avail) {
    uint64_t gen = atomic_load(&g_span_gen);
    if (!sc_off() && sc_get(gen, addr, 0, buf, off, avail)) return 1;
    rc_ent *c = rc_off() ? NULL : rc_slot(addr, 0);
    if (c && c->addr == addr && c->len == 0 && c->gen == gen) { if (c->ok) { *buf = c->buf; *off = c->off; if (avail) *avail = c->avail; } return c->ok; }
    uint64_t av = 0; span_t hit;
    pthread_mutex_lock(&g_pool_mu); gen = atomic_load(&g_span_gen); int ok = resolve_span_hit(addr, 1, buf, off, &av, &hit); pthread_mutex_unlock(&g_pool_mu);
    if (ok && !sc_off()) sc_put(gen, &hit);
    if (avail) *avail = av;
    if (c) { c->addr = addr; c->len = 0; c->gen = gen; c->ok = ok; if (ok) { c->buf = *buf; c->off = *off; c->avail = av; } }
    return ok;
}
int vk_resolve(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) {
    uint64_t gen = atomic_load(&g_span_gen);
    if (len && !sc_off() && sc_get(gen, addr, len, buf, off, NULL)) return 1;
    rc_ent *c = rc_off() || !len ? NULL : rc_slot(addr, len);
    if (c && c->addr == addr && c->len == len && c->gen == gen) { if (c->ok) { *buf = c->buf; *off = c->off; } return c->ok; }
    span_t hit;
    pthread_mutex_lock(&g_pool_mu); gen = atomic_load(&g_span_gen); int ok = resolve_span_hit(addr, len, buf, off, NULL, &hit); pthread_mutex_unlock(&g_pool_mu);
    if (ok && len && !sc_off()) sc_put(gen, &hit);
    if (c) { c->addr = addr; c->len = len; c->gen = gen; c->ok = ok; if (ok) { c->buf = *buf; c->off = *off; } }
    return ok;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    VkBuffer b; pthread_mutex_lock(&g_pool_mu); int ok = resolve(addr, len, &b, off); pthread_mutex_unlock(&g_pool_mu);
    if (ok) *buf = (void *)(uintptr_t)b;
    return ok;
}
/* mtl_pool_invalidate: vk_res.c (GPU write-back of renderings into CPU-pool textures) */
void gpu_pool_totals(uint64_t *n, uint64_t *bytes) { pthread_mutex_lock(&g_pool_mu); *n = (uint64_t)g_npool; *bytes = g_pool_bytes; pthread_mutex_unlock(&g_pool_mu); }

/* ---------- submissions ----------
   A submission is recorded into the "pre" and "main" command buffers of one slot of a small ring (vk_int.h). Its
   CPU-visible results (post ops) are applied by the completion thread once the timeline semaphore reaches the
   submission's value, in submission order, and only then is its fence done. Work the GPU cannot do (an address outside
   every pool) is done on the CPU at its place in the order: what is recorded so far is submitted and waited for first
   ("CPU step"). A slot's transient memory and descriptor sets are recycled when it is begun again (after completion). */
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type, cval; } post_op;   /* 1 event, 2 counter (cval: its value, fixed at record time) */
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; } chunk_t;
#define RING 24   /* submissions in flight: vk_draw.c splits a long replay into several (split_maybe), two frames ahead */
typedef struct {
    VkCommandPool pool; VkCommandBuffer cb, pre; int pre_open;
    uint64_t value; post_op *post; int npost, cap; int busy; uint64_t t_submit;   /* host time of its vkQueueSubmit2 (TOTK_TIMELINE) */
    chunk_t *chunks; int nchunk, capchunk;
    VkDescriptorPool *dpools; int ndpool, capdpool, curdpool, curdsets;   /* curdpool: the pool sets come from now (reset to 0 at slot begin) */
    VkQueryPool ts; int ts_on;   /* GPU timestamps at the start and end of the main command buffer */
    VkFence fence;   /* signalled when its submission completes (fences_on) */
    vk_sub_note_t note[8]; uint32_t nnote, ndraw;   /* what it holds (vk_sub_note): logged if the GPU hangs on it */
    VkQueryPool pt; uint32_t npt; vk_sub_note_t *ptn; uint64_t *pth;   /* pth: host time at each mark (replay CPU per pass) */   /* TOTK_VK_PASSTIME: a timestamp after each pass / dispatch (vk_sub_time) */
} slot_t;
/* Completion by a fence per submission (TOTK, 2026-10-23), not the timeline semaphore: Turnip on kgsl emulates timeline
   semaphores (Mesa vk_sync_timeline), and the completion thread's wait on it (even a vkGetSemaphoreCounterValue poll)
   sat in a kgsl timestamp wait holding the lock vkQueueSubmit2 takes to signal it, so every submit of the replay
   thread waited for the GPU: ~11 ms a frame of recording blocked (recording 25 -> 14 ms, 28.7 -> 33 fps at the field
   save). TOTK_VK_FENCES=0: the timeline semaphore again. */
static int fences_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_FENCES"); on = !(e && !strcmp(e, "0")); LOGV("submission completion: %s (TOTK_VK_FENCES)", on ? "a fence per submission" : "the timeline semaphore"); } return on; }
static slot_t g_ring[RING]; static int g_ring_next;
static slot_t *g_cur;
static post_op *g_carry; static int g_ncarry, g_capcarry;
static uint64_t g_fence;
static _Atomic uint64_t g_done;
static uint64_t g_done_at;   /* when g_done last moved: a woken waiter measures its wake latency from here */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_cv_work = PTHREAD_COND_INITIALIZER;
static slot_t *g_queue_[RING]; static int g_qhead, g_qtail;
static uint64_t g_stat_sub, g_stat_draw, g_stat_gpu_copy, g_stat_cpu_step, g_stat_ub, g_stat_ub_renamed, g_stat_ub_renamed_bytes;
static _Atomic uint64_t g_sub_serial = 1;
uint64_t vk_submit_serial(void) { return atomic_load(&g_sub_serial); }

/* performance counters (vk_perf_take): CPU time recording submissions, time the recording thread waited for the GPU,
   and GPU time of the main command buffers (timestamps) */
static uint64_t g_perf_replay_ns, g_perf_wait_ns, g_perf_sub0, g_perf_t0; static _Atomic uint64_t g_perf_gpu_ns; static uint64_t g_perf_subs;
enum { PT_MAX = 512 };
static int passtime_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_PASSTIME"); on = e && atoi(e) > 0 && V.props.limits.timestampPeriod > 0; if (on) LOGV("TOTK_VK_PASSTIME: GPU time per pass, logged every 10 s"); } return on; }
typedef struct { char kind; const char *k0, *k1; uint32_t a, b, c; uint64_t ns, n, cpu; } pt_ent;
static pt_ent g_pt[4096]; static uint64_t g_pt_t0, g_pt_total, g_pt_subs, g_pt_cpu;
static int pt_cmp(const void *x, const void *y) { uint64_t a = ((const pt_ent *)x)->ns, b = ((const pt_ent *)y)->ns; return a < b ? 1 : a > b ? -1 : 0; }
static void pt_add(const vk_sub_note_t *n, uint64_t ns, uint64_t cpu) {   /* completion thread */
    uintptr_t h = ((uintptr_t)n->k0 * 31 + (uintptr_t)n->k1 * 17 + (uintptr_t)n->kind) >> 3;
    for (unsigned i = 0; i < 4096; i++) {
        pt_ent *e = &g_pt[(h + i) % 4096];
        if (!e->n) { *e = (pt_ent){n->kind, n->k0, n->k1, n->a, n->b, n->c, 0, 0, 0}; }
        if (e->kind == n->kind && e->k0 == n->k0 && e->k1 == n->k1) { e->ns += ns; e->cpu += cpu; e->n++; return; }
    }
}
static void pt_log(uint64_t now) {
    static pt_ent sorted[4096];
    double secs = (now - g_pt_t0) / 1e9; int m = 0;
    for (int i = 0; i < 4096; i++) if (g_pt[i].n) sorted[m++] = g_pt[i];
    qsort(sorted, (size_t)m, sizeof *sorted, pt_cmp);
    LOGV("GPU time by pass, last %.1f s: %.1f ms of GPU work a second (replay CPU between the marks %.1f) over %llu submissions, %d kinds; the top:", secs, g_pt_total / 1e6 / secs, g_pt_cpu / 1e6 / secs, (unsigned long long)g_pt_subs, m);
    for (int i = 0; i < m && i < 30; i++) {
        const pt_ent *e = &sorted[i];
        LOGV("  %5.1f%% %6.2f ms/s (replay CPU %6.2f) %s %ux%u%s%u x%llu  %.16s %.16s", 100.0 * e->ns / (g_pt_total ? g_pt_total : 1), e->ns / 1e6 / secs, e->cpu / 1e6 / secs,
             e->kind == 'p' ? "pass" : e->kind == 'd' ? "dispatch" : e->kind == 'c' ? "copy" : "other", e->a, e->b, e->kind == 'd' ? "x" : " colour ", e->c,
             (unsigned long long)e->n, e->k0 ? e->k0 : "-", e->k1 ? e->k1 : "");
    }
    /* the kinds whose recording outruns their drawing: replay CPU - GPU time, the most first */
    for (int i = 0; i < m; i++) sorted[i].ns = sorted[i].cpu > sorted[i].ns ? sorted[i].cpu - sorted[i].ns : 0;
    qsort(sorted, (size_t)m, sizeof *sorted, pt_cmp);
    LOGV("replay CPU over GPU time by pass (the GPU can starve there):");
    for (int i = 0; i < m && i < 30 && sorted[i].ns; i++) {
        const pt_ent *e = &sorted[i];
        LOGV("  +%6.2f ms/s (CPU %6.2f) %s %ux%u%s%u x%llu  %.16s %.16s", e->ns / 1e6 / secs, e->cpu / 1e6 / secs,
             e->kind == 'p' ? "pass" : e->kind == 'd' ? "dispatch" : e->kind == 'c' ? "copy" : "other", e->a, e->b, e->kind == 'd' ? "x" : " colour ", e->c,
             (unsigned long long)e->n, e->k0 ? e->k0 : "-", e->k1 ? e->k1 : "");
    }
    memset(g_pt, 0, sizeof g_pt); g_pt_total = g_pt_subs = g_pt_cpu = 0; g_pt_t0 = now;
}
static int ts_ok(void) { static int ok = -1; if (ok < 0) ok = V.props.limits.timestampComputeAndGraphics && V.props.limits.timestampPeriod > 0 && !vk_off("ts"); return ok; }
void vk_perf_take(double *replay_ms, double *wait_ms, double *gpu_ms, uint64_t *subs) {
    *replay_ms = g_perf_replay_ns / 1e6; *wait_ms = g_perf_wait_ns / 1e6; *gpu_ms = atomic_exchange(&g_perf_gpu_ns, 0) / 1e6; *subs = g_perf_subs;
    g_perf_replay_ns = g_perf_wait_ns = 0; g_perf_subs = 0;
}
static uint64_t gpu_ticks(void) { return (uint64_t)((__uint128_t)host_now_ns() * 12 / 625); }   /* 19.2 MHz, the Switch's tick */
uint64_t vk_counter_samples(uint64_t addr);   /* vk_draw.c: occlusion result for a report at addr (1 << 20 if none) */
static void apply_posts(post_op *p, int n) {
    for (int i = 0; i < n; i++) {
        post_op *o = &p[i];
        if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; }
        else {
            uint64_t rec[2] = {o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? o->cval : 0, gpu_ticks()};
            memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
        }
    }
}
/* GPU timestamp -> ns in the GPU's own time domain (Turnip on kgsl has no calibrated timestamps; the timeline's reader
   aligns it to the host clock by the smallest GPU-start-after-submit over all submissions) */
static int gpu_to_host(uint64_t ticks, uint64_t *host) { *host = (uint64_t)((double)ticks * V.props.limits.timestampPeriod); return 1; }
static void notes_log(const slot_t *s) {   /* the content of a submission the GPU did not finish */
    LOGV("【红】submission %llu: %u draws, %u passes / dispatches / copies; the last %u:", (unsigned long long)s->value, s->ndraw, s->nnote, s->nnote < 8 ? s->nnote : 8u);
    for (uint32_t i = s->nnote > 8 ? s->nnote - 8 : 0; i < s->nnote; i++) {
        const vk_sub_note_t *n = &s->note[i % 8];
        LOGV("    %s %.80s %.80s %u x %u x %u, %u draws", n->kind == 'p' ? "pass" : n->kind == 'd' ? "dispatch" : "copy", n->k0 ? n->k0 : "-", n->k1 ? n->k1 : "", n->a, n->b, n->c, n->draws);
    }
}
static void *completion_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_qhead == g_qtail) pthread_cond_wait(&g_cv_work, &g_mu);
        slot_t *s = g_queue_[g_qhead % RING];
        pthread_mutex_unlock(&g_mu);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_timeline, .pValues = &s->value};
        VkResult r = fences_on() ? vkWaitForFences(V.dev, 1, &s->fence, VK_TRUE, 5000000000ull) : vkWaitSemaphores(V.dev, &wi, 5000000000ull);
        for (int waited = 5; r == VK_TIMEOUT; waited += 5) {   /* a GPU hang shows up in the log instead of a silent freeze */
            if (waited == 5) notes_log(s);
            LOGV("【红】the GPU has not finished submission %llu after %d s (last finished: %llu)", (unsigned long long)s->value, waited, (unsigned long long)atomic_load(&g_done));
            r = fences_on() ? vkWaitForFences(V.dev, 1, &s->fence, VK_TRUE, 5000000000ull) : vkWaitSemaphores(V.dev, &wi, 5000000000ull);
        }
        if (r) {
            pthread_mutex_lock(&g_mu);
            for (int q = g_qhead; q < g_qtail && q < g_qhead + 3; q++) notes_log(g_queue_[q % RING]);
            pthread_mutex_unlock(&g_mu);
        }
        if (r) DIE("waiting for the GPU (submission %llu): %d%s", (unsigned long long)s->value, r, r == VK_ERROR_DEVICE_LOST ? " (device lost)" : "");
        uint64_t t1 = host_now_ns();   /* how long from the driver's signal to g_done: the fence-wait latency that is ours */
        if (s->t_submit && totk_tl_on()) totk_tl_at(TL_SEEN, s->t_submit, s->value, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));   /* submit to seen complete */
        s->t_submit = 0;
        apply_posts(s->post, s->npost);
        if (s->ts_on) {
            uint64_t t[2];
            if (!vkGetQueryPoolResults(V.dev, s->ts, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) && t[1] > t[0]) {
                atomic_fetch_add(&g_perf_gpu_ns, (uint64_t)((double)(t[1] - t[0]) * V.props.limits.timestampPeriod));
                uint64_t h0, h1;   /* TOTK_TIMELINE: the GPU's own span of the main command buffer, on the host clock */
                if (totk_tl_on() && gpu_to_host(t[0], &h0) && gpu_to_host(t[1], &h1)) totk_tl_at(TL_GPU, h0, s->value, h1);
            }
        }
        if (s->npt) {   /* TOTK_VK_PASSTIME */
            static uint64_t t[PT_MAX + 1];
            if (!vkGetQueryPoolResults(V.dev, s->pt, 0, s->npt + 1, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT)) {
                for (uint32_t i = 0; i < s->npt; i++) if (t[i + 1] > t[i]) { uint64_t ns = (uint64_t)((double)(t[i + 1] - t[i]) * V.props.limits.timestampPeriod), cpu = s->pth[i + 1] - s->pth[i]; pt_add(&s->ptn[i], ns, cpu); g_pt_total += ns; g_pt_cpu += cpu; }
                g_pt_subs++;
            }
            uint64_t now = host_now_ns(); if (!g_pt_t0) g_pt_t0 = now; else if (now - g_pt_t0 > 10000000000ull) pt_log(now);
        }
        pthread_mutex_lock(&g_mu);
        s->npost = 0; s->busy = 0; g_qhead++;
        atomic_store(&g_done, s->value);
        g_done_at = host_now_ns();
        HK_ADD(comp_ns, g_done_at - t1);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void slot_begin(void) {
    slot_t *s = &g_ring[g_ring_next]; g_ring_next = (g_ring_next + 1) % RING;
    uint64_t w0 = host_now_ns();
    pthread_mutex_lock(&g_mu); while (s->busy) pthread_cond_wait(&g_cv, &g_mu); s->busy = 1; pthread_mutex_unlock(&g_mu);
    g_perf_wait_ns += host_now_ns() - w0;
    if (!s->pool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qfam};
        VKCHECK(vkCreateCommandPool(V.dev, &pci, NULL, &s->pool));
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &cai, cbs)); s->cb = cbs[0]; s->pre = cbs[1];
    } else VKCHECK(vkResetCommandPool(V.dev, s->pool, 0));
    if (fences_on()) { if (!s->fence) { VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VKCHECK(vkCreateFence(V.dev, &fci, NULL, &s->fence)); } else VKCHECK(vkResetFences(V.dev, 1, &s->fence)); }
    for (int i = 0; i < s->nchunk; i++) s->chunks[i].used = 0;
    for (int i = 0; i < s->ndpool; i++) VKCHECK(vkResetDescriptorPool(V.dev, s->dpools[i], 0));
    s->curdpool = 0; s->curdsets = 0; s->nnote = s->ndraw = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(s->cb, &bi));
    s->pre_open = 0; s->ts_on = 0;
    if (ts_ok()) {
        if (!s->ts) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &s->ts)); }
        vkCmdResetQueryPool(s->cb, s->ts, 0, 2);
        vkCmdWriteTimestamp2(s->cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s->ts, 0);
        s->ts_on = 1;
    }
    s->npt = 0;
    if (passtime_on()) {
        if (!s->pt) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PT_MAX + 1}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &s->pt)); s->ptn = calloc(PT_MAX, sizeof *s->ptn); s->pth = calloc(PT_MAX + 1, sizeof *s->pth); }
        vkCmdResetQueryPool(s->cb, s->pt, 0, PT_MAX + 1);
        vkCmdWriteTimestamp2(s->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s->pt, 0); s->pth[0] = host_now_ns();
    }
    g_cur = s;
}
/* TOTK_VK_PASSTIME=1 (diagnostic, 2026-10-23): GPU time per pass and dispatch. vk_end_rendering and dispatch() call
   vk_sub_time after their full barrier, so the time between two timestamps is the work in between (the pass, plus
   copies and clears before it); it is booked to the newest note (the pass's first draw's shaders and target, or the
   dispatch). The completion thread adds it up by shader pair and logs the top every 10 s (pt_log). */
void vk_sub_time(void) {
    if (!passtime_on() || !g_cur || g_cur->npt >= PT_MAX) return;
    slot_t *s = g_cur;
    s->ptn[s->npt] = s->nnote ? s->note[(s->nnote - 1) % 8] : (vk_sub_note_t){'?', NULL, NULL, 0, 0, 0, 0};
    vkCmdWriteTimestamp2(s->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s->pt, ++s->npt); s->pth[s->npt] = host_now_ns();
}
void vk_barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
static void barrier_host(VkCommandBuffer cb) {   /* GPU writes become visible to the game */
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
VkCommandBuffer vk_cb(void) { if (!g_cur) slot_begin(); return g_cur->cb; }
VkCommandBuffer vk_pre_cb(void) {
    if (!g_cur) slot_begin();
    if (!g_cur->pre_open) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(g_cur->pre, &bi)); g_cur->pre_open = 1;
    }
    return g_cur->pre;
}
static void chunk_new(chunk_t *c, VkDeviceSize size) {   /* host-visible, mapped */
    memset(c, 0, sizeof *c); c->size = size;
    HK_ADD(chunks, 1);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = c->size, .usage = POOL_USAGE, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &c->buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, c->buf, &req);
    int t = pick_host_type(req.memoryTypeBits);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    VKCHECK(vkAllocateMemory(V.dev, &mai, NULL, &c->mem));
    VKCHECK(vkBindBufferMemory(V.dev, c->buf, c->mem, 0));
    VKCHECK(vkMapMemory(V.dev, c->mem, 0, VK_WHOLE_SIZE, 0, (void **)&c->ptr));
}
void *vk_transient(size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_cur) slot_begin();
    slot_t *s = g_cur;
    if (!align) align = 16;
    chunk_t *c = s->nchunk ? &s->chunks[s->nchunk - 1] : NULL;
    VkDeviceSize at = c ? (c->used + align - 1) / align * align : 0;
    if (!c || at + size > c->size) {
        /* a free chunk of this slot (all are reset when it begins) is used before a new one is made */
        int found = -1;
        for (int i = 0; i < s->nchunk - 1 && found < 0; i++) if (!s->chunks[i].used && s->chunks[i].size >= size) found = i;
        if (found >= 0) { chunk_t tmp = s->chunks[found]; s->chunks[found] = s->chunks[s->nchunk - 1]; s->chunks[s->nchunk - 1] = tmp; c = &s->chunks[s->nchunk - 1]; at = 0; }
        else {
            if (s->nchunk == s->capchunk) { s->capchunk = s->capchunk ? 2 * s->capchunk : 8; s->chunks = realloc(s->chunks, (size_t)s->capchunk * sizeof *s->chunks); }
            c = &s->chunks[s->nchunk++];
            chunk_new(c, size > (8u << 20) ? (size + 0xffff) & ~(VkDeviceSize)0xffff : (8u << 20));
            at = 0;
        }
    }
    c->used = at + size;
    *buf = c->buf; *off = at;
    return c->ptr + at;
}
/* 1024 sets a pool (2026-10-23, was 4096 with 4x the descriptors): every ring slot keeps the pools it needed, and with
   24 slots (split submissions) the old size meant hundreds of MB of descriptor memory. A slot that needs more sets
   chains more pools (and keeps them for its next use). */
#define DPOOL_SETS 1024
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    if (!g_cur) slot_begin();
    slot_t *s = g_cur;
    /* TOTK (2026-10-22): sets came only from the slot's last pool, so a submission needing more than one pool made a new
       pool every time and the reset ones before it were never used again: one more pool per frame in the world (~4,400
       sets a frame), each with its driver memory, until Android ran out (~45 MB/s). Now the slot's pools are used in
       turn and a new one is made only past the last. */
    int fresh = 0;
    for (;;) {
        if (s->curdpool < s->ndpool && s->curdsets >= DPOOL_SETS) {   /* counted here too: some drivers (lavapipe) never report a full pool */
            if (s->curdpool + 1 < s->ndpool) { s->curdpool++; s->curdsets = 0; continue; }
        } else if (s->curdpool < s->ndpool) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s->dpools[s->curdpool], .descriptorSetCount = 1, .pSetLayouts = &layout};
            VkDescriptorSet set; VkResult r = vkAllocateDescriptorSets(V.dev, &ai, &set);
            if (!r) { s->curdsets++; return set; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) DIE("vkAllocateDescriptorSets: %d", r);
            if (s->curdpool + 1 < s->ndpool) { s->curdpool++; s->curdsets = 0; continue; }
        }
        static const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 512}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048}};
        VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = DPOOL_SETS, .poolSizeCount = sizeof sizes / sizeof *sizes, .pPoolSizes = sizes};
        if (s->ndpool == s->capdpool) { s->capdpool = s->capdpool ? 2 * s->capdpool : 4; s->dpools = realloc(s->dpools, (size_t)s->capdpool * sizeof *s->dpools); }
        if (fresh) break;   /* a set that does not fit an empty pool */
        HK_ADD(dpools, 1);
        VKCHECK(vkCreateDescriptorPool(V.dev, &pci, NULL, &s->dpools[s->ndpool++]));
        s->curdpool = s->ndpool - 1; s->curdsets = 0; fresh = 1;
    }
    DIE("descriptor set allocation failed in a fresh pool");
}
/* Submit what is recorded (pre, then main); with posts, the current submission's post ops ride along. */
static void ub_seal(uint64_t fence); static void ub_flush(void); static void gw_seal(uint64_t fence); static void gw_prune(void);
static int g_defer_this; static slot_t *g_pend[RING]; static int g_npend;   /* TOTK_VK_SPLIT_DEFER (slot_submit) */
enum { XSEM = 4 };
static VkSemaphore g_xsig[XSEM], g_xwait[XSEM]; static int g_nxsig, g_nxwait;
void vk_sub_signal(VkSemaphore s) { if (g_nxsig < XSEM) g_xsig[g_nxsig++] = s; else DIE("vk_sub_signal: more than %d", XSEM); }
void vk_sub_wait(VkSemaphore s) { if (g_nxwait < XSEM) g_xwait[g_nxwait++] = s; else DIE("vk_sub_wait: more than %d", XSEM); }
static uint64_t slot_submit(int with_posts) {
    if (!g_cur) slot_begin();
    ub_flush();
    vk_end_rendering();
    ub_seal(g_fence + 1);   /* the value this submission gets below (only this thread submits) */
    gw_seal(g_fence + 1);
    slot_t *s = g_cur; g_cur = NULL;
    barrier_host(s->cb);
    if (s->ts_on) vkCmdWriteTimestamp2(s->cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, s->ts, 1);
    VKCHECK(vkEndCommandBuffer(s->cb));
    if (s->pre_open) { barrier_host(s->pre); VKCHECK(vkEndCommandBuffer(s->pre)); }
    if (with_posts && g_ncarry) {
        if (s->cap < g_ncarry) { s->cap = g_ncarry; s->post = realloc(s->post, (size_t)s->cap * sizeof *s->post); }
        memcpy(s->post, g_carry, (size_t)g_ncarry * sizeof *g_carry); s->npost = g_ncarry; g_ncarry = 0;
    }
    pthread_mutex_lock(&g_mu);
    s->value = ++g_fence;
    /* TOTK_VK_SPLIT_DEFER=1 (diagnostic, 2026-10-23): a split's command buffers are recorded as usual but handed to the
       GPU only with the replay's next real submission, so the GPU's timing is the unsplit one; tells a recording-state
       bug of the split from a timing one (the GPU running a frame while the game writes the next). */
    if (g_defer_this && g_npend < RING / 2) {
        g_pend[g_npend++] = s;
        pthread_mutex_unlock(&g_mu);
        atomic_fetch_add(&g_sub_serial, 1);
        return s->value;
    }
    slot_t *all[RING]; int na = 0;
    for (int i = 0; i < g_npend; i++) all[na++] = g_pend[i];
    all[na++] = s; g_npend = 0;
    VkCommandBufferSubmitInfo cbi[RING][2]; VkSemaphoreSubmitInfo ssi[RING]; VkSubmitInfo2 si[RING];
    for (int i = 0; i < na; i++) {
        slot_t *x = all[i];
        cbi[i][0] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = x->pre};
        cbi[i][1] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = x->cb};
        ssi[i] = (VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_timeline, .value = x->value, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        si[i] = (VkSubmitInfo2){VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = x->pre_open ? 2 : 1, .pCommandBufferInfos = x->pre_open ? cbi[i] : cbi[i] + 1,
                                .signalSemaphoreInfoCount = fences_on() ? 0 : 1, .pSignalSemaphoreInfos = &ssi[i]};
    }
    /* binary semaphores asked for this submission (vk_sub_signal / vk_sub_wait: frame generation) go with its own commands, the last */
    VkSemaphoreSubmitInfo xs[1 + XSEM], xw[XSEM];
    if (g_nxsig || g_nxwait) {
        int k = 0; if (!fences_on()) xs[k++] = ssi[na - 1];
        for (int j = 0; j < g_nxsig; j++) xs[k++] = (VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_xsig[j], .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        for (int j = 0; j < g_nxwait; j++) xw[j] = (VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_xwait[j], .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        si[na - 1].signalSemaphoreInfoCount = (uint32_t)k; si[na - 1].pSignalSemaphoreInfos = xs;
        si[na - 1].waitSemaphoreInfoCount = (uint32_t)g_nxwait; si[na - 1].pWaitSemaphoreInfos = xw;
        g_nxsig = g_nxwait = 0;
    }
    VkResult r = VK_SUCCESS;
    if (fences_on()) for (int i = 0; i < na && !r; i++) r = vkQueueSubmit2(V.queue, 1, &si[i], all[i]->fence);
    else r = vkQueueSubmit2(V.queue, (uint32_t)na, si, VK_NULL_HANDLE);
    if (r) for (int q = g_qhead; q < g_qtail && q < g_qhead + 3; q++) notes_log(g_queue_[q % RING]);   /* the first unfinished: the one the GPU hung on */
    if (r) DIE("vkQueueSubmit2: %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (device lost)" : "");
    for (int i = 0; i < na; i++) { g_queue_[g_qtail++ % RING] = all[i]; if (totk_tl_on()) { all[i]->t_submit = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); totk_tl(TL_COMMIT, all[i]->value, 0); } }
    pthread_cond_signal(&g_cv_work);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_sub_serial, 1);
    return s->value;
}
uint64_t vk_submit_split(void) {   /* vk_draw.c split_maybe: a split submission (deferred with TOTK_VK_SPLIT_DEFER=1) */
    static int defer = -1; if (defer < 0) { const char *e = getenv("TOTK_VK_SPLIT_DEFER"); defer = e && atoi(e) > 0; if (defer) LOGV("TOTK_VK_SPLIT_DEFER: split submissions are handed to the GPU with the replay's next real one"); }
    if (!g_cur) return mtl_last_fence();
    g_defer_this = defer; uint64_t v = slot_submit(0); g_defer_this = 0;
    return v;
}
static void wait_value(uint64_t v) {
    uint64_t w0 = host_now_ns();
    pthread_mutex_lock(&g_mu); while (atomic_load(&g_done) < v) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu);
    HK_ADD(wake_ns, host_now_ns() - g_done_at);   /* from the completion store to the waiter running: scheduling, not the GPU */
    g_perf_wait_ns += host_now_ns() - w0;
}
void vk_gpu_idle(void) { uint64_t v, t0 = host_now_ns(); pthread_mutex_lock(&g_mu); v = g_fence; pthread_mutex_unlock(&g_mu); wait_value(v); HK_ADD(idles, 1); HK_ADD(idle_ns, host_now_ns() - t0); }
static _Atomic uint64_t g_replay_serial;   /* defined at mtl_sub_begin */
void vk_cpu_step(void) { HK_ADD(cpu_steps, 1); g_stat_cpu_step++; atomic_fetch_add(&g_replay_serial, 1);   /* the CPU writes in command order: textures look again */ ub_flush(); if (g_cur) wait_value(slot_submit(0)); else vk_gpu_idle(); }

static pthread_mutex_t g_rec_mu;
static void rec_mu_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&g_rec_mu, &a); }
void vk_rec_lock(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, rec_mu_init); pthread_mutex_lock(&g_rec_mu); }
void vk_rec_unlock(void) { pthread_mutex_unlock(&g_rec_mu); }
static void snap_reset(void);
/* Replays (game submissions) and CPU steps inside them: textures check their memory for CPU changes once per this
   serial (vk_res.c vk_tex_get), not per Vulkan submission. With the split (several submissions a replay) and async
   replay, a later chunk re-read memory the game had already written for its NEXT frame, and the rest of the frame drew
   e.g. recycled terrain tiles with the new content (2026-10-23: flat, wrongly coloured terrain tiles; gone with
   TOTK_VK_SPLIT_MS=0). */
static _Atomic uint64_t g_replay_serial = 1;
uint64_t vk_replay_serial(void) { return atomic_load(&g_replay_serial); }
void mtl_sub_begin(void) { vk_rec_lock(); mtl_init(); g_ncarry = 0; snap_reset(); atomic_fetch_add(&g_replay_serial, 1);   /* a replay without a snapshot call must not use the last one's copies */
    g_perf_t0 = host_now_ns(); g_perf_sub0 = g_perf_wait_ns; }
void vk_gpu_write(uint64_t a, uint64_t n);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!n) return;
    VkBuffer sb, db; uint64_t so, dof;
    int ok = vk_resolve(src, n, &sb, &so) && vk_resolve(dst, n, &db, &dof);
    ub_flush();
    vk_gpu_write(dst, n);
    if (!ok || (sb == db && so < dof + n && dof < so + n)) {   /* outside the pools, or overlapping in one buffer (Vulkan forbids it): the CPU does it in order */
        vk_cpu_step(); memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n); return;
    }
    vk_end_rendering();   /* transfers happen outside rendering */
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    VkBufferCopy c = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &c); vk_sub_note('c', NULL, NULL, (uint32_t)n, 0, 0);
    vk_barrier_all(cb);
    g_stat_gpu_copy++;
}
/* ---------- inline uniform updates without splitting the render pass (nvn_mtl.m "renaming") ----------
   The game updates small uniform ranges between draws (nvnCommandBufferUpdateUniformBuffer) over a hundred times per
   submission. A buffer write cannot happen inside a render pass, and on a tiled GPU every pass split stores and reloads
   all attachments. So an update is only logged: a later uniform binding that overlaps logged updates gets a private
   copy in the submission's transient memory (the pool's bytes with the logged updates laid over them in order), so draws
   before an update read the old bytes and draws after it the new ones. The logged updates reach the pool at the end of
   the submission, or earlier when anything other than a uniform binding may read or write an updated range
   (vk_ub_touch: vertex, index and storage buffers of a draw; copies, dispatches and other work outside passes).
   An entry stays logged, as an overlay for later copies, until the GPU has finished its submission (the CPU's view of
   the pool only has it from then). TOTK_VK_UB_RENAME=0 writes every update in place, splitting the pass. */
typedef struct { uint64_t lo, hi, seq, fence; int flushed; uint8_t *data; uint64_t rep; } ub_ent;   /* fence: submission it reached memory in (0: not yet); rep: the replay (game submission) that logged it */
/* The replay being recorded (bumped at each snapshot). A snapshot holds the updates of earlier replays, never this one's:
   "this replay's" must not be read off the fence, which a mid-replay submit (split, CPU step, present) sets too
   (2026-10-23: draws after a split read stale uniforms - the ground lost its material, the title's green backdrop). */
static uint64_t g_ub_rep = 1;
static ub_ent *g_ub; static uint64_t *g_ub_maxhi; static int g_nub, g_capub, g_ub_unflushed; static uint64_t g_ub_seq, g_ub_lo = UINT64_MAX, g_ub_hi, g_ub_maxlen, g_ub_gen = 1;   /* gen: bumped by every change a uniform binding's copy depends on (vk_ub_rename memo) */   /* maxlen: longest entry (ub_overlaps' scan window) */
int vk_off(const char *name) {
    const char *e = getenv("TOTK_VK_OFF"); if (!e || !*e) return 0;
    size_t n = strlen(name);
    for (const char *p = e; *p; ) {
        const char *q = strchr(p, ','); size_t l = q ? (size_t)(q - p) : strlen(p);
        if ((l == n && !strncmp(p, name, n)) || (l == 3 && !strncmp(p, "all", 3))) return 1;
        p += l; if (*p == ',') p++;
    }
    return 0;
}
static int ub_rename_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_UB_RENAME"); on = !(e && !strcmp(e, "0")) && !vk_off("rename"); } return on; }
static void ub_maxhi_from(int i) { for (; i < g_nub; i++) { uint64_t prev = i ? g_ub_maxhi[i - 1] : 0; g_ub_maxhi[i] = g_ub[i].hi > prev ? g_ub[i].hi : prev; } }
static uint64_t g_prune_floor = UINT64_MAX;
/* What each g_ub_gen bump changed (the vk_ub_rename memo stays valid across bumps that do not touch its range) */
/* per 4 KiB page (hashed): the last generation a change touched it; a whole-log change sets g_ub_full_gen */
enum { UBPG = 8192 };
static uint64_t g_ubpg[UBPG], g_ub_full_gen;
static void ub_changed(uint64_t lo, uint64_t hi) {
    g_ub_gen++;
    if (hi == UINT64_MAX || hi - lo > (uint64_t)UBPG << 12) { g_ub_full_gen = g_ub_gen; return; }
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) g_ubpg[(pg ^ (pg >> 13)) % UBPG] = g_ub_gen;
}
static int ub_unchanged_since(uint64_t gen, uint64_t addr, uint64_t size) {   /* no change touched [addr, addr+size) after gen */
    if (g_ub_full_gen > gen) return 0;
    for (uint64_t pg = addr >> 12; pg <= (addr + size - 1) >> 12; pg++) if (g_ubpg[(pg ^ (pg >> 13)) % UBPG] > gen) return 0;
    return 1;
}
static uint64_t g_unf_lo = UINT64_MAX, g_unf_hi;   /* bounds of the unflushed entries (vk_ub_touch's quick reject) */
static void ub_prune(void) {   /* drop entries whose submission the GPU has finished, and that no later snapshot still needs */
    uint64_t done = atomic_load(&g_done); int j = 0;
    if (done > g_prune_floor) done = g_prune_floor;   /* a submission queued after the one replaying captured no earlier than it */
    g_ub_lo = UINT64_MAX; g_ub_hi = 0; g_ub_maxlen = 0;
    for (int i = 0; i < g_nub; i++) {
        if (g_ub[i].fence && g_ub[i].fence <= done) { free(g_ub[i].data); continue; }
        g_ub[j++] = g_ub[i];
        if (g_ub[i].lo < g_ub_lo) g_ub_lo = g_ub[i].lo; if (g_ub[i].hi > g_ub_hi) g_ub_hi = g_ub[i].hi;
        if (g_ub[i].hi - g_ub[i].lo > g_ub_maxlen) g_ub_maxlen = g_ub[i].hi - g_ub[i].lo;
    }
    if (j != g_nub) { g_nub = j; ub_maxhi_from(0); ub_changed(0, UINT64_MAX); }
}
static void ub_log(uint64_t lo, const uint8_t *data, uint64_t n) {
    if (g_nub == g_capub) { g_capub = g_capub ? 2 * g_capub : 256; g_ub = realloc(g_ub, (size_t)g_capub * sizeof *g_ub); g_ub_maxhi = realloc(g_ub_maxhi, (size_t)g_capub * sizeof *g_ub_maxhi); }
    int a = 0, b = g_nub; while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo <= lo) a = m + 1; else b = m; }   /* sorted by lo */
    memmove(g_ub + a + 1, g_ub + a, (size_t)(g_nub - a) * sizeof *g_ub);
    uint8_t *d = malloc((size_t)n); memcpy(d, data, (size_t)n);
    g_ub[a] = (ub_ent){lo, lo + n, ++g_ub_seq, 0, 0, d, g_ub_rep}; g_nub++; g_ub_unflushed++; ub_changed(lo, lo + n);
    if (lo < g_unf_lo) g_unf_lo = lo; if (lo + n > g_unf_hi) g_unf_hi = lo + n;
    ub_maxhi_from(a);
    if (lo < g_ub_lo) g_ub_lo = lo; if (lo + n > g_ub_hi) g_ub_hi = lo + n;
    if (n > g_ub_maxlen) g_ub_maxlen = n;
}
static int ub_seq_cmp(const void *x, const void *y) { uint64_t a = g_ub[*(const int *)x].seq, b = g_ub[*(const int *)y].seq; return a < b ? -1 : a > b; }
/* the logged entries overlapping [lo, hi) (only_unflushed: not yet written), in update order; returns how many */
static int ub_overlaps_rep(uint64_t lo, uint64_t hi, int only_unflushed, uint64_t rep, int *idx, int cap);
static int ub_overlaps(uint64_t lo, uint64_t hi, int only_unflushed, int *idx, int cap) { return ub_overlaps_rep(lo, hi, only_unflushed, 0, idx, cap); }
/* rep: only the entries this replay logged (0: all) - filtered before the sort (TOTK, 2026-10-25: the filter ran after it, sorting earlier replays' entries too) */
static int ub_overlaps_rep(uint64_t lo, uint64_t hi, int only_unflushed, uint64_t rep, int *idx, int cap) {
    if (!g_nub || hi <= g_ub_lo || lo >= g_ub_hi) return 0;
    int a = 0, b = g_nub; while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < hi) a = m + 1; else b = m; }
    int n = 0;
    /* entries starting more than the longest entry's length before lo end before it (TOTK, 2026-10-22: with one long
       entry early in the log, the prefix maximum alone made every query scan to the start: ~9% of the CPU) */
    uint64_t stop = lo > g_ub_maxlen ? lo - g_ub_maxlen : 0;
    for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > lo && g_ub[i].lo >= stop; i--) if (g_ub[i].hi > lo && (!only_unflushed || !g_ub[i].flushed) && (!rep || g_ub[i].rep == rep)) { if (n == cap) return -1; idx[n++] = i; }
    if (n <= 12) { for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_ub[idx[j - 1]].seq > g_ub[idx[j]].seq; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; } }
    else qsort(idx, (size_t)n, sizeof *idx, ub_seq_cmp);   /* TOTK, 2026-10-23: a 64 KiB binding overlaps dozens of updates; the insertion sort was quadratic */
    return n;
}
/* Write every unflushed update into its pool, in order (outside any pass). */
static void ub_flush(void) {
    if (!g_ub_unflushed) return;
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    int cnt = 0; for (int i = 0; i < g_nub; i++) cnt += !g_ub[i].flushed;
    if (cnt != g_ub_unflushed) LOGV("【红】uniform update log: %d unflushed entries, %d counted", cnt, g_ub_unflushed);
    int *order = malloc((size_t)(cnt ? cnt : 1) * sizeof *order), n = 0;
    for (int i = 0; i < g_nub; i++) if (!g_ub[i].flushed) order[n++] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_ub[order[j - 1]].seq > g_ub[order[j]].seq; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    uint64_t prev_lo = 0, prev_hi = 0;
    for (int k = 0; k < n; k++) {
        ub_ent *e = &g_ub[order[k]]; VkBuffer db; uint64_t dof;
        if (vk_resolve(e->lo, e->hi - e->lo, &db, &dof)) {
            if (e->lo < prev_hi && prev_lo < e->hi) {   /* overlapping writes in order */
                VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
                VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
                vkCmdPipelineBarrier2(cb, &di);
            }
            vkCmdUpdateBuffer(cb, db, dof, e->hi - e->lo, e->data);
            prev_lo = e->lo; prev_hi = e->hi;
        }
        e->flushed = 1;
    }
    free(order);
    g_ub_unflushed = 0; g_unf_lo = UINT64_MAX; g_unf_hi = 0;
    vk_barrier_all(cb);
}
void vk_ub_touch(uint64_t addr, uint64_t n) {   /* a non-uniform access: the updates it may see go to memory first */
    if (!g_ub_unflushed || !n || addr >= g_unf_hi || addr + n <= g_unf_lo) return;
    int idx[1]; if (ub_overlaps(addr, addr + n, 1, idx, 1)) ub_flush();
}
void vk_ub_flush(void) { ub_flush(); }
int vk_ub_pending(uint64_t addr, uint64_t n) { int idx[1]; return g_ub_unflushed && n && addr < g_unf_hi && addr + n > g_unf_lo && ub_overlaps(addr, addr + n, 1, idx, 1) != 0; }
/* ---------- uniform snapshots at submit (nvn_mtl.m usnap) ----------
   The GPU reads a submission's uniforms when it gets to them, which here can be a few frames after the submit call,
   while the game rewrites its constant pools right after submitting (the next frame's values): objects then drew with
   the wrong frame's uniforms (black flicker). So the uniform ranges a submission binds are copied when it is submitted
   (mtl_sub_capture_ubos on the game thread with TOTK_ASYNC_REPLAY, else at the start of the replay, which then runs inside
   the submit call), merged per pool, into the submission's transient memory; bindings read those copies. Logged inline
   updates of earlier submissions that were not in memory at the capture are laid over the copies; this submission's own
   updates go through renaming on top of them. A GPU copy into a range drops its snapshot (later bindings read memory on
   the GPU timeline). TOTK_VK_OFF=snap turns this off (and rename, which it builds on). */
typedef struct { uint64_t done; int n; uint64_t *lo, *hi; size_t *off; uint8_t *bytes; } ucap_t;
typedef struct { uint64_t lo, hi; VkBuffer buf; VkDeviceSize off; uint8_t *cpu; int dead; } snap_t;
static snap_t *g_snap; static int g_nsnap, g_capsnap;
static uint64_t *g_snap_hi;   /* g_snap[i].hi, packed: snap_first's binary search stays in L1 (TOTK, 2026-10-25: ~2000 ranges of 48 bytes a submission, ~5 % of the replay in its cache misses) */
static uint64_t g_snap_gen = 1;   /* bumped by every change to the snapshot set: vk_ub_rename's memo skips snap_find while it holds */
static void snap_reset(void) { g_nsnap = 0; g_snap_gen++; }
static uint64_t g_stat_snap_bytes, g_stat_snap_ranges, g_stat_snap_binds;
static int snap_on(void) { static int on = -1; if (on < 0) on = ub_rename_on() && !vk_off("snap"); return on; }
/* GPU writes (writable storage bindings, buffer copies, texture-to-buffer copies) with the submission they belong to
   (0 while it is being recorded): a capture taken while one of them may still be executing would miss its result, so
   ranges they overlap are not copied (bindings there read memory on the GPU timeline). */
typedef struct { uint64_t lo, hi, fence; } gw_t;
static gw_t *g_gw; static int g_ngw, g_capgw; static pthread_mutex_t g_gw_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_stat_snap_skipped;
static int gw_pending(uint64_t lo, uint64_t hi, uint64_t done) {   /* caller holds g_gw_mu */
    for (int i = 0; i < g_ngw; i++) if (g_gw[i].lo < hi && lo < g_gw[i].hi && (!g_gw[i].fence || g_gw[i].fence > done)) return 1;
    return 0;
}
static void gw_seal(uint64_t fence) { pthread_mutex_lock(&g_gw_mu); for (int i = 0; i < g_ngw; i++) if (!g_gw[i].fence) g_gw[i].fence = fence; pthread_mutex_unlock(&g_gw_mu); }
static void gw_prune(void) {
    uint64_t done = atomic_load(&g_done); int j = 0;
    pthread_mutex_lock(&g_gw_mu);
    for (int i = 0; i < g_ngw; i++) if (!g_gw[i].fence || g_gw[i].fence > done) g_gw[j++] = g_gw[i];
    g_ngw = j;
    pthread_mutex_unlock(&g_gw_mu);
}
static void snap_drop(uint64_t a, uint64_t n);
void vk_gpu_write(uint64_t a, uint64_t n) {
    if (!n) return;
    vk_wb_gpu_wrote(a, n);
    /* the same range again in this submission (every material draw binds the per-tile flag buffer it writes):
       its snapshot copies are already dropped and its range already logged */
    { enum { NG = 256 }; static struct { uint64_t a, n, sub; } seen[NG]; uint64_t sub = vk_submit_serial(); unsigned h = (unsigned)((a >> 6) ^ (a >> 20) ^ n) % NG;
      if (seen[h].a == a && seen[h].n == n && seen[h].sub == sub) return;
      seen[h].a = a; seen[h].n = n; seen[h].sub = sub; }
    snap_drop(a, n);
    if (!snap_on()) return;
    pthread_mutex_lock(&g_gw_mu);
    int dup = 0; for (int j = g_ngw - 1; j >= 0 && j >= g_ngw - 8; j--) if (g_gw[j].lo == a && g_gw[j].hi == a + n && !g_gw[j].fence) { dup = 1; break; }   /* one buffer for many dispatches */
    if (!dup) { if (g_ngw == g_capgw) { g_capgw = g_capgw ? 2 * g_capgw : 1024; g_gw = realloc(g_gw, (size_t)g_capgw * sizeof *g_gw); } g_gw[g_ngw++] = (gw_t){a, a + n, 0}; }
    pthread_mutex_unlock(&g_gw_mu);
}
typedef struct { uint64_t lo, hi; } rng_t;
static int rng_cmp(const void *x, const void *y) { const rng_t *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
static void collect_ubos(rng_t **r, int *n, int *cap, const uint8_t *p, int depth) {
    for (int guard = 0; guard < 1000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) return;
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { const uint8_t *t; memcpy(&t, p + 8, 8); if (depth < 16) collect_ubos(r, n, cap, t, depth + 1); }
        else if (h.op == OP_BIND_UBO) {
            const p_bind_buf *b = (const p_bind_buf *)(p + 8);
            if (b->addr && b->size && b->size <= 65536) {
                if (*n == *cap) { *cap = *cap ? 2 * *cap : 4096; *r = realloc(*r, (size_t)*cap * sizeof **r); }
                (*r)[(*n)++] = (rng_t){b->addr, b->addr + b->size};
            }
        }
        p += h.size;
    }
}
static _Atomic uint64_t g_ucap_walk_ns, g_ucap_copy_ns, g_ucap_bytes, g_ucap_n;   /* (diagnostic) ucap_build's two halves */
static ucap_t *ucap_build(const uint64_t *h, int n) {
    uint64_t t0 = host_now_ns();
    ucap_t *c = calloc(1, sizeof *c);
    c->done = atomic_load(&g_done);   /* before the bytes: a write completing meanwhile only makes this conservative */
    rng_t *r = NULL; int nr = 0, cap = 0;
    for (int i = 0; i < n; i++) collect_ubos(&r, &nr, &cap, (const uint8_t *)(uintptr_t)h[i], 0);
    if (nr) {
        qsort(r, (size_t)nr, sizeof *r, rng_cmp);
        int k = -1;   /* merge overlapping / touching ranges inside one pool; drop ranges outside every pool */
        for (int i = 0; i < nr; i++) {
            VkBuffer b; uint64_t o;
            if (k >= 0 && r[i].lo <= r[k].hi) { if (r[i].hi <= r[k].hi) continue; if (vk_resolve(r[k].lo, r[i].hi - r[k].lo, &b, &o)) { r[k].hi = r[i].hi; continue; } }
            if (!vk_resolve(r[i].lo, r[i].hi - r[i].lo, &b, &o)) continue;
            if (k >= 0 && r[i].lo < r[k].hi) r[i].lo = r[k].hi;   /* keep the copies disjoint */
            r[++k] = r[i];
        }
        /* ranges a GPU write still in flight overlaps are left to memory (the GPU timeline) */
        pthread_mutex_lock(&g_gw_mu);
        int m = 0; for (int i = 0; i <= k; i++) { if (g_ngw && gw_pending(r[i].lo, r[i].hi, c->done)) { g_stat_snap_skipped++; continue; } r[m++] = r[i]; }
        pthread_mutex_unlock(&g_gw_mu);
        k = m - 1;
        c->n = k + 1;
        c->lo = malloc(sizeof *c->lo * (size_t)(c->n + 1)); c->hi = malloc(sizeof *c->hi * (size_t)(c->n + 1)); c->off = malloc(sizeof *c->off * (size_t)(c->n + 1));
        size_t total = 0; for (int i = 0; i < c->n; i++) { c->lo[i] = r[i].lo; c->hi[i] = r[i].hi; c->off[i] = total; total += (size_t)(r[i].hi - r[i].lo); }
        c->bytes = malloc(total ? total : 1);
        uint64_t t1 = host_now_ns();
        for (int i = 0; i < c->n; i++) memcpy(c->bytes + c->off[i], (const void *)(uintptr_t)c->lo[i], (size_t)(c->hi[i] - c->lo[i]));
        g_ucap_copy_ns += host_now_ns() - t1; g_ucap_bytes += total;
    }
    free(r);
    g_ucap_walk_ns += host_now_ns() - t0; uint64_t k = ++g_ucap_n;
    if (k % 1000 == 0) LOGV("ucap: %llu submissions: %.3f ms each (copy %.3f ms of it, %.0f KiB)", (unsigned long long)k, g_ucap_walk_ns / 1e6 / (double)k, g_ucap_copy_ns / 1e6 / (double)k, g_ucap_bytes / 1024.0 / (double)k);
    return c;
}
static void ucap_free(ucap_t *c) { if (c) { free(c->lo); free(c->hi); free(c->off); free(c->bytes); free(c); } }
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { return snap_on() ? ucap_build(h, n) : NULL; }   /* game thread (TOTK_ASYNC_REPLAY) */
static int snap_first(uint64_t a) { int lo = 0, hi = g_nsnap; while (lo < hi) { int m = (lo + hi) >> 1; if (g_snap_hi[m] <= a) lo = m + 1; else hi = m; } return lo; }
static const snap_t *snap_find(uint64_t a, uint64_t n) {   /* the live copy holding all of [a, a+n) */
    int i = snap_first(a);
    return i < g_nsnap && !g_snap[i].dead && g_snap[i].lo <= a && a + n <= g_snap[i].hi ? &g_snap[i] : NULL;
}
static void snap_drop(uint64_t a, uint64_t n) {   /* a GPU / CPU-step write into [a, a+n): those copies no longer hold the truth */
    for (int i = snap_first(a); i < g_nsnap && g_snap[i].lo < a + n; i++) { g_snap[i].dead = 1; g_snap_gen++; }
}
/* Snapshot memory (2026-10-23). Every submission of a replay reads its snapshot, so the copies cannot live in one
   slot's transient memory: a replay of more submissions than the ring has slots (TOTK_VK_SPLIT_MS=-1: ~40 a frame,
   also many CPU steps) began that slot again mid-replay, and its new transient data overwrote the snapshot the rest of
   the replay still bound (a loop count read as 1.0f's bits on the loading screen: GPU hang, device lost). The copies
   get chunks of their own, free again once the replay's last submission is done. */
typedef struct { chunk_t c; uint64_t until; } snapmem_t;   /* until: the last submission reading it (0: the replay being recorded) */
static snapmem_t *g_sm; static int g_nsm, g_capsm, g_sm_cur = -1;
static void snapmem_seal(uint64_t until) { if (g_sm_cur >= 0) { g_sm[g_sm_cur].until = until; g_sm_cur = -1; } }
static chunk_t *snapmem_take(VkDeviceSize size) {
    snapmem_seal(g_fence + 1);   /* a replay without its commit (none expected): up to whatever is open now */
    uint64_t done = atomic_load(&g_done); int f = -1;
    for (int i = 0; i < g_nsm; i++) if (g_sm[i].until && g_sm[i].until <= done && g_sm[i].c.size >= size && (f < 0 || g_sm[i].c.size < g_sm[f].c.size)) f = i;
    if (f < 0) {
        if (g_nsm == g_capsm) { g_capsm = g_capsm ? 2 * g_capsm : 8; g_sm = realloc(g_sm, (size_t)g_capsm * sizeof *g_sm); }
        f = g_nsm++; chunk_new(&g_sm[f].c, size > (2u << 20) ? (size + 0xffff) & ~(VkDeviceSize)0xffff : (2u << 20));
    }
    g_sm[f].until = 0; g_sm[f].c.used = 0; g_sm_cur = f;
    return &g_sm[f].c;
}
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *capv) {   /* the start of a replay */
    ucap_t *cap = capv;
    g_nsnap = 0; g_snap_gen++; g_ub_rep++; ub_changed(0, UINT64_MAX);   /* a new replay: the earlier ones' updates are in (or laid over) the snapshot */
    if (!snap_on()) { ucap_free(cap); return; }
    if (!cap) cap = ucap_build(h, n);   /* the replay runs inside the submit call: memory now is memory at the submit */
    g_prune_floor = cap->done;
    if (cap->n > g_capsnap) { g_capsnap = cap->n + 256; g_snap = realloc(g_snap, (size_t)g_capsnap * sizeof *g_snap); g_snap_hi = realloc(g_snap_hi, (size_t)g_capsnap * sizeof *g_snap_hi); }
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment; if (!align) align = 256;
    static int *idx; static int icap;
    VkDeviceSize need = 0; for (int i = 0; i < cap->n; i++) need += cap->hi[i] - cap->lo[i] + 2 * align;
    chunk_t *sc = cap->n ? snapmem_take(need) : NULL;
    for (int i = 0; i < cap->n; i++) {
        uint64_t lo = cap->lo[i], hi = cap->hi[i], sz = hi - lo, shift = lo % align;
        VkDeviceSize o = (sc->used + align - 1) / align * align; sc->used = o + shift + sz;
        VkBuffer b = sc->buf; uint8_t *p = sc->ptr + o;
        p += shift; o += shift;   /* the copy keeps the guest address's offset within the alignment: aligned bindings stay aligned */
        memcpy(p, cap->bytes + cap->off[i], (size_t)sz);
        /* inline updates of earlier submissions that were not in memory at the capture */
        int m; for (;;) { m = ub_overlaps(lo, hi, 0, idx, icap); if (m >= 0) break; icap = icap ? 2 * icap : 64; idx = realloc(idx, (size_t)icap * sizeof *idx); }
        for (int k = 0; k < m; k++) {
            const ub_ent *e = &g_ub[idx[k]];
            if (e->fence && e->fence <= cap->done) continue;
            uint64_t a = e->lo > lo ? e->lo : lo, z = e->hi < hi ? e->hi : hi;
            memcpy(p + (a - lo), e->data + (a - e->lo), (size_t)(z - a));
        }
        g_snap_hi[g_nsnap] = hi; g_snap[g_nsnap++] = (snap_t){lo, hi, b, o, p, 0};
        g_stat_snap_bytes += sz;
    }
    g_stat_snap_ranges += (uint64_t)g_nsnap; g_snap_gen++;
    ucap_free(cap);
}
/* A uniform binding of [addr, addr+size): its snapshot copy, or a private copy with this submission's inline updates laid
   over the snapshot (or over memory and every logged update when no snapshot holds it); 0: bind memory. */
/* The bytes a uniform binding of [addr, addr+n) shows the GPU at this point of the recording (snapshot copy or memory,
   with this submission's logged inline updates laid over): diagnostics (the pass probe). */
void vk_ub_peek(uint64_t addr, uint64_t n, void *dst) {
    const snap_t *sn = g_nsnap ? snap_find(addr, n) : NULL;
    memcpy(dst, sn ? (const void *)(sn->cpu + (addr - sn->lo)) : (const void *)(uintptr_t)addr, (size_t)n);
    if (!g_nub || addr + n <= g_ub_lo || addr >= g_ub_hi) return;
    static int *idx; static int cap; int m;
    for (;;) { m = ub_overlaps(addr, addr + n, 0, idx, cap); if (m >= 0) break; cap = cap ? 2 * cap : 64; idx = realloc(idx, (size_t)cap * sizeof *idx); }
    for (int k = 0; k < m; k++) {
        const ub_ent *e = &g_ub[idx[k]]; if (sn && e->rep != g_ub_rep) continue;
        uint64_t lo = e->lo > addr ? e->lo : addr, hi = e->hi < addr + n ? e->hi : addr + n;
        memcpy((uint8_t *)dst + (lo - addr), e->data + (lo - e->lo), (size_t)(hi - lo));
    }
}
static int ub_rename_slow(uint64_t addr, uint64_t size, const snap_t *sn, VkDescriptorBufferInfo *out);
int vk_ub_rename(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    const snap_t *sn = NULL;
    /* Memo (TOTK, 2026-10-22: the overlap search was ~10% of the CPU, for ~15k uniform bindings a frame against ~100
       logged updates): with the same range, snapshot and submission and no change to the log since, the answer is the
       same as last time. */
    enum { NM = 4096 };
    static struct { uint64_t addr, size, gen, sub, sgen; const void *sn; int ret; VkDescriptorBufferInfo d; } memo[NM];
    uint64_t msub = vk_submit_serial(); unsigned mh = (unsigned)((addr >> 4) ^ (addr >> 14) ^ (size * 2654435761u)) % NM;
    int same = memo[mh].addr == addr && memo[mh].size == size && memo[mh].sub == msub;
    /* the snapshot set unchanged since the memo: the same snapshot holds the range (TOTK, 2026-10-25: snap_find's binary
       search over ~2000 ranges was most of this function's time) */
    if (same && memo[mh].sgen == g_snap_gen) sn = memo[mh].sn;
    else { sn = g_nsnap ? snap_find(addr, size) : NULL; same = same && memo[mh].sn == (const void *)sn; if (same) memo[mh].sgen = g_snap_gen; }
    if (same) {
        /* log changes since the memo that do not overlap this range do not change the answer (TOTK, 2026-10-23: each of
           the 100+ updates a submission bumped the generation and voided every memo: ub_overlaps was still ~13%).
           TOTK_VK_OFF=ubmemo: exact generation only (the old rule). */
        int ok = memo[mh].gen == g_ub_gen;
        static int exact = -1; if (exact < 0) exact = vk_off("ubmemo");
        if (!ok && !exact && ub_unchanged_since(memo[mh].gen, addr, size)) { ok = 1; memo[mh].gen = g_ub_gen; }
        if (ok) { if (memo[mh].ret) *out = memo[mh].d; return memo[mh].ret; }
    }
    int ret = ub_rename_slow(addr, size, sn, out);
    memo[mh].addr = addr; memo[mh].size = size; memo[mh].gen = g_ub_gen; memo[mh].sub = msub; memo[mh].sn = sn; memo[mh].sgen = g_snap_gen; memo[mh].ret = ret; if (ret) memo[mh].d = *out;
    return ret;
}
static int ub_rename_slow(uint64_t addr, uint64_t size, const snap_t *sn, VkDescriptorBufferInfo *out) {
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment; if (!align) align = 256;
    int n = 0;
    static int *idx; static int cap;
    if (g_nub && addr + size > g_ub_lo && addr < g_ub_hi) {
        { static uint64_t last_done = UINT64_MAX; uint64_t d = atomic_load(&g_done); if (d != last_done) { last_done = d; ub_prune(); } }   /* prune only when the GPU moved on */
        /* with a snapshot, only this replay's updates: the snapshot already holds earlier replays' */
        for (;;) { n = ub_overlaps_rep(addr, addr + size, 0, sn ? g_ub_rep : 0, idx, cap); if (n >= 0) break; cap = cap ? 2 * cap : 64; idx = realloc(idx, (size_t)cap * sizeof *idx); }
    }
    if (!n) {
        if (!sn) return 0;
        VkDeviceSize o = sn->off + (addr - sn->lo);
        if (o % align == 0) { *out = (VkDescriptorBufferInfo){sn->buf, o, size}; g_stat_snap_binds++; return 1; }
    }
    /* the same range with the same newest update in this submission: the copy made for it is still right */
    enum { NC = 256 };
    static struct { uint64_t addr, size, seq, sub; const void *base; VkDescriptorBufferInfo d; } cache[NC];
    uint64_t newest = n ? g_ub[idx[n - 1]].seq : 0, sub = vk_submit_serial();
    unsigned h = (unsigned)((addr >> 4) ^ (addr >> 16) ^ size) % NC;
    if (cache[h].addr == addr && cache[h].size == size && cache[h].seq == newest && cache[h].sub == sub && cache[h].base == (const void *)sn) { *out = cache[h].d; return 1; }
    VkBuffer tb; VkDeviceSize to;
    uint8_t *p = vk_transient(size, align, &tb, &to);
    memcpy(p, sn ? (const void *)(sn->cpu + (addr - sn->lo)) : (const void *)(uintptr_t)addr, (size_t)size);
    for (int k = 0; k < n; k++) {
        const ub_ent *e = &g_ub[idx[k]];
        uint64_t lo = e->lo > addr ? e->lo : addr, hi = e->hi < addr + size ? e->hi : addr + size;
        memcpy(p + (lo - addr), e->data + (lo - e->lo), (size_t)(hi - lo));
    }
    *out = (VkDescriptorBufferInfo){tb, to, size};
    cache[h].addr = addr; cache[h].size = size; cache[h].seq = newest; cache[h].sub = sub; cache[h].base = sn; cache[h].d = *out;
    g_stat_ub_renamed++; g_stat_ub_renamed_bytes += size;
    return 1;
}
static void update_ub(uint64_t dst, const uint8_t *data, uint64_t n) {
    VkBuffer db; uint64_t dof;
    int ok = vk_resolve(dst, n, &db, &dof);
    if (!ok || (dof & 3) || (n & 3) || n > 65536 || !n) { vk_cpu_step(); memcpy((void *)(uintptr_t)dst, data, (size_t)n); vk_gpu_write(dst, n); return; }
    g_stat_ub++;
    if (ub_rename_on()) { ub_log(dst, data, n); return; }
    vk_end_rendering();
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    vkCmdUpdateBuffer(cb, db, dof, n, data);
    vk_barrier_all(cb);
}
static void post(post_op o) {
    if (g_ncarry == g_capcarry) { g_capcarry = g_capcarry ? 2 * g_capcarry : 64; g_carry = realloc(g_carry, (size_t)g_capcarry * sizeof *g_carry); }
    g_carry[g_ncarry++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) {   /* after vk_draw_exec recorded it */
    post_op o = {2, NULL, 0, 0, addr, type, vk_counter_samples(addr)};
    /* TOTK_SYNC_EARLY (hle_nvn.c): the game's fence no longer waits for this submission's GPU, so a report it reads
       after the fence is written now (its value is fixed already; the timestamp is the translation time) */
    extern int nvn_sync_early(void) __attribute__((weak));
    if (nvn_sync_early && nvn_sync_early()) apply_posts(&o, 1); else post(o);
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
void vk_note_draw(void) { g_stat_draw++; if (g_cur) { g_cur->ndraw++; if (g_cur->nnote) g_cur->note[(g_cur->nnote - 1) % 8].draws++; } }
void vk_sub_note(char kind, const char *k0, const char *k1, uint32_t a, uint32_t b, uint32_t c) {
    if (!g_cur) slot_begin();
    g_cur->note[g_cur->nnote++ % 8] = (vk_sub_note_t){kind, k0, k1, a, b, c, 0};
}
static void ub_seal(uint64_t fence) { for (int i = 0; i < g_nub; i++) if (g_ub[i].flushed && !g_ub[i].fence) g_ub[i].fence = fence; ub_changed(0, UINT64_MAX); }

void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) DIE("【红】corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); vk_draw_exec(h.op, a); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); update_ub(d, a + 16, n); break; }
            default: vk_draw_exec(h.op, a); break;
        }
        p += h.size;
    }
}
uint64_t mtl_sub_commit(void) {
    mtl_init();
    uint64_t v = slot_submit(1);
    snapmem_seal(v);
    g_stat_sub++;
    /* TOTK_VK_SYNC=1 (diagnostic): every submission finishes on the GPU before the game goes on, as if the GPU were
       infinitely fast; whatever still differs between frames then is not a race with the GPU. */
    { static int sync = -1; if (sync < 0) { const char *e = getenv("TOTK_VK_SYNC"); sync = e && atoi(e) > 0; if (sync) LOGV("TOTK_VK_SYNC: each submission is waited for"); }
      if (sync) mtl_wait(v, 5000000000ull); }
    ub_prune(); gw_prune();
    if (g_perf_t0) { uint64_t dt = host_now_ns() - g_perf_t0, w = g_perf_wait_ns - g_perf_sub0; g_perf_replay_ns += dt > w ? dt - w : 0; g_perf_t0 = 0;
        HK_ADD(subs, 1); HK_ADD(replay_ns, dt > w ? dt - w : 0); HK_ADD(wait_ns, w); g_perf_subs++; }
    vk_rec_unlock();   /* taken in mtl_sub_begin */
    vk_res_sweep();
    if (g_stat_sub % 3000 == 0) {
        uint64_t n, b; gpu_pool_totals(&n, &b);
        LOGV("uniform snapshots: %.1f KiB in %.0f ranges per submission, %llu bindings read them, %llu ranges left to memory (GPU writes in flight)", g_stat_snap_bytes / 1024.0 / g_stat_sub, (double)g_stat_snap_ranges / g_stat_sub, (unsigned long long)g_stat_snap_binds, (unsigned long long)g_stat_snap_skipped);
        LOGV("%llu submissions: %llu GPU copies, %llu uniform updates (%llu renamed copies, %.1f KiB each on average), %llu CPU steps; %llu pools (%llu MiB on the GPU, %llu CPU-only)",
             (unsigned long long)g_stat_sub, (unsigned long long)g_stat_gpu_copy, (unsigned long long)g_stat_ub, (unsigned long long)g_stat_ub_renamed, g_stat_ub_renamed ? g_stat_ub_renamed_bytes / 1024.0 / g_stat_ub_renamed : 0.0, (unsigned long long)g_stat_cpu_step,
             (unsigned long long)n, (unsigned long long)(b >> 20), (unsigned long long)g_pool_cpu);
        vk_res_stats();
    }
    return v;
}
/* A submission of the backend's own (present, CPU reads): whatever is recorded goes to the GPU and is waited for. */
/* Memory the backend holds (TOTK, 2026-10-22: the app's memory grew ~45 MB/s in the world, native heap and GPU-mapped
   memory together, until Android ran out). Logged with the per-300-frame lines: whichever number climbs is the leak. */
void vk_res_mem(uint64_t v[8]);
int vk_dpool_count(void) { int n = 0; for (int i = 0; i < RING; i++) n += g_ring[i].ndpool; return n; }
void vk_mem_report(void) {
    uint64_t chunk = 0, nchunk = 0, ndp = 0, posts = 0;
    for (int i = 0; i < RING; i++) { slot_t *s = &g_ring[i]; nchunk += (uint64_t)s->nchunk; ndp += (uint64_t)s->ndpool; posts += (uint64_t)s->cap; for (int c = 0; c < s->nchunk; c++) chunk += s->chunks[c].size; }
    uint64_t pn, pb; gpu_pool_totals(&pn, &pb);
    uint64_t r[8]; vk_res_mem(r);
#ifdef __GLIBC__
    struct mallinfo2 mi = mallinfo2();
#else
    struct mallinfo mi = mallinfo();
#endif
    LOGV("memory: malloc %zu MiB in use; image blocks %llu MiB + dedicated %llu MiB, %llu images retired waiting, %llu texture objects, %llu shared images, %llu write-back records, %llu retired list capacity, %llu idle images swept so far; "
         "pools %llu (%llu MiB); transient chunks %llu (%llu MiB), descriptor pools %llu, post-op capacity %llu; uniform log %d (cap %d), snapshots %d (cap %d), GPU-write ranges %d (cap %d)",
         (size_t)mi.uordblks >> 20, (unsigned long long)(r[0] >> 20), (unsigned long long)(r[1] >> 20), (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5], (unsigned long long)r[6], (unsigned long long)r[7],
         (unsigned long long)pn, (unsigned long long)(pb >> 20), (unsigned long long)nchunk, (unsigned long long)(chunk >> 20), (unsigned long long)ndp, (unsigned long long)posts,
         g_nub, g_capub, g_nsnap, g_capsnap, g_ngw, g_capgw);
}
uint64_t vk_submit_now(void) { return g_cur ? slot_submit(0) : mtl_last_fence(); }
void vk_flush_wait(void) { HK_ADD(idle_flush, 1); if (g_cur) { uint64_t v = slot_submit(0); wait_value(v); } else vk_gpu_idle(); }
uint64_t mtl_last_fence(void) { pthread_mutex_lock(&g_mu); uint64_t f = g_fence; pthread_mutex_unlock(&g_mu); return f; }
static _Atomic uint64_t g_gwait_guest_ns, g_gwait_guest_n, g_gwait_core_ns;
uint64_t vk_guest_wait_take(uint64_t *n, uint64_t *core_ns) { *n = atomic_exchange(&g_gwait_guest_n, 0); *core_ns = atomic_exchange(&g_gwait_core_ns, 0); return atomic_exchange(&g_gwait_guest_ns, 0); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    if (!timeout_ns) return 0;
    uint64_t t0 = host_now_ns();
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000u); dl.tv_nsec = (long)(ns % 1000000000u);
    pthread_mutex_lock(&g_mu);
    while (atomic_load(&g_done) < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &dl)) break;
    int ok = atomic_load(&g_done) >= fence;
    HK_ADD(wake_ns, host_now_ns() - g_done_at);   /* from the completion store to the waiter running */
    pthread_mutex_unlock(&g_mu);
    /* any thread blocked on GPU completion (the game's fences and present waits with async replay) */
    uint64_t dt = host_now_ns() - t0;
    HK_ADD(gwaits, 1); HK_ADD(gwait_ns, dt);
    {   /* perf summary (vk_draw.c): guest threads only, and the part spent holding a virtual core (others could not use it) */
        extern uint32_t runtime_thread_current_handle(void) __attribute__((weak)); extern int runtime_core_owned(void) __attribute__((weak));
        if (runtime_thread_current_handle && runtime_thread_current_handle()) {
            atomic_fetch_add(&g_gwait_guest_ns, dt); atomic_fetch_add(&g_gwait_guest_n, 1);
            if (runtime_core_owned && runtime_core_owned()) atomic_fetch_add(&g_gwait_core_ns, dt);
        }
    }
    return ok;
}
void mtl_finish(void) { HK_ADD(idle_fin, 1); vk_gpu_idle(); }   /* the game's nvnQueueFinish: everything done */
void mtl_drain(void) { vk_gpu_idle(); }
uint64_t gpu_stat_submits(void) { return g_stat_sub; }
uint64_t gpu_stat_draws(void) { return g_stat_draw; }
