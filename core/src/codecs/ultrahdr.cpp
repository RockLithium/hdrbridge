#include "codecs/ultrahdr.h"
#include "codecs/uhdr_helpers.h"
#include "metadata/exif_output.h"
#include "metadata/exif_payload.h"
#include "metadata/source_metadata.h"
#include "color/gamut.h"
#include "color/pq.h"
#include "color/half_float.h"
#include "color/signaling.h"
#include "codecs/jpeg_probe.h"
#include "image/orientation.h"
#include "infrastructure/files.h"
#include "infrastructure/pixel_workers.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include "infrastructure/wic.h"
#endif

namespace hdrbridge::detail {

std::vector<uint8_t> encode_ultrahdr(const DecodedImage& decoded, const ConversionOptions& options,
                                     const HdrStats& stats) {
  DecodedImage gamut_source = decoded;
  if (options.output_gamut == "p3") {
    gamut_source.rgb = convert_rec2020_pq_to_p3_pq(decoded);
  } else if (options.output_gamut == "rec709") {
    gamut_source.rgb = convert_rec2020_pq_to_rec709_pq(decoded);
  }
  auto packed = pq16_to_rgba1010102(gamut_source);
  uhdr_raw_image_t hdr{};
  hdr.fmt = UHDR_IMG_FMT_32bppRGBA1010102;
  hdr.cg = options.output_gamut == "rec709" ? UHDR_CG_BT_709 :
           options.output_gamut == "p3" ? UHDR_CG_DISPLAY_P3 : UHDR_CG_BT_2100;
  hdr.ct = UHDR_CT_PQ;
  hdr.range = UHDR_CR_FULL_RANGE;
  hdr.w = decoded.info.width;
  hdr.h = decoded.info.height;
  hdr.planes[UHDR_PLANE_PACKED] = packed.data();
  hdr.stride[UHDR_PLANE_PACKED] = decoded.info.width;

  UhdrEncoderPtr encoder(uhdr_create_encoder());
  if (!encoder) throw std::runtime_error("cannot create Ultra HDR encoder");
  require_uhdr(uhdr_enc_set_raw_image(encoder.get(), &hdr, UHDR_HDR_IMG), "set Ultra HDR PQ input");
  require_uhdr(uhdr_enc_set_quality(encoder.get(), std::clamp(options.base_quality, 0, 100), UHDR_BASE_IMG), "set Ultra HDR base quality");
  require_uhdr(uhdr_enc_set_quality(encoder.get(), std::clamp(options.gainmap_quality, 0, 100), UHDR_GAIN_MAP_IMG), "set Ultra HDR gain-map quality");
  require_uhdr(uhdr_enc_set_gainmap_scale_factor(encoder.get(), std::clamp(options.gainmap_scale, 1, 128)), "set Ultra HDR gain-map scale");
  require_uhdr(uhdr_enc_set_using_multi_channel_gainmap(encoder.get(), options.multi_channel_gainmap ? 1 : 0), "set Ultra HDR gain-map channel mode");
  require_uhdr(uhdr_enc_set_target_display_peak_brightness(
                   encoder.get(), static_cast<float>(stats.chosen_target_nits)),
               "set faithful Ultra HDR target peak brightness");
  require_uhdr(uhdr_enc_set_preset(encoder.get(), UHDR_USAGE_BEST_QUALITY), "set Ultra HDR preset");
  require_uhdr(uhdr_enc_set_output_format(encoder.get(), UHDR_CODEC_JPG), "set Ultra HDR JPEG output");
  auto jpeg_exif = options.copy_exif
      ? jpeg_exif_from_heif(decoded.exif, decoded.info.width, decoded.info.height)
      : std::vector<uint8_t>{};
  if (!jpeg_exif.empty()) {
    uhdr_mem_block_t exif{jpeg_exif.data(), jpeg_exif.size(), jpeg_exif.size()};
    require_uhdr(uhdr_enc_set_exif_data(encoder.get(), &exif), "set Ultra HDR Exif");
  }
  require_uhdr(uhdr_encode(encoder.get()), "encode Ultra HDR");
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(encoder.get());
  if (!output || !output->data || output->data_sz == 0) throw std::runtime_error("Ultra HDR encoder returned empty output");
  const auto* first = static_cast<const uint8_t*>(output->data);
  return {first, first + output->data_sz};
}

Verification decode_ultrahdr(const std::vector<uint8_t>& bytes,
                             const DecodedImage* expected,
                             const HdrStats* expected_stats,
                             const ConversionOptions* expected_options) {
  Verification v;
  if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()))) {
    v.checks.push_back("libultrahdr did not detect a gain map");
    return v;
  }
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data());
  input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create Ultra HDR decoder");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set Ultra HDR input");
  require_uhdr(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat), "set Ultra HDR verification format");
  require_uhdr(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR), "set Ultra HDR verification transfer");
  const float full_boost = expected_stats
      ? static_cast<float>(expected_stats->chosen_target_nits / 203.0)
      : static_cast<float>(10000.0 / 203.0);
  require_uhdr(uhdr_dec_set_out_max_display_boost(decoder.get(), std::max(1.0f, full_boost)), "set Ultra HDR verification boost");
  require_uhdr(uhdr_dec_probe(decoder.get()), "probe Ultra HDR");
  v.width = static_cast<uint32_t>(uhdr_dec_get_image_width(decoder.get()));
  v.height = static_cast<uint32_t>(uhdr_dec_get_image_height(decoder.get()));
  const int gain_width = uhdr_dec_get_gainmap_width(decoder.get());
  const int gain_height = uhdr_dec_get_gainmap_height(decoder.get());
  uhdr_mem_block_t* base = uhdr_dec_get_base_image(decoder.get());
  uhdr_mem_block_t* gain_map = uhdr_dec_get_gainmap_image(decoder.get());
  uhdr_gainmap_metadata_t* metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  if (metadata) v.hdr_capacity_max = metadata->hdr_capacity_max;
  v.gain_map_width = static_cast<uint32_t>(std::max(gain_width, 0));
  v.gain_map_height = static_cast<uint32_t>(std::max(gain_height, 0));
  if (gain_map && gain_map->data && gain_map->data_sz) {
    const auto probe = jpeg::inspect(gain_map->data, gain_map->data_sz);
    v.gain_map_channels = probe.channels;
    if (probe.channels == 3) {
#ifdef _WIN32
      if (gain_map->data_sz <= std::numeric_limits<DWORD>::max()) {
      ComScope com;
      auto factory = wic_factory();
      ComPtr<IWICStream> stream;
      require_hr(factory->CreateStream(&stream), "create gain-map JPEG memory stream");
      require_hr(stream->InitializeFromMemory(static_cast<BYTE*>(gain_map->data),
                 static_cast<DWORD>(gain_map->data_sz)), "initialize gain-map JPEG memory stream");
      ComPtr<IWICBitmapDecoder> jpeg_decoder;
      require_hr(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                 WICDecodeMetadataCacheOnDemand, &jpeg_decoder), "open gain-map JPEG decoder");
      ComPtr<IWICBitmapFrameDecode> frame;
      require_hr(jpeg_decoder->GetFrame(0, &frame), "read gain-map JPEG frame");
      ComPtr<IWICFormatConverter> converter;
      require_hr(factory->CreateFormatConverter(&converter), "create gain-map RGB converter");
      require_hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB,
                 WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
                 "convert gain-map JPEG to RGB");
      std::vector<uint8_t> rgb(static_cast<size_t>(probe.width) * probe.height * 3u);
      require_hr(converter->CopyPixels(nullptr, probe.width * 3u,
                 static_cast<UINT>(rgb.size()), rgb.data()), "decode gain-map RGB samples");
      uint8_t maximum = 0;
      for (size_t index = 0; index + 2u < rgb.size(); index += 3u) {
        maximum = std::max<uint8_t>(maximum, static_cast<uint8_t>(std::max({
            std::abs(static_cast<int>(rgb[index]) - rgb[index + 1u]),
            std::abs(static_cast<int>(rgb[index]) - rgb[index + 2u]),
            std::abs(static_cast<int>(rgb[index + 1u]) - rgb[index + 2u])})));
      }
      v.gain_map_channel_difference_max = maximum / 255.0;
      }
