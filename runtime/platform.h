/* Target macros shared by the runtime. Darwin: Apple's TargetConditionals. Elsewhere (Linux / Android) the Apple
   macros are 0, so "#if TARGET_OS_IPHONE ... #else" keeps its desktop branch (Android maps and patches code at run
   time like the Mac does) and "#if TARGET_OS_OSX" Mac-only features stay off. TOTK_ANDROID / TOTK_LINUX name the new
   hosts. */
#pragma once
#if defined(__APPLE__)
#include <TargetConditionals.h>
#define TOTK_ANDROID 0
#define TOTK_LINUX 0
#else
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 0
#endif
#ifndef TARGET_OS_MAC
#define TARGET_OS_MAC 0
#endif
#if defined(__ANDROID__)
#define TOTK_ANDROID 1
#else
#define TOTK_ANDROID 0
#endif
#define TOTK_LINUX 1   /* Linux kernel ABI: Android and desktop Linux */
#endif

/* Tables collected by the linker from every object (graphics overrides, NVN entry points):
   TOTK_SECTION(name) is the attribute argument for an entry, TOTK_SECTION_TABLE(name, Type, begin, count) finds them.
   Mach-O keeps them in __DATA,<name>; ELF in <name>, bounded by the linker's __start_<name> / __stop_<name>. */
#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#define TOTK_SECTION(name) section("__DATA," #name)
#define TOTK_SECTION_TABLE(name, Type, begin, count) do { unsigned long size_ = 0; \
    (begin) = (const Type *)(const void *)getsectiondata(&_mh_execute_header, "__DATA", #name, &size_); \
    (count) = (begin) ? (size_t)(size_ / sizeof(Type)) : 0; } while (0)
#else
#define TOTK_SECTION(name) section(#name)
#define TOTK_SECTION_TABLE(name, Type, begin, count) do { \
    extern const Type __start_##name[] __attribute__((weak, visibility("hidden"))); \
    extern const Type __stop_##name[] __attribute__((weak, visibility("hidden"))); \
    (begin) = __start_##name; (count) = __start_##name ? (size_t)(__stop_##name - __start_##name) : 0; } while (0)
#endif
