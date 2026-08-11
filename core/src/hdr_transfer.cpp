#include "hdr_transfer.h"

#include <algorithm>
#include <cmath>

namespace hdrbridge::transfer {
namespace {

constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 1.0 - 4.0 * kHlgA;
const double kHlgC = 0.5 - kHlgA * std::log(4.0 * kHlgA);
constexpr double kHlgSystemGamma1000 = 1.2;

double inverse_hlg_oetf(double signal) {
  const double value = std::clamp(signal, 0.0, 1.0);
  return value <= 0.5
      ? value * value / 3.0
      : (std::exp((value - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

double forward_hlg_oetf(double scene_linear) {
  const double value = std::max(scene_linear, 0.0);
  return value <= (1.0 / 12.0)
      ? std::sqrt(3.0 * value)
      : kHlgA * std::log(12.0 * value - kHlgB) + kHlgC;
}

double rec2020_luminance(const std::array<double, 3>& rgb) {
  return std::max(0.0, 0.2627 * rgb[0] + 0.6780 * rgb[1] + 0.0593 * rgb[2]);
}

}  // namespace

Kind from_cicp(uint16_t transfer_characteristics) {
  if (transfer_characteristics == 16) return Kind::pq_st2084;
  if (transfer_characteristics == 18) return Kind::hlg_bt2100;
  return Kind::unknown;
}

const char* name(Kind kind) {
  switch (kind) {
    case Kind::pq_st2084: return "PQ / ST2084";
    case Kind::hlg_bt2100: return "HLG / BT.2100";
    default: return "Unknown";
  }
}

double pq_to_nits(double encoded) {
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 32.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 128.0;
  constexpr double c3 = 2392.0 / 128.0;
  const double p = std::pow(std::clamp(encoded, 0.0, 1.0), 1.0 / m2);
  return std::pow(std::max(p - c1, 0.0) / std::max(c2 - c3 * p, 1e-12),
                  1.0 / m1) * 10000.0;
}

double nits_to_pq(double nits) {
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 32.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 128.0;
  constexpr double c3 = 2392.0 / 128.0;
  const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
  return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

std::array<double, 3> hlg_to_linear_nits(
    const std::array<double, 3>& signal, double peak_nits) {
  std::array<double, 3> scene{
      inverse_hlg_oetf(signal[0]),
      inverse_hlg_oetf(signal[1]),
      inverse_hlg_oetf(signal[2])};
  const double scene_luminance = rec2020_luminance(scene);
  const double ootf = peak_nits *
      std::pow(scene_luminance, kHlgSystemGamma1000 - 1.0);
  return {scene[0] * ootf, scene[1] * ootf, scene[2] * ootf};
}

std::array<double, 3> linear_nits_to_hlg(
    const std::array<double, 3>& nits, double peak_nits) {
  const double safe_peak = std::max(peak_nits, 1.0);
  const std::array<double, 3> display{
      std::max(nits[0], 0.0) / safe_peak,
      std::max(nits[1], 0.0) / safe_peak,
      std::max(nits[2], 0.0) / safe_peak};
  const double display_luminance = rec2020_luminance(display);
  if (display_luminance <= 0.0) return {0.0, 0.0, 0.0};
  const double scene_luminance =
      std::pow(display_luminance, 1.0 / kHlgSystemGamma1000);
  const double inverse_ootf =
      1.0 / std::pow(scene_luminance, kHlgSystemGamma1000 - 1.0);
  return {
      std::clamp(forward_hlg_oetf(display[0] * inverse_ootf), 0.0, 1.0),
      std::clamp(forward_hlg_oetf(display[1] * inverse_ootf), 0.0, 1.0),
      std::clamp(forward_hlg_oetf(display[2] * inverse_ootf), 0.0, 1.0)};
}

}  // namespace hdrbridge::transfer