#endif
    }
  }

  uint32_t base_width = 0, base_height = 0;
  if (base && base->data && base->data_sz > 0) {
#ifdef _WIN32
    if (base->data_sz <= std::numeric_limits<DWORD>::max()) {
    ComScope com;
    auto factory = wic_factory();
    ComPtr<IWICStream> stream;
    require_hr(factory->CreateStream(&stream), "create base JPEG memory stream");
    require_hr(stream->InitializeFromMemory(static_cast<BYTE*>(base->data), static_cast<DWORD>(base->data_sz)), "initialize base JPEG memory stream");
    ComPtr<IWICBitmapDecoder> base_decoder;
    require_hr(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &base_decoder), "ordinary JPEG decode of Ultra HDR base");
    ComPtr<IWICBitmapFrameDecode> frame;
    require_hr(base_decoder->GetFrame(0, &frame), "read Ultra HDR base frame");
    require_hr(frame->GetSize(&base_width, &base_height), "read Ultra HDR base dimensions");
    }
#else
    const auto base_probe = jpeg::inspect(base->data, base->data_sz);
    base_width = base_probe.width;
    base_height = base_probe.height;
#endif
  }

  require_uhdr(uhdr_decode(decoder.get()), "decode Ultra HDR reconstruction");
  uhdr_raw_image_t* image = uhdr_get_decoded_image(decoder.get());
  if (!image || !image->planes[UHDR_PLANE_PACKED]) throw std::runtime_error("Ultra HDR decoded image is empty");
  v.bit_depth = 16;
  v.pixel_format = "RGBA half-float linear";
  v.color_encoding = "Ultra HDR SDR base + ISO 21496-1 gain map";
  v.min_value = std::numeric_limits<double>::infinity();
  v.max_value = -std::numeric_limits<double>::infinity();
  const auto* pixels = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  for (uint32_t y = 0; y < image->h; ++y) {
    const auto* row = pixels + static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] * 4u;
    for (uint32_t x = 0; x < image->w; ++x) {
      for (size_t c = 0; c < 3; ++c) {
        const float value = half_to_float(row[static_cast<size_t>(x) * 4u + c]);
        if (!std::isfinite(value)) v.finite = false;
        v.min_value = std::min(v.min_value, static_cast<double>(value));
        v.max_value = std::max(v.max_value, static_cast<double>(value));
      }
    }
  }
  if (expected && expected->info.width == image->w && expected->info.height == image->h &&
      image->cg == UHDR_CG_BT_2100) {
    const auto& lut = pq_nits_lut();
    long double squared_error = 0.0;
    double max_error = 0.0;
    uint64_t sample_count = 0;
    for (uint32_t y = 0; y < image->h; ++y) {
      const auto* row = pixels + static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] * 4u;
      for (uint32_t x = 0; x < image->w; ++x) {
        const size_t p = static_cast<size_t>(y) * image->w + x;
        for (size_t c = 0; c < 3; ++c) {
          const double reconstructed = half_to_float(row[static_cast<size_t>(x) * 4u + c]);
          const double source = lut[expected->rgb[p * 3u + c]] / 203.0;
          const double error = std::abs(reconstructed - source);
          squared_error += static_cast<long double>(error) * error;
          max_error = std::max(max_error, error);
          ++sample_count;
        }
      }
    }
    v.reconstruction_rmse = sample_count
        ? std::sqrt(static_cast<double>(squared_error / sample_count)) : 0.0;
    v.reconstruction_max_abs_error = max_error;
  }
  if (expected_stats) attach_hdr_stats(v, *expected_stats);
  v.checks.push_back("libultrahdr detected gain-map JPEG");
