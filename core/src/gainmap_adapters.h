#pragma once

#include "hdrbridge_core.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hdrbridge::gainmap {

struct ReconstructedHdr {
  SourceInfo info;
  std::vector<uint16_t> rec2020_pq_rgb;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  double decode_ms = 0.0;
  double base_decode_ms = 0.0;
  double gain_map_decode_ms = 0.0;
  double gain_map_upsample_ms = 0.0;
  double gain_apply_ms = 0.0;
  double color_conversion_ms = 0.0;
  double reconstruction_ms = 0.0;
  double orientation_ms = 0.0;
  double independent_formula_max_error = 0.0;
  uint16_t max_cll = 0;
  uint16_t max_pall = 0;
};

bool is_adobe_tmap_avif(const std::filesystem::path& path);
SourceInfo inspect_adobe_tmap_avif(const std::filesystem::path& path);
ReconstructedHdr reconstruct_adobe_tmap_avif(const std::filesystem::path& path,
                                              std::atomic_bool* cancel = nullptr);

bool is_adobe_gainmap_tiff(const std::filesystem::path& path);
SourceInfo inspect_adobe_gainmap_tiff(const std::filesystem::path& path);
ReconstructedHdr reconstruct_adobe_gainmap_tiff(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);

struct AppleAuxiliaryProbe {
  bool detected = false;
  std::string container;
  std::string auxiliary_type;
  uint32_t item_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bit_depth = 0;
  bool extraction_supported = false;
};

// Container-specific Apple inputs are kept separate until they have been
// decoded to this neutral gain-map asset.  Adobe tmap and Ultra HDR metadata
// are deliberately not represented by this type.
struct GainMapAsset {
  SourceInfo info;
  uint32_t storage_width = 0;
  uint32_t storage_height = 0;
  uint32_t map_storage_width = 0;
  uint32_t map_storage_height = 0;
  std::vector<uint16_t> base_rgb16;
  std::vector<uint16_t> gain_map_rgb16;
  std::vector<uint8_t> base_icc;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  double container_parse_ms = 0.0;
  double base_decode_ms = 0.0;
  double gain_map_decode_ms = 0.0;
  double reference_white_nits = 203.0;
};

AppleAuxiliaryProbe probe_apple_heif(const std::filesystem::path& path);
AppleAuxiliaryProbe probe_apple_jpeg(const std::filesystem::path& path);
SourceInfo inspect_apple_heif_gainmap(const std::filesystem::path& path);
SourceInfo inspect_apple_jpeg_gainmap(const std::filesystem::path& path);
std::vector<uint16_t> extract_apple_heif_auxiliary_plane(
    const std::filesystem::path& path,
    const AppleAuxiliaryProbe& probe,
    std::atomic_bool* cancel = nullptr);
GainMapAsset extract_apple_heif_gainmap_asset(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);
ReconstructedHdr reconstruct_apple_heif_gainmap(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);
ReconstructedHdr reconstruct_apple_jpeg_gainmap(
    const std::filesystem::path& path,
    std::atomic_bool* cancel = nullptr);

}  // namespace hdrbridge::gainmap
