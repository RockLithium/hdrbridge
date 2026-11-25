#pragma once

#include <array>
#include <cstdint>

namespace hdrbridge::transfer {

enum class Kind : uint8_t {
  unknown = 0,
  pq_st2084,
  hlg_bt2100,
};

Kind from_cicp(uint16_t transfer_characteristics);
const char* name(Kind kind);

double pq_to_nits(double signal);
double nits_to_pq(double nits);

// BT.2100 HLG is scene-referred. These functions include the reference OOTF
// for a 1000-nit display (system gamma 1.2), returning/accepting absolute
// display-linear BT.2020 RGB values in cd/m2.
std::array<double, 3> hlg_to_linear_nits(
    const std::array<double, 3>& signal,
    double peak_nits = 1000.0);
std::array<double, 3> linear_nits_to_hlg(
    const std::array<double, 3>& nits,
    double peak_nits = 1000.0);

}  // namespace hdrbridge::transfer
