#pragma once

#include "hdrbridge_core.h"

namespace hdrbridge::gainmap {

inline constexpr char kAppleHdrGainMapUrn[] =
    "urn:com:apple:photo:2020:aux:hdrgainmap";
inline constexpr char kIsoHdrGainMapUrn[] =
    "urn:iso:std:iso:ts:21496:-1";

struct GainMapAuxiliaryProbe {
  bool detected = false;
  bool is_apple = false;
  std::string container;
  std::string auxiliary_type;
  uint32_t item_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bit_depth = 0;
  bool extraction_supported = false;
};

}  // namespace hdrbridge::gainmap
