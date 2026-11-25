#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>

inline size_t hdrbridge_uhdr_mul(size_t a, size_t b) {
  if (b && a > std::numeric_limits<size_t>::max() / b)
    throw std::overflow_error("Ultra HDR buffer size overflow");
  return a * b;
}
inline size_t hdrbridge_uhdr_add(size_t a, size_t b) {
  if (a > std::numeric_limits<size_t>::max() - b)
    throw std::overflow_error("Ultra HDR buffer size overflow");
  return a + b;
}
