#include "metadata/exif_payload.h"
#include "metadata/exif_output.h"
#include "metadata/exif_repack.h"
#include "infrastructure/big_endian.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace hdrbridge::detail {

namespace {

constexpr size_t kMaxJpegApp1ExifTiffBytes = 65527u;  // 65533 - 6 ("Exif\0\0")

bool starts_tiff_header(const uint8_t* data, size_t size) {
  return size >= 4 &&
      ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
       (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42));
}

std::pair<uint32_t, uint32_t> read_tiff_dimensions(const uint8_t* tiff, size_t size) {
  if (!starts_tiff_header(tiff, size) || size < 8) return {1u, 1u};
  const bool le = tiff[0] == 'I';
  const auto u16 = [&](size_t p) -> uint16_t {
    return le ? uint16_t(tiff[p] | uint16_t(tiff[p + 1]) << 8)
              : uint16_t(uint16_t(tiff[p]) << 8 | tiff[p + 1]);
  };
  const auto u32 = [&](size_t p) -> uint32_t {
    return le ? uint32_t(tiff[p]) | uint32_t(tiff[p + 1]) << 8 |
                    uint32_t(tiff[p + 2]) << 16 | uint32_t(tiff[p + 3]) << 24
              : uint32_t(tiff[p]) << 24 | uint32_t(tiff[p + 1]) << 16 |
                    uint32_t(tiff[p + 2]) << 8 | uint32_t(tiff[p + 3]);
  };
  const uint32_t ifd0 = u32(4);
  if (ifd0 < 8 || ifd0 + 2 > size) return {1u, 1u};
  const uint16_t count = u16(ifd0);
  if (size_t(ifd0) + 2 + size_t(count) * 12 > size) return {1u, 1u};
  uint32_t width = 0;
  uint32_t height = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t entry = size_t(ifd0) + 2 + i * 12;
    const uint16_t tag = u16(entry);
    const uint16_t type = u16(entry + 2);
    if ((tag == 0x0100 || tag == 0x0101) && u32(entry + 4) == 1) {
      const uint32_t val = type == 3 ? u16(entry + 8) : type == 4 ? u32(entry + 8) : 0u;
      if (tag == 0x0100) width = val;
      else height = val;
    }
  }
  return {width ? width : 1u, height ? height : 1u};
}

}  // namespace

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

std::vector<uint8_t> jpeg_exif_from_heif(const std::vector<uint8_t>& heif_exif,
                                         uint32_t width,
                                         uint32_t height) {
  auto tiff = exif_tiff_payload(heif_exif);
  if (tiff.empty() || tiff.size() > kMaxJpegApp1ExifTiffBytes) {
    uint32_t target_width = width;
    uint32_t target_height = height;
    if ((!target_width || !target_height) && !tiff.empty()) {
      const auto dims = read_tiff_dimensions(tiff.data(), tiff.size());
      if (!target_width) target_width = dims.first;
      if (!target_height) target_height = dims.second;
    }
    if (!target_width) target_width = 1u;
    if (!target_height) target_height = 1u;
    try {
      tiff = metadata::repack_exif(heif_exif, target_width, target_height);
    } catch (...) {
      return {};
    }
  }
  if (tiff.empty() || tiff.size() > kMaxJpegApp1ExifTiffBytes) return {};
  std::vector<uint8_t> output{'E', 'x', 'i', 'f', 0, 0};
  output.insert(output.end(), tiff.begin(), tiff.end());
  return output;
}

}  // namespace hdrbridge::detail
