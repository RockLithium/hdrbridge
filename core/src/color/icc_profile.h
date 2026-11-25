#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hdrbridge::detail {

std::vector<uint8_t> make_pq_icc(bool display_p3,
                                 const std::string& description_override = {},
                                 bool include_cicp = false);

std::vector<uint8_t> make_hdr_tiff_icc(bool display_p3);

std::vector<uint8_t> make_windows_compatible_pq_icc(bool display_p3);

}  // namespace hdrbridge::detail
