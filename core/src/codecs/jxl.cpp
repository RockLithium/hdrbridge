#include "codecs/jxl.h"
#include "codecs/gainmap/jxl.h"
#include "codecs/jxl_metadata_boxes.h"
#include "color/pq.h"
#include "color/gamut.h"
#include "color/signaling.h"
#include "color/icc_profile.h"
#include "image/orientation.h"
#include "infrastructure/pixel_workers.h"
#include "infrastructure/progress.h"
#include "infrastructure/files.h"
#include "infrastructure/timing.h"
#include "infrastructure/big_endian.h"
#include "metadata/exif_output.h"
#include "metadata/exif_payload.h"
#include "metadata/source_metadata.h"
#include "hdrbridge_threads.h"
#include <jxl/color_encoding.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace hdrbridge::detail {

std::vector<uint8_t> encode_jxl(const DecodedImage& decoded, const ConversionOptions& options,
                                const std::vector<uint16_t>& pixels) {
  auto encoder = JxlEncoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!encoder || !runner) throw std::runtime_error("cannot create JPEG XL encoder");
  if (JxlEncoderSetParallelRunner(encoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_ENC_SUCCESS) {
    throw std::runtime_error("cannot attach JPEG XL parallel runner");
  }
  JxlResizableParallelRunnerSetThreads(runner.get(), codec_background_workers());
  if (JxlEncoderUseContainer(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS) throw std::runtime_error("cannot enable JXL container");
  if (JxlEncoderUseBoxes(encoder.get()) != JXL_ENC_SUCCESS) throw std::runtime_error("cannot enable JXL metadata boxes");

  JxlBasicInfo basic{};
  JxlEncoderInitBasicInfo(&basic);
  basic.xsize = decoded.info.width;
  basic.ysize = decoded.info.height;
  basic.bits_per_sample = 16;
  basic.exponent_bits_per_sample = 0;
  basic.num_color_channels = 3;
  basic.num_extra_channels = 0;
  basic.uses_original_profile = JXL_TRUE;
  if (JxlEncoderSetBasicInfo(encoder.get(), &basic) != JXL_ENC_SUCCESS) throw std::runtime_error("cannot set JXL basic info");

  if (options.diagnostic_icc_only) {
    const auto icc = make_windows_compatible_pq_icc(options.output_gamut == "p3");
    if (JxlEncoderSetICCProfile(encoder.get(), icc.data(), icc.size()) != JXL_ENC_SUCCESS) {
      throw std::runtime_error("cannot set actual ICC-coded JPEG XL profile");
    }
  } else {
    JxlColorEncoding color{};
    color.color_space = JXL_COLOR_SPACE_RGB;
    color.white_point = JXL_WHITE_POINT_D65;
    color.primaries = options.output_gamut == "rec709" ? JXL_PRIMARIES_SRGB :
                      options.output_gamut == "p3" ? JXL_PRIMARIES_P3 : JXL_PRIMARIES_2100;
    color.transfer_function = options.output_transfer == "hlg"
        ? JXL_TRANSFER_FUNCTION_HLG : JXL_TRANSFER_FUNCTION_PQ;
    color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
    if (JxlEncoderSetColorEncoding(encoder.get(), &color) != JXL_ENC_SUCCESS) {
      throw std::runtime_error("cannot set JPEG XL structured color encoding");
    }
  }

  JxlEncoderFrameSettings* frame = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
  if (!frame) throw std::runtime_error("cannot create JXL frame settings");
  if (JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT,
                                       std::clamp(options.effort, 1, 10)) != JXL_ENC_SUCCESS) {
    throw std::runtime_error("cannot set JXL effort");
  }
  if (options.lossless) {
    if (JxlEncoderSetFrameLossless(frame, JXL_TRUE) != JXL_ENC_SUCCESS) throw std::runtime_error("cannot enable JXL lossless");
  } else {
    JxlEncoderSetFrameDistance(frame, std::max(0.01f, 1.0f - options.image_quality) * 4.0f);
  }
  const JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  if (JxlEncoderAddImageFrame(frame, &format, pixels.data(), pixels.size() * sizeof(uint16_t)) != JXL_ENC_SUCCESS) {
    throw std::runtime_error("cannot add JXL RGB16 frame");
  }
  JxlEncoderCloseFrames(encoder.get());

  if (options.copy_exif && !decoded.exif.empty()) {
    const JxlBoxType type = {'E', 'x', 'i', 'f'};
    const auto exif = exif_for_jxl(decoded.exif);
    if (!exif.empty() && JxlEncoderAddBox(encoder.get(), type, exif.data(), exif.size(), JXL_FALSE) != JXL_ENC_SUCCESS) {
      throw std::runtime_error("cannot add JXL Exif box");
    }
  }
  if (options.copy_xmp && !decoded.xmp.empty()) {
    const JxlBoxType type = {'x', 'm', 'l', ' '};
    if (JxlEncoderAddBox(encoder.get(), type, decoded.xmp.data(), decoded.xmp.size(), JXL_FALSE) != JXL_ENC_SUCCESS) {
      throw std::runtime_error("cannot add JXL XMP box");
    }
  }
  JxlEncoderCloseBoxes(encoder.get());
  JxlEncoderCloseInput(encoder.get());

  std::vector<uint8_t> output(1u << 20);
  uint8_t* next = output.data();
  size_t available = output.size();
  for (;;) {
    const JxlEncoderStatus status = JxlEncoderProcessOutput(encoder.get(), &next, &available);
    if (status == JXL_ENC_SUCCESS) break;
    if (status != JXL_ENC_NEED_MORE_OUTPUT) throw std::runtime_error("JPEG XL encoding failed");
    const size_t used = static_cast<size_t>(next - output.data());
    output.resize(output.size() * 2u);
    next = output.data() + used;
    available = output.size() - used;
  }
  output.resize(static_cast<size_t>(next - output.data()));
  return output;
}

