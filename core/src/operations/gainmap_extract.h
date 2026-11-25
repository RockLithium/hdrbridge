#pragma once

#include "hdrbridge_core.h"

namespace hdrbridge::detail {

ConversionResult convert_gainmap_extract(const std::filesystem::path& input,
                                         const std::filesystem::path& output,
                                         const ConversionOptions& options,
                                         ProgressCallback progress,
                                         std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
