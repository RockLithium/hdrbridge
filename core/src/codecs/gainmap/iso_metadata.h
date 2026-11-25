#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hdrbridge::gainmap::detail {

struct IsoGainMapMetadata {
  bool multichannel = false;
  bool use_base_color_space = true;
  double base_hdr_headroom = 0.0;
  double alternate_hdr_headroom = 0.0;
  std::array<double, 3> minimum{};
  std::array<double, 3> maximum{};
  std::array<double, 3> gamma{};
  std::array<double, 3> base_offset{};
  std::array<double, 3> alternate_offset{};
};

IsoGainMapMetadata parse_iso_gain_map_metadata(const void* data, size_t size);
IsoGainMapMetadata parse_tiff_gain_map_metadata(const void* data, uint32_t size);

}  // namespace hdrbridge::gainmap::detail