static std::vector<uint8_t> encode_jxl_gainmap_raster(uint32_t width, uint32_t height,
                                               const std::vector<uint16_t>& pixels,
                                               uint32_t channels,
                                               const JxlColorEncoding& color,
                                               bool use_container,
                                               bool lossless, float quality, int effort) {
  auto encoder = JxlEncoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!encoder || !runner) throw std::runtime_error("cannot create gain-map JPEG XL encoder");
  if (JxlEncoderSetParallelRunner(encoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_ENC_SUCCESS)
    throw std::runtime_error("cannot attach gain-map JPEG XL runner");
  JxlResizableParallelRunnerSetThreads(runner.get(), codec_background_workers());
  if (use_container && JxlEncoderUseContainer(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS)
    throw std::runtime_error("cannot enable gain-map JPEG XL container");
  JxlBasicInfo basic{};
  JxlEncoderInitBasicInfo(&basic);
  basic.xsize = width; basic.ysize = height; basic.bits_per_sample = 16;
  basic.num_color_channels = channels; basic.num_extra_channels = 0;
  basic.uses_original_profile = JXL_TRUE;
  if (JxlEncoderSetBasicInfo(encoder.get(), &basic) != JXL_ENC_SUCCESS ||
      JxlEncoderSetColorEncoding(encoder.get(), &color) != JXL_ENC_SUCCESS)
    throw std::runtime_error("cannot configure gain-map JPEG XL raster");
  auto* frame = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
  if (!frame) throw std::runtime_error("cannot create gain-map JPEG XL frame");
  if (JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT,
                                       std::clamp(effort, 1, 10)) != JXL_ENC_SUCCESS) {
    throw std::runtime_error("cannot set gain-map JPEG XL effort");
  }
  if (lossless) JxlEncoderSetFrameLossless(frame, JXL_TRUE);
  else JxlEncoderSetFrameDistance(frame, std::max(0.01f, 1.0f - quality) * 4.0f);
  const JxlPixelFormat format{channels, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  if (JxlEncoderAddImageFrame(frame, &format, pixels.data(), pixels.size() * sizeof(uint16_t)) != JXL_ENC_SUCCESS)
    throw std::runtime_error("cannot add gain-map JPEG XL raster");
  JxlEncoderCloseInput(encoder.get());
  std::vector<uint8_t> output(1u << 20);
  uint8_t* next = output.data(); size_t available = output.size();
  for (;;) {
    const auto status = JxlEncoderProcessOutput(encoder.get(), &next, &available);
    if (status == JXL_ENC_SUCCESS) break;
    if (status != JXL_ENC_NEED_MORE_OUTPUT) throw std::runtime_error("gain-map JPEG XL encoding failed");
    const size_t used = static_cast<size_t>(next - output.data());
    output.resize(output.size() * 2u); next = output.data() + used; available = output.size() - used;
  }
  output.resize(static_cast<size_t>(next - output.data()));
  return output;
}

static void append_be16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
static void append_be32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24)); out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
static void append_signed_fraction(std::vector<uint8_t>& out, double value, uint32_t denominator) {
  append_be32(out, static_cast<uint32_t>(static_cast<int32_t>(std::llround(value * denominator))));
}
static void append_unsigned_fraction(std::vector<uint8_t>& out, double value, uint32_t denominator) {
  append_be32(out, static_cast<uint32_t>(std::llround(std::max(0.0, value) * denominator)));
}

