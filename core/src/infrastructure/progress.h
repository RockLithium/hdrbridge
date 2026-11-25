#pragma once

#include "hdrbridge_core.h"

namespace hdrbridge::detail {

void report(const ProgressCallback& cb, int value, const std::string& stage);

void check_cancel(std::atomic_bool* cancel);

}  // namespace hdrbridge::detail
