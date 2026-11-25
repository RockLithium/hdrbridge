#pragma once

#include "hdrbridge_core.h"

namespace hdrbridge::detail {

const char* metadata_status(bool present);

void finalize_source_metadata(SourceInfo& info);

}  // namespace hdrbridge::detail
