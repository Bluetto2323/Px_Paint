#pragma once

#include <cstdint>
#include <string>

namespace pe {

enum class SelfTestStatus : uint8_t { NotRun, Pending, Passed, Failed, Unsupported };

inline const char* toString(SelfTestStatus s) {
    switch (s) {
        case SelfTestStatus::NotRun: return "not run";
        case SelfTestStatus::Pending: return "pending";
        case SelfTestStatus::Passed: return "PASSED";
        case SelfTestStatus::Failed: return "FAILED";
        case SelfTestStatus::Unsupported: return "unsupported";
    }
    return "?";
}

struct SelfTestResult {
    SelfTestStatus status = SelfTestStatus::NotRun;
    std::string detail;
    uint32_t runs = 0;
};

}  // namespace pe
