#include "jpeg_probe.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace hdrbridge::jpeg {
namespace {

bool contains(const uint8_t* data, size_t size, const char* text) {
  const size_t length = std::strlen(text);
  return length <= size &&
      std::search(data, data + size, text, text + length) != data + size;
}

bool is_sof(uint8_t marker) {
  if (marker < 0xc0 || marker > 0xcf) return false;
  return marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
}

}  // namespace

Probe inspect(const void* bytes, size_t size) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) {
    throw std::runtime_error("JPEG probe input is not a JPEG stream");
  }
  Probe result;
  size_t offset = 2;
  while (offset + 1 < size) {
    while (offset < size && data[offset] != 0xff) ++offset;
    while (offset < size && data[offset] == 0xff) ++offset;
    if (offset >= size) break;
    const uint8_t marker = data[offset++];
    if (marker == 0xd9 || marker == 0xda) break;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (offset + 2 > size) break;
    const size_t segment_size = (static_cast<size_t>(data[offset]) << 8) |
                                data[offset + 1u];
    if (segment_size < 2 || segment_size > size - offset) break;
    const uint8_t* payload = data + offset + 2u;
    const size_t payload_size = segment_size - 2u;
    if (is_sof(marker) && payload_size >= 6) {
      result.bit_depth = payload[0];
      result.height = (static_cast<uint32_t>(payload[1]) << 8) | payload[2];
      result.width = (static_cast<uint32_t>(payload[3]) << 8) | payload[4];
      result.channels = payload[5];
    } else if (marker == 0xe1) {
      const bool exif = payload_size >= 6 && std::memcmp(payload, "Exif\0\0", 6) == 0;
      constexpr char kXmpId[] = "http://ns.adobe.com/xap/1.0/";
      const size_t xmp_prefix = sizeof(kXmpId);
      const bool standard_xmp = payload_size >= xmp_prefix &&
          std::memcmp(payload, kXmpId, xmp_prefix) == 0;
      const bool extended_xmp = contains(payload, payload_size,
          "http://ns.adobe.com/xmp/extension/");
      result.exif_present = result.exif_present || exif;
      result.xmp_present = result.xmp_present || standard_xmp || extended_xmp;
      if (exif && result.exif.empty()) {
        // Keep the TIFF payload as the internal Exif representation.
        result.exif.assign(payload + 6, payload + payload_size);
      }
      if (standard_xmp && result.xmp.empty()) {
        result.xmp.assign(payload + xmp_prefix, payload + payload_size);
      }
    } else if (marker == 0xe2) {
      const bool icc = payload_size >= 14 &&
          std::memcmp(payload, "ICC_PROFILE\0", 12) == 0;
      result.icc_present = result.icc_present || icc;
      if (icc && payload[12] == 1 && result.icc.empty()) {
        result.icc.assign(payload + 14, payload + payload_size);
      } else if (icc && payload[12] > 1 && !result.icc.empty()) {
        result.icc.insert(result.icc.end(), payload + 14, payload + payload_size);
      }
    }
    offset += segment_size;
  }
  return result;
}

}  // namespace hdrbridge::jpeg