std::vector<uint8_t> encode_gainmap_jxl(const DecodedImage& decoded,
                                        const ConversionOptions& options) {
  std::vector<uint16_t> hdr = decoded.rgb;
  if (options.output_gamut == "p3") hdr = convert_rec2020_pq_to_p3_pq(decoded);
  else if (options.output_gamut == "rec709") hdr = convert_rec2020_pq_to_rec709_pq(decoded);
  const size_t pixel_count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  std::vector<uint16_t> base(pixel_count * 3u);
  std::vector<double> channel_gain(pixel_count * 3u);
  std::vector<double> local_maximum(default_worker_count());
  const size_t gain_workers = parallel_indexed_blocks(
      hdr.size(), [&](size_t worker, size_t begin, size_t end) {
        double maximum = 1.0;
        for (size_t i = begin; i < end; ++i) {
          const double linear = std::max(
              0.0, transfer::pq_to_nits(hdr[i] / 65535.0) / 203.0);
          const double base_linear = std::min(linear, 1.0);
          const double srgb = base_linear <= 0.0031308 ? 12.92 * base_linear
              : 1.055 * std::pow(base_linear, 1.0 / 2.4) - 0.055;
          base[i] = static_cast<uint16_t>(
              std::lround(std::clamp(srgb, 0.0, 1.0) * 65535.0));
          channel_gain[i] = std::log2(
              std::max(linear, 1e-8) / std::max(base_linear, 1e-8));
          maximum = std::max(maximum, channel_gain[i]);
        }
        local_maximum[worker] = maximum;
      });
  double maximum_gain = 1.0;
  for (size_t worker = 0; worker < gain_workers; ++worker) {
    maximum_gain = std::max(maximum_gain, local_maximum[worker]);
  }
  const uint32_t scale = static_cast<uint32_t>(std::clamp(options.gainmap_scale, 1, 4));
  const uint32_t map_width = std::max(1u, (decoded.info.width + scale - 1u) / scale);
  const uint32_t map_height = std::max(1u, (decoded.info.height + scale - 1u) / scale);
  const uint32_t channels = options.multi_channel_gainmap ? 3u : 1u;
  std::vector<uint16_t> map(static_cast<size_t>(map_width) * map_height * channels);
  const size_t map_pixels = static_cast<size_t>(map_width) * map_height;
  parallel_pixel_blocks(map_pixels, [&](size_t begin, size_t end) {
    for (size_t target = begin; target < end; ++target) {
      const uint32_t x = static_cast<uint32_t>(target % map_width);
      const uint32_t y = static_cast<uint32_t>(target / map_width);
      const uint32_t sx = std::min(decoded.info.width - 1u, x * scale + scale / 2u);
      const uint32_t sy = std::min(decoded.info.height - 1u, y * scale + scale / 2u);
      const size_t source = (static_cast<size_t>(sy) * decoded.info.width + sx) * 3u;
      if (channels == 1) {
        const double gain = std::max({channel_gain[source], channel_gain[source + 1u],
                                     channel_gain[source + 2u]});
        map[target] = static_cast<uint16_t>(
            std::lround(std::clamp(gain / maximum_gain, 0.0, 1.0) * 65535.0));
      } else {
        for (size_t c = 0; c < 3; ++c) {
          map[target * 3u + c] = static_cast<uint16_t>(std::lround(
              std::clamp(channel_gain[source + c] / maximum_gain, 0.0, 1.0) * 65535.0));
        }
      }
    }
  });
  JxlColorEncoding base_color{}; base_color.color_space = JXL_COLOR_SPACE_RGB;
  base_color.white_point = JXL_WHITE_POINT_D65;
  base_color.primaries = options.output_gamut == "rec709" ? JXL_PRIMARIES_SRGB :
                         options.output_gamut == "p3" ? JXL_PRIMARIES_P3 : JXL_PRIMARIES_2100;
  base_color.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
  base_color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
  JxlColorEncoding map_color{}; map_color.color_space = channels == 1 ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
  map_color.white_point = JXL_WHITE_POINT_D65; map_color.primaries = JXL_PRIMARIES_SRGB;
  map_color.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
  map_color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
  auto base_bytes = encode_jxl_gainmap_raster(decoded.info.width, decoded.info.height, base, 3,
                                               base_color, true, options.lossless,
                                               options.image_quality, options.effort);
  auto map_bytes = encode_jxl_gainmap_raster(map_width, map_height, map, channels,
                                              map_color, false, options.lossless,
                                              options.gainmap_quality / 100.0f, options.effort);
  constexpr uint32_t denominator = 1000000u;
  std::vector<uint8_t> metadata;
  append_be16(metadata, 0); append_be16(metadata, 0);
  metadata.push_back(static_cast<uint8_t>(0x40 | 0x08 | (channels == 3 ? 0x80 : 0)));
  append_be32(metadata, denominator);
  append_unsigned_fraction(metadata, 0.0, denominator);
  append_unsigned_fraction(metadata, maximum_gain, denominator);
  for (uint32_t c = 0; c < channels; ++c) {
    append_signed_fraction(metadata, 0.0, denominator);
    append_signed_fraction(metadata, maximum_gain, denominator);
    append_unsigned_fraction(metadata, 1.0, denominator);
    append_signed_fraction(metadata, 0.0, denominator);
    append_signed_fraction(metadata, 0.0, denominator);
  }
  std::vector<uint8_t> payload;
  payload.push_back(0); append_be16(payload, static_cast<uint16_t>(metadata.size()));
  payload.insert(payload.end(), metadata.begin(), metadata.end());
  payload.push_back(0); append_be32(payload, 0);
  payload.insert(payload.end(), map_bytes.begin(), map_bytes.end());
  append_be32(base_bytes, static_cast<uint32_t>(payload.size() + 8u));
  base_bytes.insert(base_bytes.end(), {'j','h','g','m'});
  base_bytes.insert(base_bytes.end(), payload.begin(), payload.end());
  return base_bytes;
}

