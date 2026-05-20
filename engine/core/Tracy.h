// SPDX-License-Identifier: MIT
// Psynder — Tracy profiler wrapper. Header-only; expands to either real
// Tracy macros or no-op stubs depending on `PSYNDER_ENABLE_TRACY`.
//
// Usage:
//   #include "core/Tracy.h"
//   void run_frame() {
//       PSY_ZONE_FRAME();            // marks one full frame
//       {
//           PSY_ZONE("vertex transform");
//           // ...
//       }
//   }
//
// When PSYNDER_ENABLE_TRACY is off (the default), every macro collapses
// to `do {} while(0)`. The compiler eliminates the call sites entirely,
// so leaving zones sprinkled through the codebase costs nothing.
//
// When PSYNDER_ENABLE_TRACY is on, the macros forward to <tracy/Tracy.hpp>:
//   PSY_ZONE(name)      -> ZoneScopedN(name)
//   PSY_ZONE_FRAME()    -> FrameMark
//   PSY_ZONE_ALLOC(p,n) -> TracyAlloc(p,n) for allocator instrumentation
//   PSY_ZONE_FREE(p)    -> TracyFree(p)
//   PSY_ZONE_MSG(s,n)   -> TracyMessage(s,n)
//
// Richer vocabulary (also no-op when the toggle is off):
//   PSY_ZONE_C(name, rgb)         -> ZoneScopedNC(name, rgb)   colored zone
//   PSY_ZONE_NAMED_C(v, name, c)  -> ZoneNamedNC(v, name, c, 1) handle-named
//   PSY_ZONE_TEXT(txt, n)         -> ZoneText(txt, n)   annotate enclosing zone
//   PSY_ZONE_VALUE(u64)           -> ZoneValue(u64)     "     "        "
//   PSY_ZONE_PLOT_CONFIG(...)     -> TracyPlotConfig(...) plot axis/format
//   PSY_ZONE_MSG_C(s, n, rgb)     -> TracyMessageC(s, n, rgb)
//   PSY_ZONE_APP_INFO(s, n)       -> TracyAppInfo(s, n)  capture banner
//   PSY_ZONE_THREAD(name)         -> tracy::SetThreadName(name)
//   PSY_ZONE_FIBER(name)/_LEAVE   -> TracyFiberEnter/Leave (jobs fibers)
//
// PSY_ZONE_TEXT / PSY_ZONE_VALUE annotate the innermost zone in the same
// lexical scope, so they must follow a PSY_ZONE / PSY_ZONE_C in that scope.
//
// The tracy CMake option lives in the root CMakeLists.txt; this header
// just keys off the preprocessor define so callers don't need to thread
// CMake conditionals through their own subdirectory.

#pragma once

#if defined(PSYNDER_ENABLE_TRACY) && PSYNDER_ENABLE_TRACY

#   include <tracy/Tracy.hpp>

#   define PSY_ZONE(name)             ZoneScopedN(name)
#   define PSY_ZONE_NAMED(var, name)  ZoneNamedN(var, name, true)
#   define PSY_ZONE_FRAME()           FrameMark
#   define PSY_ZONE_FRAME_NAMED(name) FrameMarkNamed(name)
#   define PSY_ZONE_PLOT(name, value) TracyPlot(name, value)
#   define PSY_ZONE_MSG(str, n)       TracyMessage(str, n)
#   define PSY_ZONE_ALLOC(ptr, size)  TracyAlloc(ptr, size)
#   define PSY_ZONE_FREE(ptr)         TracyFree(ptr)

#   define PSY_ZONE_C(name, color)            ZoneScopedNC(name, color)
#   define PSY_ZONE_NAMED_C(var, name, color) ZoneNamedNC(var, name, color, true)
#   define PSY_ZONE_TEXT(str, n)              ZoneText(str, n)
#   define PSY_ZONE_VALUE(value)              ZoneValue(value)
#   define PSY_ZONE_PLOT_CONFIG(name, type, step, fill, color)                 \
        TracyPlotConfig(name, type, step, fill, color)
#   define PSY_ZONE_MSG_C(str, n, color)      TracyMessageC(str, n, color)
#   define PSY_ZONE_APP_INFO(str, n)          TracyAppInfo(str, n)
#   define PSY_ZONE_THREAD(name)              ::tracy::SetThreadName(name)
#   define PSY_ZONE_FIBER(name)               TracyFiberEnter(name)
#   define PSY_ZONE_FIBER_LEAVE()             TracyFiberLeave

#else

#   define PSY_ZONE(name)             do {} while (0)
#   define PSY_ZONE_NAMED(var, name)  do {} while (0)
#   define PSY_ZONE_FRAME()           do {} while (0)
#   define PSY_ZONE_FRAME_NAMED(name) do {} while (0)
#   define PSY_ZONE_PLOT(name, value) do {} while (0)
#   define PSY_ZONE_MSG(str, n)       do {} while (0)
#   define PSY_ZONE_ALLOC(ptr, size)  do {} while (0)
#   define PSY_ZONE_FREE(ptr)         do {} while (0)

#   define PSY_ZONE_C(name, color)            do {} while (0)
#   define PSY_ZONE_NAMED_C(var, name, color) do {} while (0)
#   define PSY_ZONE_TEXT(str, n)              do {} while (0)
#   define PSY_ZONE_VALUE(value)              do {} while (0)
#   define PSY_ZONE_PLOT_CONFIG(name, type, step, fill, color) do {} while (0)
#   define PSY_ZONE_MSG_C(str, n, color)      do {} while (0)
#   define PSY_ZONE_APP_INFO(str, n)          do {} while (0)
#   define PSY_ZONE_THREAD(name)              do {} while (0)
#   define PSY_ZONE_FIBER(name)               do {} while (0)
#   define PSY_ZONE_FIBER_LEAVE()             do {} while (0)

#endif
