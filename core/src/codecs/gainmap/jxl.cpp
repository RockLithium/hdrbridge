#include "codecs/gainmap/jxl.h"
#include "codecs/gainmap/iso_metadata.h"
#include "codecs/gainmap/decode_support.h"
#include "codecs/gainmap/extract_support.h"
#include "codecs/gainmap/reconstruction_math.h"
#include "codecs/jxl_metadata_boxes.h"
#include "image/orientation.h"
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

namespace hdrbridge::gainmap {

using namespace detail;

namespace {

double sample_jxl_map_bilinear(const std::vector<uint16_t>& map, uint32_t map_width,
                               uint32_t map_height, uint32_t map_channels,
                               uint32_t base_width, uint32_t base_height,
                               uint32_t x, uint32_t y, size_t channel) {
  const size_t actual_channel = map_channels == 1 ? 0 : channel;
  const double mx = (static_cast<double>(x) + 0.5) * map_width / base_width - 0.5;
  const double my = (static_cast<double>(y) + 0.5) * map_height / base_height - 0.5;
  const int x0 = std::clamp(static_cast<int>(std::floor(mx)), 0, static_cast<int>(map_width) - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor(my)), 0, static_cast<int>(map_height) - 1);
  const int x1 = std::min(x0 + 1, static_cast<int>(map_width) - 1);
  const int y1 = std::min(y0 + 1, static_cast<int>(map_height) - 1);
  const double fx = std::clamp(mx - std::floor(mx), 0.0, 1.0);
  const double fy = std::clamp(my - std::floor(my), 0.0, 1.0);
  const auto at = [&](int px, int py) {
    return map[(static_cast<size_t>(py) * map_width + static_cast<size_t>(px)) * map_channels + actual_channel] / 65535.0;
  };
  return (at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx) * (1.0 - fy) +
         (at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx) * fy;
}

struct JhgmView {
  const uint8_t* metadata = nullptr;
  size_t metadata_size = 0;
  const uint8_t* gain_map = nullptr;
  size_t gain_map_size = 0;
};

uint32_t read_u32_at(const uint8_t* value) {
  return (static_cast<uint32_t>(value[0]) << 24) |
         (static_cast<uint32_t>(value[1]) << 16) |
         (static_cast<uint32_t>(value[2]) << 8) | value[3];
}

JhgmView find_jhgm(const std::vector<uint8_t>& file) {
  if (file.size() < 12 || std::memcmp(file.data() + 4, "JXL ", 4) != 0) return {};
  size_t offset = 12;
  while (offset + 8 <= file.size()) {
    uint64_t box_size = read_u32_at(file.data() + offset);
    const uint8_t* type = file.data() + offset + 4;
    size_t header = 8;
    if (box_size == 1) {
      if (offset + 16 > file.size()) throw std::runtime_error("truncated JPEG XL extended box");
      box_size = (static_cast<uint64_t>(read_u32_at(file.data() + offset + 8)) << 32) |
                 read_u32_at(file.data() + offset + 12);
      header = 16;
    } else if (box_size == 0) {
      box_size = file.size() - offset;
    }
    if (box_size < header || box_size > file.size() - offset) {
      throw std::runtime_error("invalid JPEG XL box size");
    }
    if (std::memcmp(type, "jhgm", 4) == 0) {
      const uint8_t* payload = file.data() + offset + header;
      const size_t payload_size = static_cast<size_t>(box_size - header);
      if (payload_size < 8 || payload[0] != 0) {
        throw std::runtime_error("unsupported JPEG XL gain-map bundle version");
      }
      const size_t metadata_size = (static_cast<size_t>(payload[1]) << 8) | payload[2];
      size_t cursor = 3;
      if (metadata_size > payload_size - cursor) throw std::runtime_error("truncated jhgm metadata");
      const uint8_t* metadata = payload + cursor;
      cursor += metadata_size;
      if (cursor >= payload_size) throw std::runtime_error("truncated jhgm color encoding");
      const size_t color_size = payload[cursor++];
      if (color_size > payload_size - cursor) throw std::runtime_error("truncated jhgm color encoding");
      cursor += color_size;
      if (payload_size - cursor < 4) throw std::runtime_error("truncated jhgm ICC size");
      const size_t icc_size = read_u32_at(payload + cursor);
      cursor += 4;
      if (icc_size > payload_size - cursor) throw std::runtime_error("truncated jhgm alternate ICC");
      cursor += icc_size;
      if (cursor >= payload_size) throw std::runtime_error("jhgm gain-map codestream is empty");
      return {metadata, metadata_size, payload + cursor, payload_size - cursor};
    }
    offset += static_cast<size_t>(box_size);
  }
  return {};
}

struct JxlRaster {
  uint32_t width = 0, height = 0, bits = 0, channels = 0;
  uint8_t orientation = 1;
  JxlColorEncoding color{};
  bool color_valid = false;
  bool exif_present = false, xmp_present = false;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  std::vector<uint16_t> pixels;
};

JxlRaster decode_jxl_raster(const uint8_t* bytes, size_t size, bool decode_pixels,
                            bool read_metadata = false) {
  auto decoder = JxlDecoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!decoder || !runner ||
      JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("cannot initialize gain-map JPEG XL decoder");
  }
  JxlResizableParallelRunnerSetThreads(runner.get(), codec_background_workers());
  const int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                     (decode_pixels ? JXL_DEC_FULL_IMAGE : 0) |
                     (read_metadata ? JXL_DEC_BOX : 0);
  if ((read_metadata && JxlDecoderSetDecompressBoxes(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS) ||
      JxlDecoderSubscribeEvents(decoder.get(), events) != JXL_DEC_SUCCESS ||
      JxlDecoderSetInput(decoder.get(), bytes, size) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("cannot initialize embedded JPEG XL decode");
  }
  JxlDecoderCloseInput(decoder.get());
  JxlRaster output;
  JxlMetadataBoxes metadata(decoder.get(), output.exif, output.xmp);
  JxlPixelFormat format{};
  for (;;) {
    const auto status = JxlDecoderProcessInput(decoder.get());
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
      throw std::runtime_error("embedded JPEG XL decode failed");
    }
    if (status == JXL_DEC_BASIC_INFO) {
      JxlBasicInfo info{};
      if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS) {
        throw std::runtime_error("cannot read embedded JPEG XL info");
      }
      output.width = info.xsize; output.height = info.ysize; output.bits = info.bits_per_sample;
      output.channels = info.num_color_channels == 1 ? 1u : 3u;
      output.orientation = static_cast<uint8_t>(info.orientation);
      format = {output.channels, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      output.color_valid = JxlDecoderGetColorAsEncodedProfile(
          decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &output.color) == JXL_DEC_SUCCESS;
      if (read_metadata && decode_pixels && output.color_valid) {
        JxlColorEncoding decoded_color{};
        if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_DATA,
                                               &decoded_color) != JXL_DEC_SUCCESS ||
            decoded_color.color_space != output.color.color_space ||
            decoded_color.white_point != output.color.white_point ||
            decoded_color.primaries != output.color.primaries ||
            decoded_color.transfer_function != output.color.transfer_function) {
          throw std::runtime_error("jhgm decoded RGB does not match the structured base color");
        }
      }
    } else if (status == JXL_DEC_BOX) {
      metadata.start_box();
    } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
      metadata.continue_box();
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t byte_count = 0;
      if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &byte_count) != JXL_DEC_SUCCESS) {
        throw std::runtime_error("cannot size embedded JPEG XL raster");
      }
      output.pixels.resize((byte_count + 1u) / 2u);
      if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, output.pixels.data(), byte_count) != JXL_DEC_SUCCESS) {
        throw std::runtime_error("cannot set embedded JPEG XL raster buffer");
      }
    } else if (status == JXL_DEC_SUCCESS) {
      metadata.finish_box();
      break;
    }
  }
  output.exif_present = metadata.exif_present;
  output.xmp_present = metadata.xmp_present;
  return output;
}

