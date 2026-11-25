#pragma once

#include "codecs/gainmap/raster.h"

namespace hdrbridge::detail {

std::vector<uint8_t> encode_gainmap_jpeg(const gainmap::GainMapRaster& raster,
                                         uint32_t, int quality);

void verify_gainmap_jpeg(const std::vector<uint8_t>& actual,
                        const gainmap::GainMapRaster& raster, size_t channels);

}  // namespace hdrbridge::detail
