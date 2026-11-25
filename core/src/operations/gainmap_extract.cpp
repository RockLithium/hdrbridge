#include "operations/gainmap_extract.h"
#include "codecs/gainmap/jpeg_export.h"
#include "codecs/png.h"
#include "codecs/tiff.h"
#include "infrastructure/progress.h"
#include "infrastructure/files.h"
#include "infrastructure/timing.h"
#include "infrastructure/output_transaction.h"
#include "diagnostics/digest.h"
#include <algorithm>
#include <cwctype>
#include <limits>
#include <stdexcept>

namespace hdrbridge::detail {

// Verify the file that will be committed, independently of the encoder buffers.
static void verify_gainmap_export(const std::filesystem::path& path,
                           const std::vector<uint8_t>& bytes,
                           const gainmap::GainMapRaster& raster,
                           const std::string& format, bool original) {
  const auto actual = read_file(path);
  if (actual != bytes) throw std::runtime_error("gain-map export byte readback mismatch");
  if (original) return;
  const size_t channels = raster.channels == 3 ? 3u : 1u;
  if (raster.rgb16.size() != static_cast<size_t>(raster.width) * raster.height * 3u)
    throw std::runtime_error("invalid source gain-map raster size");
  if (format == "png") {
    verify_gainmap_png(actual, raster, channels);
  } else if (format == "tiff") {
    verify_gainmap_tiff(path, raster, channels);
  } else if (format == "jpeg") {
    verify_gainmap_jpeg(actual, raster, channels);
  }
}

ConversionResult convert_gainmap_extract(const std::filesystem::path& input,
                                         const std::filesystem::path& output,
                                         const ConversionOptions& options,
                                         ProgressCallback progress,
                                         std::atomic_bool* cancel) {
  const auto total_start = Clock::now();
  if (std::filesystem::exists(output) && !options.overwrite) {
    throw std::runtime_error("output exists; overwrite not enabled");
  }
  if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
  OutputTransaction transaction(output);
  std::string commit_error;
  if (!transaction.create(commit_error)) throw std::runtime_error(commit_error);
  const auto& temp = transaction.stage_path();
  try {
    check_cancel(cancel);
    report(progress, 8, "Parsing Gain Map container and metadata");
    const auto decode_start = Clock::now();
    const bool request_original = options.gainmap_export_format == "original";
    std::wstring input_extension = input.extension().wstring();
    std::transform(input_extension.begin(), input_extension.end(), input_extension.begin(), std::towlower);
    const bool exact_source = input_extension == L".jxl" || input_extension == L".jpg" ||
                              input_extension == L".jpeg" || input_extension == L".jpe";
    auto raster = gainmap::extract_gain_map(input, cancel, !(request_original && exact_source));
    const double decode_ms = elapsed_ms(decode_start);
    std::string format = options.gainmap_export_format;
    if (format != "original" && format != "png" && format != "tiff" && format != "jpeg") {
      throw std::runtime_error("unknown gain-map export format");
    }
    const bool exact_original = format == "original" && !raster.original_bytes.empty();
    if (format == "original" && !exact_original) {
      const auto ext = output.extension().wstring();
      format = (ext == L".tif" || ext == L".tiff") ? "tiff" : "png";
    }
    report(progress, 70, exact_original ? "Copying original Gain Map image" :
           "Writing decoded Gain Map image");
    const auto encode_start = Clock::now();
    std::vector<uint8_t> bytes;
    if (exact_original) bytes = raster.original_bytes;
    else if (format == "png") bytes = encode_gainmap_png(raster, 0);
    else if (format == "jpeg") bytes = encode_gainmap_jpeg(raster, 0, options.base_quality);
    if (format == "tiff") {
      encode_gainmap_tiff(temp, raster, 0);
      bytes = read_file(temp);
    } else {
      write_file(temp, bytes);
    }
    const double encode_ms = elapsed_ms(encode_start);
    const auto verification_start = Clock::now();
    verify_gainmap_export(temp, bytes, raster, format, exact_original);
    const double verification_ms = elapsed_ms(verification_start);
    Verification verification;
    verification.passed = true;
    verification.width = raster.width;
    verification.height = raster.height;
    verification.bit_depth = exact_original ? raster.bit_depth : (format == "jpeg" ? 8u : 16u);
    verification.pixel_format = raster.channels == 3 ?
        "RGB Gain Map image" : "Mono Gain Map image";
    verification.color_encoding = raster.encoding;
    verification.gain_map_width = raster.width;
    verification.gain_map_height = raster.height;
    verification.gain_map_channels = raster.channels;
    verification.min_value = raster.rgb16.empty() ? 0.0 : std::numeric_limits<double>::infinity();
    verification.max_value = raster.rgb16.empty() ? 1.0 : -std::numeric_limits<double>::infinity();
    for (const uint16_t value : raster.rgb16) {
      verification.min_value = std::min(verification.min_value, value / 65535.0);
      verification.max_value = std::max(verification.max_value, value / 65535.0);
    }
    verification.checks.push_back("Gain Map metadata parsed from source container");
    verification.checks.push_back(exact_original ? "Written original payload compared byte-for-byte" :
        "Written image bytes and header metadata checked");
    verification.checks.push_back("Gain Map image decoded without HDR reconstruction");
    verification.checks.push_back(exact_original ?
        (raster.original_extension == ".jpg" ?
         "Original JPEG scan data copied without re-encoding; parent-container metadata removed" :
         "Original embedded Gain Map payload copied byte-for-byte") :
        (format == "jpeg" ? "Decoded Gain Map exported as 8-bit lossy JPEG" :
         "Decoded Gain Map samples exported losslessly"));
    if (raster.channels == 3 && !exact_original) {
      verification.checks.push_back("RGB Gain Map retained as one three-channel image");
    }
    ConversionResult result;
    result.success = true;
    result.output_path = output;
    result.output_bytes = static_cast<uint64_t>(bytes.size());
    result.mode = "gainmap-extract";
    result.sha256 = sha256(bytes);
    result.verification = std::move(verification);
    result.timings.decode_ms = decode_ms;
    result.timings.gain_map_decode_ms = decode_ms;
    result.timings.verification_ms = verification_ms;
    result.timings.encode_ms = encode_ms;
    check_cancel(cancel);
    if (!transaction.commit(options.overwrite, commit_error)) {
      throw std::runtime_error(commit_error);
    }
    result.timings.total_ms = elapsed_ms(total_start);
    // A notification failure cannot undo a committed file or make it a failure.
    try { report(progress, 100, "Gain Map export committed"); } catch (...) {}
    return result;
  } catch (...) {
    throw;
  }
}

}  // namespace hdrbridge::detail