#ifdef __ANDROID__
  v.checks.push_back(base_width == v.width && base_height == v.height
      ? "SDR base JPEG header has full-size dimensions" : "SDR base dimensions failed");
  v.checks.push_back(v.gain_map_channels == 3
      ? "RGB gain map confirmed by JPEG SOF; channel difference is not measured by this adapter"
      : v.gain_map_channels == 1 ? "monochrome gain map confirmed by JPEG SOF"
                                 : "gain-map JPEG channel count unavailable");
#else
  v.checks.push_back(base_width == v.width && base_height == v.height ? "ordinary WIC JPEG decoder opened full-size SDR base" : "SDR base decode/dimensions failed");
  v.checks.push_back(v.gain_map_channels == 3
      ? (v.gain_map_channel_difference_max > 0.0
          ? "RGB gain map contains distinct per-channel samples"
          : "RGB gain map channels decode identically")
      : v.gain_map_channels == 1 ? "monochrome gain map confirmed by JPEG SOF"
                                 : "gain-map JPEG channel count unavailable");
#endif
  v.checks.push_back(gain_width > 0 && gain_height > 0 ? "gain-map dimensions present" : "gain map missing");
  v.checks.push_back(metadata ? "ISO gain-map metadata parsed" : "gain-map metadata missing");
  if (expected_stats && metadata) {
    const double expected_capacity = expected_stats->chosen_target_nits / 203.0;
    const bool capacity_matches = std::abs(metadata->hdr_capacity_max - expected_capacity) <=
                                  std::max(0.02, expected_capacity * 0.02);
    v.checks.push_back(capacity_matches
        ? "gain-map capacity matches measured target peak"
        : "gain-map capacity does not match chosen target peak");
    v.checks.push_back("linear-light source reconstruction RMSE recorded");
  }
  v.checks.push_back(v.finite && v.max_value > 0.0 ? "finite non-empty HDR reconstruction" : "invalid HDR reconstruction");
  v.passed = v.width > 0 && v.height > 0 && base_width == v.width && base_height == v.height &&
             gain_width > 0 && gain_height > 0 && metadata && v.finite && v.max_value > 0.0;
  if (expected_stats && metadata) {
    const double expected_capacity = expected_stats->chosen_target_nits / 203.0;
    v.passed = v.passed && std::abs(metadata->hdr_capacity_max - expected_capacity) <=
               std::max(0.02, expected_capacity * 0.02);
  }
  if (expected && expected_options) {
    const uint32_t expected_gain_width = expected->info.width /
        static_cast<uint32_t>(expected_options->gainmap_scale);
    const uint32_t expected_gain_height = expected->info.height /
        static_cast<uint32_t>(expected_options->gainmap_scale);
    const uint32_t expected_channels = expected_options->multi_channel_gainmap ? 3u : 1u;
    const bool layout_matches = v.gain_map_width == expected_gain_width &&
                                v.gain_map_height == expected_gain_height &&
                                v.gain_map_channels == expected_channels;
    v.checks.push_back(layout_matches
        ? "gain-map resolution and channel mode match requested options"
        : "gain-map layout does not match requested options");
    v.passed = v.passed && layout_matches;
  }
  return v;
}

