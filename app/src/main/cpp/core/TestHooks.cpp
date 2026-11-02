#include "core/TestHooks.h"

#if defined(PE_ENABLE_TEST_HOOKS)
#include <memory>
#include <mutex>

namespace pe::testhooks {
namespace {
std::mutex gMutex;
std::shared_ptr<Hook> gHook;
}  // namespace

void install(Hook hook) {
    std::lock_guard<std::mutex> lock(gMutex);
    gHook = hook ? std::make_shared<Hook>(std::move(hook)) : nullptr;
}

bool fire(const char* point) {
    std::shared_ptr<Hook> hook;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        hook = gHook;
    }
    return hook && (*hook)(point);  // called outside the lock: may block
}
}  // namespace pe::testhooks
#endif
