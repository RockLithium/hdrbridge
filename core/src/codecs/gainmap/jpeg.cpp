#include "codecs/gainmap/jpeg.h"
#include "codecs/gainmap/decode_support.h"
#include "codecs/gainmap/extract_support.h"
#include "codecs/gainmap/icc_description.h"
#include "codecs/gainmap/reconstruction_math.h"
#include "codecs/jpeg_probe.h"
#include "image/orientation.h"
#include <ultrahdr_api.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

#if defined(_WIN32) || defined(__ANDROID__)
#include <jpeglib.h>
#include <setjmp.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace hdrbridge::gainmap {

using namespace detail;

namespace {

struct UhdrDecoderDeleter {
  void operator()(uhdr_codec_private_t* value) const {
    if (value) uhdr_release_decoder(value);
  }
};
using UhdrDecoderPtr = std::unique_ptr<uhdr_codec_private_t, UhdrDecoderDeleter>;

bool contains_ascii(const std::vector<uint8_t>& bytes, const char* text) {
  const size_t length = std::strlen(text);
  return length > 0 && std::search(bytes.begin(), bytes.end(), text, text + length) != bytes.end();
}

void require_uhdr(uhdr_error_info_t status, const char* operation) {
  if (status.error_code == UHDR_CODEC_OK) return;
  std::string message(operation);
  if (status.has_detail && status.detail[0]) {
    message += ": ";
    message += status.detail;
  }
  throw std::runtime_error(message);
}

float half_to_float_local(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16u;
  uint32_t exponent = (half >> 10u) & 0x1fu;
  uint32_t mantissa = half & 0x03ffu;
  uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      int shift = 0;
      while ((mantissa & 0x0400u) == 0) { mantissa <<= 1u; ++shift; }
      mantissa &= 0x03ffu;
      bits = sign | static_cast<uint32_t>(127 - 15 - shift) << 23u | mantissa << 13u;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | mantissa << 13u;
  } else {
    bits = sign | (exponent + 112u) << 23u | mantissa << 13u;
  }
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::vector<uint8_t> standalone_gainmap_jpeg(const uint8_t* data, size_t size) {
  if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) {
    throw std::runtime_error("embedded gain-map JPEG is invalid");
  }
  std::vector<uint8_t> output{0xff, 0xd8};
  size_t cursor = 2;
  while (cursor < size) {
    const size_t marker_start = cursor;
    if (data[cursor++] != 0xff) throw std::runtime_error("invalid gain-map JPEG marker stream");
    while (cursor < size && data[cursor] == 0xff) ++cursor;
    if (cursor >= size) throw std::runtime_error("truncated gain-map JPEG marker");
    const uint8_t marker = data[cursor++];
    if (marker == 0xda) {
      output.insert(output.end(), data + marker_start, data + size);
      return output;
    }
    if (marker == 0xd9) {
      output.push_back(0xff); output.push_back(0xd9);
      return output;
    }
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) {
      output.insert(output.end(), data + marker_start, data + cursor);
      continue;
    }
    if (cursor + 2 > size) throw std::runtime_error("truncated gain-map JPEG segment");
    const size_t segment_size = (static_cast<size_t>(data[cursor]) << 8) | data[cursor + 1];
    if (segment_size < 2 || segment_size > size - cursor) {
      throw std::runtime_error("invalid gain-map JPEG segment size");
    }
    const size_t segment_end = cursor + segment_size;
    // APP1/APP2 and the other private APP segments describe the parent
    // MPF/Ultra-HDR relationship. They are invalid or misleading on the
    // detached auxiliary JPEG. Keep only ordinary JFIF and Adobe markers.
    const bool private_metadata = (marker >= 0xe1 && marker <= 0xed) ||
                                  marker == 0xef || marker == 0xfe;
    if (!private_metadata) {
      output.insert(output.end(), data + marker_start, data + segment_end);
    }
    cursor = segment_end;
  }
  throw std::runtime_error("gain-map JPEG ended before image data");
}