double jxl_base_to_linear(double value, uint16_t transfer) {
  if (transfer == JXL_TRANSFER_FUNCTION_709) {
    return value < 0.081 ? value / 4.5 : std::pow((value + 0.099) / 1.099, 1.0 / 0.45);
  }
  return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

void validate_jxl_hdr_base_color(const JxlRaster& base) {
  if (!base.color_valid || base.channels != 3 || base.color.color_space != JXL_COLOR_SPACE_RGB ||
      base.color.white_point != JXL_WHITE_POINT_D65 ||
      (base.color.transfer_function != JXL_TRANSFER_FUNCTION_SRGB &&
       base.color.transfer_function != JXL_TRANSFER_FUNCTION_709) ||
      (base.color.primaries != JXL_PRIMARIES_P3 &&
       base.color.primaries != JXL_PRIMARIES_SRGB &&
       base.color.primaries != JXL_PRIMARIES_2100)) {
    throw std::runtime_error("unsupported jhgm SDR base color encoding");
  }
}

JxlRaster read_jxl_basic_info(const uint8_t* bytes, size_t size) {
  auto decoder = JxlDecoderMake(nullptr);
  if (!decoder || JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS ||
      JxlDecoderSetInput(decoder.get(), bytes, size) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("cannot inspect JPEG XL basic info");
  }
  JxlDecoderCloseInput(decoder.get());
  JxlBasicInfo basic{};
  for (;;) {
    const auto status = JxlDecoderProcessInput(decoder.get());
    if (status == JXL_DEC_BASIC_INFO) {
      if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS) {
        throw std::runtime_error("cannot read JPEG XL basic info");
      }
      break;
    }
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT || status == JXL_DEC_SUCCESS) {
      throw std::runtime_error("JPEG XL basic info is missing");
    }
  }
  JxlRaster output;
  output.width = basic.xsize;
  output.height = basic.ysize;
  output.bits = basic.bits_per_sample;
  output.channels = basic.num_color_channels == 1 ? 1u : 3u;
  output.orientation = static_cast<uint8_t>(basic.orientation);
  return output;
}

