#include "metadata/exif_payload.h"
#include "metadata/exif_output.h"
#include "infrastructure/big_endian.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace hdrbridge::detail {

static bool starts_tiff_header(const uint8_t* data, size_t size) {
  return size >= 4 &&
      ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
       (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42));
}

std::vector<uint8_t> exif_tiff_payload(const std::vector<uint8_t>& exif) {
  if (exif.empty()) return {};
  if (starts_tiff_header(exif.data(), exif.size())) { auto result = exif; return metadata::prepare_output_exif(result) ? result : std::vector<uint8_t>{}; }
  if (exif.size() >= 6 && std::memcmp(exif.data(), "Exif\0\0", 6) == 0 &&
      starts_tiff_header(exif.data() + 6, exif.size() - 6)) {
    std::vector<uint8_t> result(exif.begin() + 6, exif.end());
    return metadata::prepare_output_exif(result) ? result : std::vector<uint8_t>{};
  }
  if (exif.size() >= 4) {
    const size_t offset = 4u + be32(exif.data());
    if (offset < exif.size() && starts_tiff_header(exif.data() + offset, exif.size() - offset)) {
      std::vector<uint8_t> result(exif.begin() + static_cast<std::ptrdiff_t>(offset), exif.end());
      return metadata::prepare_output_exif(result) ? result : std::vector<uint8_t>{};
    }
  }
  return {};
}

std::vector<uint8_t> exif_for_jxl(const std::vector<uint8_t>& exif) {
  auto tiff = exif_tiff_payload(exif);
  if (tiff.empty()) return {};
  std::vector<uint8_t> output(4, 0);
  output.insert(output.end(), tiff.begin(), tiff.end());
  return output;
}

std::vector<uint8_t> jpeg_exif_from_heif(const std::vector<uint8_t>& heif_exif) {
  const auto tiff = exif_tiff_payload(heif_exif);
  if (tiff.empty()) return {};
  std::vector<uint8_t> output{'E', 'x', 'i', 'f', 0, 0};
  output.insert(output.end(), tiff.begin(), tiff.end());
  return output;
}

}  // namespace hdrbridge::detail
