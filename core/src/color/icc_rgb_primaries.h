#pragma once

#include <lcms2.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hdrbridge::icc {

inline uint16_t identify_rgb_primaries(cmsHPROFILE profile) {
  const auto* r = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigRedColorantTag));
  const auto* g = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigGreenColorantTag));
  const auto* b = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigBlueColorantTag));
  if (!r || !g || !b) return 0;
  const std::array<double, 9> actual{r->X, r->Y, r->Z, g->X, g->Y, g->Z, b->X, b->Y, b->Z};
  struct Candidate { uint16_t cicp; cmsCIExyYTRIPLE xy; };
  const std::array<Candidate, 3> candidates{{
      {1, {{0.6400, 0.3300, 1.0}, {0.3000, 0.6000, 1.0}, {0.1500, 0.0600, 1.0}}},
      {9, {{0.7080, 0.2920, 1.0}, {0.1700, 0.7970, 1.0}, {0.1310, 0.0460, 1.0}}},
      {12, {{0.6800, 0.3200, 1.0}, {0.2650, 0.6900, 1.0}, {0.1500, 0.0600, 1.0}}},
  }};
  const cmsCIExyY white{0.3127, 0.3290, 1.0};
  uint16_t best = 0; double best_error = std::numeric_limits<double>::max();
  for (const auto& candidate : candidates) {
    cmsToneCurve* curve = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve* curves[3]{curve, curve, curve};
    cmsHPROFILE reference = curve ? cmsCreateRGBProfile(&white, &candidate.xy, curves) : nullptr;
    if (curve) cmsFreeToneCurve(curve);
    if (!reference) continue;
    const auto* rr = static_cast<const cmsCIEXYZ*>(cmsReadTag(reference, cmsSigRedColorantTag));
    const auto* rg = static_cast<const cmsCIEXYZ*>(cmsReadTag(reference, cmsSigGreenColorantTag));
    const auto* rb = static_cast<const cmsCIEXYZ*>(cmsReadTag(reference, cmsSigBlueColorantTag));
    if (rr && rg && rb) {
      const std::array<double, 9> expected{rr->X, rr->Y, rr->Z, rg->X, rg->Y, rg->Z,
                                           rb->X, rb->Y, rb->Z};
      double error = 0.0;
      for (size_t i = 0; i < actual.size(); ++i) {
        const double delta = actual[i] - expected[i]; error += delta * delta;
      }
      if (error < best_error) { best_error = error; best = candidate.cicp; }
    }
    cmsCloseProfile(reference);
  }
  return best_error < 1e-4 ? best : 0;
}

inline uint16_t identify_sdr_transfer(const std::array<const cmsToneCurve*, 3>& curves) {
  if (!curves[0] || !curves[1] || !curves[2]) return 0;
  bool matches_srgb = true, matches_rec2020 = true;
  constexpr double alpha = 1.09929682680944, beta = 0.018053968510807;
  for (const auto* curve : curves) {
    for (const float value : {0.03f, 0.07f, 0.2f, 0.4f, 0.7f, 0.9f}) {
      const double actual = cmsEvalToneCurveFloat(curve, value);
      const double srgb = value <= 0.04045 ? value / 12.92
          : std::pow((value + 0.055) / 1.055, 2.4);
      const double rec2020 = value < 4.5 * beta ? value / 4.5
          : std::pow((value + alpha - 1.0) / alpha, 1.0 / 0.45);
      matches_srgb &= std::abs(actual - srgb) < 0.0003;
      matches_rec2020 &= std::abs(actual - rec2020) < 0.0003;
    }
  }
  if (matches_srgb) return 13;
  if (matches_rec2020) return 14;
  return 0;
}

inline uint16_t identify_sdr_transfer(cmsHPROFILE profile) {
  return identify_sdr_transfer({
      static_cast<const cmsToneCurve*>(cmsReadTag(profile, cmsSigRedTRCTag)),
      static_cast<const cmsToneCurve*>(cmsReadTag(profile, cmsSigGreenTRCTag)),
      static_cast<const cmsToneCurve*>(cmsReadTag(profile, cmsSigBlueTRCTag))});
}

}  // namespace hdrbridge::icc
