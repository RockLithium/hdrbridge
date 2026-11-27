#pragma once

#include <lcms2.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace hdrbridge::gainmap::detail {

struct CmsProfileDeleter {
  void operator()(cmsHPROFILE value) const { if (value) cmsCloseProfile(value); }
};

using CmsProfilePtr = std::unique_ptr<std::remove_pointer_t<cmsHPROFILE>, CmsProfileDeleter>;

std::string profile_description(cmsHPROFILE profile);
std::string profile_description(const void* data, uint32_t size);

bool is_adaptive_hdr_icc(const void* data, size_t size);
std::optional<double> adaptive_hdr_icc_reference_white_nits(const void* data,
                                                            size_t size,
                                                            bool allow_lumi = true);
std::vector<std::vector<uint8_t>> extract_jpeg_icc_profiles(const uint8_t* data,
                                                            size_t size);
std::optional<double> jpeg_adaptive_hdr_icc_reference_white_nits(
    const uint8_t* base_data,
    size_t base_size,
    const uint8_t* gain_data,
    size_t gain_size,
    bool allow_lumi = false);

}  // namespace hdrbridge::gainmap::detail
