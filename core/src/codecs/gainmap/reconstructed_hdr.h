#pragma once

#include "hdrbridge_core.h"

namespace hdrbridge::gainmap {

struct ReconstructedHdr {
  SourceInfo info;
  std::vector<uint16_t> rec2020_pq_rgb;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  double decode_ms = 0.0;
  double base_decode_ms = 0.0;
  double gain_map_decode_ms = 0.0;
  double gain_map_upsample_ms = 0.0;
  double gain_apply_ms = 0.0;
  double color_conversion_ms = 0.0;
  double reconstruction_ms = 0.0;
  double orientation_ms = 0.0;
  double independent_formula_max_error = 0.0;
  uint16_t max_cll = 0;
  uint16_t max_pall = 0;
};

}  // namespace hdrbridge::gainmap
