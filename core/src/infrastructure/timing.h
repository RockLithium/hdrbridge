#pragma once

#include <chrono>

namespace hdrbridge::detail {

using Clock = std::chrono::steady_clock;

inline double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace hdrbridge::detail
