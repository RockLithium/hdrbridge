#pragma once

#include "hdrbridge_core.h"

#include <tiffio.h>

namespace hdrbridge {

std::vector<uint8_t> read_tiff_input_exif(TIFF* tiff, SourceInfo& info);

}  // namespace hdrbridge
