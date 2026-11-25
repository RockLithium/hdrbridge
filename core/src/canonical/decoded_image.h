#pragma once

#include "hdrbridge_core.h"

namespace hdrbridge::detail {

struct DecodedImage {
  SourceInfo info;
  std::vector<uint16_t> rgb;
  // Straight, full-range alpha retained independently from the canonical RGB
  // master. RGB color math remains unchanged; output policy decides whether
  // alpha is preserved or composited.
  std::vector<uint16_t> alpha;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  TimingDiagnostics timings;
  double gain_map_formula_max_error = 0.0;
  double transfer_conversion_rmse = 0.0;
  double transfer_conversion_max_abs_error = 0.0;
  double source_to_canonical_rmse = 0.0;
  double source_to_canonical_max_abs_error = 0.0;
};

}  // namespace hdrbridge::detail
