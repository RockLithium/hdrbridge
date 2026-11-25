#include "color/signaling.h"
#include "color/icc_rgb_primaries.h"
#include "color/pq.h"
#include <lcms2.h>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace hdrbridge::detail {

HdrTransfer public_transfer_kind(uint16_t cicp_transfer) {
  if (cicp_transfer == 16) return HdrTransfer::pq_st2084;
  if (cicp_transfer == 18) return HdrTransfer::hlg_bt2100;
  return HdrTransfer::unknown;
}

const char* public_transfer_name(uint16_t cicp_transfer) {
  if (cicp_transfer == 1) return "BT.709";
  if (cicp_transfer == 8) return "Linear";
  if (cicp_transfer == 13) return "sRGB";
  if (cicp_transfer == 14) return "BT.2020 SDR";
  return transfer::name(transfer::from_cicp(cicp_transfer));
}

static std::string icc_info_ascii(cmsHPROFILE profile, cmsInfoType type) {
  const cmsUInt32Number size = cmsGetProfileInfoASCII(profile, type, "en", "US", nullptr, 0);
  if (!size) return {};
  std::string text(size, '\0');
  cmsGetProfileInfoASCII(profile, type, "en", "US", text.data(), size);
  if (!text.empty() && text.back() == '\0') text.pop_back();
  return text;
}

static uint16_t identify_icc_transfer(cmsHPROFILE profile) {
  const auto* curve = static_cast<const cmsToneCurve*>(cmsReadTag(profile, cmsSigRedTRCTag));
  if (!curve) return 0;
  constexpr std::array<double, 5> samples{0.10, 0.25, 0.50, 0.75, 0.90};
  double pq_error = 0.0, hlg_error = 0.0;
  constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
  for (double encoded : samples) {
    const double actual = cmsEvalToneCurveFloat(curve, static_cast<cmsFloat32Number>(encoded));
    const double pq = inverse_pq(encoded) / 10000.0;
    const double hlg = encoded <= 0.5 ? encoded * encoded / 3.0
                                     : (std::exp((encoded - c) / a) + b) / 12.0;
    pq_error += std::pow(actual - pq, 2.0);
    hlg_error += std::pow(actual - hlg, 2.0);
  }
  if (pq_error < 2e-5) return 16;
  if (hlg_error < 2e-4) return 18;
  return 0;
}

IccSignal parse_icc_signal(const void* data, size_t size) {
  IccSignal result;
  result.present = data && size > 0;
  if (!result.present || size > std::numeric_limits<cmsUInt32Number>::max()) return result;
  cmsHPROFILE profile = cmsOpenProfileFromMem(data, static_cast<cmsUInt32Number>(size));
  if (!profile) return result;
  result.valid = true;
  result.description = icc_info_ascii(profile, cmsInfoDescription);
  const double version = cmsGetProfileVersion(profile);
  std::ostringstream version_text;
  version_text << std::fixed << std::setprecision(1) << version;
  result.version = version_text.str();
  const uint16_t transform_primaries = icc::identify_rgb_primaries(profile);
  const uint16_t transform_transfer = identify_icc_transfer(profile);
  const auto* cicp = static_cast<const cmsVideoSignalType*>(cmsReadTag(profile, cmsSigcicpTag));
  if (cicp) {
    result.cicp_present = true;
    result.primaries = static_cast<uint16_t>(cicp->ColourPrimaries);
    result.transfer = static_cast<uint16_t>(cicp->TransferCharacteristics);
    result.sdr_transfer = result.transfer;
    result.matrix = static_cast<uint16_t>(cicp->MatrixCoefficients);
    result.full_range = cicp->VideoFullRangeFlag != 0;
    result.transfer_interpretation = std::string("CICP ") + public_transfer_name(result.transfer);
    if (transform_transfer == result.transfer) result.transfer_interpretation += "; TRC agrees";
    else if (transform_transfer != 0) result.transfer_interpretation += "; legacy/fallback TRC differs";
  } else {
    result.primaries = transform_primaries;
    result.transfer = transform_transfer;
    result.sdr_transfer = icc::identify_sdr_transfer(profile);
    result.matrix = 0;
    result.full_range = transform_transfer != 0;
    result.transfer_interpretation = transform_transfer
        ? std::string("matrix/TRC interpreted as ") + public_transfer_name(transform_transfer)
        : "matrix/TRC is not a recognized PQ or HLG transform";
  }
  cmsCloseProfile(profile);
  return result;
}

void store_icc_signal(SourceInfo& info, const IccSignal& icc) {
  info.icc_present = icc.present;
  info.icc_status = icc.present ? (icc.valid ? "present" : "read-error") : "absent";
  info.icc_description = icc.description;
  info.icc_version = icc.version;
  info.icc_cicp_present = icc.cicp_present;
  info.icc_primaries = icc.primaries;
  info.icc_transfer = icc.transfer;
  info.icc_matrix = icc.matrix;
  info.icc_full_range = icc.full_range;
  info.icc_transfer_interpretation = icc.transfer_interpretation;
}

bool hdr_color(uint16_t primaries, uint16_t transfer) {
  return (transfer == 16 || transfer == 18) &&
         (primaries == 1 || primaries == 9 || primaries == 12);
}

void resolve_color_signaling(SourceInfo& info, bool native_preferred) {
  const bool native_hdr = info.native_color_present &&
                          hdr_color(info.native_primaries, info.native_transfer);
  const bool icc_hdr = info.icc_present &&
                       hdr_color(info.icc_primaries, info.icc_transfer);
  const bool both = info.native_color_present && icc_hdr;
  info.color_signaling_conflict = both &&
      (info.native_primaries != info.icc_primaries ||
       info.native_transfer != info.icc_transfer ||
       info.native_full_range != info.icc_full_range);
  if (info.color_signaling_conflict) {
    info.resolved_signaling_source = "Conflict";
  } else if (both) {
    info.resolved_signaling_source = "Native + ICC";
  } else if (native_hdr || (info.native_color_present && !icc_hdr)) {
    info.resolved_signaling_source = "Native";
  } else if (icc_hdr || info.icc_cicp_present) {
    info.resolved_signaling_source = "ICC";
  } else {
    info.resolved_signaling_source = "Unknown";
  }
  const bool use_native = native_hdr && (native_preferred || !icc_hdr);
  if (use_native) {
    info.primaries = info.native_primaries;
    info.transfer = info.native_transfer;
    info.matrix = info.native_matrix;
    info.full_range = info.native_full_range;
    info.range_known = info.native_range_known;
  } else if (icc_hdr) {
    info.primaries = info.icc_primaries;
    info.transfer = info.icc_transfer;
    // ICC matrix=0 describes RGB profile interpretation, not stored YCbCr.
    // Preserve a native storage matrix when one exists.
    info.matrix = info.native_color_present ? info.native_matrix : info.icc_matrix;
    info.full_range = info.icc_full_range;
    info.range_known = true;
  } else if (info.native_color_present) {
    info.primaries = info.native_primaries;
    info.transfer = info.native_transfer;
    info.matrix = info.native_matrix;
    info.full_range = info.native_full_range;
    info.range_known = info.native_range_known;
  }
  info.transfer_kind = public_transfer_kind(info.transfer);
  if (!info.gain_map_present) info.asset_kind = hdr_color(info.primaries, info.transfer)
      ? "direct-hdr" : "non-HDR";
}

}  // namespace hdrbridge::detail
