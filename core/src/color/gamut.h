#pragma once

#include "canonical/decoded_image.h"

namespace hdrbridge::detail {

void convert_rec709_pq_to_rec2020_pq(std::vector<uint16_t>& pixels);

std::vector<uint16_t> convert_rec2020_pq_to_p3_pq(const DecodedImage& decoded);

std::vector<uint16_t> convert_rec2020_pq_to_rec709_pq(const DecodedImage& decoded);

std::vector<uint16_t> convert_rec2020_pq_to_hlg(const DecodedImage& decoded,
                                                 const std::string& gamut);

void convert_p3_pq_to_rec2020_pq(std::vector<uint16_t>& pixels);

}  // namespace hdrbridge::detail
