#pragma once

#include <lcms2.h>
#include <memory>
#include <string>
#include <type_traits>

namespace hdrbridge::gainmap::detail {

struct CmsProfileDeleter {
  void operator()(cmsHPROFILE value) const { if (value) cmsCloseProfile(value); }
};

using CmsProfilePtr = std::unique_ptr<std::remove_pointer_t<cmsHPROFILE>, CmsProfileDeleter>;

std::string profile_description(cmsHPROFILE profile);
std::string profile_description(const void* data, uint32_t size);

}  // namespace hdrbridge::gainmap::detail