#if defined(_WIN32) || defined(__ANDROID__)
std::vector<uint16_t> decode_jpeg_rgb16(const uint8_t* bytes, size_t size,
                                        uint32_t& width, uint32_t& height,
                                        std::atomic_bool* cancel) {
  if (!bytes || size == 0 || size > std::numeric_limits<unsigned long>::max()) {
    throw std::runtime_error("embedded gain-map JPEG is empty or too large");
  }
  struct ErrorManager {
    jpeg_error_mgr base{};
    jmp_buf jump{};
    char message[JMSG_LENGTH_MAX]{};
  } error;
  jpeg_decompress_struct decoder{};
  decoder.err = jpeg_std_error(&error.base);
  error.base.error_exit = [](j_common_ptr common) {
    auto* manager = reinterpret_cast<ErrorManager*>(common->err);
    (*common->err->format_message)(common, manager->message);
    longjmp(manager->jump, 1);
  };
  if (setjmp(error.jump)) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error(std::string("embedded gain-map JPEG decode failed: ") + error.message);
  }
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, const_cast<unsigned char*>(bytes), static_cast<unsigned long>(size));
  if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK || !jpeg_start_decompress(&decoder)) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("embedded gain-map JPEG header is invalid");
  }
  width = decoder.output_width;
  height = decoder.output_height;
  const int components = decoder.output_components;
  if (!width || !height || (components != 1 && components != 3 && components != 4)) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("embedded gain-map JPEG has an unsupported pixel layout");
  }
  std::vector<uint8_t> row(static_cast<size_t>(width) * components);
  std::vector<uint16_t> samples(static_cast<size_t>(width) * height * 3u);
  while (decoder.output_scanline < decoder.output_height) {
    check_gainmap_cancel(cancel);
    JSAMPROW row_ptr = row.data();
    jpeg_read_scanlines(&decoder, &row_ptr, 1);
    const uint32_t y = decoder.output_scanline - 1u;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t value_r = row[static_cast<size_t>(x) * components];
      const uint8_t value_g = components == 1 ? value_r : row[static_cast<size_t>(x) * components + 1u];
      const uint8_t value_b = components == 1 ? value_r : row[static_cast<size_t>(x) * components + 2u];
      const size_t p = (static_cast<size_t>(y) * width + x) * 3u;
      samples[p] = static_cast<uint16_t>(value_r) * 257u;
      samples[p + 1u] = static_cast<uint16_t>(value_g) * 257u;
      samples[p + 2u] = static_cast<uint16_t>(value_b) * 257u;
    }
  }
  jpeg_finish_decompress(&decoder);
  jpeg_destroy_decompress(&decoder);
  return samples;
}
#endif

}  // namespace

AppleAuxiliaryProbe probe_apple_jpeg(const std::filesystem::path& path) {
  AppleAuxiliaryProbe probe;
  probe.container = "JPEG";
  const auto bytes = read_file(path);
  const bool jpeg = bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8;
  const bool apple_marker = contains_ascii(bytes, kAppleHdrGainMapUrn) ||
                            contains_ascii(bytes, "HDRGainMapVersion") ||
                            contains_ascii(bytes, "HDRGainMapHeadroom");
  const bool multi_picture = contains_ascii(bytes, "MPF\0");
  probe.detected = jpeg && apple_marker && multi_picture;
  if (probe.detected) {
    probe.auxiliary_type = kAppleHdrGainMapUrn;
    probe.extraction_supported = false;
  }
  return probe;
}

