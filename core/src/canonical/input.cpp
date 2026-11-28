#include "canonical/input.h"
#include "canonical/cache.h"
#include "codecs/heif.h"
#include "codecs/hsp.h"
#include "codecs/gainmap/avif.h"
#include "codecs/gainmap/tiff.h"
#include "codecs/gainmap/heif.h"
#include "codecs/gainmap/jpeg.h"
#include "codecs/gainmap/jxl.h"
#include "codecs/jxl.h"
#include "codecs/jxr.h"
#include "codecs/png.h"
#include "codecs/tiff.h"
#include "codecs/ultrahdr.h"
#include "image/orientation.h"
#include "infrastructure/files.h"
#include "infrastructure/timing.h"
#include "infrastructure/progress.h"
#include <algorithm>
#include <stdexcept>

namespace hdrbridge::detail {

DecodedImage decode_input_uncached(const std::filesystem::path& path,
                                   const ProgressCallback& progress,
                                   std::atomic_bool* cancel) {
  const auto decode_start = Clock::now();
  const std::string extension = extension_lower(path);
  if (extension == ".avif" && gainmap::is_adobe_tmap_avif(path)) {
    report(progress, 6, "Parsing AVIF base/gain-map/tmap item graph and ISO metadata");
    auto reconstructed = gainmap::reconstruct_adobe_tmap_avif(path, cancel);
    DecodedImage decoded;
    decoded.info = std::move(reconstructed.info);
    decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
    decoded.exif = std::move(reconstructed.exif);
    decoded.xmp = std::move(reconstructed.xmp);
    decoded.timings.decode_ms = reconstructed.decode_ms;
    decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
    decoded.timings.orientation_ms = reconstructed.orientation_ms;
    decoded.gain_map_formula_max_error = reconstructed.independent_formula_max_error;
    report(progress, 42, "Adobe tmap gain-map reconstructed to canonical high-precision HDR master");
    return decoded;
  }
  if ((extension == ".tif" || extension == ".tiff") &&
      inspect_tiff_metadata(path).gain_map_present) {
    report(progress, 6, "Parsing TIFF base/gain-map SubIFD and ISO metadata");
    auto reconstructed = gainmap::reconstruct_adobe_gainmap_tiff(path, cancel);
    DecodedImage decoded;
    decoded.info = std::move(reconstructed.info);
    decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
    decoded.exif = std::move(reconstructed.exif);
    decoded.xmp = std::move(reconstructed.xmp);
    decoded.timings.decode_ms = reconstructed.decode_ms;
    decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
    decoded.timings.orientation_ms = reconstructed.orientation_ms;
    report(progress, 42, "Adobe TIFF gain map reconstructed to canonical high-precision HDR master");
    return decoded;
  }
  if (extension == ".hif" || extension == ".heic" || extension == ".heif" || extension == ".avif") {
    const auto gainmap = gainmap::probe_heif_gainmap(path);
    if (gainmap.detected) {
      report(progress, 6, gainmap.is_apple
          ? "Parsing Apple HEIC base/auxiliary/tmap item graph and metadata"
          : "Parsing HEIF/HEIC base/gain-map/tmap item graph and ISO metadata");
      auto reconstructed = gainmap::reconstruct_heif_gainmap(path, cancel);
      DecodedImage decoded;
      decoded.info = std::move(reconstructed.info);
      decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
      decoded.exif = std::move(reconstructed.exif);
      decoded.xmp = std::move(reconstructed.xmp);
      decoded.timings.decode_ms = reconstructed.decode_ms;
      decoded.timings.base_decode_ms = reconstructed.base_decode_ms;
      decoded.timings.gain_map_decode_ms = reconstructed.gain_map_decode_ms;
      decoded.timings.gain_map_upsample_ms = reconstructed.gain_map_upsample_ms;
      decoded.timings.gain_apply_ms = reconstructed.gain_apply_ms;
      decoded.timings.gain_map_color_conversion_ms = reconstructed.color_conversion_ms;
      decoded.timings.color_conversion_ms = reconstructed.color_conversion_ms;
      decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
      decoded.timings.orientation_ms = reconstructed.orientation_ms;
      report(progress, 42, gainmap.is_apple
          ? "Apple HEIC gain map reconstructed to canonical high-precision HDR master"
          : "ISO tmap HEIF/HEIC gain map reconstructed to canonical high-precision HDR master");
      return decoded;
    }
    return decode_direct_hdr(path, progress, cancel);
  }
  if (extension == ".hsp") {
    return decode_hsp_input(path, progress, cancel);
  }
  report(progress, 8, "Inspecting high-dynamic-range input");
  check_cancel(cancel);
  DecodedImage decoded;
  if (extension == ".jxl" && gainmap::is_iso_gainmap_jxl(path)) {
    report(progress, 6, "Parsing JPEG XL jhgm base, gain-map codestream, and ISO metadata");
    auto reconstructed = gainmap::reconstruct_iso_gainmap_jxl(path, cancel);
    decoded.info = std::move(reconstructed.info);
    decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
    decoded.exif = std::move(reconstructed.exif);
    decoded.xmp = std::move(reconstructed.xmp);
    decoded.timings.decode_ms = reconstructed.decode_ms;
    decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
    report(progress, 42, "JPEG XL jhgm reconstructed to canonical high-precision HDR master");
    return decoded;
  }
  if (extension == ".jxl") decoded = decode_jxl_input(path);
  else if (extension == ".jxr" || extension == ".wdp" || extension == ".hdp") decoded = decode_jxr_input(path);
  else if (extension == ".png") decoded = decode_png_input(path);
  else if (extension == ".tif" || extension == ".tiff") decoded = decode_tiff_input(path);
  else if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe") {
    decoded = gainmap::decode_jpeg_asset(path, progress, cancel);
  }
  else throw std::runtime_error("unsupported input format: " + extension);
  check_cancel(cancel);
  if (decoded.timings.decode_ms == 0.0) decoded.timings.decode_ms = elapsed_ms(decode_start);
  const auto orientation_start = Clock::now();
  if (!decoded.rgb.empty() && decoded.info.original_orientation != 1 &&
      !decoded.info.orientation_normalized) {
    const uint32_t source_width = decoded.info.width;
    const uint32_t source_height = decoded.info.height;
    const auto transformed = orientation::normalize_rgb16(
        decoded.rgb, decoded.info.width, decoded.info.height,
        decoded.info.original_orientation);
    if (!decoded.alpha.empty()) {
      orientation::normalize_plane16(decoded.alpha, source_width, source_height,
                                     decoded.info.original_orientation);
    }
    decoded.info.width = transformed.width;
    decoded.info.height = transformed.height;
  }
  orientation::set_exif_orientation_to_one(decoded.exif);
  orientation::set_xmp_orientation_to_one(decoded.xmp);
  decoded.info.orientation_normalized = true;
  decoded.timings.orientation_ms += elapsed_ms(orientation_start);
  report(progress, 42, decoded.info.asset_kind == "direct-hdr" ||
                       decoded.info.asset_kind == "gain-map-hdr"
      ? "Canonical high-precision HDR master ready"
      : "Source decoded; no HDR representation found");
  return decoded;
}

