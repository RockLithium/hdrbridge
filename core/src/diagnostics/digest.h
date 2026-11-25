#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hdrbridge::detail {

std::string sha256(const std::vector<uint8_t>& data);

}  // namespace hdrbridge::detail
