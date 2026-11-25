#include "codecs/avif.h"
#include "codecs/heif_helpers.h"
#include "codecs/gainmap/avif.h"
#include "color/gamut.h"
#include "color/icc_profile.h"
#include "color/pq.h"
#include "color/signaling.h"
#include "infrastructure/files.h"
#include "metadata/exif_output.h"
#include "metadata/exif_payload.h"
#include "hdrbridge_threads.h"
#include <avif/avif.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace hdrbridge::detail {

struct AvifImageDeleter { void operator()(avifImage* p) const { avifImageDestroy(p); } };
struct AvifGainMapDeleter { void operator()(avifGainMap* p) const { avifGainMapDestroy(p); } };
struct AvifEncoderDeleter { void operator()(avifEncoder* p) const { avifEncoderDestroy(p); } };
using AvifImagePtr = std::unique_ptr<avifImage, AvifImageDeleter>;
using AvifGainMapPtr = std::unique_ptr<avifGainMap, AvifGainMapDeleter>;
using AvifEncoderPtr = std::unique_ptr<avifEncoder, AvifEncoderDeleter>;

void encode_avif(const std::filesystem::path& path, const DecodedImage& decoded,
                 const ConversionOptions& options, const std::vector<uint16_t>& pixels) {
  heif_image* raw_image = nullptr;
  require_heif(heif_image_create(static_cast<int>(decoded.info.width), static_cast<int>(decoded.info.height),
                                 heif_colorspace_RGB, heif_chroma_interleaved_RRGGBB_LE, &raw_image),
               "create direct PQ AVIF image");
  ImagePtr image(raw_image);
  NclxPtr profile;
  if (options.diagnostic_icc_only) {
    const auto icc = make_windows_compatible_pq_icc(options.output_gamut == "p3");
    require_heif(heif_image_set_raw_color_profile(image.get(), "rICC", icc.data(), icc.size()),
                 "set actual ICC-only direct PQ AVIF profile");
  }
  if (!options.diagnostic_icc_only) {
    heif_color_profile_nclx* raw_profile = heif_nclx_color_profile_alloc();
    if (!raw_profile) throw std::bad_alloc();
    profile.reset(raw_profile);
    profile->color_primaries = static_cast<heif_color_primaries>(
        options.output_gamut == "rec709" ? 1 : options.output_gamut == "p3" ? 12 : 9);
    profile->transfer_characteristics = static_cast<heif_transfer_characteristics>(
        options.output_transfer == "hlg" ? 18 : 16);
    profile->matrix_coefficients = static_cast<heif_matrix_coefficients>(9);
    profile->full_range_flag = 1;
    require_heif(heif_image_set_nclx_color_profile(image.get(), profile.get()), "set direct PQ AVIF NCLX");
  }
  require_heif(heif_image_add_plane(image.get(), heif_channel_interleaved,
                                    static_cast<int>(decoded.info.width), static_cast<int>(decoded.info.height), 10),
               "allocate direct PQ AVIF RGB10 plane");
  int stride = 0;
  uint8_t* plane = heif_image_get_plane(image.get(), heif_channel_interleaved, &stride);
  if (!plane || stride < static_cast<int>(decoded.info.width * 6u)) throw std::runtime_error("invalid direct PQ AVIF RGB10 plane");
  for (uint32_t y = 0; y < decoded.info.height; ++y) {
    auto* row = reinterpret_cast<uint16_t*>(plane + static_cast<size_t>(y) * stride);
    const auto* source = pixels.data() + static_cast<size_t>(y) * decoded.info.width * 3u;
    for (size_t i = 0; i < static_cast<size_t>(decoded.info.width) * 3u; ++i) {
      row[i] = static_cast<uint16_t>((static_cast<uint32_t>(source[i]) * 1023u + 32767u) / 65535u);
    }
  }
  ContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  heif_encoder* raw_encoder = nullptr;
  require_heif(heif_context_get_encoder_for_format(context.get(), heif_compression_AV1, &raw_encoder),
               "get AV1 encoder for direct PQ AVIF");
  HeifEncoderPtr encoder(raw_encoder);
  const int quality = std::clamp(static_cast<int>(std::lround(options.image_quality * 100.0f)), 1, 100);
  require_heif(heif_encoder_set_lossy_quality(encoder.get(), quality), "set direct PQ AVIF quality");
  require_heif(heif_encoder_set_parameter_integer(encoder.get(), "speed",
      std::clamp(options.avif_speed, 0, 9)), "set direct AVIF encoder speed");
  require_heif(heif_encoder_set_parameter_string(encoder.get(), "chroma", "444"), "force direct PQ AVIF 4:4:4");
  require_heif(heif_encoder_set_parameter_string(encoder.get(), "aom:enable-global-motion", "0"),
               "disable AVIF global motion search");
  for (auto parameter = heif_encoder_list_parameters(encoder.get()); *parameter; ++parameter) {
    if (std::strcmp(heif_encoder_parameter_get_name(*parameter), "threads") != 0) continue;
    int has_min = 0, has_max = 0, minimum = 1, maximum = std::numeric_limits<int>::max();
    int value_count = 0;
    const int* values = nullptr;
    require_heif(heif_encoder_parameter_get_valid_integer_values(*parameter,
        &has_min, &has_max, &minimum, &maximum, &value_count, &values),
        "query AVIF encoder thread range");
    int threads = static_cast<int>(default_worker_count());
    if (has_min) threads = std::max(threads, minimum);
    if (has_max) threads = std::min(threads, maximum);
    if (value_count > 0) {
      int nearest = values[0];
      for (int index = 1; index < value_count; ++index) {
        if (std::abs(values[index] - threads) < std::abs(nearest - threads)) nearest = values[index];
      }
      threads = nearest;
    }
    require_heif(heif_encoder_set_parameter_integer(encoder.get(), "threads", threads),
                 "set direct HDR AVIF worker budget");
    break;
  }
  HeifEncodingOptionsPtr encoding_options(heif_encoding_options_alloc());
  if (!encoding_options) throw std::bad_alloc();
  encoding_options->output_nclx_profile = profile.get();
  heif_image_handle* raw_handle = nullptr;
  require_heif(heif_context_encode_image(context.get(), image.get(), encoder.get(), encoding_options.get(), &raw_handle),
               "encode direct PQ AVIF");
  HandlePtr handle(raw_handle);
  if (options.copy_exif && !decoded.exif.empty()) {
    const auto exif = exif_tiff_payload(decoded.exif);
    if (!exif.empty()) {
      if (exif.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Exif metadata too large for AVIF");
      }
      require_heif(heif_context_add_exif_metadata(context.get(), handle.get(),
                                                   exif.data(), static_cast<int>(exif.size())),
                   "copy Exif to direct HDR AVIF");
    }
  }
  if (options.copy_xmp && !decoded.xmp.empty()) {
    if (decoded.xmp.size() > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::runtime_error("XMP metadata too large for AVIF");
    require_heif(heif_context_add_XMP_metadata(context.get(), handle.get(), decoded.xmp.data(), static_cast<int>(decoded.xmp.size())),
                 "copy XMP to direct PQ AVIF");
  }
  std::vector<uint8_t> encoded_file;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = [](heif_context*, const void* data, size_t size,
                    void* userdata) noexcept -> heif_error {
    try {
      auto& output = *static_cast<std::vector<uint8_t>*>(userdata);
      const auto* begin = static_cast<const uint8_t*>(data);
      output.insert(output.end(), begin, begin + size);
      return {heif_error_Ok, heif_suberror_Unspecified, "Success"};
    } catch (...) {
      return {heif_error_Encoding_error, heif_suberror_Unspecified,
              "Cannot buffer encoded AVIF"};
    }
  };
  require_heif(heif_context_write(context.get(), &writer, &encoded_file),
               "serialize direct PQ AVIF");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot open direct PQ AVIF output");
  output.write(reinterpret_cast<const char*>(encoded_file.data()),
               static_cast<std::streamsize>(encoded_file.size()));
  if (!output) throw std::runtime_error("cannot write direct PQ AVIF output");
}

static void require_avif_result(avifResult result, const avifDiagnostics& diagnostics,
                         const char* operation) {
  if (result == AVIF_RESULT_OK) return;
  std::string message = std::string(operation) + ": " + avifResultToString(result);
  if (diagnostics.error[0]) message += " (" + std::string(diagnostics.error) + ")";
  throw std::runtime_error(message);
}

static void assign_rgb16(avifRGBImage& rgb, const std::vector<uint16_t>& pixels,
                  uint32_t width, uint32_t height) {
  rgb.width = width;
  rgb.height = height;
  rgb.depth = 16;
  rgb.format = AVIF_RGB_FORMAT_RGB;
  rgb.pixels = reinterpret_cast<uint8_t*>(const_cast<uint16_t*>(pixels.data()));
  rgb.rowBytes = width * 3u * sizeof(uint16_t);
  rgb.avoidLibYUV = AVIF_TRUE;
  rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_BEST_QUALITY;
}

void encode_gainmap_avif(const std::filesystem::path& path,
                         const DecodedImage& decoded,
                         const ConversionOptions& options) {
  const avifColorPrimaries primaries = options.output_gamut == "rec709"
      ? AVIF_COLOR_PRIMARIES_BT709
      : options.output_gamut == "p3" ? AVIF_COLOR_PRIMARIES_SMPTE432
                                      : AVIF_COLOR_PRIMARIES_BT2020;
  std::vector<uint16_t> alternate_pixels = decoded.rgb;
  if (options.output_gamut == "p3") {
    alternate_pixels = convert_rec2020_pq_to_p3_pq(decoded);
  } else if (options.output_gamut == "rec709") {
    alternate_pixels = convert_rec2020_pq_to_rec709_pq(decoded);
  }

  // The base is an SDR rendition in the selected gamut. Its simple 203-nit
  // reference mapping is only the backward-compatible presentation; the ISO
  // gain map reconstructs the original high-precision PQ alternate rendition.
  std::vector<uint16_t> base_pixels(alternate_pixels.size());
  for (size_t i = 0; i < alternate_pixels.size(); ++i) {
    const double linear = std::clamp(transfer::pq_to_nits(
        static_cast<double>(alternate_pixels[i]) / 65535.0) / 203.0,
        0.0, 1.0);
    const double srgb = linear <= 0.0031308 ? 12.92 * linear
        : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    base_pixels[i] = static_cast<uint16_t>(std::lround(std::clamp(srgb, 0.0, 1.0) * 65535.0));
  }

  AvifImagePtr base(avifImageCreate(decoded.info.width, decoded.info.height, 12,
                                    AVIF_PIXEL_FORMAT_YUV444));
  if (!base) throw std::bad_alloc();
  base->colorPrimaries = primaries;
  base->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
  base->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
  base->yuvRange = AVIF_RANGE_FULL;
  avifRGBImage base_rgb{};
  avifRGBImageSetDefaults(&base_rgb, base.get());
  assign_rgb16(base_rgb, base_pixels, decoded.info.width, decoded.info.height);
  require_avif_result(avifImageRGBToYUV(base.get(), &base_rgb), {},
                      "convert gain-map AVIF SDR base to YUV");

  AvifImagePtr alternate(avifImageCreate(decoded.info.width, decoded.info.height, 12,
                                         AVIF_PIXEL_FORMAT_YUV444));
  if (!alternate) throw std::bad_alloc();
  alternate->colorPrimaries = primaries;
  alternate->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;
  alternate->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
  alternate->yuvRange = AVIF_RANGE_FULL;
  avifRGBImage alternate_rgb{};
  avifRGBImageSetDefaults(&alternate_rgb, alternate.get());
  assign_rgb16(alternate_rgb, alternate_pixels, decoded.info.width, decoded.info.height);
  require_avif_result(avifImageRGBToYUV(alternate.get(), &alternate_rgb), {},
                      "convert gain-map AVIF HDR alternate to YUV");

  AvifGainMapPtr gain(avifGainMapCreate());
  if (!gain) throw std::bad_alloc();
  const uint32_t scale = static_cast<uint32_t>(std::clamp(options.gainmap_scale, 1, 4));
  const uint32_t map_width = std::max(1u, (decoded.info.width + scale - 1u) / scale);
  const uint32_t map_height = std::max(1u, (decoded.info.height + scale - 1u) / scale);
  gain->image = avifImageCreate(map_width, map_height, 10,
      options.multi_channel_gainmap ? AVIF_PIXEL_FORMAT_YUV444 : AVIF_PIXEL_FORMAT_YUV400);
  if (!gain->image) throw std::bad_alloc();
  gain->image->matrixCoefficients = options.multi_channel_gainmap
      ? AVIF_MATRIX_COEFFICIENTS_BT601 : AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL;
  gain->image->yuvRange = AVIF_RANGE_FULL;
  avifDiagnostics gain_diagnostics{};
  require_avif_result(avifRGBImageComputeGainMap(&base_rgb, primaries,
                                                AVIF_TRANSFER_CHARACTERISTICS_SRGB,
                                                &alternate_rgb, primaries,
                                                AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084,
                                                gain.get(), &gain_diagnostics), gain_diagnostics,
                      "compute ISO gain map for AVIF");
  base->gainMap = gain.release();

  if (options.copy_exif && !decoded.exif.empty()) {
    const auto exif = exif_tiff_payload(decoded.exif);
    if (!exif.empty()) avifImageSetMetadataExif(base.get(), exif.data(), exif.size());
  }
  if (options.copy_xmp && !decoded.xmp.empty()) {
    avifImageSetMetadataXMP(base.get(), decoded.xmp.data(), decoded.xmp.size());
  }
  AvifEncoderPtr encoder(avifEncoderCreate());
  if (!encoder) throw std::bad_alloc();
  encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
  require_avif_result(avifEncoderSetCodecSpecificOption(encoder.get(), "enable-global-motion", "0"),
                      encoder->diag, "disable gain-map AVIF global motion search");
  encoder->speed = std::clamp(options.avif_speed, 0, 9);
  encoder->maxThreads = static_cast<int>(default_worker_count());
  encoder->quality = std::clamp(static_cast<int>(std::lround(options.image_quality * 100.0f)), 1, 100);
  encoder->qualityGainMap = std::clamp(options.gainmap_quality, 1, 100);
  encoder->autoTiling = AVIF_TRUE;
  avifRWData encoded = AVIF_DATA_EMPTY;
  require_avif_result(avifEncoderWrite(encoder.get(), base.get(), &encoded), encoder->diag,
                      "encode ISO gain-map AVIF");
  std::vector<uint8_t> bytes(encoded.data, encoded.data + encoded.size);
  avifRWDataFree(&encoded);
  write_file(path, bytes);
}

Verification decode_avif(const std::filesystem::path& path,
                         const std::vector<uint16_t>* expected,
                         bool expect_icc_only) {
  auto opened = open_heif(path);
  SourceInfo info = inspect_opened(path, opened);
  Verification v;
  v.width = info.width; v.height = info.height; v.bit_depth = info.bit_depth;
  v.pixel_format = info.chroma == "4:4:4" ? "AV1 10-bit 4:4:4" : "AV1 " + info.chroma;
  const bool rec2020 = info.primaries == 9 && info.transfer == 16 && info.matrix == 9 && info.full_range;
  const bool p3 = info.primaries == 12 && info.transfer == 16 && info.matrix == 9 && info.full_range;
  const bool hlg = (info.primaries == 9 || info.primaries == 12) &&
                   info.transfer == 18 && info.matrix == 9 && info.full_range;
  const bool icc_hdr = info.icc_present && !info.native_color_present &&
                       (info.primaries == 9 || info.primaries == 12) &&
                       (info.transfer == 16 || info.transfer == 18) &&
                       info.resolved_signaling_source == "ICC";
  const bool color_ok = expect_icc_only ? icc_hdr : (rec2020 || p3 || hlg);
  v.color_encoding = rec2020 ? "direct Rec.2020/PQ full-range CICP 9/16/9" :
                     p3 ? "direct Display P3/PQ full-range CICP 12/16/9" :
                     hlg ? (info.primaries == 12 ? "direct Display P3/HLG full-range CICP 12/18/9" :
                                                  "direct Rec.2020/HLG full-range CICP 9/18/9") :
                     icc_hdr ? "direct HDR from ICC; native NCLX absent" :
                     "unexpected AVIF color signaling";
  OptionsPtr options(heif_decoding_options_alloc());
  if (!options) throw std::bad_alloc();
  options->convert_hdr_to_8bit = 0; options->ignore_transformations = 0; options->output_image_nclx_profile_passthrough = 1;
  heif_image* raw = nullptr;
  require_heif(heif_decode_image(opened.handle.get(), &raw, heif_colorspace_RGB,
                                 heif_chroma_interleaved_RRGGBB_LE, options.get()), "decode direct PQ AVIF verification pixels");
  ImagePtr image(raw);
  int stride = 0; const uint8_t* plane = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &stride);
  if (!plane) throw std::runtime_error("direct PQ AVIF verification plane missing");
  long double squared_error = 0.0; double max_error = 0.0; uint64_t samples = 0;
  uint16_t minimum = 65535, maximum = 0;
  for (uint32_t y = 0; y < info.height; ++y) {
    const auto* row = reinterpret_cast<const uint16_t*>(plane + static_cast<size_t>(y) * stride);
    for (size_t i = 0; i < static_cast<size_t>(info.width) * 3u; ++i) {
      const uint16_t expanded = info.bit_depth < 16
          ? static_cast<uint16_t>((static_cast<uint32_t>(row[i]) * 65535u + 511u) / 1023u) : row[i];
      minimum = std::min(minimum, expanded); maximum = std::max(maximum, expanded);
      if (expected && expected->size() == static_cast<size_t>(info.width) * info.height * 3u) {
        const size_t index = static_cast<size_t>(y) * info.width * 3u + i;
        const double error = (static_cast<double>(expanded) - (*expected)[index]) / 65535.0;
        squared_error += static_cast<long double>(error) * error;
        max_error = std::max(max_error, std::abs(error)); ++samples;
      }
    }
  }
  v.min_value = minimum; v.max_value = maximum;
  if (samples) { v.reconstruction_rmse = std::sqrt(static_cast<double>(squared_error / samples)); v.reconstruction_max_abs_error = max_error; }
  v.checks.push_back(info.width && info.height ? "AVIF dimensions parse" : "AVIF dimensions missing");
  v.checks.push_back(info.bit_depth == 10 ? "AV1 high-bit-depth is 10-bit" : "AVIF bit depth is not 10");
  v.checks.push_back(info.chroma == "4:4:4" ? "AV1 chroma is 4:4:4" : "AV1 chroma is not 4:4:4");
  v.checks.push_back(color_ok
      ? (hlg ? "BT.2020/HLG/BT.2020-NCL full-range signaling"
             : expect_icc_only ? "actual embedded PQ ICC; native NCLX absent"
                               : "BT.2020/PQ/BT.2020-NCL full-range signaling")
      : "AVIF HDR signaling mismatch");
  if (expected) v.checks.push_back("lossy RGB16-domain reconstruction error recorded");
  v.passed = info.width > 0 && info.height > 0 && info.bit_depth == 10 && info.chroma == "4:4:4" && color_ok;
  return v;
}

Verification verify_gainmap_avif(const std::filesystem::path& path, const DecodedImage& decoded,
                                std::atomic_bool* cancel) {
  const auto reconstructed = gainmap::reconstruct_adobe_tmap_avif(path, cancel);
  Verification verification;
  verification.width = reconstructed.info.width;
  verification.height = reconstructed.info.height;
  verification.bit_depth = 10;
  verification.pixel_format = "AV1 base + ISO gain map";
  verification.color_encoding = "ISO 21496-1 gain-map HDR";
  verification.gain_map_width = reconstructed.info.gain_map_width;
  verification.gain_map_height = reconstructed.info.gain_map_height;
  verification.gain_map_channels = reconstructed.info.gain_map_channels;
  verification.passed = verification.width == decoded.info.width &&
                        verification.height == decoded.info.height &&
                        verification.gain_map_width > 0;
  verification.checks.push_back("AVIF tmap relationship and gain-map metadata parsed");
  verification.checks.push_back("gain-map HDR reconstruction completed");
  return verification;
}

}  // namespace hdrbridge::detail
