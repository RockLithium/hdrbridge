#pragma once

#include "hdrbridge_core.h"

namespace hdrbridge::gainmap {

// A decoded gain-map raster in a neutral, display-independent form. The
// source channel count is retained in `channels`; `rgb16` is always packed
// RGB16 for simple lossless PNG export, with mono maps repeated into all
// three channels without changing their values.
struct GainMapRaster {
  SourceInfo info;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t channels = 0;
  uint32_t bit_depth = 16;
  std::vector<uint16_t> rgb16;
  std::string encoding;
  double decode_ms = 0.0;
  // Exact embedded image payload when it is independently usable (JPEG/JXL).
  std::vector<uint8_t> original_bytes;
  std::string original_extension;
};

// Extract only the encoded gain-map image. This does not reconstruct HDR and
// does not apply the gain-map metadata; it is intended for inspection/export.
GainMapRaster extract_gain_map(const std::filesystem::path& path,
                               std::atomic_bool* cancel = nullptr,
                               bool decode_pixels = true);

}  // namespace hdrbridge::gainmap
