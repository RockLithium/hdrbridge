#include "color/scrgb.h"
#include "color/pq.h"
#include "color/half_float.h"
#include "infrastructure/progress.h"
#include "infrastructure/pixel_workers.h"
#include <algorithm>

namespace hdrbridge::detail {

std::vector<uint16_t> pq2020_to_scrgb_half(const DecodedImage& decoded,
                                            const ProgressCallback& progress,
                                            std::atomic_bool* cancel) {
  const size_t pixels = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  std::vector<uint16_t> rgba(pixels * 4u);
  report(progress, 44, "Transforming Rec.2020/PQ to linear FP16 scRGB");
  const auto& lut = pq_nits_lut();
  parallel_pixel_blocks(pixels, [&](size_t begin, size_t end) {
    check_cancel(cancel);
    for (size_t p = begin; p < end; ++p) {
      const double r = lut[decoded.rgb[p * 3u]] / 80.0;
      const double g = lut[decoded.rgb[p * 3u + 1u]] / 80.0;
      const double b = lut[decoded.rgb[p * 3u + 2u]] / 80.0;
      const float sr = static_cast<float>(std::clamp(1.660491 * r - 0.587641 * g - 0.072850 * b, -65504.0, 65504.0));
      const float sg = static_cast<float>(std::clamp(-0.124550 * r + 1.132900 * g - 0.008349 * b, -65504.0, 65504.0));
      const float sb = static_cast<float>(std::clamp(-0.018151 * r - 0.100579 * g + 1.118730 * b, -65504.0, 65504.0));
      rgba[p * 4u] = float_to_half(sr);
      rgba[p * 4u + 1u] = float_to_half(sg);
      rgba[p * 4u + 2u] = float_to_half(sb);
      const float alpha = decoded.alpha.size() == pixels
          ? decoded.alpha[p] / 65535.0f : 1.0f;
      rgba[p * 4u + 3u] = float_to_half(alpha);
    }
  });
  report(progress, 62, "Linear FP16 scRGB transform complete");
  return rgba;
}

}  // namespace hdrbridge::detail
