#include "color/pq.h"

#include <cstddef>
#include <memory>

namespace hdrbridge::detail {

const std::array<double, 65536>& pq_nits_lut() {
  static const auto values = [] {
    auto result = std::make_unique<std::array<double, 65536>>();
    for (size_t i = 0; i < result->size(); ++i) (*result)[i] = inverse_pq(i / 65535.0);
    return result;
  }();
  return *values;
}

}  // namespace hdrbridge::detail
