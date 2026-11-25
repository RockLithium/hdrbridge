#pragma once

#include "codecs/gainmap/raster.h"

namespace hdrbridge::gainmap::detail {

void check_gainmap_cancel(std::atomic_bool* cancel);
GainMapRaster make_raster(SourceInfo info, uint32_t width, uint32_t height,
                          uint32_t channels, std::vector<uint16_t> samples,
                          const char* encoding, std::atomic_bool* cancel);
void normalize_gainmap_orientation(GainMapRaster& raster, uint8_t orientation);

}  // namespace hdrbridge::gainmap::detail