SourceInfo inspect_any(const std::filesystem::path& path) {
  const std::string extension = extension_lower(path);
  if (extension == ".avif" && gainmap::is_adobe_tmap_avif(path)) {
    return gainmap::inspect_adobe_tmap_avif(path);
  }
  if (extension == ".hif" || extension == ".heic" || extension == ".heif" || extension == ".avif") {
    return inspect_heif_input(path);
  }
  if (extension == ".hsp") {
    return inspect_hsp_input(path);
  }
  if (extension == ".tif" || extension == ".tiff") {
    auto info = inspect_tiff_metadata(path);
    if (info.gain_map_present) return gainmap::inspect_adobe_gainmap_tiff(path);
    if (info.original_orientation >= 5) std::swap(info.width, info.height);
    return info;
  }
  if (extension == ".jxl" && gainmap::is_iso_gainmap_jxl(path)) {
    return gainmap::inspect_iso_gainmap_jxl(path);
  }
  if (extension == ".jxl" || extension == ".jxr" || extension == ".wdp" ||
      extension == ".hdp" || extension == ".png") {
    TimingDiagnostics timings;
    return decode_input(path, {}, nullptr, timings)->info;
  }
  if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe") {
    return gainmap::inspect_jpeg_asset(path);
  }
  throw std::runtime_error("unsupported input format: " + extension);
}

}  // namespace hdrbridge::detail
