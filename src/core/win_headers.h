#pragma once

// Single entry point for <windows.h> so every translation unit gets the same
// feature macros. NOMINMAX and WIN32_LEAN_AND_MEAN are also defined by the
// MinGW libstdc++ headers, so they must not be redefined unconditionally.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
