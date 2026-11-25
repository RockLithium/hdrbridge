#include "codecs/gainmap/extract_support.h"
#include "image/orientation.h"
#include <algorithm>
#include <stdexcept>

namespace hdrbridge::gainmap::detail {

void check_gainmap_cancel(std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("gain-map extraction cancelled");
}

GainMapRaster make_raster(SourceInfo info, uint32_t width, uint32_t height,
                          uint32_t channels, std::vector<uint16_t> samples,
                          const char* encoding, std::atomic_bool* cancel) {
  check_gainmap_cancel(cancel);
  if (!width || !height || samples.empty()) {
    throw std::runtime_error("gain-map decoder returned an empty raster");
  }
  GainMapRaster result;
  result.info = std::move(info);
  result.channels = channels == 1 ? 1u : 3u;
  result.encoding = encoding ? encoding : "decoded gain-map raster";
  result.width = width;
  result.height = height;
  if (result.channels == 1) {
    if (samples.size() != static_cast<size_t>(width) * height) {
      throw std::runtime_error("gain-map mono raster size is inconsistent");
    }
    result.rgb16.resize(samples.size() * 3u);
    for (size_t i = 0; i < samples.size(); ++i) {
      result.rgb16[i * 3u] = samples[i];
      result.rgb16[i * 3u + 1u] = samples[i];
      result.rgb16[i * 3u + 2u] = samples[i];
    }
  } else {
    if (samples.size() != static_cast<size_t>(width) * height * 3u) {
      throw std::runtime_error("gain-map RGB raster size is inconsistent");
    }
    result.rgb16 = std::move(samples);
  }
  return result;
}

void normalize_gainmap_orientation(GainMapRaster& raster, uint8_t orientation) {
  if (orientation <= 1 || orientation > 8) return;
  const auto transformed = orientation::normalize_rgb16(
      raster.rgb16, raster.width, raster.height, orientation);
  raster.width = transformed.width;
  raster.height = transformed.height;
  raster.info.orientation_normalized = true;
}

}  // namespace hdrbridge::gainmap::detail
