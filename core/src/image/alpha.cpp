#include "image/alpha.h"
#include "color/pq.h"
#include "infrastructure/pixel_workers.h"
#include <cmath>

namespace hdrbridge::detail {

void composite_straight_alpha_over_black(DecodedImage& decoded) {
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  if (decoded.alpha.size() != count || decoded.rgb.size() != count * 3u) {
    throw std::runtime_error("invalid RGBA16 canonical raster");
  }
  const auto& lut = pq_nits_lut();
  parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
    for (size_t p = begin; p < end; ++p) {
      const double alpha = decoded.alpha[p] / 65535.0;
      for (size_t c = 0; c < 3; ++c) {
        const double nits = lut[decoded.rgb[p * 3u + c]] * alpha;
        decoded.rgb[p * 3u + c] = static_cast<uint16_t>(std::llround(
            forward_pq(std::clamp(nits, 0.0, 10000.0)) * 65535.0));
      }
    }
  });
  decoded.alpha.clear();
  decoded.info.pixel_format = "RGB16 integer (source alpha composited over black in linear light)";
  decoded.info.chroma = "4:4:4 RGB";
}

}  // namespace hdrbridge::detail
