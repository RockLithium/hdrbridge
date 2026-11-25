#include "codecs/gainmap/icc_description.h"
#include <array>

namespace hdrbridge::gainmap::detail {

std::string profile_description(cmsHPROFILE profile) {
  std::array<char, 256> description{};
  cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US",
                         description.data(), static_cast<cmsUInt32Number>(description.size()));
  return description.data();
}

std::string profile_description(const void* data, uint32_t size) {
  const CmsProfilePtr profile(cmsOpenProfileFromMem(data, size));
  return profile ? profile_description(profile.get()) : std::string{};
}

}  // namespace hdrbridge::gainmap::detail
