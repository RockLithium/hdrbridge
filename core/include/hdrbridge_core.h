#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hdrbridge {

enum class HdrTransfer : uint8_t {
  unknown = 0,
  pq_st2084,
  hlg_bt2100,
};

struct SourceInfo {
  std::filesystem::path path;
  std::string format;
  std::string asset_kind = "direct-hdr";
  bool gain_map_present = false;
  std::string gain_map_family;
  uint32_t base_item_id = 0;
  uint32_t gain_map_item_id = 0;
  uint32_t tone_map_item_id = 0;
  uint32_t gain_map_width = 0;
  uint32_t gain_map_height = 0;
  uint32_t gain_map_channels = 0;
  double gain_map_scale_x = 0.0;
  double gain_map_scale_y = 0.0;
  double hdr_capacity_min = 0.0;
  double hdr_capacity_max = 0.0;
  uint32_t base_width = 0;
  uint32_t base_height = 0;
  uint32_t base_bit_depth = 0;
  uint32_t base_channels = 0;
  std::string base_codec;
  std::string base_color_space;
  std::string base_transfer;
  std::string reconstructed_color_space = "BT.2020";
  std::string reconstructed_transfer = "Linear HDR";
  std::string reconstructed_precision = "high precision";
  double base_hdr_headroom = 0.0;
  double alternate_hdr_headroom = 0.0;
  std::array<double, 3> gain_map_min{};
  std::array<double, 3> gain_map_max{};
  std::array<double, 3> gain_map_gamma{};
  std::array<double, 3> base_offset{};
  std::array<double, 3> alternate_offset{};
  bool gain_map_uses_base_color_space = true;
  std::string auxiliary_type;
  uint8_t original_orientation = 1;
  bool orientation_normalized = false;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string container_brand;
  bool is_grid = false;
  uint32_t grid_columns = 0;
  uint32_t grid_rows = 0;
  uint32_t tile_width = 0;
  uint32_t tile_height = 0;
  std::string codec;
  std::string profile;
  // Description of how the source container signals color. This is kept
  // separate from the canonical working representation.
  std::string color_signal_kind = "CICP / NCLX";
  std::string pixel_format;
  uint32_t bit_depth = 0;
  std::string chroma;
  uint16_t primaries = 0;
  uint16_t transfer = 0;
  HdrTransfer transfer_kind = HdrTransfer::unknown;
  uint16_t matrix = 0;
  bool full_range = false;
  bool range_known = false;
  bool exif_present = false;
  bool xmp_present = false;
  bool icc_present = false;
  // Values are "present", "absent", "unsupported", or "read-error".
  std::string exif_status = "unsupported";
  std::string xmp_status = "unsupported";
  std::string icc_status = "unsupported";
  std::string orientation_status = "unsupported";
};

struct ConversionOptions {
  std::string mode = "jxl-pq16";
  bool lossless = true;
  float image_quality = 1.0f;
  int effort = 7;
  int base_quality = 95;
  int gainmap_quality = 90;
  int gainmap_scale = 2;
  bool multi_channel_gainmap = false;
  // Zero selects the documented Faithful/Auto estimator. Explicit values must
  // be in libultrahdr's supported 203..10000 nit interval.
  float target_peak_nits = 0.0f;
  // Direct video/NLE outputs default to Rec.2020/PQ. "p3" is optional.
  std::string output_gamut = "rec2020";
  // JPEG XL and AVIF can additionally carry an ISO gain-map representation.
  std::string output_representation = "direct";
  // Direct HDR outputs default to PQ.
  std::string output_transfer = "pq";
  // PNG A/B validation keeps this true for preset A and disables it only for
  // the explicit cICP-only B artifact.
  bool embed_hdr_icc = true;
  // Optional PNG-only ICC identification/description override. The profile
  // primaries, PQ curves and pixel data remain unchanged.
  std::string png_icc_name_override;
  int png_compression_level = 4;
  int tiff_compression_level = 6;
  // Direct HDR TIFF defaults to Adobe Deflate without a horizontal predictor.
  // When false, the same single-strip RGB16 raster is written uncompressed.
  bool tiff_compressed = true;
  bool copy_exif = true;
  bool copy_xmp = true;
  bool overwrite = false;
};

struct TimingDiagnostics {
  double inspect_ms = 0.0;
  double decode_ms = 0.0;
  double base_decode_ms = 0.0;
  double gain_map_decode_ms = 0.0;
  double gain_map_upsample_ms = 0.0;
  double gain_apply_ms = 0.0;
  double gain_map_color_conversion_ms = 0.0;
  double orientation_ms = 0.0;
  double color_conversion_ms = 0.0;
  double gain_map_ms = 0.0;
  double encode_ms = 0.0;
  double verification_ms = 0.0;
  double total_ms = 0.0;
  bool canonical_cache_hit = false;
};

struct Verification {
  bool passed = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bit_depth = 0;
  std::string pixel_format;
  std::string color_encoding;
  bool exact_roundtrip = false;
  bool finite = true;
  double min_value = 0.0;
  double max_value = 0.0;
  double max_channel_nits = 0.0;
  double max_luminance_nits = 0.0;
  double percentile_99_9_nits = 0.0;
  double percentile_99_99_nits = 0.0;
  double chosen_target_peak_nits = 0.0;
  double clipped_pixel_fraction = 0.0;
  double reconstruction_rmse = 0.0;
  double reconstruction_max_abs_error = 0.0;
  double transfer_conversion_rmse = 0.0;
  double transfer_conversion_max_abs_error = 0.0;
  double source_to_canonical_rmse = 0.0;
  double source_to_canonical_max_abs_error = 0.0;
  double hdr_capacity_max = 0.0;
  uint32_t gain_map_width = 0;
  uint32_t gain_map_height = 0;
  uint32_t gain_map_channels = 0;
  double gain_map_channel_difference_max = 0.0;
  std::string peak_choice_reason;
  double input_gain_map_formula_max_error = 0.0;
  std::vector<std::string> checks;
};

struct ConversionResult {
  bool success = false;
  std::filesystem::path output_path;
  uint64_t output_bytes = 0;
  std::string mode;
  std::string sha256;
  Verification verification;
  TimingDiagnostics timings;
};

using ProgressCallback = std::function<void(int, const std::string&)>;

SourceInfo inspect(const std::filesystem::path& path);
ConversionResult convert(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         const ConversionOptions& options,
                         ProgressCallback progress = {},
                         std::atomic_bool* cancel = nullptr);
Verification verify(const std::filesystem::path& output,
                    const std::string& mode);
std::string to_json(const SourceInfo& info);
std::string to_json(const Verification& verification);
std::string to_json(const ConversionResult& result);

}  // namespace hdrbridge
