#pragma once

#include <chrono>
#include <cstdint>

namespace pe {

// Monotonic time in nanoseconds (steady_clock maps to CLOCK_MONOTONIC on
// Android, which is what Choreographer / the compositor use).
inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline double nsToMs(int64_t ns) { return static_cast<double>(ns) / 1.0e6; }

}  // namespace pe
