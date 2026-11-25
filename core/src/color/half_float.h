#pragma once

#include <bit>
#include <cstdint>

namespace hdrbridge::detail {

inline uint16_t float_to_half(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponent <= 0) {
    if (exponent < -10) return static_cast<uint16_t>(sign);
    mantissa = (mantissa | 0x800000u) >> static_cast<uint32_t>(1 - exponent);
    return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
  }
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7bffu);
  mantissa += 0x1000u;
  if (mantissa & 0x800000u) { mantissa = 0; ++exponent; }
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7bffu);
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

inline float half_to_float(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
  uint32_t exponent = (half >> 10) & 0x1fu;
  uint32_t mantissa = half & 0x3ffu;
  uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) bits = sign;
    else {
      int shift = 0;
      while ((mantissa & 0x400u) == 0) { mantissa <<= 1; ++shift; }
      mantissa &= 0x3ffu;
      bits = sign | (static_cast<uint32_t>(127 - 15 - shift) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float result = 0;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

}  // namespace hdrbridge::detail