SourceInfo inspect_apple_jpeg_gainmap(const std::filesystem::path& path) {
  if (!probe_apple_jpeg(path).detected) {
    throw std::runtime_error("Apple HDR JPEG MPF/gain map was not found");
  }
  const auto bytes = read_file(path);
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data());
  input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create Apple HDR JPEG inspector");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set Apple HDR JPEG inspection input");
  require_uhdr(uhdr_dec_probe(decoder.get()), "inspect Apple JPEG MPF/ISO gain-map metadata");
  uhdr_mem_block_t* base = uhdr_dec_get_base_image(decoder.get());
  uhdr_mem_block_t* gain = uhdr_dec_get_gainmap_image(decoder.get());
  uhdr_gainmap_metadata_t* metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  if (!base || !base->data_sz || !gain || !gain->data_sz || !metadata) {
    throw std::runtime_error("Apple JPEG did not expose base, gain map and ISO metadata");
  }
  SourceInfo info;
  info.path = path;
  info.format = "Apple HDR JPEG";
  info.container_brand = "JPEG/MPF";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "apple-mpf-iso";
  info.auxiliary_type = kAppleHdrGainMapUrn;
  info.width = static_cast<uint32_t>(uhdr_dec_get_image_width(decoder.get()));
  info.height = static_cast<uint32_t>(uhdr_dec_get_image_height(decoder.get()));
  info.gain_map_width = static_cast<uint32_t>(uhdr_dec_get_gainmap_width(decoder.get()));
  info.gain_map_height = static_cast<uint32_t>(uhdr_dec_get_gainmap_height(decoder.get()));
  const auto base_probe = jpeg::inspect(base->data, base->data_sz);
  const auto gain_probe = jpeg::inspect(gain->data, gain->data_sz);
  const auto outer_probe = jpeg::inspect(bytes.data(), bytes.size());
  info.base_width = base_probe.width;
  info.base_height = base_probe.height;
  info.base_bit_depth = base_probe.bit_depth;
  info.base_channels = base_probe.channels;
  info.base_codec = "JPEG";
  info.base_color_space = "Display P3";
  info.base_transfer = "SDR gamma";
  info.gain_map_channels = gain_probe.channels;
  if (info.gain_map_width) info.gain_map_scale_x =
      info.base_width / static_cast<double>(info.gain_map_width);
  if (info.gain_map_height) info.gain_map_scale_y =
      info.base_height / static_cast<double>(info.gain_map_height);
  info.xmp_present = outer_probe.xmp_present || base_probe.xmp_present;
  info.icc_present = outer_probe.icc_present || base_probe.icc_present;
  info.codec = "JPEG SDR base + MPF JPEG gain map + ISO 21496-1 metadata";
  info.color_signal_kind = "JPEG ICC / MPF + ISO gain-map metadata";
  info.pixel_format = "JPEG SDR base + JPEG gain map";
  info.profile = "Apple Display P3 SDR base + ISO gain-map HDR reconstruction";
  info.bit_depth = 8;
  info.chroma = "JPEG RGB base + gain map";
  info.base_hdr_headroom = std::log2(std::max(metadata->hdr_capacity_min, 1e-12f));
  info.alternate_hdr_headroom = std::log2(std::max(metadata->hdr_capacity_max, 1e-12f));
  info.hdr_capacity_min = metadata->hdr_capacity_min;
  info.hdr_capacity_max = metadata->hdr_capacity_max;
  info.gain_map_uses_base_color_space = metadata->use_base_cg != 0;
  for (size_t channel = 0; channel < 3; ++channel) {
    info.gain_map_min[channel] = std::log2(std::max(metadata->min_content_boost[channel], 1e-12f));
    info.gain_map_max[channel] = std::log2(std::max(metadata->max_content_boost[channel], 1e-12f));
    info.gain_map_gamma[channel] = metadata->gamma[channel];
    info.base_offset[channel] = metadata->offset_sdr[channel];
    info.alternate_offset[channel] = metadata->offset_hdr[channel];
  }
  if (auto* exif = uhdr_dec_get_exif(decoder.get()); exif && exif->data && exif->data_sz) {
    const auto* first = static_cast<const uint8_t*>(exif->data);
    std::vector<uint8_t> exif_bytes(first, first + exif->data_sz);
    info.exif_present = true;
    info.original_orientation = orientation::read_exif_orientation(exif_bytes);
  }
  if (info.original_orientation >= 5) {
    std::swap(info.width, info.height);
    std::swap(info.base_width, info.base_height);
    if (info.gain_map_width) info.gain_map_scale_x =
        info.base_width / static_cast<double>(info.gain_map_width);
    if (info.gain_map_height) info.gain_map_scale_y =
        info.base_height / static_cast<double>(info.gain_map_height);
  }
  info.orientation_normalized = true;
  info.primaries = 12;
  info.transfer = 13;
  info.matrix = 0;
  info.full_range = true;
  info.range_known = true;
  return info;
}

