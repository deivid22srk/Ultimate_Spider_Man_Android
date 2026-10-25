#pragma once
#include "x86.h"
typedef struct { uint32_t addr; GuestFn fn; } FnEntry;
/* One table per recompiled module (exe, d3dx9_35, binkw32); see modules.c. */
typedef struct { const char *name; const FnEntry *table; unsigned count; } FnModule;
extern const FnModule g_modules[];
extern const unsigned g_nmodules;
