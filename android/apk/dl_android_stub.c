/* libdl_android.so for the app's own linker namespace (bundled in the APK).
   The Vulkan driver's system dependencies (libhardware.so -> libvndksupport.so) need libdl_android.so, the platform's
   linker-namespace API. An app cannot load the real one, and a copy of it calls into ld-android.so, whose functions
   trap outside the platform's own namespace. These answers say "no namespaces here": libvndksupport then loads vendor
   libraries with plain dlopen (which fails for gralloc HALs; Mesa falls back to its own gralloc handling, as it
   already does when the driver runs from adb shell). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void *android_get_exported_namespace(const char *name) { (void)name; return NULL; }
void *android_create_namespace(const char *name, const char *ld_library_path, const char *default_library_path, uint64_t type,
                               const char *permitted_when_isolated_path, void *parent) {
    (void)name; (void)ld_library_path; (void)default_library_path; (void)type; (void)permitted_when_isolated_path; (void)parent; return NULL;
}
bool android_link_namespaces(void *from, void *to, const char *shared_libs_sonames) { (void)from; (void)to; (void)shared_libs_sonames; return false; }
bool android_link_namespaces_all_libs(void *from, void *to) { (void)from; (void)to; return false; }
bool android_init_anonymous_namespace(const char *shared_libs_sonames, const char *library_search_path) { (void)shared_libs_sonames; (void)library_search_path; return false; }
void android_get_LD_LIBRARY_PATH(char *buffer, size_t buffer_size) { if (buffer && buffer_size) buffer[0] = 0; }
void android_update_LD_LIBRARY_PATH(const char *ld_library_path) { (void)ld_library_path; }
void android_dlwarning(void *obj, void (*f)(void *, const char *)) { (void)obj; (void)f; }
void android_set_application_target_sdk_version(int target) { (void)target; }