ReconstructedHdr reconstruct_apple_jpeg_gainmap(
    const std::filesystem::path& path, std::atomic_bool* cancel) {
  if (!probe_apple_jpeg(path).detected) {
    throw std::runtime_error("Apple HDR JPEG MPF/gain map was not found");
  }
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  const auto decode_start = Clock::now();
  const auto bytes = read_file(path);
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data());
  input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create Apple HDR JPEG decoder");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set Apple HDR JPEG input");
  require_uhdr(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat),
               "set Apple HDR JPEG high-precision output");
  require_uhdr(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR),
               "set Apple HDR JPEG linear output");
  require_uhdr(uhdr_dec_set_out_max_display_boost(
                   decoder.get(), static_cast<float>(10000.0 / 203.0)),
               "request complete Apple JPEG gain-map headroom");
  require_uhdr(uhdr_dec_probe(decoder.get()), "parse Apple JPEG MPF/ISO gain-map metadata");
  uhdr_mem_block_t* base = uhdr_dec_get_base_image(decoder.get());
  uhdr_mem_block_t* gain = uhdr_dec_get_gainmap_image(decoder.get());
  uhdr_gainmap_metadata_t* metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  if (!base || !base->data_sz || !gain || !gain->data_sz || !metadata) {
    throw std::runtime_error("Apple JPEG did not expose base, gain map and ISO metadata");
  }
  ReconstructedHdr output;
  auto& info = output.info;
  info.path = path;
  info.format = "Apple HDR JPEG";
  info.container_brand = "JPEG/MPF";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "apple-mpf-iso";
  info.auxiliary_type = kAppleHdrGainMapUrn;
  info.width = static_cast<uint32_t>(uhdr_dec_get_image_width(decoder.get()));
  info.height = static_cast<uint32_t>(uhdr_dec_get_image_height(decoder.get()));
  info.gain_map_width = static_cast<uint32_t>(uhdr_dec_get_gainmap_width(decoder.get()));
  info.gain_map_height = static_cast<uint32_t>(uhdr_dec_get_gainmap_height(decoder.get()));
  const auto base_probe = jpeg::inspect(base->data, base->data_sz);
  const auto gain_probe = jpeg::inspect(gain->data, gain->data_sz);
  const auto outer_probe = jpeg::inspect(bytes.data(), bytes.size());
  info.base_width = base_probe.width;
  info.base_height = base_probe.height;
  info.base_bit_depth = base_probe.bit_depth;
  info.base_channels = base_probe.channels;
  info.base_codec = "JPEG";
  info.base_color_space = "Display P3";
  info.base_transfer = "SDR gamma";
  info.gain_map_channels = gain_probe.channels;
  if (info.gain_map_width) info.gain_map_scale_x =
      info.base_width / static_cast<double>(info.gain_map_width);
  if (info.gain_map_height) info.gain_map_scale_y =
      info.base_height / static_cast<double>(info.gain_map_height);
  info.xmp_present = outer_probe.xmp_present || base_probe.xmp_present;
  info.codec = "JPEG SDR base + MPF JPEG gain map + ISO 21496-1 metadata";
  info.profile = "Apple Display P3 SDR base + ISO gain-map HDR reconstruction";
  info.bit_depth = 8;
  info.chroma = "JPEG RGB base + gain map";
  info.base_hdr_headroom = std::log2(std::max(metadata->hdr_capacity_min, 1e-12f));
  info.alternate_hdr_headroom = std::log2(std::max(metadata->hdr_capacity_max, 1e-12f));
  info.hdr_capacity_min = metadata->hdr_capacity_min;
  info.hdr_capacity_max = metadata->hdr_capacity_max;
  info.gain_map_uses_base_color_space = metadata->use_base_cg != 0;
  for (size_t channel = 0; channel < 3; ++channel) {
    info.gain_map_min[channel] = std::log2(std::max(metadata->min_content_boost[channel], 1e-12f));
    info.gain_map_max[channel] = std::log2(std::max(metadata->max_content_boost[channel], 1e-12f));
    info.gain_map_gamma[channel] = metadata->gamma[channel];
    info.base_offset[channel] = metadata->offset_sdr[channel];
    info.alternate_offset[channel] = metadata->offset_hdr[channel];
  }
  if (!outer_probe.exif.empty()) {
    output.exif = outer_probe.exif;
    info.exif_present = true;
  } else if (auto* exif = uhdr_dec_get_exif(decoder.get()); exif && exif->data && exif->data_sz) {
    const auto* first = static_cast<const uint8_t*>(exif->data);
    output.exif.assign(first, first + exif->data_sz);
    info.exif_present = true;
  }
  info.original_orientation = orientation::read_exif_orientation(output.exif);
  // iOS 27+ Adaptive HDR embeds an SMPTE ST 2094-50 HAGC tag whose reference
  // white defaults to 203 nits or carries a custom value; legacy iOS 18 JPEG
  // reconstruction anchors libultrahdr's linear output at 203 nits.
  const double reference_white_nits =
      jpeg_adaptive_hdr_icc_reference_white_nits(
          static_cast<const uint8_t*>(base->data), base->data_sz,
          static_cast<const uint8_t*>(gain->data), gain->data_sz,
          /*allow_lumi=*/false)
          .value_or(203.0);
  require_uhdr(uhdr_decode(decoder.get()), "reconstruct Apple HDR JPEG");
  uhdr_raw_image_t* image = uhdr_get_decoded_image(decoder.get());
  if (!image || image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat ||
      !image->planes[UHDR_PLANE_PACKED]) {
    throw std::runtime_error("Apple HDR JPEG reconstruction did not produce linear FP16 RGB");
  }
  output.decode_ms = elapsed_ms(decode_start);
  const auto reconstruction_start = Clock::now();
  output.rec2020_pq_rgb.resize(static_cast<size_t>(image->w) * image->h * 3u);
  const auto* pixels = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  struct JpegWorkerStats {
    double maximum_nits = 0.0;
    long double sum_max_nits = 0.0;
  };
  const uint32_t worker_count = std::min<uint32_t>(codec_worker_count(), image->h);
  std::vector<JpegWorkerStats> worker_stats(worker_count);
  std::atomic<uint32_t> next_row{0};
  const auto convert_rows = [&](uint32_t worker_index) {
    auto& stats = worker_stats[worker_index];
    while (true) {
      const uint32_t row_begin = next_row.fetch_add(8u);
      if (row_begin >= image->h) break;
      const uint32_t row_end = std::min(row_begin + 8u, image->h);
      for (uint32_t y = row_begin; y < row_end; ++y) {
        if (cancel && cancel->load()) return;
        const auto* row = pixels +
            static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] * 4u;
        for (uint32_t x = 0; x < image->w; ++x) {
          const size_t pixel = static_cast<size_t>(y) * image->w + x;
          const std::array<double, 3> linear{
              half_to_float_local(row[static_cast<size_t>(x) * 4u]) * reference_white_nits,
              half_to_float_local(row[static_cast<size_t>(x) * 4u + 1u]) * reference_white_nits,
              half_to_float_local(row[static_cast<size_t>(x) * 4u + 2u]) * reference_white_nits};
          std::array<double, 3> rec2020{};
          if (image->cg == UHDR_CG_BT_2100) {
            rec2020 = linear;
          } else if (image->cg == UHDR_CG_DISPLAY_P3) {
            rec2020 = {0.7538330 * linear[0] + 0.1985974 * linear[1] + 0.0475696 * linear[2],
                       0.0457438 * linear[0] + 0.9417772 * linear[1] + 0.0124789 * linear[2],
                      -0.0012103 * linear[0] + 0.0176017 * linear[1] + 0.9836086 * linear[2]};
          } else {
            rec2020 = {0.6274040 * linear[0] + 0.3292830 * linear[1] + 0.0433130 * linear[2],
                       0.0690970 * linear[0] + 0.9195400 * linear[1] + 0.0113620 * linear[2],
                       0.0163910 * linear[0] + 0.0880130 * linear[1] + 0.8955950 * linear[2]};
          }
          const double pixel_max = std::max({rec2020[0], rec2020[1], rec2020[2], 0.0});
          stats.maximum_nits = std::max(stats.maximum_nits, pixel_max);
          stats.sum_max_nits += pixel_max;
          for (size_t channel = 0; channel < 3; ++channel) {
            output.rec2020_pq_rgb[pixel * 3u + channel] =
                static_cast<uint16_t>(std::llround(forward_pq(
                    std::clamp(rec2020[channel], 0.0, 10000.0)) * 65535.0));
          }
        }
      }
    }
  };
  std::vector<std::thread> workers;
  workers.reserve(worker_count > 0 ? worker_count - 1u : 0u);
  for (uint32_t worker = 1; worker < worker_count; ++worker) {
    workers.emplace_back(convert_rows, worker);
  }
  convert_rows(0);
  for (auto& worker : workers) worker.join();
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  double maximum_nits = 0.0;
  long double sum_max_nits = 0.0;
  for (const auto& stats : worker_stats) {
    maximum_nits = std::max(maximum_nits, stats.maximum_nits);
    sum_max_nits += stats.sum_max_nits;
  }
  output.max_cll = static_cast<uint16_t>(std::clamp(std::llround(maximum_nits), 0ll, 65535ll));
  output.max_pall = static_cast<uint16_t>(std::clamp(std::llround(
      static_cast<double>(sum_max_nits /
          static_cast<long double>(static_cast<uint64_t>(image->w) * image->h))), 0ll, 65535ll));
  output.reconstruction_ms = elapsed_ms(reconstruction_start);
  output.color_conversion_ms = output.reconstruction_ms;
  const auto orientation_start = Clock::now();
  const auto transformed = orientation::normalize_rgb16(
      output.rec2020_pq_rgb, image->w, image->h, info.original_orientation);
  info.width = transformed.width;
  info.height = transformed.height;
  info.orientation_normalized = true;
  info.primaries = 9;
  info.transfer = 16;
  info.matrix = 0;
  info.full_range = true;
  info.range_known = true;
  orientation::set_exif_orientation_to_one(output.exif);
  output.orientation_ms = elapsed_ms(orientation_start);
  return output;
}