SourceInfo describe_jxl_gainmap(const std::filesystem::path& path, const IsoGainMapMetadata& metadata,
                                const JxlRaster& base, const JxlRaster& map) {
  SourceInfo info;
  info.path = path; info.format = "JPEG XL"; info.container_brand = "JXL / jhgm";
  info.asset_kind = "gain-map-hdr"; info.gain_map_present = true;
  info.gain_map_family = "iso-jxl-jhgm";
  info.codec = "JPEG XL SDR base + JPEG XL gain map + jhgm";
  info.profile = "ISO 21496-1 gain-map HDR reconstruction";
  info.color_signal_kind = base.color_valid
      ? "JPEG XL structured base color + jhgm ISO metadata"
      : "JPEG XL base + jhgm ISO metadata";
  info.width = info.base_width = base.width; info.height = info.base_height = base.height;
  info.bit_depth = info.base_bit_depth = base.bits; info.base_channels = base.channels;
  info.base_codec = "JPEG XL";
  if (base.color_valid && base.color.color_space == JXL_COLOR_SPACE_RGB) {
    info.base_color_space = base.color.primaries == JXL_PRIMARIES_SRGB ? "Rec.709" :
                            base.color.primaries == JXL_PRIMARIES_2100 ? "Rec.2020" :
                            base.color.primaries == JXL_PRIMARIES_P3 ? "Display P3" : "Custom ICC / RGB";
    info.native_color_present = true;
    info.native_color_description = "JPEG XL structured base color (CICP equivalent)";
    info.native_primaries = info.primaries = base.color.primaries == JXL_PRIMARIES_P3 ?
        12 : static_cast<uint16_t>(base.color.primaries);
    info.native_transfer = info.transfer = static_cast<uint16_t>(base.color.transfer_function);
    info.native_matrix = info.matrix = 0;
    info.native_full_range = info.full_range = true;
    info.native_range_known = info.range_known = true;
    info.resolved_signaling_source = "Native";
    info.base_transfer = base.color.transfer_function == JXL_TRANSFER_FUNCTION_709 ? "BT.709" :
                         base.color.transfer_function == JXL_TRANSFER_FUNCTION_SRGB ? "sRGB" : "Custom transfer";
  } else {
    info.base_color_space = "ICC / unspecified";
    info.base_transfer = "Unspecified";
  }
  info.gain_map_width = map.width; info.gain_map_height = map.height;
  info.gain_map_channels = map.channels;
  info.gain_map_scale_x = map.width ? base.width / static_cast<double>(map.width) : 0.0;
  info.gain_map_scale_y = map.height ? base.height / static_cast<double>(map.height) : 0.0;
  info.gain_map_uses_base_color_space = metadata.use_base_color_space;
  info.base_hdr_headroom = metadata.base_hdr_headroom;
  info.alternate_hdr_headroom = metadata.alternate_hdr_headroom;
  info.hdr_capacity_min = std::exp2(metadata.base_hdr_headroom);
  info.hdr_capacity_max = std::exp2(metadata.alternate_hdr_headroom);
  info.gain_map_min = metadata.minimum; info.gain_map_max = metadata.maximum;
  info.gain_map_gamma = metadata.gamma; info.base_offset = metadata.base_offset;
  info.alternate_offset = metadata.alternate_offset;
  info.pixel_format = "RGB" + std::to_string(base.bits) +
      " SDR base + " + std::to_string(map.channels) + "-channel gain map";
  info.chroma = "4:4:4 RGB";
  info.original_orientation = base.orientation; info.orientation_status = "present";
  info.exif_present = base.exif_present;
  info.xmp_present = base.xmp_present;
  info.exif_status = base.exif_present ? "present" : "absent";
  info.xmp_status = base.xmp_present ? "present" : "absent";
  info.icc_status = "absent";
  return info;
}

}  // namespace

