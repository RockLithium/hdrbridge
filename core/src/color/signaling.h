#pragma once

#include "hdrbridge_core.h"
#include <cstddef>

namespace hdrbridge::detail {

struct IccSignal {
  bool present = false;
  bool valid = false;
  bool cicp_present = false;
  uint16_t primaries = 0;
  uint16_t transfer = 0;
  uint16_t sdr_transfer = 0;
  uint16_t matrix = 0;
  bool full_range = false;
  std::string description;
  std::string version;
  std::string transfer_interpretation = "Unknown";
};

HdrTransfer public_transfer_kind(uint16_t cicp_transfer);

const char* public_transfer_name(uint16_t cicp_transfer);

IccSignal parse_icc_signal(const void* data, size_t size);

void store_icc_signal(SourceInfo& info, const IccSignal& icc);

bool hdr_color(uint16_t primaries, uint16_t transfer);

void resolve_color_signaling(SourceInfo& info, bool native_preferred);

}  // namespace hdrbridge::detail
