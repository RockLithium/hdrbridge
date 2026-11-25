#include "canonical/cache.h"
#include "image/alpha.h"
#include "image/resize.h"
#include "color/scrgb.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hdrbridge {

using namespace detail;

PreviewImage decode_preview(const std::filesystem::path& input,
                            uint32_t max_dimension,
                            ProgressCallback progress,
                            std::atomic_bool* cancel) {
  if (max_dimension == 0) throw std::runtime_error("preview dimensions must be non-zero");
  TimingDiagnostics timings;
  const auto decoded = decode_input(input, progress, cancel, timings);
  if (decoded->info.asset_kind != "direct-hdr" &&
      decoded->info.asset_kind != "gain-map-hdr") {
    throw std::runtime_error("No HDR data");
  }
  PreviewImage result;
  result.info = decoded->info;
  result.timings = timings;
  if (decoded->info.width > max_dimension || decoded->info.height > max_dimension) {
    const double scale = std::min(max_dimension / static_cast<double>(decoded->info.width),
                                  max_dimension / static_cast<double>(decoded->info.height));
    result.info.width = std::max(1u, static_cast<uint32_t>(std::lround(decoded->info.width * scale)));
    result.info.height = std::max(1u, static_cast<uint32_t>(std::lround(decoded->info.height * scale)));
    result.rgba_fp16 = resize_preview_scrgb(*decoded, result.info.width, result.info.height,
                                            max_dimension, progress, cancel);
  } else if (!decoded->alpha.empty()) {
    DecodedImage composited = *decoded;
    composite_straight_alpha_over_black(composited);
    result.rgba_fp16 = pq2020_to_scrgb_half(composited, progress, cancel);
  } else {
    result.rgba_fp16 = pq2020_to_scrgb_half(*decoded, progress, cancel);
  }
  result.width = result.info.width;
  result.height = result.info.height;
  return result;
}

}  // namespace hdrbridge