bool is_iso_gainmap_jxl(const std::filesystem::path& path) {
  try { return find_jhgm(read_file(path)).gain_map != nullptr; }
  catch (...) { return false; }
}

SourceInfo inspect_iso_gainmap_jxl(const std::filesystem::path& path) {
  const auto file = read_file(path);
  const auto bundle = find_jhgm(file);
  if (!bundle.gain_map) throw std::runtime_error("JPEG XL does not contain a jhgm gain map");
  const auto metadata = parse_iso_gain_map_metadata(bundle.metadata, bundle.metadata_size);
  const auto base = decode_jxl_raster(file.data(), file.size(), false, true);
  const auto map = decode_jxl_raster(bundle.gain_map, bundle.gain_map_size, false);
  return describe_jxl_gainmap(path, metadata, base, map);
}

ReconstructedHdr reconstruct_iso_gainmap_jxl(const std::filesystem::path& path,
                                              std::atomic_bool* cancel) {
  const auto begin = Clock::now();
  const auto file = read_file(path);
  const auto bundle = find_jhgm(file);
  if (!bundle.gain_map) throw std::runtime_error("JPEG XL does not contain a jhgm gain map");
  const auto metadata = parse_iso_gain_map_metadata(bundle.metadata, bundle.metadata_size);
  auto base = decode_jxl_raster(file.data(), file.size(), true, true);
  validate_jxl_hdr_base_color(base);
  const auto map = decode_jxl_raster(bundle.gain_map, bundle.gain_map_size, true);
  ReconstructedHdr output;
  output.info = describe_jxl_gainmap(path, metadata, base, map);
  output.exif = std::move(base.exif);
  output.xmp = std::move(base.xmp);
  output.decode_ms = elapsed_ms(begin);
  if (!metadata.use_base_color_space || base.width == 0 || base.height == 0 ||
      base.pixels.size() != static_cast<size_t>(base.width) * base.height * 3u) {
    throw std::runtime_error("unsupported jhgm base/color relationship");
  }
  output.rec2020_pq_rgb.resize(base.pixels.size());
  const double full_headroom = std::max(metadata.base_hdr_headroom, metadata.alternate_hdr_headroom);
  double weight = metadata.alternate_hdr_headroom == metadata.base_hdr_headroom ? 0.0 :
      std::clamp((full_headroom - metadata.base_hdr_headroom) /
                 (metadata.alternate_hdr_headroom - metadata.base_hdr_headroom), 0.0, 1.0);
  if (metadata.alternate_hdr_headroom < metadata.base_hdr_headroom) weight = -weight;
  double max_nits = 0.0; long double sum_nits = 0.0;
  const auto reconstruction = Clock::now();
  const uint16_t base_transfer = output.info.native_transfer;
  const uint16_t base_primaries = output.info.native_primaries;
  for (uint32_t y = 0; y < base.height; ++y) {
    if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
    for (uint32_t x = 0; x < base.width; ++x) {
      const size_t pixel = static_cast<size_t>(y) * base.width + x;
      std::array<double, 3> base_linear_rgb{};
      for (size_t channel = 0; channel < 3; ++channel) {
        const double base_linear = jxl_base_to_linear(base.pixels[pixel * 3u + channel] / 65535.0,
                                                       base_transfer);
        const double encoded_map = sample_jxl_map_bilinear(
            map.pixels, map.width, map.height, map.channels,
            base.width, base.height, x, y, channel);
        const double log_gain = metadata.minimum[channel] +
            (metadata.maximum[channel] - metadata.minimum[channel]) *
            std::pow(encoded_map, 1.0 / metadata.gamma[channel]);
        base_linear_rgb[channel] = (base_linear + metadata.base_offset[channel]) *
                          std::exp2(log_gain * weight) - metadata.alternate_offset[channel];
      }
      const auto rec2020 = rec2020_from_gainmap_base(base_linear_rgb, base_primaries);
      const double pixel_max = std::max({rec2020[0], rec2020[1], rec2020[2], 0.0}) * 203.0;
      max_nits = std::max(max_nits, pixel_max); sum_nits += pixel_max;
      for (size_t channel = 0; channel < 3; ++channel) {
        output.rec2020_pq_rgb[pixel * 3u + channel] = static_cast<uint16_t>(std::llround(
            forward_pq(std::max(rec2020[channel], 0.0) * 203.0) * 65535.0));
      }
    }
  }
  output.reconstruction_ms = elapsed_ms(reconstruction);
  output.max_cll = static_cast<uint16_t>(std::clamp(std::llround(max_nits), 0ll, 65535ll));
  output.max_pall = static_cast<uint16_t>(std::clamp(std::llround(
      static_cast<double>(sum_nits / (static_cast<uint64_t>(base.width) * base.height))), 0ll, 65535ll));
  const auto oriented = orientation::normalize_rgb16(
      output.rec2020_pq_rgb, base.width, base.height, base.orientation);
  output.info.width = oriented.width; output.info.height = oriented.height;
  output.info.orientation_normalized = true; output.info.primaries = 9;
  output.info.transfer = 16; output.info.matrix = 0; output.info.full_range = true;
  output.info.range_known = true;
  orientation::set_exif_orientation_to_one(output.exif);
  orientation::set_xmp_orientation_to_one(output.xmp);
  return output;
}

