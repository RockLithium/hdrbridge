#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hdrbridge::jpeg {

struct Probe {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bit_depth = 0;
  uint32_t channels = 0;
  bool exif_present = false;
  bool xmp_present = false;
  bool icc_present = false;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  std::vector<uint8_t> icc;
};

Probe inspect(const void* data, size_t size);

}  // namespace hdrbridge::jpeg
