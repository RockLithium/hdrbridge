#include "infrastructure/progress.h"
#include <stdexcept>

namespace hdrbridge::detail {

void report(const ProgressCallback& cb, int value, const std::string& stage) {
  if (cb) cb(value, stage);
}

void check_cancel(std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
}

}  // namespace hdrbridge::detail