GainMapRaster extract_jpeg_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel,
                                   bool decode_pixels) {
#if !defined(_WIN32) && !defined(__ANDROID__)
  (void)path; (void)cancel; (void)decode_pixels;
  throw std::runtime_error("embedded JPEG gain-map extraction is unavailable on this platform");
#else
  const auto begin = Clock::now();
  const auto bytes = read_file(path);
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data());
  input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create gain-map JPEG decoder");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set gain-map JPEG input");
  require_uhdr(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat),
               "set gain-map JPEG precision");
  require_uhdr(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR),
               "set gain-map JPEG transfer");
  require_uhdr(uhdr_dec_probe(decoder.get()), "parse gain-map JPEG");
  uhdr_mem_block_t* gain = uhdr_dec_get_gainmap_image(decoder.get());
  if (!gain || !gain->data || !gain->data_sz) throw std::runtime_error("gain-map JPEG image is missing");
  const auto probe = jpeg::inspect(gain->data, gain->data_sz);
  if (!decode_pixels) {
    GainMapRaster result;
    result.info.path = path;
    result.info.format = "JPEG";
    result.info.asset_kind = "gain-map-hdr";
    result.width = probe.width;
    result.height = probe.height;
    result.channels = probe.channels == 1 ? 1u : 3u;
    result.bit_depth = 8;
    result.encoding = "embedded JPEG gain-map image";
    result.original_bytes = standalone_gainmap_jpeg(
        static_cast<const uint8_t*>(gain->data), gain->data_sz);
    result.original_extension = ".jpg";
    result.decode_ms = elapsed_ms(begin);
    return result;
  }
  uint32_t width = 0, height = 0;
  auto samples = decode_jpeg_rgb16(static_cast<const uint8_t*>(gain->data), gain->data_sz,
                                   width, height, cancel);
  SourceInfo info;
  try {
    info = inspect_apple_jpeg_gainmap(path);
  } catch (...) {
    info = hdrbridge::inspect(path);
  }
  if (probe.channels == 1) {
    std::vector<uint16_t> mono(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = samples[i * 3u];
    samples = std::move(mono);
  }
  auto result = make_raster(std::move(info), width, height, probe.channels,
                            std::move(samples), "embedded JPEG gain-map image", cancel);
  result.original_bytes = standalone_gainmap_jpeg(
      static_cast<const uint8_t*>(gain->data), gain->data_sz);
  result.original_extension = ".jpg";
  normalize_gainmap_orientation(result, result.info.original_orientation);
  result.decode_ms = elapsed_ms(begin);
  return result;
#endif
}

}  // namespace hdrbridge::gainmap