GainMapRaster extract_jxl_gainmap(const std::filesystem::path& path,
                                  std::atomic_bool* cancel,
                                  bool decode_pixels) {
  const auto begin = Clock::now();
  const auto file = read_file(path);
  const auto bundle = find_jhgm(file);
  if (!bundle.gain_map) throw std::runtime_error("JPEG XL does not contain a jhgm gain map");
  if (!decode_pixels) {
    const auto basic = read_jxl_basic_info(bundle.gain_map, bundle.gain_map_size);
    GainMapRaster result;
    result.info.path = path;
    result.info.format = "JPEG XL";
    result.info.asset_kind = "gain-map-hdr";
    result.width = basic.width;
    result.height = basic.height;
    result.channels = basic.channels;
    result.bit_depth = basic.bits;
    result.encoding = "JPEG XL jhgm gain-map codestream";
    result.original_bytes.assign(bundle.gain_map, bundle.gain_map + bundle.gain_map_size);
    result.original_extension = ".jxl";
    result.decode_ms = elapsed_ms(begin);
    return result;
  }
  auto map = decode_jxl_raster(bundle.gain_map, bundle.gain_map_size, true);
  const auto base = read_jxl_basic_info(file.data(), file.size());
  const auto metadata = parse_iso_gain_map_metadata(bundle.metadata, bundle.metadata_size);
  auto info = describe_jxl_gainmap(path, metadata, base, map);
  auto result = make_raster(std::move(info), map.width, map.height, map.channels,
                            std::move(map.pixels), "JPEG XL jhgm gain-map codestream", cancel);
  result.original_bytes.assign(bundle.gain_map, bundle.gain_map + bundle.gain_map_size);
  result.original_extension = ".jxl";
  normalize_gainmap_orientation(result, result.info.original_orientation);
  result.decode_ms = elapsed_ms(begin);
  return result;
}

}  // namespace hdrbridge::gainmap
