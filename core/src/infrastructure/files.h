#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hdrbridge::detail {

std::vector<uint8_t> read_file(const std::filesystem::path& path);

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data);

std::string extension_lower(const std::filesystem::path& path);

}  // namespace hdrbridge::detail
