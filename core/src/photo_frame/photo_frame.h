#pragma once

#include "hdrbridge_core.h"

namespace hdrbridge::detail {

ConversionResult convert_photo_frame(const std::filesystem::path& input,const std::filesystem::path& output,
                                     const ConversionOptions& options,const ProgressCallback& progress,std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
