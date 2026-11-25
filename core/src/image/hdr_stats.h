#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

struct HdrStats {
  double max_channel_nits = 0.0;
  double max_luminance_nits = 0.0;
  double p999_nits = 0.0;
  double p9999_nits = 0.0;
  double chosen_target_nits = 203.0;
  double clipped_fraction = 0.0;
  std::string reason;
};

HdrStats measure_hdr(const DecodedImage& decoded);

void attach_hdr_stats(Verification& verification, const HdrStats& stats);

}  // namespace hdrbridge::detail