Verification decode_jxl(const std::vector<uint8_t>& data, const std::vector<uint16_t>* expected) {
  Verification verification;
  auto decoder = JxlDecoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!decoder || !runner) throw std::runtime_error("cannot create JXL decoder");
  if (JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot attach JXL decoder runner");
  JxlResizableParallelRunnerSetThreads(runner.get(), codec_background_workers());
  const int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE;
  if (JxlDecoderSubscribeEvents(decoder.get(), events) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot subscribe JXL events");
  if (JxlDecoderSetInput(decoder.get(), data.data(), data.size()) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot set JXL input");
  JxlDecoderCloseInput(decoder.get());
  const JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint16_t> pixels;
  JxlBasicInfo basic{};
  JxlColorEncoding color{};
  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) throw std::runtime_error("JPEG XL verification decode failed");
    if (status == JXL_DEC_BASIC_INFO) {
      if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot read JXL info");
      verification.width = basic.xsize;
      verification.height = basic.ysize;
      verification.bit_depth = basic.bits_per_sample;
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &color) == JXL_DEC_SUCCESS) {
        verification.color_encoding =
            color.transfer_function == JXL_TRANSFER_FUNCTION_HLG &&
                    color.primaries == JXL_PRIMARIES_2100 ? "Rec.2020/HLG" :
            color.transfer_function == JXL_TRANSFER_FUNCTION_HLG &&
                    color.primaries == JXL_PRIMARIES_P3 ? "Display P3/HLG" :
            color.transfer_function == JXL_TRANSFER_FUNCTION_PQ &&
                    color.primaries == JXL_PRIMARIES_2100 ? "Rec.2020/PQ" :
            color.transfer_function == JXL_TRANSFER_FUNCTION_PQ &&
                    color.primaries == JXL_PRIMARIES_P3 ? "Display P3/PQ" : "unexpected";
      } else {
        size_t icc_size = 0;
        if (JxlDecoderGetICCProfileSize(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                       &icc_size) == JXL_DEC_SUCCESS && icc_size > 0) {
          std::vector<uint8_t> icc(icc_size);
          if (JxlDecoderGetColorAsICCProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                            icc.data(), icc.size()) == JXL_DEC_SUCCESS) {
            const auto signal = parse_icc_signal(icc.data(), icc.size());
            if (signal.cicp_present && (signal.transfer == 16 || signal.transfer == 18)) {
              verification.color_encoding = signal.transfer == 18
                  ? (signal.primaries == 12 ? "Display P3/HLG ICC" : signal.primaries == 9
                                             ? "Rec.2020/HLG ICC" : "unexpected ICC")
                  : (signal.primaries == 12 ? "Display P3/PQ ICC" : signal.primaries == 9
                                             ? "Rec.2020/PQ ICC" : "unexpected ICC");
            }
          }
        }
      }
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t size = 0;
      if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &size) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot size JXL output");
      pixels.resize((size + sizeof(uint16_t) - 1u) / sizeof(uint16_t));
      if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, pixels.data(), size) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot set JXL output");
    } else if (status == JXL_DEC_SUCCESS) {
      break;
    }
  }
  verification.pixel_format = "RGB uint16";
  verification.exact_roundtrip = expected && expected->size() == pixels.size() &&
                                 std::memcmp(expected->data(), pixels.data(), pixels.size() * sizeof(uint16_t)) == 0;
  verification.checks.push_back(verification.width && verification.height ? "dimensions parse" : "dimensions missing");
  verification.checks.push_back(verification.bit_depth == 16 ? "16-bit integer" : "unexpected bit depth");
  const bool pq_profile = verification.color_encoding == "Rec.2020/PQ" ||
                          verification.color_encoding == "Display P3/PQ" ||
                          verification.color_encoding == "Rec.2020/PQ ICC" ||
                          verification.color_encoding == "Display P3/PQ ICC";
  const bool hlg_profile = verification.color_encoding == "Rec.2020/HLG" ||
                           verification.color_encoding == "Display P3/HLG" ||
                           verification.color_encoding == "Rec.2020/HLG ICC" ||
                           verification.color_encoding == "Display P3/HLG ICC";
  verification.checks.push_back(pq_profile || hlg_profile
      ? verification.color_encoding + " profile" : "profile mismatch");
  if (expected) verification.checks.push_back(verification.exact_roundtrip ? "exact RGB16 encoder-buffer roundtrip" : "RGB16 roundtrip mismatch");
  verification.passed = verification.width > 0 && verification.height > 0 && verification.bit_depth == 16 &&
                        (pq_profile || hlg_profile) && (!expected || verification.exact_roundtrip);
  return verification;
}

