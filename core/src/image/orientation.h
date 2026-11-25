#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hdrbridge::orientation {

struct RasterTransformResult {
  uint32_t width = 0;
  uint32_t height = 0;
  bool changed = false;
};

RasterTransformResult normalize_rgb16(std::vector<uint16_t>& rgb,
                                      uint32_t width,
                                      uint32_t height,
                                      uint8_t exif_orientation);
RasterTransformResult normalize_plane16(std::vector<uint16_t>& plane,
                                        uint32_t width,
                                        uint32_t height,
                                        uint8_t exif_orientation);

uint8_t read_exif_orientation(const std::vector<uint8_t>& exif);
bool has_exif_orientation(const std::vector<uint8_t>& exif);
void set_exif_orientation_to_one(std::vector<uint8_t>& exif);
void set_xmp_orientation_to_one(std::vector<uint8_t>& xmp);

}  // namespace hdrbridge::orientation
