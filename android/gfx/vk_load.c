/* See vk_load.h. */
#include "vk_load.h"
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

/* The Android HAL structures (hardware/libhardware/include/hardware/hardware.h and hwvulkan.h), declared here so the
   build needs no platform headers. Layouts are the LP64 ones. */
typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t {
    uint32_t tag, version;
    hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *);
} hw_device_t;
typedef struct { int (*open)(const hw_module_t *, const char *id, hw_device_t **); } hw_module_methods_t;
struct hw_module_t {
    uint32_t tag; uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};
typedef struct {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;
#define HWVULKAN_DEVICE_0 "vk0"

#ifdef __ANDROID__
/* Inside an app, libraries resolve in the app's linker namespace, which sees only the NDK's public system libraries.
   A Vulkan driver built for the system image (a Turnip HAL module) needs others too (libhardware.so, libcutils.so ...).
   With TOTK_VK_LIBDIR set (frontend_android.c: a folder in the app's private storage), every library the driver needs
   that this namespace cannot load is copied there from the system partitions, its own needs first, and loaded from
   there; the driver's lookups by name then find the copies already loaded. */
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int elf_needed(const char *path, char names[][128], int max) {   /* DT_NEEDED entries of an ELF64 file */
    int fd = open(path, O_RDONLY | O_CLOEXEC), n = 0; if (fd < 0) return -1;
    Elf64_Ehdr eh; Elf64_Phdr ph[64];
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_phnum > 64
        || pread(fd, ph, sizeof(Elf64_Phdr) * eh.e_phnum, (off_t)eh.e_phoff) != (ssize_t)(sizeof(Elf64_Phdr) * eh.e_phnum)) { close(fd); return -1; }
    const Elf64_Phdr *dyn = NULL; for (int i = 0; i < eh.e_phnum; i++) if (ph[i].p_type == PT_DYNAMIC) dyn = &ph[i];
    if (!dyn || dyn->p_filesz > (1u << 20)) { close(fd); return 0; }
    Elf64_Dyn *d = malloc(dyn->p_filesz); size_t nd = dyn->p_filesz / sizeof *d;
    if (pread(fd, d, dyn->p_filesz, (off_t)dyn->p_offset) != (ssize_t)dyn->p_filesz) { free(d); close(fd); return -1; }
    uint64_t strtab = 0, strsz = 0; for (size_t i = 0; i < nd; i++) { if (d[i].d_tag == DT_STRTAB) strtab = d[i].d_un.d_ptr; if (d[i].d_tag == DT_STRSZ) strsz = d[i].d_un.d_val; }
    off_t stroff = -1; for (int i = 0; i < eh.e_phnum; i++) if (ph[i].p_type == PT_LOAD && strtab >= ph[i].p_vaddr && strtab < ph[i].p_vaddr + ph[i].p_filesz) stroff = (off_t)(strtab - ph[i].p_vaddr + ph[i].p_offset);
    char *str = stroff >= 0 && strsz && strsz < (8u << 20) ? malloc(strsz) : NULL;
    if (str && pread(fd, str, strsz, stroff) == (ssize_t)strsz)
        for (size_t i = 0; i < nd && n < max; i++) if (d[i].d_tag == DT_NEEDED && d[i].d_un.d_val < strsz) { snprintf(names[n], 128, "%s", str + d[i].d_un.d_val); n++; }
    free(str); free(d); close(fd);
    return n;
}
static int copy_file(const char *src, const char *dst) {
    struct stat a, b; if (stat(src, &a)) return 0;
    if (!stat(dst, &b) && b.st_size == a.st_size) return 1;   /* copied at an earlier start */
    int in = open(src, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755), ok = in >= 0 && out >= 0;
    char buf[1 << 16]; ssize_t r;
    while (ok && (r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)r) != r) ok = 0;
    if (in >= 0) close(in); if (out >= 0) close(out);
    return ok;
}
static void preload_needs(const char *path, const char *dir, int depth) {
    char need[64][128]; int n = elf_needed(path, need, 64);
    for (int i = 0; i < n; i++) {
        const char *name = need[i];
        if (dlopen(name, RTLD_NOW | RTLD_NOLOAD)) continue;
        if (!strcmp(name, "ld-android.so")) continue;   /* the linker's own stand-in: its functions trap outside the platform namespace */
        void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL); if (h) continue;   /* a public library */
        static const char *const from[] = {"/system/lib64", "/system_ext/lib64", "/vendor/lib64", "/odm/lib64", "/apex/com.android.vndk.v33/lib64", "/apex/com.android.vndk.current/lib64"};
        char src[512] = "", dst[512]; struct stat st;
        for (unsigned k = 0; k < sizeof from / sizeof *from && !src[0]; k++) { char p[512]; snprintf(p, sizeof p, "%s/%s", from[k], name); if (!stat(p, &st)) snprintf(src, sizeof src, "%s", p); }
        if (!src[0]) { fprintf(stderr, "[nvn-vk] driver dependency %s: not found on the system partitions\n", name); continue; }
        snprintf(dst, sizeof dst, "%s/%s", dir, name);
        if (!copy_file(src, dst)) { fprintf(stderr, "[nvn-vk] driver dependency %s: copying %s failed\n", name, src); continue; }
        if (depth < 8) preload_needs(dst, dir, depth + 1);
        if (!dlopen(dst, RTLD_NOW | RTLD_GLOBAL)) fprintf(stderr, "[nvn-vk] driver dependency %s (from %s): %s\n", name, src, dlerror());
        else fprintf(stderr, "[nvn-vk] driver dependency %s loaded from a copy of %s\n", name, src);
    }
}
#endif

PFN_vkGetInstanceProcAddr vk_load_driver(const char *path, const char **kind, const char **why) {
    *kind = "?"; *why = NULL;
    void *lib;
    if (!path) {
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) { *why = dlerror(); return NULL; }
        *kind = "system loader";
        PFN_vkGetInstanceProcAddr g = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
        if (!g) *why = "libvulkan has no vkGetInstanceProcAddr";
        return g;
    }
#ifdef __ANDROID__
    { const char *dir = getenv("TOTK_VK_LIBDIR"); if (dir && *dir) { mkdir(dir, 0755); preload_needs(path, dir, 0); } }
#endif
    lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { *why = dlerror(); return NULL; }

    PFN_vkGetInstanceProcAddr g = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (g) {
        VkResult (*neg)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
        if (neg) { uint32_t v = 5; neg(&v); }
        *kind = "ICD";
        return g;
    }
    g = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (g) { *kind = "library"; return g; }

    hw_module_t *m = (hw_module_t *)dlsym(lib, "HMI");
    if (m) {
        hw_device_t *d = NULL;
        if (!m->methods || !m->methods->open) { *why = "HAL module without an open method"; return NULL; }
        if (m->methods->open(m, HWVULKAN_DEVICE_0, &d) || !d) { *why = "HAL module refused to open device vk0"; return NULL; }
        *kind = "Android HAL module";
        g = ((hwvulkan_device_t *)d)->GetInstanceProcAddr;
        if (!g) *why = "HAL device has no GetInstanceProcAddr";
        return g;
    }
    *why = "no vk_icdGetInstanceProcAddr, vkGetInstanceProcAddr or HMI symbol";
    return NULL;
}
