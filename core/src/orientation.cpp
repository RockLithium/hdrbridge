#include "orientation.h"

#include <avif/avif.h>

#include <algorithm>
#include <cstring>
#include <regex>
#include <stdexcept>

namespace hdrbridge::orientation {

namespace {

struct Coordinates {
  uint32_t x;
  uint32_t y;
};

Coordinates destination_for(uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height, uint8_t orientation) {
  switch (orientation) {
    case 1: return {x, y};
    case 2: return {width - 1u - x, y};
    case 3: return {width - 1u - x, height - 1u - y};
    case 4: return {x, height - 1u - y};
    case 5: return {y, x};
    case 6: return {height - 1u - y, x};
    case 7: return {height - 1u - y, width - 1u - x};
    case 8: return {y, width - 1u - x};
    default: throw std::runtime_error("unsupported EXIF orientation");
  }
}

}  // namespace

RasterTransformResult normalize_rgb16(std::vector<uint16_t>& rgb,
                                      uint32_t width,
                                      uint32_t height,
                                      uint8_t exif_orientation) {
  if (width == 0 || height == 0 ||
      rgb.size() != static_cast<size_t>(width) * height * 3u) {
    throw std::runtime_error("invalid RGB16 raster for orientation normalization");
  }
  if (exif_orientation == 0) exif_orientation = 1;
  if (exif_orientation == 1) return {width, height, false};
  if (exif_orientation > 8) throw std::runtime_error("invalid EXIF orientation value");

  const bool swaps_axes = exif_orientation >= 5;
  const uint32_t output_width = swaps_axes ? height : width;
  const uint32_t output_height = swaps_axes ? width : height;
  std::vector<uint16_t> output(rgb.size());
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const Coordinates destination = destination_for(x, y, width, height, exif_orientation);
      const size_t source_index = (static_cast<size_t>(y) * width + x) * 3u;
      const size_t destination_index =
          (static_cast<size_t>(destination.y) * output_width + destination.x) * 3u;
      std::copy_n(rgb.data() + source_index, 3u, output.data() + destination_index);
    }
  }
  rgb.swap(output);
  return {output_width, output_height, true};
}

RasterTransformResult normalize_plane16(std::vector<uint16_t>& plane,
                                        uint32_t width,
                                        uint32_t height,
                                        uint8_t exif_orientation) {
  if (width == 0 || height == 0 ||
      plane.size() != static_cast<size_t>(width) * height) {
    throw std::runtime_error("invalid 16-bit plane for orientation normalization");
  }
  if (exif_orientation == 0) exif_orientation = 1;
  if (exif_orientation == 1) return {width, height, false};
  if (exif_orientation > 8) throw std::runtime_error("invalid EXIF orientation value");
  const bool swaps_axes = exif_orientation >= 5;
  const uint32_t output_width = swaps_axes ? height : width;
  const uint32_t output_height = swaps_axes ? width : height;
  std::vector<uint16_t> output(plane.size());
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const Coordinates destination = destination_for(x, y, width, height, exif_orientation);
      output[static_cast<size_t>(destination.y) * output_width + destination.x] =
          plane[static_cast<size_t>(y) * width + x];
    }
  }
  plane.swap(output);
  return {output_width, output_height, true};
}

uint8_t read_exif_orientation(const std::vector<uint8_t>& exif) {
  if (exif.empty()) return 1;
  size_t offset = 0;
  if (avifGetExifOrientationOffset(exif.data(), exif.size(), &offset) != AVIF_RESULT_OK ||
      offset >= exif.size()) {
    return 1;
  }
  const uint8_t value = exif[offset];
  return value >= 1 && value <= 8 ? value : 1;
}

bool has_exif_orientation(const std::vector<uint8_t>& exif) {
  if (exif.empty()) return false;
  size_t offset = 0;
  return avifGetExifOrientationOffset(exif.data(), exif.size(), &offset) == AVIF_RESULT_OK &&
         offset < exif.size();
}

void set_exif_orientation_to_one(std::vector<uint8_t>& exif) {
  if (exif.empty()) return;
  size_t offset = 0;
  if (avifGetExifOrientationOffset(exif.data(), exif.size(), &offset) == AVIF_RESULT_OK &&
      offset < exif.size()) {
    exif[offset] = 1;
  }
}

void set_xmp_orientation_to_one(std::vector<uint8_t>& xmp) {
  if (xmp.empty()) return;
  std::string text(xmp.begin(), xmp.end());
  text = std::regex_replace(text,
      std::regex(R"((tiff|exif):Orientation\s*=\s*[\"'][1-8][\"'])",
                 std::regex::icase),
      "$1:Orientation=\"1\"");
  text = std::regex_replace(text,
      std::regex(R"(<(tiff|exif):Orientation>\s*[1-8]\s*</(tiff|exif):Orientation>)",
                 std::regex::icase),
      "<$1:Orientation>1</$2:Orientation>");
  xmp.assign(text.begin(), text.end());
}

}  // namespace hdrbridge::orientation
