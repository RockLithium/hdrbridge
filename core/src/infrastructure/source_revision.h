#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace hdrbridge::detail {

struct SourceRevision {
  uint64_t volume = 0;
  std::array<uint8_t, 16> file_id{};
  int64_t change_time = 0;
  bool valid = false;
  bool operator==(const SourceRevision&) const = default;
};

SourceRevision source_revision(const std::filesystem::path& path);

}  // namespace hdrbridge::detail
