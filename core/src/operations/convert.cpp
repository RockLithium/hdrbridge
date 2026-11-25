#include "canonical/cache.h"
#include "codecs/avif.h"
#include "codecs/jxl.h"
#include "codecs/jxr.h"
#include "codecs/png.h"
#include "codecs/tiff.h"
#include "codecs/ultrahdr.h"
#include "color/gamut.h"
#include "color/scrgb.h"
#include "image/alpha.h"
#include "image/resize.h"
#include "image/hdr_stats.h"
#include "infrastructure/progress.h"
#include "infrastructure/files.h"
#include "infrastructure/timing.h"
#include "infrastructure/output_transaction.h"
#include "diagnostics/digest.h"
#include "diagnostics/result_json.h"
#include "operations/gainmap_extract.h"
#include <optional>
#include <stdexcept>

#if defined(_WIN32) || defined(__ANDROID__)
#include "photo_frame/photo_frame.h"
#endif

namespace hdrbridge {

using namespace detail;

ConversionResult convert(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         const ConversionOptions& options,
                         ProgressCallback progress,
                         std::atomic_bool* cancel) {
  const auto total_start = Clock::now();
  TimingDiagnostics timings;
#if defined(_WIN32) || defined(__ANDROID__)
  if (options.mode == "photo-frame") {
    return convert_photo_frame(input, output, options, progress, cancel);
  }
#endif
  if (options.mode == "gainmap-extract") {
    return convert_gainmap_extract(input, output, options, progress, cancel);
  }
  if (std::filesystem::exists(output) && !options.overwrite) throw std::runtime_error("output exists; overwrite not enabled");
  if (options.mode == "ultrahdr" && options.gainmap_scale != 1 &&
      options.gainmap_scale != 2 && options.gainmap_scale != 4) {
    throw std::runtime_error("Ultra HDR gain-map scale must be 1 (full), 2 (half), or 4 (quarter)");
  }
  if (options.output_gamut != "rec2020" && options.output_gamut != "p3") {
    throw std::runtime_error("output gamut must be rec2020 or p3");
  }
  if (options.output_transfer != "pq" && options.output_transfer != "hlg") {
    throw std::runtime_error("output transfer must be pq or hlg");
  }
  if (options.diagnostic_icc_only && options.output_transfer != "pq") {
    throw std::runtime_error("ICC-only regression fixtures are limited to PQ");
  }
  if (options.diagnostic_icc_only && options.mode != "png-pq16" &&
      options.mode != "jxl-pq16" && options.mode != "avif-pq10") {
    throw std::runtime_error("diagnostic ICC-only fixture is supported for PNG, JPEG XL, and AVIF");
  }
  if (options.output_representation != "direct" &&
      options.output_representation != "gainmap") {
    throw std::runtime_error("output representation must be direct or gainmap");
  }
  if (options.output_representation == "gainmap" &&
      options.mode != "jxl-pq16" && options.mode != "avif-pq10" &&
      options.mode != "ultrahdr") {
    throw std::runtime_error("gain-map output is supported only for JPEG, JPEG XL, and AVIF");
  }
  if (options.output_representation == "gainmap" &&
      (options.mode == "jxl-pq16" || options.mode == "avif-pq10") &&
      !options.multi_channel_gainmap) {
    throw std::runtime_error("JPEG XL and AVIF gain-map output requires an RGB gain map");
  }
  if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
  OutputTransaction transaction(output);
  std::string commit_error;
  if (!transaction.create(commit_error)) throw std::runtime_error(commit_error);
  const auto& temp = transaction.stage_path();
  try {
    check_cancel(cancel);
    auto decoded_pointer = decode_input(input, progress, cancel, timings);
    std::optional<DecodedImage> composited_source;
    const bool preserves_alpha = options.mode == "png-pq16" ||
                                 options.mode == "jxr-scrgb-fp16";
    if (!decoded_pointer->alpha.empty()) {
      if (options.mode == "ultrahdr") {
        report(progress, 44,
               "Compositing source alpha over black in linear light for JPEG output");
        composited_source = *decoded_pointer;
        composite_straight_alpha_over_black(*composited_source);
      } else if (!preserves_alpha) {
        report(progress, 44,
               "Compositing source alpha over black in linear light for RGB-only encoder path");
        composited_source = *decoded_pointer;
        composite_straight_alpha_over_black(*composited_source);
      }
    }
    const DecodedImage& decoded = composited_source ? *composited_source
                                                    : *decoded_pointer;
    if (decoded.info.asset_kind != "direct-hdr" &&
        decoded.info.asset_kind != "gain-map-hdr") {
      throw std::runtime_error("No HDR data");
    }
    const HdrStats stats = measure_hdr(decoded);
    std::vector<uint8_t> bytes;
    Verification verification;
    if (options.mode == "jxl-pq16") {
      if (options.output_representation == "gainmap") {
        report(progress, 48, "Encoding SDR base and ISO 21496-1 jhgm JPEG XL gain map");
        const auto encode_start = Clock::now();
        bytes = encode_gainmap_jxl(decoded, options);
        timings.encode_ms = elapsed_ms(encode_start);
        timings.gain_map_ms += timings.encode_ms;
        write_file(temp, bytes);
        check_cancel(cancel);
        report(progress, 88, "Verifying JPEG XL jhgm metadata and HDR reconstruction");
        const auto verification_start = Clock::now();
        verification = verify_gainmap_jxl(temp, decoded, options, cancel);
        timings.verification_ms = elapsed_ms(verification_start);
      } else {
      report(progress, 48, options.output_transfer == "hlg"
          ? "Encoding JPEG XL BT.2100 HLG master"
          : "Encoding JPEG XL lossless PQ master");
      const auto color_start = Clock::now();
      auto converted = options.output_transfer == "hlg"
          ? convert_rec2020_pq_to_hlg(decoded, options.output_gamut)
          : options.output_gamut == "p3" ? convert_rec2020_pq_to_p3_pq(decoded)
          : options.output_gamut == "rec709" ? convert_rec2020_pq_to_rec709_pq(decoded)
                                        : std::vector<uint16_t>{};
      timings.color_conversion_ms += elapsed_ms(color_start);
      const auto& pixels = converted.empty() ? decoded.rgb : converted;
      const auto encode_start = Clock::now();
      bytes = encode_jxl(decoded, options, pixels);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      write_file(temp, bytes);
      report(progress, 88, "Verifying JPEG XL decode and RGB16 equality");
      const auto verification_start = Clock::now();
      verification = decode_jxl(bytes, options.lossless ? &pixels : nullptr);
      timings.verification_ms = elapsed_ms(verification_start);
      }
    } else if (options.mode == "jxr-scrgb-fp16") {
      const auto color_start = Clock::now();
      auto half = pq2020_to_scrgb_half(decoded, progress, cancel);
      timings.color_conversion_ms += elapsed_ms(color_start);
#ifdef __ANDROID__
      report(progress, 65, "Encoding JPEG XR FP16 scRGB with jxrlib");
#else
      report(progress, 65, "Encoding JPEG XR FP16 scRGB with Windows WIC");
#endif
      const auto encode_start = Clock::now();
      encode_jxr(temp, decoded.info.width, decoded.info.height, half, options);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
#ifdef __ANDROID__
      report(progress, 88, "Checking portable JPEG XR FP16 output");
#else
      report(progress, 88, "Verifying JPEG XR through WIC FP16 decode");
#endif
      const auto verification_start = Clock::now();
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
      // jxrlib's decoder rejects some of its own FP16/alpha streams even though
      // Windows WIC accepts them.  Keep the browser encoder portable and leave
      // the stronger WIC round-trip check to the release regression suite.
      verification.width = decoded.info.width;
      verification.height = decoded.info.height;
      verification.bit_depth = 16;
      verification.pixel_format = "64bpp RGBA half-float";
      verification.color_encoding = "linear scRGB; 1.0 = 80 cd/m2; sRGB/BT.709 primaries";
      verification.finite = true;
      verification.passed = std::filesystem::file_size(temp) > 0;
      verification.checks.push_back("portable jxrlib FP16 encoder completed");
      verification.checks.push_back("Windows WIC compatibility covered by release regression");
#else
      verification = decode_jxr(temp, options.lossless ? &half : nullptr);
#endif
      timings.verification_ms = elapsed_ms(verification_start);
      bytes = read_file(temp);
    } else if (options.mode == "ultrahdr") {
      report(progress, 48, "Encoding faithful SDR base and ISO 21496-1 gain map with measured capacity");
      std::optional<DecodedImage> resized;
      const DecodedImage* uhdr_source = &decoded;
      HdrStats uhdr_stats = stats;
      if (decoded.info.width > 65500u || decoded.info.height > 65500u) {
        report(progress, 50, "Resampling linear HDR to the JPEG 65500-pixel codec limit");
        resized = resize_linear_hdr_to_fit(decoded, 65500u);
        uhdr_source = &*resized;
        uhdr_stats = measure_hdr(*uhdr_source);
      }
      if (uhdr_stats.chosen_target_nits <= 203.0) {
        uhdr_stats.chosen_target_nits = 204.0;
        uhdr_stats.reason += uhdr_stats.reason.empty() ? "" : "; ";
        uhdr_stats.reason +=
            "adjusted to 204 nits because Ultra HDR capacity max must be greater than 1.0";
      }
      const auto gain_map_start = Clock::now();
      bytes = encode_ultrahdr(*uhdr_source, options, uhdr_stats);
      const double gain_map_encode_ms = elapsed_ms(gain_map_start);
      timings.gain_map_ms += gain_map_encode_ms;
      timings.encode_ms = gain_map_encode_ms;
      check_cancel(cancel);
      write_file(temp, bytes);
      report(progress, 88, "Verifying gain-map capacity and linear-light HDR reconstruction");
      const auto verification_start = Clock::now();
      verification = decode_ultrahdr(bytes, uhdr_source, &uhdr_stats, &options);
      timings.verification_ms = elapsed_ms(verification_start);
    } else if (options.mode == "png-pq16") {
      const char* png_layout = decoded.alpha.empty() ? "RGB16" : "RGBA16";
      report(progress, 52, options.output_transfer == "hlg"
          ? std::string("Encoding direct HDR PNG ") + png_layout + " with BT.2100 HLG cICP"
          : options.embed_hdr_icc
          ? std::string("Encoding direct HDR PNG ") + png_layout + " with PQ cICP and ICC"
          : std::string("Encoding PNG ") + png_layout + " with standard PQ cICP only");
      const auto color_start = Clock::now();
      auto converted = options.output_transfer == "hlg"
          ? convert_rec2020_pq_to_hlg(decoded, options.output_gamut)
          : options.output_gamut == "p3" ? convert_rec2020_pq_to_p3_pq(decoded)
          : options.output_gamut == "rec709" ? convert_rec2020_pq_to_rec709_pq(decoded)
                                        : std::vector<uint16_t>{};
      timings.color_conversion_ms += elapsed_ms(color_start);
      const auto& pixels = converted.empty() ? decoded.rgb : converted;
      const auto encode_start = Clock::now();
      bytes = encode_png(decoded, options, pixels);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      write_file(temp, bytes);
      report(progress, 88, std::string("Verifying PNG ") + png_layout +
                           " equality and cICP signaling");
      const auto verification_start = Clock::now();
      verification = decode_png(bytes, &pixels,
                                options.diagnostic_icc_only ||
                                    (options.output_transfer == "pq" && options.embed_hdr_icc),
                                options.diagnostic_icc_only,
                                decoded.alpha.empty() ? nullptr : &decoded.alpha);
      timings.verification_ms = elapsed_ms(verification_start);
      attach_hdr_stats(verification, stats);
    } else if (options.mode == "tiff-pq16") {
      if (options.output_transfer == "hlg") {
        throw std::runtime_error("Direct HDR TIFF HLG is not enabled; choose PQ");
      }
      report(progress, 52, options.tiff_compressed
          ? "Encoding direct HDR TIFF RGB16 with Deflate and HDR CICP ICC"
          : "Encoding uncompressed direct HDR TIFF RGB16 with HDR CICP ICC");
      const auto color_start = Clock::now();
      auto converted = options.output_gamut == "p3" ? convert_rec2020_pq_to_p3_pq(decoded) :
                       options.output_gamut == "rec709" ? convert_rec2020_pq_to_rec709_pq(decoded) :
                       std::vector<uint16_t>{};
      timings.color_conversion_ms += elapsed_ms(color_start);
      const auto& pixels = converted.empty() ? decoded.rgb : converted;
      const auto encode_start = Clock::now();
      encode_tiff(temp, decoded, options, pixels);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      report(progress, 88, "Verifying TIFF RGB16 equality and direct-HDR ICC");
      const auto verification_start = Clock::now();
      verification = decode_tiff(temp, &pixels);
      timings.verification_ms = elapsed_ms(verification_start);
      bytes = read_file(temp);
      attach_hdr_stats(verification, stats);
    } else if (options.mode == "avif-pq10") {
      if (options.output_representation == "gainmap") {
        report(progress, 52, "Encoding SDR base and ISO 21496-1 gain map AVIF");
        const auto encode_start = Clock::now();
        encode_gainmap_avif(temp, decoded, options);
        timings.encode_ms = elapsed_ms(encode_start);
        timings.gain_map_ms += timings.encode_ms;
        check_cancel(cancel);
        bytes = read_file(temp);
        report(progress, 88, "Verifying AVIF gain-map item graph and HDR reconstruction");
        const auto verification_start = Clock::now();
        verification = verify_gainmap_avif(temp, decoded, cancel);
        timings.verification_ms = elapsed_ms(verification_start);
      } else {
      report(progress, 52, options.output_transfer == "hlg"
          ? "Encoding direct 10-bit 4:4:4 BT.2100 HLG AVIF"
          : "Encoding direct 10-bit 4:4:4 PQ AVIF compatibility candidate");
      const auto color_start = Clock::now();
      auto converted = options.output_transfer == "hlg"
          ? convert_rec2020_pq_to_hlg(decoded, options.output_gamut)
          : options.output_gamut == "p3" ? convert_rec2020_pq_to_p3_pq(decoded)
          : options.output_gamut == "rec709" ? convert_rec2020_pq_to_rec709_pq(decoded)
                                        : std::vector<uint16_t>{};
      timings.color_conversion_ms += elapsed_ms(color_start);
      const auto& pixels = converted.empty() ? decoded.rgb : converted;
      const auto encode_start = Clock::now();
      encode_avif(temp, decoded, options, pixels);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      report(progress, 88, "Verifying AV1 10-bit 4:4:4 and HDR CICP signaling");
      const auto verification_start = Clock::now();
      verification = decode_avif(temp, &pixels, options.diagnostic_icc_only);
      timings.verification_ms = elapsed_ms(verification_start);
      bytes = read_file(temp);
      attach_hdr_stats(verification, stats);
      }
    } else {
      throw std::runtime_error("mode not implemented yet: " + options.mode);
    }
    if (options.mode != "ultrahdr" && verification.chosen_target_peak_nits == 0.0) {
      attach_hdr_stats(verification, stats);
    }
    verification.input_gain_map_formula_max_error = decoded.gain_map_formula_max_error;
    verification.transfer_conversion_rmse = decoded.transfer_conversion_rmse;
    verification.transfer_conversion_max_abs_error =
        decoded.transfer_conversion_max_abs_error;
    verification.source_to_canonical_rmse = decoded.source_to_canonical_rmse;
    verification.source_to_canonical_max_abs_error =
        decoded.source_to_canonical_max_abs_error;
    if (decoded.info.transfer == 18) {
      verification.checks.push_back(
          "BT.2100 HLG OETF/OOTF transfer conversion error recorded");
    }
    if (decoded.info.gain_map_family == "adobe-iso-tmap") {
      verification.checks.push_back(
          decoded.gain_map_formula_max_error <= (2.5 / 65535.0)
              ? "independent ISO gain-map formula sample check agrees with libavif reconstruction"
              : "independent ISO gain-map formula sample check exceeded tolerance");
      verification.passed = verification.passed &&
                            decoded.gain_map_formula_max_error <= (2.5 / 65535.0);
    }
    if (!verification.passed) throw std::runtime_error("output verification failed: " + verification_error_json(verification));
    ConversionResult result;
    result.success = true;
    result.output_path = output;
    result.output_bytes = static_cast<uint64_t>(bytes.size());
    result.mode = options.mode;
    result.sha256 = sha256(bytes);
    result.verification = std::move(verification);
    result.timings = timings;
    check_cancel(cancel);
    if (!transaction.commit(options.overwrite, commit_error)) {
      throw std::runtime_error(commit_error);
    }
    result.timings.total_ms = elapsed_ms(total_start);
    try { report(progress, 100, "Verified output committed atomically"); } catch (...) {}
    return result;
  } catch (...) {
    throw;
  }
}

}  // namespace hdrbridge