DecodedImage decode_jxl_input(const std::filesystem::path& path) {
  const auto data = read_file(path);
  auto decoder = JxlDecoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!decoder || !runner) throw std::runtime_error("cannot create JPEG XL input decoder");
  if (JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("cannot attach JPEG XL input runner");
  }
  JxlResizableParallelRunnerSetThreads(runner.get(), codec_background_workers());
  if (JxlDecoderSetDecompressBoxes(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
      JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                                               JXL_DEC_FULL_IMAGE | JXL_DEC_BOX) != JXL_DEC_SUCCESS ||
      JxlDecoderSetInput(decoder.get(), data.data(), data.size()) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("cannot initialize JPEG XL input decode");
  }
  JxlDecoderCloseInput(decoder.get());
  const JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  DecodedImage decoded;
  decoded.info.path = path; decoded.info.format = "JPEG XL"; decoded.info.container_brand = "JXL ";
  decoded.info.asset_kind = "non-HDR"; decoded.info.codec = "JPEG XL"; decoded.info.chroma = "4:4:4 RGB";
  decoded.info.pixel_format = "RGB integer";
  decoded.info.color_signal_kind = "Unknown";
  decoded.info.exif_status = "absent";
  decoded.info.xmp_status = "absent";
  decoded.info.icc_status = "absent";
  JxlColorEncoding color{};
  bool structured_available = false;
  std::vector<uint8_t> original_icc;
  JxlMetadataBoxes metadata(decoder.get(), decoded.exif, decoded.xmp);
  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) throw std::runtime_error("JPEG XL input decode failed");
    if (status == JXL_DEC_BASIC_INFO) {
      JxlBasicInfo basic{};
      if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot read JPEG XL input info");
      decoded.info.width = basic.xsize; decoded.info.height = basic.ysize; decoded.info.bit_depth = basic.bits_per_sample;
      decoded.info.profile = "RGB" + std::to_string(basic.bits_per_sample);
      decoded.info.pixel_format = "RGB" + std::to_string(basic.bits_per_sample) +
          (basic.alpha_bits ? "A integer" : " integer");
      decoded.info.original_orientation = static_cast<uint8_t>(basic.orientation);
      decoded.info.orientation_status = "present";
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      structured_available = JxlDecoderGetColorAsEncodedProfile(
          decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &color) == JXL_DEC_SUCCESS;
      size_t icc_size = 0;
      if (!structured_available &&
          JxlDecoderGetICCProfileSize(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                      &icc_size) == JXL_DEC_SUCCESS && icc_size > 0) {
        original_icc.resize(icc_size);
        if (JxlDecoderGetColorAsICCProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                           original_icc.data(), original_icc.size()) != JXL_DEC_SUCCESS) {
          original_icc.clear();
        }
      }
    } else if (status == JXL_DEC_BOX) {
      metadata.start_box();
    } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
      metadata.continue_box();
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t size = 0;
      if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &size) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot size JPEG XL input pixels");
      decoded.rgb.resize((size + sizeof(uint16_t) - 1u) / sizeof(uint16_t));
      if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, decoded.rgb.data(), size) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot set JPEG XL input buffer");
    } else if (status == JXL_DEC_SUCCESS) {
      metadata.finish_box();
      break;
    }
  }
  decoded.info.exif_present = metadata.exif_present;
  decoded.info.xmp_present = metadata.xmp_present;
  decoded.info.exif_status = metadata_status(metadata.exif_present);
  decoded.info.xmp_status = metadata_status(metadata.xmp_present);
  if (structured_available && color.color_space == JXL_COLOR_SPACE_RGB) {
    decoded.info.native_color_present = true;
    decoded.info.native_primaries = color.primaries == JXL_PRIMARIES_P3 ? 12 :
        color.primaries == JXL_PRIMARIES_2100 ? 9 :
        color.primaries == JXL_PRIMARIES_SRGB ? 1 : 0;
    decoded.info.native_transfer = static_cast<uint16_t>(color.transfer_function);
    decoded.info.native_matrix = 0;
    decoded.info.native_full_range = true;
    decoded.info.native_range_known = true;
    decoded.info.native_color_description = "JPEG XL structured color encoding (CICP equivalent)";
  }
  store_icc_signal(decoded.info, parse_icc_signal(original_icc.data(), original_icc.size()));
  resolve_color_signaling(decoded.info, true);
  decoded.info.color_signal_kind = decoded.info.native_color_present
      ? "CICP equivalent (JPEG XL structured color encoding)"
      : decoded.info.resolved_signaling_source;
  if (!decoded.info.icc_description.empty()) decoded.info.profile = decoded.info.icc_description;
  if (decoded.info.asset_kind != "direct-hdr") return decoded;
  if (decoded.info.bit_depth > 16 || decoded.info.bit_depth == 0) {
    throw std::runtime_error("JPEG XL direct HDR integer storage exceeds the supported 16-bit precision");
  }
  if (decoded.info.transfer == 18) {
    const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
    parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
    for (size_t p = begin; p < end; ++p) {
      const auto nits = transfer::hlg_to_linear_nits({
          decoded.rgb[p * 3u] / 65535.0,
          decoded.rgb[p * 3u + 1u] / 65535.0,
          decoded.rgb[p * 3u + 2u] / 65535.0});
      for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] =
          static_cast<uint16_t>(std::llround(transfer::nits_to_pq(nits[c]) * 65535.0));
    }
    });
  } else if (decoded.info.primaries == 12) {
    convert_p3_pq_to_rec2020_pq(decoded.rgb);
  } else if (decoded.info.primaries == 1) {
    convert_rec709_pq_to_rec2020_pq(decoded.rgb);
  }
  return decoded;
}

