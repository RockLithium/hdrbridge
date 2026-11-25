#include "image/resize.h"
#include "color/pq.h"
#include "color/half_float.h"
#include "infrastructure/progress.h"
#include "infrastructure/pixel_workers.h"
#include <algorithm>
#include <cmath>

namespace hdrbridge::detail {

DecodedImage resize_linear_hdr_to_fit(const DecodedImage& source,
                                      uint32_t max_dimension) {
  if (source.info.width <= max_dimension && source.info.height <= max_dimension) {
    return source;
  }
  const double scale = std::min(
      max_dimension / static_cast<double>(source.info.width),
      max_dimension / static_cast<double>(source.info.height));
  DecodedImage output;
  output.info = source.info;
  output.info.width = std::max(1u, static_cast<uint32_t>(
      std::lround(source.info.width * scale)));
  output.info.height = std::max(1u, static_cast<uint32_t>(
      std::lround(source.info.height * scale)));
  output.exif = source.exif;
  output.xmp = source.xmp;
  output.rgb.resize(static_cast<size_t>(output.info.width) * output.info.height * 3u);
  const auto& lut = pq_nits_lut();
  parallel_pixel_blocks(output.info.height, [&](size_t begin, size_t end) {
  for (size_t y = begin; y < end; ++y) {
    const double sy = (y + 0.5) / scale - 0.5;
    const uint32_t y0 = static_cast<uint32_t>(std::clamp(
        static_cast<int64_t>(std::floor(sy)), int64_t{0},
        static_cast<int64_t>(source.info.height - 1u)));
    const uint32_t y1 = std::min(y0 + 1u, source.info.height - 1u);
    const double fy = std::clamp(sy - std::floor(sy), 0.0, 1.0);
    for (uint32_t x = 0; x < output.info.width; ++x) {
      const double sx = (x + 0.5) / scale - 0.5;
      const uint32_t x0 = static_cast<uint32_t>(std::clamp(
          static_cast<int64_t>(std::floor(sx)), int64_t{0},
          static_cast<int64_t>(source.info.width - 1u)));
      const uint32_t x1 = std::min(x0 + 1u, source.info.width - 1u);
      const double fx = std::clamp(sx - std::floor(sx), 0.0, 1.0);
      const size_t destination =
          (static_cast<size_t>(y) * output.info.width + x) * 3u;
      for (size_t c = 0; c < 3; ++c) {
        const double a = lut[source.rgb[(static_cast<size_t>(y0) * source.info.width + x0) * 3u + c]];
        const double b = lut[source.rgb[(static_cast<size_t>(y0) * source.info.width + x1) * 3u + c]];
        const double d = lut[source.rgb[(static_cast<size_t>(y1) * source.info.width + x0) * 3u + c]];
        const double e = lut[source.rgb[(static_cast<size_t>(y1) * source.info.width + x1) * 3u + c]];
        const double top = a + (b - a) * fx;
        const double bottom = d + (e - d) * fx;
        output.rgb[destination + c] = static_cast<uint16_t>(std::llround(
            forward_pq(top + (bottom - top) * fy) * 65535.0));
      }
    }
  }
  });
  return output;
}

// Resample in linear light and composite straight alpha over black before
// converting to the display's scRGB representation.
std::vector<uint16_t> resize_preview_scrgb(const DecodedImage& source,
                                          uint32_t width, uint32_t height,
                                          uint32_t max_dimension,
                                          const ProgressCallback& progress,
                                          std::atomic_bool* cancel) {
  const double scale = std::min(max_dimension / static_cast<double>(source.info.width),
                                max_dimension / static_cast<double>(source.info.height));
  const size_t count = static_cast<size_t>(width) * height;
  const bool has_alpha = source.alpha.size() ==
      static_cast<size_t>(source.info.width) * source.info.height;
  const auto& lut = pq_nits_lut();
  std::vector<uint16_t> rgba(count * 4u);
  report(progress, 44, "Resampling linear HDR preview");
  parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
    check_cancel(cancel);
    for (size_t p = begin; p < end; ++p) {
      const uint32_t x = static_cast<uint32_t>(p % width);
      const uint32_t y = static_cast<uint32_t>(p / width);
      const double sx = (x + 0.5) / scale - 0.5;
      const double sy = (y + 0.5) / scale - 0.5;
      const uint32_t x0 = static_cast<uint32_t>(std::clamp(static_cast<int64_t>(std::floor(sx)), int64_t{0}, static_cast<int64_t>(source.info.width - 1u)));
      const uint32_t y0 = static_cast<uint32_t>(std::clamp(static_cast<int64_t>(std::floor(sy)), int64_t{0}, static_cast<int64_t>(source.info.height - 1u)));
      const uint32_t x1 = std::min(x0 + 1u, source.info.width - 1u);
      const uint32_t y1 = std::min(y0 + 1u, source.info.height - 1u);
      const double fx = std::clamp(sx - std::floor(sx), 0.0, 1.0);
      const double fy = std::clamp(sy - std::floor(sy), 0.0, 1.0);
      const size_t samples[] = {static_cast<size_t>(y0) * source.info.width + x0,
                                static_cast<size_t>(y0) * source.info.width + x1,
                                static_cast<size_t>(y1) * source.info.width + x0,
                                static_cast<size_t>(y1) * source.info.width + x1};
      double rgb[3];
      for (size_t c = 0; c < 3; ++c) {
        double v[4];
        for (size_t i = 0; i < 4; ++i) {
          v[i] = lut[source.rgb[samples[i] * 3u + c]];
          if (has_alpha) v[i] *= source.alpha[samples[i]] / 65535.0;
        }
        const double top = v[0] + (v[1] - v[0]) * fx;
        const double bottom = v[2] + (v[3] - v[2]) * fx;
        rgb[c] = (top + (bottom - top) * fy) / 80.0;
      }
      const double r = rgb[0], g = rgb[1], b = rgb[2];
      rgba[p * 4u] = float_to_half(static_cast<float>(std::clamp(1.660491 * r - 0.587641 * g - 0.072850 * b, -65504.0, 65504.0)));
      rgba[p * 4u + 1u] = float_to_half(static_cast<float>(std::clamp(-0.124550 * r + 1.132900 * g - 0.008349 * b, -65504.0, 65504.0)));
      rgba[p * 4u + 2u] = float_to_half(static_cast<float>(std::clamp(-0.018151 * r - 0.100579 * g + 1.118730 * b, -65504.0, 65504.0)));
      rgba[p * 4u + 3u] = float_to_half(1.0f);
    }
  });
  report(progress, 62, "Linear FP16 scRGB transform complete");
  return rgba;
}

}  // namespace hdrbridge::detail
