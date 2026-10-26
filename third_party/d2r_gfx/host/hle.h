#pragma once
#include "dm.h"
#include "../../../runtime/platform.h"   /* TOTK_SECTION: Mach-O __DATA,x or ELF x */
typedef struct {const char *name;void *fn;const char *why;} hle_force_ent;
#define HLE_FORCE(mangled,reason,ret,cname,...) static ret cname(__VA_ARGS__); __attribute__((used,TOTK_SECTION(totkgfx))) static const hle_force_ent ent_##cname={mangled,(void *)cname,reason}; static ret cname(__VA_ARGS__)