Verification verify_gainmap_jxl(const std::filesystem::path& path, const DecodedImage& decoded,
                                const ConversionOptions& options,
                                std::atomic_bool* cancel) {
  const auto reconstructed = gainmap::reconstruct_iso_gainmap_jxl(path, cancel);
  Verification verification;
  verification.width = reconstructed.info.width;
  verification.height = reconstructed.info.height;
  verification.bit_depth = 16;
  verification.pixel_format = "JPEG XL SDR base + jhgm gain map";
  verification.color_encoding = "ISO 21496-1 gain-map HDR";
  verification.gain_map_width = reconstructed.info.gain_map_width;
  verification.gain_map_height = reconstructed.info.gain_map_height;
  verification.gain_map_channels = reconstructed.info.gain_map_channels;
  const uint32_t scale = static_cast<uint32_t>(std::clamp(options.gainmap_scale, 1, 4));
  const uint32_t expected_map_width = (decoded.info.width + scale - 1u) / scale;
  const uint32_t expected_map_height = (decoded.info.height + scale - 1u) / scale;
  const size_t pixel_count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  verification.passed = verification.width == decoded.info.width &&
                        verification.height == decoded.info.height &&
                        verification.gain_map_width == expected_map_width &&
                        verification.gain_map_height == expected_map_height &&
                        verification.gain_map_channels == 3 &&
                        decoded.rgb.size() == pixel_count * 3u &&
                        reconstructed.rec2020_pq_rgb.size() == pixel_count * 3u;
  if (verification.passed && pixel_count) {
    // Both rasters use canonical Rec.2020 PQ16; LUT values are nits.
    // Match the existing reconstruction metrics (linear, 1 = 203 nits).
    const size_t samples = std::min<size_t>(pixel_count, 4096u);
    const auto& nits = pq_nits_lut();
    double squared = 0.0;
    double maximum = 0.0;
    for (size_t sample = 0; sample < samples; ++sample) {
      const size_t pixel = sample * pixel_count / samples;
      for (size_t channel = 0; channel < 3; ++channel) {
        const size_t i = pixel * 3u + channel;
        const double error = std::abs(nits[decoded.rgb[i]] -
            nits[reconstructed.rec2020_pq_rgb[i]]) / 203.0;
        squared += error * error;
        maximum = std::max(maximum, error);
      }
    }
    verification.reconstruction_rmse = std::sqrt(squared / (samples * 3u));
    verification.reconstruction_max_abs_error = maximum;
    verification.checks.push_back("linear reconstruction error measured over " +
        std::to_string(samples) + " sampled pixels (1.0 = 203 nits)");
  }
  verification.checks.push_back("JPEG XL jhgm bundle and ISO metadata parsed");
  verification.checks.push_back("gain-map dimensions and RGB channels checked");
  verification.checks.push_back("gain-map HDR reconstruction completed");
  return verification;
}

}  // namespace hdrbridge::detail
