// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// Single source of truth for the module version.  Previously the version
// macros lived only in NukeXModule.cpp, so any other translation unit that
// wanted to emit the version (e.g. FITS keyword provenance in
// NukeXInstance's output windows) had to either hardcode or wire up a
// linkage path.  Centralising here keeps bump discipline simple: update
// these five macros, and every consumer picks it up.

#ifndef __NukeXVersion_h
#define __NukeXVersion_h

#define NUKEX_MODULE_VERSION_MAJOR     4
#define NUKEX_MODULE_VERSION_MINOR     0
#define NUKEX_MODULE_VERSION_REVISION  1
#define NUKEX_MODULE_VERSION_BUILD     0

#define NUKEX_MODULE_RELEASE_YEAR      2026
#define NUKEX_MODULE_RELEASE_MONTH     4
#define NUKEX_MODULE_RELEASE_DAY       25

#define NUKEX_STR_HELPER(x) #x
#define NUKEX_STR(x) NUKEX_STR_HELPER(x)

#define NUKEX_VERSION_STRING \
    NUKEX_STR(NUKEX_MODULE_VERSION_MAJOR) "." \
    NUKEX_STR(NUKEX_MODULE_VERSION_MINOR) "." \
    NUKEX_STR(NUKEX_MODULE_VERSION_REVISION) "." \
    NUKEX_STR(NUKEX_MODULE_VERSION_BUILD)

#endif // __NukeXVersion_h
