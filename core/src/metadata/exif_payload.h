#pragma once

#include <cstdint>
#include <vector>

namespace hdrbridge::detail {

std::vector<uint8_t> exif_tiff_payload(const std::vector<uint8_t>& exif);

std::vector<uint8_t> exif_for_jxl(const std::vector<uint8_t>& exif);

std::vector<uint8_t> jpeg_exif_from_heif(const std::vector<uint8_t>& heif_exif,
                                         uint32_t width = 0,
                                         uint32_t height = 0);

}  // namespace hdrbridge::detail
