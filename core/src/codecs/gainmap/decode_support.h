#pragma once

#include "hdrbridge_threads.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace hdrbridge::gainmap::detail {

using Clock = std::chrono::steady_clock;

inline uint32_t codec_worker_count() {
  return default_worker_count();
}

inline double elapsed_ms(Clock::time_point begin) {
  return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

std::vector<uint8_t> read_file(const std::filesystem::path& path);

}  // namespace hdrbridge::gainmap::detail
