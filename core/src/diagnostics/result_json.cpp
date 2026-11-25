#include "diagnostics/result_json.h"
#include "color/signaling.h"
#include <nlohmann/json.hpp>

namespace hdrbridge {

using namespace detail;

using json = nlohmann::json;

static json source_json(const SourceInfo& i) {
  json result = {{"path", i.path.u8string()}, {"format", i.format},
                 {"assetKind", i.asset_kind}, {"gainMapPresent", i.gain_map_present},
                 {"width", i.width}, {"height", i.height},
                 {"orientation", {{"source", i.original_orientation},
                                   {"canonical", i.orientation_normalized ? 1 : i.original_orientation}}},
                 {"containerBrand", i.container_brand}, {"isGrid", i.is_grid},
                 {"gridColumns", i.grid_columns}, {"gridRows", i.grid_rows},
                 {"tileWidth", i.tile_width}, {"tileHeight", i.tile_height},
                 {"codec", i.codec}, {"profile", i.profile}, {"bitDepth", i.bit_depth},
                 {"pixelFormat", i.pixel_format}, {"colorSignalKind", i.color_signal_kind},
                 {"chroma", i.chroma},
                 {"range", !i.range_known ? "unknown" : i.full_range ? "full" : "limited"},
                 {"color", {{"primaries", i.primaries}, {"transfer", i.transfer},
                             {"transferName", public_transfer_name(i.transfer)},
                             {"matrix", i.matrix}}},
                 {"exifPresent", i.exif_present}, {"xmpPresent", i.xmp_present},
                 {"iccPresent", i.icc_present},
                 {"metadata", {{"exif", i.exif_status}, {"xmp", i.xmp_status},
                                {"icc", i.icc_status}, {"orientation", i.orientation_status}}}};
  result["nativeSignal"] = {{"present", i.native_color_present},
                            {"description", i.native_color_description},
                            {"primaries", i.native_primaries},
                            {"transfer", i.native_transfer},
                            {"matrix", i.native_matrix},
                            {"range", !i.native_range_known ? "unknown" :
                                      i.native_full_range ? "full" : "limited"}};
  result["iccSignal"] = {{"present", i.icc_present},
                         {"description", i.icc_description},
                         {"version", i.icc_version},
                         {"cicpPresent", i.icc_cicp_present},
                         {"primaries", i.icc_primaries},
                         {"transfer", i.icc_transfer},
                         {"transferName", i.icc_transfer_interpretation},
                         {"matrix", i.icc_matrix},
                         {"range", i.icc_cicp_present ?
                                   (i.icc_full_range ? "full" : "limited") : "unknown"}};
  result["resolvedColor"] = {{"source", i.resolved_signaling_source},
                             {"conflict", i.color_signaling_conflict},
                             {"primaries", i.primaries},
                             {"transfer", i.transfer},
                             {"transferName", public_transfer_name(i.transfer)},
                             {"matrix", i.matrix},
                             {"representation", i.asset_kind}};
  if (i.gain_map_present) {
    result["gainMapFamily"] = i.gain_map_family;
    if (i.base_item_id || i.gain_map_item_id || i.tone_map_item_id) {
      result["gainMapItems"] = {{"base", i.base_item_id}, {"gainMap", i.gain_map_item_id},
                                {"toneMap", i.tone_map_item_id}};
    }
    result["gainMapSize"] = {{"width", i.gain_map_width}, {"height", i.gain_map_height}};
    result["baseRendition"] = {{"width", i.base_width}, {"height", i.base_height},
                               {"bitDepth", i.base_bit_depth}, {"channels", i.base_channels},
                               {"codec", i.base_codec}, {"colorSpace", i.base_color_space},
                               {"transfer", i.base_transfer}};
    result["gainMapLayout"] = {{"channels", i.gain_map_channels},
                               {"scaleX", i.gain_map_scale_x},
                               {"scaleY", i.gain_map_scale_y}};
    result["gainMapMetadata"] = {{"baseHdrHeadroom", i.base_hdr_headroom},
                                 {"alternateHdrHeadroom", i.alternate_hdr_headroom},
                                 {"hdrCapacityMin", i.hdr_capacity_min},
                                 {"hdrCapacityMax", i.hdr_capacity_max},
                                 {"gainMapMin", i.gain_map_min}, {"gainMapMax", i.gain_map_max},
                                 {"gamma", i.gain_map_gamma}, {"baseOffset", i.base_offset},
                                 {"alternateOffset", i.alternate_offset},
                                 {"useBaseColorSpace", i.gain_map_uses_base_color_space}};
    result["reconstructedHdr"] = {{"width", i.width}, {"height", i.height},
                                  {"colorSpace", i.reconstructed_color_space},
                                  {"transfer", i.reconstructed_transfer},
                                  {"precision", i.reconstructed_precision}};
    if (!i.auxiliary_type.empty()) result["auxiliaryType"] = i.auxiliary_type;
  }
  return result;
}

static json verification_json(const Verification& v) {
  return {{"passed", v.passed}, {"width", v.width}, {"height", v.height},
          {"bitDepth", v.bit_depth}, {"pixelFormat", v.pixel_format},
          {"colorEncoding", v.color_encoding}, {"exactRoundtrip", v.exact_roundtrip},
          {"finite", v.finite}, {"minValue", v.min_value}, {"maxValue", v.max_value},
          {"hdrDiagnostics", {{"maxChannelNits", v.max_channel_nits},
                              {"maxLuminanceNits", v.max_luminance_nits},
                              {"percentile99_9Nits", v.percentile_99_9_nits},
                              {"percentile99_99Nits", v.percentile_99_99_nits},
                              {"chosenTargetPeakNits", v.chosen_target_peak_nits},
                              {"clippedPixelFraction", v.clipped_pixel_fraction},
                              {"peakChoiceReason", v.peak_choice_reason},
                              {"hdrCapacityMax", v.hdr_capacity_max},
                              {"gainMapWidth", v.gain_map_width},
                              {"gainMapHeight", v.gain_map_height},
                              {"gainMapChannels", v.gain_map_channels},
                              {"gainMapChannelDifferenceMax", v.gain_map_channel_difference_max},
                              {"inputGainMapFormulaMaxError", v.input_gain_map_formula_max_error},
                              {"reconstructionRmse", v.reconstruction_rmse},
                              {"reconstructionMaxAbsError", v.reconstruction_max_abs_error},
                              {"transferConversionRmse", v.transfer_conversion_rmse},
                              {"transferConversionMaxAbsError", v.transfer_conversion_max_abs_error},
                              {"sourceToCanonicalRmse", v.source_to_canonical_rmse},
                              {"sourceToCanonicalMaxAbsError", v.source_to_canonical_max_abs_error}}},
          {"checks", v.checks}};
}

std::string detail::verification_error_json(const Verification& verification) {
  return verification_json(verification).dump();
}

std::string to_json(const SourceInfo& info) { return source_json(info).dump(2); }
std::string to_json(const Verification& v) { return verification_json(v).dump(2); }
std::string to_json(const ConversionResult& r) {
  return json{{"success", r.success}, {"outputPath", r.output_path.u8string()},
              {"outputBytes", r.output_bytes}, {"mode", r.mode}, {"sha256", r.sha256},
              {"timings", {{"inspectMs", r.timings.inspect_ms}, {"decodeMs", r.timings.decode_ms},
                            {"baseDecodeMs", r.timings.base_decode_ms},
                            {"gainMapDecodeMs", r.timings.gain_map_decode_ms},
                            {"gainMapUpsampleMs", r.timings.gain_map_upsample_ms},
                            {"gainApplyMs", r.timings.gain_apply_ms},
                            {"gainMapColorConversionMs", r.timings.gain_map_color_conversion_ms},
                            {"orientationMs", r.timings.orientation_ms},
                            {"colorConversionMs", r.timings.color_conversion_ms},
                            {"gainMapMs", r.timings.gain_map_ms}, {"encodeMs", r.timings.encode_ms},
                            {"verificationMs", r.timings.verification_ms},
                            {"totalMs", r.timings.total_ms},
                            {"canonicalCacheHit", r.timings.canonical_cache_hit}}},
              {"verification", verification_json(r.verification)}}.dump(2);
}

}  // namespace hdrbridge
