#include "orientation.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

std::vector<uint16_t> raster() {
  std::vector<uint16_t> rgb;
  for (uint16_t value = 1; value <= 6; ++value) {
    rgb.insert(rgb.end(), {value, value, value});
  }
  return rgb;
}

bool check(uint8_t orientation, uint32_t expected_width, uint32_t expected_height,
           const std::vector<uint16_t>& expected_values) {
  auto rgb = raster();
  const auto result = hdrbridge::orientation::normalize_rgb16(rgb, 2, 3, orientation);
  std::vector<uint16_t> actual;
  for (size_t i = 0; i < rgb.size(); i += 3) actual.push_back(rgb[i]);
  if (result.width != expected_width || result.height != expected_height ||
      actual != expected_values) {
    std::cerr << "orientation " << static_cast<int>(orientation) << " failed\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool passed =
      check(1, 2, 3, {1, 2, 3, 4, 5, 6}) &&
      check(3, 2, 3, {6, 5, 4, 3, 2, 1}) &&
      check(6, 3, 2, {5, 3, 1, 6, 4, 2}) &&
      check(8, 3, 2, {2, 4, 6, 1, 3, 5});
  std::vector<uint16_t> auxiliary{1, 2, 3, 4, 5, 6};
  const auto transformed = hdrbridge::orientation::normalize_plane16(auxiliary, 2, 3, 6);
  passed = passed && transformed.width == 3 && transformed.height == 2 &&
           auxiliary == std::vector<uint16_t>({5, 3, 1, 6, 4, 2});
  return passed ? 0 : 1;
}
