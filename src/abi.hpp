#pragma once
#ifndef _WIN32
#ifndef __cdecl
#define __cdecl
#endif
#endif
#include "../third_party/vst2sdk/vst.h"
#include <cstddef>
#include <type_traits>
// This release deliberately supports 64-bit hosts only.
static_assert(sizeof(void*)==8, "Build a 64-bit plugin");
static_assert(offsetof(vst_effect_t,control)==8);
static_assert(offsetof(vst_effect_t,effect_internal)==96);
static_assert(offsetof(vst_effect_t,process_float)==120);
static_assert(sizeof(vst_effect_t)==192);