static void apply_jpeg_icc_signal(SourceInfo& info, IccSignal icc) {
  icc.present = icc.present || info.icc_present;
  store_icc_signal(info, icc);
  if (!icc.valid) return;
  info.primaries = icc.primaries;
  info.transfer = icc.transfer ? icc.transfer : icc.sdr_transfer;
  info.matrix = icc.matrix;
  if (info.primaries != 0 || info.transfer != 0 || icc.cicp_present) {
    info.resolved_signaling_source = "ICC";
  }
}

static SourceInfo ultrahdr_source_info(const std::filesystem::path& path,
                                const std::vector<uint8_t>& bytes,
                                uhdr_codec_private_t* decoder) {
  SourceInfo info;
  info.path = path;
  info.format = "Ultra HDR JPEG";
  info.container_brand = "JPEG/MPF";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "iso-ultrahdr-jpeg";
  info.codec = "JPEG SDR base + JPEG ISO 21496-1 gain map";
  info.profile = "ISO 21496-1 gain-map HDR reconstruction";
  info.color_signal_kind = "JPEG ICC / gain-map metadata";
  info.pixel_format = "JPEG SDR base + JPEG gain map";
  info.width = static_cast<uint32_t>(uhdr_dec_get_image_width(decoder));
  info.height = static_cast<uint32_t>(uhdr_dec_get_image_height(decoder));
  info.gain_map_width = static_cast<uint32_t>(uhdr_dec_get_gainmap_width(decoder));
  info.gain_map_height = static_cast<uint32_t>(uhdr_dec_get_gainmap_height(decoder));

  const auto outer_probe = jpeg::inspect(bytes.data(), bytes.size());
  info.exif_present = outer_probe.exif_present;
  info.xmp_present = outer_probe.xmp_present;
  info.icc_present = outer_probe.icc_present;
  auto base_icc = parse_icc_signal(outer_probe.icc.data(), outer_probe.icc.size());
  if (auto* base = uhdr_dec_get_base_image(decoder);
      base && base->data && base->data_sz) {
    const auto base_probe = jpeg::inspect(base->data, base->data_sz);
    info.base_width = base_probe.width;
    info.base_height = base_probe.height;
    info.base_bit_depth = base_probe.bit_depth;
    info.base_channels = base_probe.channels;
    info.exif_present = info.exif_present || base_probe.exif_present;
    info.xmp_present = info.xmp_present || base_probe.xmp_present;
    info.icc_present = info.icc_present || base_probe.icc_present;
    if (!base_icc.valid) base_icc = parse_icc_signal(base_probe.icc.data(), base_probe.icc.size());
  }
  if (!info.base_width) info.base_width = info.width;
  if (!info.base_height) info.base_height = info.height;
  info.base_codec = "JPEG";
  info.base_transfer = "SDR gamma";
  info.bit_depth = info.base_bit_depth;
  info.chroma = info.base_channels == 1 ? "grayscale SDR base" : "JPEG SDR base";
  info.full_range = true;

  if (auto* gain = uhdr_dec_get_gainmap_image(decoder);
      gain && gain->data && gain->data_sz) {
    const auto gain_probe = jpeg::inspect(gain->data, gain->data_sz);
    info.gain_map_channels = gain_probe.channels;
  }
  if (info.gain_map_width) {
    info.gain_map_scale_x = info.base_width /
        static_cast<double>(info.gain_map_width);
  }
  if (info.gain_map_height) {
    info.gain_map_scale_y = info.base_height /
        static_cast<double>(info.gain_map_height);
  }

  if (auto* metadata = uhdr_dec_get_gainmap_metadata(decoder)) {
    info.hdr_capacity_min = metadata->hdr_capacity_min;
    info.hdr_capacity_max = metadata->hdr_capacity_max;
    info.base_hdr_headroom = std::log2(std::max(
        static_cast<double>(metadata->hdr_capacity_min), 1e-12));
    info.alternate_hdr_headroom = std::log2(std::max(
        static_cast<double>(metadata->hdr_capacity_max), 1e-12));
    info.gain_map_uses_base_color_space = metadata->use_base_cg != 0;
    for (size_t channel = 0; channel < 3; ++channel) {
      info.gain_map_min[channel] = std::log2(std::max(
          static_cast<double>(metadata->min_content_boost[channel]), 1e-12));
      info.gain_map_max[channel] = std::log2(std::max(
          static_cast<double>(metadata->max_content_boost[channel]), 1e-12));
      info.gain_map_gamma[channel] = metadata->gamma[channel];
      info.base_offset[channel] = metadata->offset_sdr[channel];
      info.alternate_offset[channel] = metadata->offset_hdr[channel];
    }
  }

  if (!base_icc.valid) {
    if (auto* icc = uhdr_dec_get_icc(decoder); icc && icc->data && icc->data_sz) {
      base_icc = parse_icc_signal(icc->data, icc->data_sz);
    }
  }
  apply_jpeg_icc_signal(info, std::move(base_icc));
  if (info.transfer == 13) info.base_transfer = "sRGB";
  else if (info.transfer == 14) info.base_transfer = "BT.2020 SDR";
  else if (info.transfer == 1) info.base_transfer = "BT.709";
  info.base_color_space = info.icc_description;
  if (info.base_color_space.empty()) {
    info.base_color_space = info.primaries == 1 ? "BT.709 / sRGB primaries" :
                            info.primaries == 9 ? "BT.2020" :
                            info.primaries == 12 ? "Display P3" :
                            "SDR / unspecified primaries";
  }
  if (auto* exif = uhdr_dec_get_exif(decoder); exif && exif->data && exif->data_sz) {
    const auto* first = static_cast<const uint8_t*>(exif->data);
    std::vector<uint8_t> exif_bytes(first, first + exif->data_sz);
    info.exif_present = true;
    info.original_orientation = orientation::read_exif_orientation(exif_bytes);
    info.orientation_status = orientation::has_exif_orientation(exif_bytes)
        ? "present" : "absent";
  }
  info.exif_status = metadata_status(info.exif_present);
  info.xmp_status = metadata_status(info.xmp_present);
  if (info.orientation_status == "unsupported") info.orientation_status = "absent";
  info.orientation_normalized = true;
  return info;
}

