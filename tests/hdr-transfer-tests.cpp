#include "hdr_transfer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

int main() {
  using hdrbridge::transfer::hlg_to_linear_nits;
  using hdrbridge::transfer::linear_nits_to_hlg;
  using hdrbridge::transfer::nits_to_pq;
  using hdrbridge::transfer::pq_to_nits;

  double max_hlg_signal_error = 0.0;
  double max_pq_signal_error = 0.0;
  for (int r = 0; r <= 16; ++r) {
    for (int g = 0; g <= 16; ++g) {
      for (int b = 0; b <= 16; ++b) {
        const std::array<double, 3> source{
            r / 16.0, g / 16.0, b / 16.0};
        const auto nits = hlg_to_linear_nits(source);
        const auto roundtrip = linear_nits_to_hlg(nits);
        for (size_t c = 0; c < 3; ++c) {
          max_hlg_signal_error = std::max(
              max_hlg_signal_error, std::abs(roundtrip[c] - source[c]));
          const double pq = nits_to_pq(nits[c]);
          max_pq_signal_error = std::max(
              max_pq_signal_error, std::abs(pq_to_nits(pq) - nits[c]));
        }
      }
    }
  }

  const auto reference_white = hlg_to_linear_nits({0.75, 0.75, 0.75});
  const double white_error = std::abs(reference_white[1] - 203.0);
  std::cout << "maxHlgSignalRoundtripError=" << max_hlg_signal_error << '\n'
            << "maxPqNitsRoundtripError=" << max_pq_signal_error << '\n'
            << "hlg75ReferenceWhiteNits=" << reference_white[1] << '\n';
  if (max_hlg_signal_error > 2e-12 || max_pq_signal_error > 2e-8 ||
      white_error > 0.25) {
    return 1;
  }
  return 0;
}
