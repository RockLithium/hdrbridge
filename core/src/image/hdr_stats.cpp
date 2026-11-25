#include "image/hdr_stats.h"
#include <numeric>
#include "color/pq.h"
#include "infrastructure/pixel_workers.h"
#include <algorithm>
#include <cmath>

namespace hdrbridge::detail {

static double histogram_percentile(const std::array<uint64_t, 10001>& histogram,
                            uint64_t count, double percentile) {
  if (count == 0) return 0.0;
  const uint64_t wanted = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(count * percentile)));
  uint64_t seen = 0;
  for (size_t i = 0; i < histogram.size(); ++i) {
    seen += histogram[i];
    if (seen >= wanted) return static_cast<double>(i);
  }
  return 10000.0;
}

HdrStats measure_hdr(const DecodedImage& decoded) {
  HdrStats stats;
  std::array<uint64_t, 10001> luminance_histogram{};
  std::array<uint64_t, 10001> channel_histogram{};
  const auto& lut = pq_nits_lut();
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  struct PartialStats {
    double max_channel = 0.0;
    double max_luminance = 0.0;
    std::array<uint64_t, 10001> luminance{};
    std::array<uint64_t, 10001> channel{};
  };
  std::vector<PartialStats> partials(default_worker_count());
  const size_t workers = parallel_indexed_blocks(
      count, [&](size_t worker, size_t begin, size_t end) {
        auto& partial = partials[worker];
        for (size_t p = begin; p < end; ++p) {
          const double r = lut[decoded.rgb[p * 3u]];
          const double g = lut[decoded.rgb[p * 3u + 1u]];
          const double b = lut[decoded.rgb[p * 3u + 2u]];
          const double channel = std::max({r, g, b});
          const double luminance = std::max(
              0.0, 0.2627 * r + 0.6780 * g + 0.0593 * b);
          partial.max_channel = std::max(partial.max_channel, channel);
          partial.max_luminance = std::max(partial.max_luminance, luminance);
          ++partial.luminance[static_cast<size_t>(
              std::clamp(std::llround(luminance), 0ll, 10000ll))];
          ++partial.channel[static_cast<size_t>(
              std::clamp(std::llround(channel), 0ll, 10000ll))];
        }
      });
  for (size_t worker = 0; worker < workers; ++worker) {
    stats.max_channel_nits = std::max(
        stats.max_channel_nits, partials[worker].max_channel);
    stats.max_luminance_nits = std::max(
        stats.max_luminance_nits, partials[worker].max_luminance);
    for (size_t bin = 0; bin < luminance_histogram.size(); ++bin) {
      luminance_histogram[bin] += partials[worker].luminance[bin];
      channel_histogram[bin] += partials[worker].channel[bin];
    }
  }
  stats.p999_nits = histogram_percentile(luminance_histogram, count, 0.999);
  stats.p9999_nits = histogram_percentile(luminance_histogram, count, 0.9999);
  const double p9999_channel = histogram_percentile(channel_histogram, count, 0.9999);
  const double robust = std::max(stats.p9999_nits, p9999_channel) * 1.05;
  const double absolute_peak = std::max(stats.max_luminance_nits, stats.max_channel_nits);
  if (absolute_peak <= 203.0) {
    stats.chosen_target_nits = 204.0;
    stats.reason = "source peak is below the API floor; 204 nits is the minimum decodable capacity above 1.0";
  } else if (absolute_peak <= std::max(robust * 1.25, robust + 25.0)) {
    stats.chosen_target_nits = std::ceil(absolute_peak);
    stats.reason = "finite source maximum is within the 99.99-percentile robustness envelope";
  } else {
    stats.chosen_target_nits = std::ceil(std::clamp(robust, 203.0, 10000.0));
    stats.reason = "99.99-percentile guard excluded sparse hot pixels from capacity selection";
  }
  stats.chosen_target_nits = std::clamp(stats.chosen_target_nits, 203.0, 10000.0);
  std::vector<uint64_t> clipped_parts(default_worker_count());
  const size_t clipped_workers = parallel_indexed_blocks(
      count, [&](size_t worker, size_t begin, size_t end) {
        uint64_t clipped = 0;
        for (size_t p = begin; p < end; ++p) {
          if (lut[decoded.rgb[p * 3u]] > stats.chosen_target_nits ||
              lut[decoded.rgb[p * 3u + 1u]] > stats.chosen_target_nits ||
              lut[decoded.rgb[p * 3u + 2u]] > stats.chosen_target_nits) ++clipped;
        }
        clipped_parts[worker] = clipped;
      });
  const uint64_t clipped = std::accumulate(
      clipped_parts.begin(), clipped_parts.begin() + clipped_workers, uint64_t{0});
  stats.clipped_fraction = count ? static_cast<double>(clipped) / static_cast<double>(count) : 0.0;
  return stats;
}

void attach_hdr_stats(Verification& verification, const HdrStats& stats) {
  verification.max_channel_nits = stats.max_channel_nits;
  verification.max_luminance_nits = stats.max_luminance_nits;
  verification.percentile_99_9_nits = stats.p999_nits;
  verification.percentile_99_99_nits = stats.p9999_nits;
  verification.chosen_target_peak_nits = stats.chosen_target_nits;
  verification.clipped_pixel_fraction = stats.clipped_fraction;
  verification.peak_choice_reason = stats.reason;
}

}  // namespace hdrbridge::detail