static SourceInfo inspect_ultrahdr_input(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()))) {
    throw std::runtime_error("JPEG input is not a standard Ultra HDR gain-map asset");
  }
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data());
  input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create Ultra HDR inspector");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set Ultra HDR inspection input");
  require_uhdr(uhdr_dec_probe(decoder.get()), "probe Ultra HDR inspection input");
  return ultrahdr_source_info(path, bytes, decoder.get());
}

static SourceInfo inspect_plain_jpeg(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  const auto probe = jpeg::inspect(bytes.data(), bytes.size());
  if (probe.width == 0 || probe.height == 0 || probe.bit_depth == 0 || probe.channels == 0) {
    throw std::runtime_error("JPEG input decode failed");
  }
  SourceInfo info;
  info.path = path;
  info.format = "JPEG";
  info.container_brand = "JPEG";
  info.asset_kind = "non-HDR";
  info.width = probe.width;
  info.height = probe.height;
  info.bit_depth = probe.bit_depth;
  info.codec = "JPEG";
  info.chroma = probe.channels == 1 ? "grayscale" :
                probe.channels == 3 ? "YCbCr/RGB, 3 components" :
                std::to_string(probe.channels) + " components";
  info.pixel_format = "JPEG " + info.chroma;
  info.color_signal_kind = "JPEG ICC / application markers";
  info.exif_present = probe.exif_present;
  info.xmp_present = probe.xmp_present;
  info.icc_present = probe.icc_present;
  apply_jpeg_icc_signal(info, parse_icc_signal(
      probe.icc_present && !probe.icc.empty() ? probe.icc.data() : nullptr,
      probe.icc_present && !probe.icc.empty() ? probe.icc.size() : 0));
  info.profile = !info.icc_description.empty()
      ? info.icc_description
      : "unprofiled / unspecified";
  info.exif_status = metadata_status(info.exif_present);
  info.xmp_status = metadata_status(info.xmp_present);
  info.original_orientation = orientation::read_exif_orientation(probe.exif);
  info.orientation_status = orientation::has_exif_orientation(probe.exif)
      ? "present" : "absent";
  info.orientation_normalized = false;
  return info;
}

