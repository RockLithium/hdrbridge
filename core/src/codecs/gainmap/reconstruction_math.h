#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace hdrbridge::gainmap::detail {

inline double inverse_srgb(double value) {
  return value <= 0.04045 ? value / 12.92
                          : std::pow((value + 0.055) / 1.055, 2.4);
}

inline double forward_pq(double nits) {
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 32.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 128.0;
  constexpr double c3 = 2392.0 / 128.0;
  const double linear = std::clamp(nits / 10000.0, 0.0, 1.0);
  const double powered = std::pow(linear, m1);
  return std::pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
}

inline std::array<double, 3> rec2020_from_gainmap_base(
    const std::array<double, 3>& rgb, uint16_t primaries) {
  if (primaries == 12) {
    return {0.7538330 * rgb[0] + 0.1985974 * rgb[1] + 0.0475696 * rgb[2],
            0.0457438 * rgb[0] + 0.9417772 * rgb[1] + 0.0124789 * rgb[2],
           -0.0012103 * rgb[0] + 0.0176017 * rgb[1] + 0.9836086 * rgb[2]};
  }
  if (primaries == 1) {
    return {0.6274040 * rgb[0] + 0.3292820 * rgb[1] + 0.0433136 * rgb[2],
            0.0690970 * rgb[0] + 0.9195400 * rgb[1] + 0.0113612 * rgb[2],
            0.0163916 * rgb[0] + 0.0880132 * rgb[1] + 0.8955950 * rgb[2]};
  }
  return rgb;
}

}  // namespace hdrbridge::gainmap::detail
