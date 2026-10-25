/* Placeholder for gen/exe_table.c in an "app-only" CI build.
 *
 * The real file is produced by tools/lift.py from the user's own USM.exe and
 * must never be committed or published (see README, "Legal"). This stub has no
 * functions, so the app builds, installs and its launcher works, but starting
 * the game stops with a clear message instead of running. */
#include "rt_table.h"
const FnEntry g_fn_table_exe[] = {{0u, 0}};
const unsigned g_fn_count_exe = 0;
