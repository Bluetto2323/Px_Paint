// Paint Engine - fault-injection / timing hooks for host tests.
//
// Compiled in only when PE_ENABLE_TEST_HOOKS is defined (the host test
// build). In the Android build PE_TEST_HOOK(name) is the constant `false`,
// so the hooks cost nothing and cannot be triggered.
//
// A hook receives the name of the point being crossed and returns true to
// request a fault at that point (what "fault" means is documented where the
// point is used). It may also block to widen a timing window.
//
// Points currently used:
//   "engine.frame.recorded"     after the scene was recorded, before endFrame
//   "engine.frame.device_lost"  return true -> frame reports DeviceLost
//   "engine.frame.throw"        return true -> render thread throws
//   "surface.rebuild"           inside a swapchain / EGL surface rebuild
#pragma once

#if defined(PE_ENABLE_TEST_HOOKS)
#include <functional>

namespace pe::testhooks {
using Hook = std::function<bool(const char* point)>;
void install(Hook hook);  // nullptr removes it
bool fire(const char* point);
}  // namespace pe::testhooks

#define PE_TEST_HOOK(point) ::pe::testhooks::fire(point)
#else
#define PE_TEST_HOOK(point) false
#endif
