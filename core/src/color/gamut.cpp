#include "color/gamut.h"
#include "color/pq.h"
#include "infrastructure/pixel_workers.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace hdrbridge::detail {

void convert_rec709_pq_to_rec2020_pq(std::vector<uint16_t>& pixels) {
  parallel_pixel_blocks(pixels.size() / 3u, [&](size_t begin, size_t end) {
  for (size_t p = begin; p < end; ++p) {
    const double r = transfer::pq_to_nits(pixels[p * 3u] / 65535.0);
    const double g = transfer::pq_to_nits(pixels[p * 3u + 1u] / 65535.0);
    const double b = transfer::pq_to_nits(pixels[p * 3u + 2u] / 65535.0);
    const std::array<double, 3> rec2020{
        0.6274040 * r + 0.3292830 * g + 0.0433130 * b,
        0.0690970 * r + 0.9195400 * g + 0.0113620 * b,
        0.0163910 * r + 0.0880130 * g + 0.8955950 * b};
    for (size_t c = 0; c < 3; ++c) pixels[p * 3u + c] =
        static_cast<uint16_t>(std::llround(transfer::nits_to_pq(
            std::clamp(rec2020[c], 0.0, 10000.0)) * 65535.0));
  }
  });
}

std::vector<uint16_t> convert_rec2020_pq_to_p3_pq(const DecodedImage& decoded) {
  const auto& lut = pq_nits_lut();
  std::vector<uint16_t> output(decoded.rgb.size());
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
  for (size_t p = begin; p < end; ++p) {
    const double r = lut[decoded.rgb[p * 3u]];
    const double g = lut[decoded.rgb[p * 3u + 1u]];
    const double b = lut[decoded.rgb[p * 3u + 2u]];
    const std::array<double, 3> p3{
        1.3435783 * r - 0.2821797 * g - 0.0613986 * b,
       -0.0652975 * r + 1.0757879 * g - 0.0104904 * b,
        0.0028218 * r - 0.0195985 * g + 1.0167767 * b};
    for (size_t c = 0; c < 3; ++c) {
      output[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(p3[c], 0.0, 10000.0)) * 65535.0));
    }
  }
  });
  return output;
}

std::vector<uint16_t> convert_rec2020_pq_to_rec709_pq(const DecodedImage& decoded) {
  const auto& lut = pq_nits_lut();
  std::vector<uint16_t> output(decoded.rgb.size());
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
  for (size_t p = begin; p < end; ++p) {
    const double r = lut[decoded.rgb[p * 3u]];
    const double g = lut[decoded.rgb[p * 3u + 1u]];
    const double b = lut[decoded.rgb[p * 3u + 2u]];
    const std::array<double, 3> rec709{
        1.6604910 * r - 0.5876411 * g - 0.0728499 * b,
       -0.1245505 * r + 1.1328999 * g - 0.0083494 * b,
       -0.0181508 * r - 0.1005789 * g + 1.1187297 * b};
    for (size_t c = 0; c < 3; ++c) {
      output[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(rec709[c], 0.0, 10000.0)) * 65535.0));
    }
  }
  });
  return output;
}

std::vector<uint16_t> convert_rec2020_pq_to_hlg(const DecodedImage& decoded,
                                                 const std::string& gamut) {
  const auto& lut = pq_nits_lut();
  std::vector<uint16_t> output(decoded.rgb.size());
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
  for (size_t p = begin; p < end; ++p) {
    const double r = lut[decoded.rgb[p * 3u]];
    const double g = lut[decoded.rgb[p * 3u + 1u]];
    const double b = lut[decoded.rgb[p * 3u + 2u]];
    std::array<double, 3> linear{r, g, b};
    if (gamut == "p3") {
      linear = {1.3435783 * r - 0.2821797 * g - 0.0613986 * b,
               -0.0652975 * r + 1.0757879 * g - 0.0104904 * b,
                0.0028218 * r - 0.0195985 * g + 1.0167767 * b};
    } else if (gamut == "rec709") {
      linear = {1.6604910 * r - 0.5876411 * g - 0.0728499 * b,
               -0.1245505 * r + 1.1328999 * g - 0.0083494 * b,
               -0.0181508 * r - 0.1005789 * g + 1.1187297 * b};
    }
    for (double& value : linear) value = std::max(value, 0.0);
    const auto signal = transfer::linear_nits_to_hlg(linear);
    for (size_t c = 0; c < 3; ++c) {
      output[p * 3u + c] = static_cast<uint16_t>(std::llround(signal[c] * 65535.0));
    }
  }
  });
  return output;
}

void convert_p3_pq_to_rec2020_pq(std::vector<uint16_t>& pixels) {
  const auto& lut = pq_nits_lut();
  parallel_pixel_blocks(pixels.size() / 3u, [&](size_t begin, size_t end) {
  for (size_t p = begin; p < end; ++p) {
    const double r = lut[pixels[p * 3u]];
    const double g = lut[pixels[p * 3u + 1u]];
    const double b = lut[pixels[p * 3u + 2u]];
    const std::array<double, 3> rec2020{
        0.7538330 * r + 0.1985974 * g + 0.0475696 * b,
        0.0457438 * r + 0.9417772 * g + 0.0124789 * b,
       -0.0012103 * r + 0.0176017 * g + 0.9836086 * b};
    for (size_t c = 0; c < 3; ++c) {
      pixels[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(rec2020[c], 0.0, 10000.0)) * 65535.0));
    }
  }
  });
}

}  // namespace hdrbridge::detail