DecodedImage decode_ultrahdr_input(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()))) {
    DecodedImage decoded;
    decoded.info = inspect_plain_jpeg(path);
    const auto probe = jpeg::inspect(bytes.data(), bytes.size());
    decoded.exif = probe.exif;
    decoded.xmp = probe.xmp;
    return decoded;
  }
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data()); input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED; input.ct = UHDR_CT_UNSPECIFIED; input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create Ultra HDR input decoder");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set Ultra HDR input asset");
  require_uhdr(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat), "set Ultra HDR input precision");
  require_uhdr(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR), "set Ultra HDR input linear decode");
  require_uhdr(uhdr_dec_set_out_max_display_boost(decoder.get(), static_cast<float>(10000.0 / 203.0)), "request full Ultra HDR gain map");
  require_uhdr(uhdr_dec_probe(decoder.get()), "probe Ultra HDR input");
  SourceInfo source_info = ultrahdr_source_info(path, bytes, decoder.get());
  require_uhdr(uhdr_decode(decoder.get()), "reconstruct Ultra HDR input");
  uhdr_raw_image_t* image = uhdr_get_decoded_image(decoder.get());
  if (!image || image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || !image->planes[UHDR_PLANE_PACKED]) {
    throw std::runtime_error("Ultra HDR input did not decode to linear half float");
  }
  DecodedImage decoded;
  decoded.info = std::move(source_info);
  const auto source_probe = jpeg::inspect(bytes.data(), bytes.size());
  decoded.exif = source_probe.exif;
  // ISO/Ultra HDR XMP describes the source gain-map relationship and must not
  // be copied as ordinary descriptive XMP onto a new direct-HDR output.
  const size_t count = static_cast<size_t>(image->w) * image->h;
  decoded.rgb.resize(count * 3u);
  const auto* pixels = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  for (uint32_t y = 0; y < image->h; ++y) {
    const auto* row = pixels + static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] * 4u;
    for (uint32_t x = 0; x < image->w; ++x) {
      const size_t p = static_cast<size_t>(y) * image->w + x;
      const double r = static_cast<double>(half_to_float(row[static_cast<size_t>(x) * 4u])) * 203.0;
      const double g = static_cast<double>(half_to_float(row[static_cast<size_t>(x) * 4u + 1u])) * 203.0;
      const double b = static_cast<double>(half_to_float(row[static_cast<size_t>(x) * 4u + 2u])) * 203.0;
      std::array<double, 3> rec2020{};
      if (image->cg == UHDR_CG_BT_2100) {
        rec2020 = {r, g, b};
      } else if (image->cg == UHDR_CG_DISPLAY_P3) {
        rec2020 = {0.7538330 * r + 0.1985974 * g + 0.0475696 * b,
                   0.0457438 * r + 0.9417772 * g + 0.0124789 * b,
                  -0.0012103 * r + 0.0176017 * g + 0.9836086 * b};
      } else {
        rec2020 = {0.6274040 * r + 0.3292830 * g + 0.0433130 * b,
                   0.0690970 * r + 0.9195400 * g + 0.0113620 * b,
                   0.0163910 * r + 0.0880130 * g + 0.8955950 * b};
      }
      for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(rec2020[c], 0.0, 10000.0)) * 65535.0));
    }
  }
  return decoded;
}

SourceInfo inspect_jpeg(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  if (bytes.size() <= static_cast<size_t>(std::numeric_limits<int>::max()) &&
      is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()))) {
    return inspect_ultrahdr_input(path);
  }
  return inspect_plain_jpeg(path);
}

}  // namespace hdrbridge::detail
