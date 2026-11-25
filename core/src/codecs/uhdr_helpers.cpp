#include "codecs/uhdr_helpers.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hdrbridge::detail {

void require_uhdr(const uhdr_error_info_t& error, const char* operation) {
  if (error.error_code != UHDR_CODEC_OK) {
    throw std::runtime_error(std::string(operation) + ": " +
                             (error.has_detail ? error.detail : "libultrahdr error"));
  }
}

std::vector<uint32_t> pq16_to_rgba1010102(const DecodedImage& decoded) {
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  std::vector<uint32_t> packed(count);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t r = (static_cast<uint32_t>(decoded.rgb[i * 3u]) * 1023u + 32767u) / 65535u;
    const uint32_t g = (static_cast<uint32_t>(decoded.rgb[i * 3u + 1u]) * 1023u + 32767u) / 65535u;
    const uint32_t b = (static_cast<uint32_t>(decoded.rgb[i * 3u + 2u]) * 1023u + 32767u) / 65535u;
    packed[i] = r | (g << 10) | (b << 20) | (3u << 30);
  }
  return packed;
}

}  // namespace hdrbridge::detail
