#include "codecs/gainmap/avif.h"
#include "codecs/gainmap/bmff_items.h"
#include "codecs/gainmap/decode_support.h"
#include "codecs/gainmap/extract_support.h"
#include "codecs/gainmap/reconstruction_math.h"
#include "image/orientation.h"
#include <avif/avif.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace hdrbridge::gainmap {

using namespace detail;

namespace {

struct AvifDecoderDeleter {
  void operator()(avifDecoder* value) const { avifDecoderDestroy(value); }
};
struct AvifRgbDeleter {
  void operator()(avifRGBImage* value) const {
    if (value) {
      avifRGBImageFreePixels(value);
      delete value;
    }
  }
};

using AvifDecoderPtr = std::unique_ptr<avifDecoder, AvifDecoderDeleter>;
using AvifRgbPtr = std::unique_ptr<avifRGBImage, AvifRgbDeleter>;

void require_avif(avifResult result, const avifDiagnostics& diagnostics,
                  const char* operation) {
  if (result == AVIF_RESULT_OK) return;
  std::string message(operation);
  message += ": ";
  message += avifResultToString(result);
  if (diagnostics.error[0] != '\0') {
    message += " (";
    message += diagnostics.error;
    message += ')';
  }
  throw std::runtime_error(message);
}

double fraction(avifUnsignedFraction value) {
  return value.d == 0 ? 0.0 : static_cast<double>(value.n) / value.d;
}

double fraction(avifSignedFraction value) {
  return value.d == 0 ? 0.0 : static_cast<double>(value.n) / value.d;
}

uint8_t orientation_from_avif(const avifImage* image) {
  if ((image->transformFlags & AVIF_TRANSFORM_IROT) && image->irot.angle == 1) {
    if (image->transformFlags & AVIF_TRANSFORM_IMIR) return image->imir.axis ? 7 : 5;
    return 8;
  }
  if ((image->transformFlags & AVIF_TRANSFORM_IROT) && image->irot.angle == 2) {
    if (image->transformFlags & AVIF_TRANSFORM_IMIR) return image->imir.axis ? 4 : 2;
    return 3;
  }
  if ((image->transformFlags & AVIF_TRANSFORM_IROT) && image->irot.angle == 3) {
    if (image->transformFlags & AVIF_TRANSFORM_IMIR) return image->imir.axis ? 5 : 7;
    return 6;
  }
  if (image->transformFlags & AVIF_TRANSFORM_IMIR) return image->imir.axis ? 2 : 4;
  return 1;
}

AvifDecoderPtr parse_avif(const std::vector<uint8_t>& bytes,
                          bool decode_pixels) {
  AvifDecoderPtr decoder(avifDecoderCreate());
  if (!decoder) throw std::bad_alloc();
  decoder->maxThreads = static_cast<int>(codec_worker_count());
  decoder->imageContentToDecode = AVIF_IMAGE_CONTENT_ALL;
  require_avif(avifDecoderSetIOMemory(decoder.get(), bytes.data(), bytes.size()),
               decoder->diag, "set AVIF input memory");
  require_avif(avifDecoderParse(decoder.get()), decoder->diag,
               "parse AVIF item graph and gain-map metadata");
  if (!decoder->image || !decoder->image->gainMap) {
    throw std::runtime_error("AVIF does not contain a standards-parsed tmap gain map");
  }
  if (decode_pixels) {
    require_avif(avifDecoderNextImage(decoder.get()), decoder->diag,
                 "decode AVIF base and gain-map items");
    if (!decoder->image->gainMap->image ||
        !decoder->image->gainMap->image->yuvPlanes[AVIF_CHAN_Y]) {
      throw std::runtime_error("AVIF gain-map item was parsed but not decoded");
    }
  }
  return decoder;
}

SourceInfo source_info(const std::filesystem::path& path,
                       const avifImage* image,
                       const ItemGraph& graph) {
  const avifGainMap* gain = image->gainMap;
  SourceInfo info;
  info.path = path;
  info.format = "AVIF";
  info.container_brand = "avif/tmap";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "adobe-iso-tmap";
  info.base_item_id = graph.base;
  info.gain_map_item_id = graph.gain;
  info.tone_map_item_id = graph.tone_map;
  info.width = image->width;
  info.height = image->height;
  info.base_width = image->width;
  info.base_height = image->height;
  info.base_bit_depth = image->depth;
  info.base_channels = image->yuvFormat == AVIF_PIXEL_FORMAT_YUV400 ? 1u : 3u;
  info.base_codec = "AV1";
  switch (image->colorPrimaries) {
    case AVIF_COLOR_PRIMARIES_BT709: info.base_color_space = "BT.709 / sRGB primaries"; break;
    case AVIF_COLOR_PRIMARIES_BT2020: info.base_color_space = "BT.2020"; break;
    case AVIF_COLOR_PRIMARIES_SMPTE432: info.base_color_space = "Display P3"; break;
    default: info.base_color_space = "Unspecified / other primaries"; break;
  }
  switch (image->transferCharacteristics) {
    case AVIF_TRANSFER_CHARACTERISTICS_BT709: info.base_transfer = "BT.709"; break;
    case AVIF_TRANSFER_CHARACTERISTICS_SRGB: info.base_transfer = "sRGB"; break;
    case AVIF_TRANSFER_CHARACTERISTICS_LINEAR: info.base_transfer = "Linear"; break;
    case AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084: info.base_transfer = "PQ / ST 2084"; break;
    case AVIF_TRANSFER_CHARACTERISTICS_HLG: info.base_transfer = "HLG"; break;
    default: info.base_transfer = "Unspecified / other transfer"; break;
  }
  info.gain_map_width = gain->image ? gain->image->width : 0;
  info.gain_map_height = gain->image ? gain->image->height : 0;
  info.gain_map_channels = gain->image &&
      gain->image->yuvFormat == AVIF_PIXEL_FORMAT_YUV400 ? 1u : 3u;
  if (info.gain_map_width) info.gain_map_scale_x =
      info.base_width / static_cast<double>(info.gain_map_width);
  if (info.gain_map_height) info.gain_map_scale_y =
      info.base_height / static_cast<double>(info.gain_map_height);
  info.codec = "AV1 base + AV1 gain map + tmap derived item";
  info.profile = "ISO 21496-1 gain-map HDR reconstruction";
  info.bit_depth = image->depth;
  info.chroma = avifPixelFormatToString(image->yuvFormat);
  info.pixel_format = info.chroma;
  info.primaries = static_cast<uint16_t>(image->colorPrimaries);
  info.transfer = static_cast<uint16_t>(image->transferCharacteristics);
  info.matrix = static_cast<uint16_t>(image->matrixCoefficients);
  info.full_range = image->yuvRange == AVIF_RANGE_FULL;
  info.range_known = true;
  info.native_color_present = image->colorPrimaries != AVIF_COLOR_PRIMARIES_UNSPECIFIED ||
      image->transferCharacteristics != AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED ||
      image->matrixCoefficients != AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED;
  info.color_signal_kind = "AVIF base color signal / tmap metadata";
  if (info.native_color_present) {
    info.native_color_description = "AVIF nclx / AV1 color signal";
    info.native_primaries = info.primaries;
    info.native_transfer = info.transfer;
    info.native_matrix = info.matrix;
    info.native_full_range = info.full_range;
    info.native_range_known = true;
    info.resolved_signaling_source = "Native";
  }
  info.exif_present = image->exif.size > 0;
  info.xmp_present = image->xmp.size > 0;
  info.icc_present = image->icc.size > 0;
  info.base_hdr_headroom = fraction(gain->baseHdrHeadroom);
  info.alternate_hdr_headroom = fraction(gain->alternateHdrHeadroom);
  info.hdr_capacity_min = std::exp2(info.base_hdr_headroom);
  info.hdr_capacity_max = std::exp2(info.alternate_hdr_headroom);
  info.gain_map_uses_base_color_space = gain->useBaseColorSpace != AVIF_FALSE;
  for (size_t channel = 0; channel < 3; ++channel) {
    info.gain_map_min[channel] = fraction(gain->gainMapMin[channel]);
    info.gain_map_max[channel] = fraction(gain->gainMapMax[channel]);
    info.gain_map_gamma[channel] = fraction(gain->gainMapGamma[channel]);
    info.base_offset[channel] = fraction(gain->baseOffset[channel]);
    info.alternate_offset[channel] = fraction(gain->alternateOffset[channel]);
  }
  info.original_orientation = orientation_from_avif(image);
  if (info.original_orientation >= 5) {
    std::swap(info.width, info.height);
    std::swap(info.base_width, info.base_height);
    info.gain_map_scale_x = info.base_width /
        static_cast<double>(info.gain_map_width);
    info.gain_map_scale_y = info.base_height /
        static_cast<double>(info.gain_map_height);
  }
  info.orientation_normalized = true;
  return info;
}

double independent_formula_check(const avifImage* base_image,
                                 const avifGainMap* gain,
                                 const avifRGBImage& official) {
  if (!gain->image || gain->image->width != base_image->width ||
      gain->image->height != base_image->height ||
      base_image->colorPrimaries != AVIF_COLOR_PRIMARIES_SMPTE432 ||
      base_image->transferCharacteristics != AVIF_TRANSFER_CHARACTERISTICS_SRGB ||
      !gain->useBaseColorSpace) {
    return 0.0;
  }

  avifRGBImage base{};
  avifRGBImageSetDefaults(&base, base_image);
  base.format = AVIF_RGB_FORMAT_RGB;
  if (avifRGBImageAllocatePixels(&base) != AVIF_RESULT_OK) throw std::bad_alloc();
  struct FreeRgb { avifRGBImage* image; ~FreeRgb() { avifRGBImageFreePixels(image); } } free_base{&base};
  avifResult result = avifImageYUVToRGB(base_image, &base);
  if (result != AVIF_RESULT_OK) return 0.0;

  avifRGBImage map{};
  avifRGBImageSetDefaults(&map, gain->image);
  map.format = AVIF_RGB_FORMAT_RGB;
  if (avifRGBImageAllocatePixels(&map) != AVIF_RESULT_OK) throw std::bad_alloc();
  FreeRgb free_map{&map};
  result = avifImageYUVToRGB(gain->image, &map);
  if (result != AVIF_RESULT_OK) return 0.0;

  const double base_headroom = fraction(gain->baseHdrHeadroom);
  const double alternate_headroom = fraction(gain->alternateHdrHeadroom);
  const double selected_headroom = std::max(base_headroom, alternate_headroom);
  const double f = std::clamp((selected_headroom - base_headroom) /
                              (alternate_headroom - base_headroom), 0.0, 1.0);
  const double weight = alternate_headroom < base_headroom ? -f : f;
  const uint32_t step_x = std::max(base_image->width / 31u, 1u);
  const uint32_t step_y = std::max(base_image->height / 31u, 1u);
  const double base_max = static_cast<double>((1u << base.depth) - 1u);
  const double map_max = static_cast<double>((1u << map.depth) - 1u);
  double maximum_error = 0.0;
  for (uint32_t y = 0; y < base_image->height; y += step_y) {
    const auto* base_row = reinterpret_cast<const uint16_t*>(base.pixels + static_cast<size_t>(y) * base.rowBytes);
    const auto* map_row = reinterpret_cast<const uint16_t*>(map.pixels + static_cast<size_t>(y) * map.rowBytes);
    const auto* official_row = reinterpret_cast<const uint16_t*>(official.pixels + static_cast<size_t>(y) * official.rowBytes);
    for (uint32_t x = 0; x < base_image->width; x += step_x) {
      std::array<double, 3> p3_linear{};
      for (size_t channel = 0; channel < 3; ++channel) {
        const double base_encoded = base_row[static_cast<size_t>(x) * 3u + channel] / base_max;
        const double map_encoded = map_row[static_cast<size_t>(x) * 3u + channel] / map_max;
        const double gamma = fraction(gain->gainMapGamma[channel]);
        const double encoded_log2 = fraction(gain->gainMapMin[channel]) +
            (fraction(gain->gainMapMax[channel]) - fraction(gain->gainMapMin[channel])) *
            std::pow(map_encoded, gamma > 0.0 ? 1.0 / gamma : 1.0);
        p3_linear[channel] =
            (inverse_srgb(base_encoded) + fraction(gain->baseOffset[channel])) *
                std::exp2(encoded_log2 * weight) -
            fraction(gain->alternateOffset[channel]);
      }
      const std::array<double, 3> rec2020{
          0.7538330 * p3_linear[0] + 0.1985974 * p3_linear[1] + 0.0475696 * p3_linear[2],
          0.0457438 * p3_linear[0] + 0.9417772 * p3_linear[1] + 0.0124789 * p3_linear[2],
         -0.0012103 * p3_linear[0] + 0.0176017 * p3_linear[1] + 0.9836086 * p3_linear[2]};
      for (size_t channel = 0; channel < 3; ++channel) {
        const double expected = forward_pq(std::max(rec2020[channel], 0.0) * 203.0);
        const double actual = official_row[static_cast<size_t>(x) * 3u + channel] / 65535.0;
        maximum_error = std::max(maximum_error, std::abs(expected - actual));
      }
    }
  }
  return maximum_error;
}

}  // namespace

bool is_adobe_tmap_avif(const std::filesystem::path& path) {
  try {
    const auto bytes = read_file(path);
    auto decoder = parse_avif(bytes, false);
    return decoder->image && decoder->image->gainMap;
  } catch (...) {
    return false;
  }
}

SourceInfo inspect_adobe_tmap_avif(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  auto decoder = parse_avif(bytes, false);
  return source_info(path, decoder->image, read_item_graph(path));
}

ReconstructedHdr reconstruct_adobe_tmap_avif(const std::filesystem::path& path,
                                              std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  const auto decode_start = Clock::now();
  const auto bytes = read_file(path);
  auto decoder = parse_avif(bytes, true);
  const ItemGraph graph = read_item_graph(path);
  ReconstructedHdr output;
  output.info = source_info(path, decoder->image, graph);
  output.decode_ms = elapsed_ms(decode_start);
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");

  const auto reconstruction_start = Clock::now();
  avifRGBImage* raw_rgb = new avifRGBImage{};
  AvifRgbPtr rgb(raw_rgb);
  avifRGBImageSetDefaults(rgb.get(), decoder->image);
  rgb->format = AVIF_RGB_FORMAT_RGB;
  rgb->depth = 16;
  rgb->isFloat = AVIF_FALSE;
  rgb->pixels = nullptr;
  rgb->rowBytes = 0;
  const float full_headroom = static_cast<float>(std::max(
      output.info.base_hdr_headroom, output.info.alternate_hdr_headroom));
  avifContentLightLevelInformationBox clli{};
  require_avif(avifImageApplyGainMap(
                   decoder->image, decoder->image->gainMap, full_headroom,
                   AVIF_COLOR_PRIMARIES_BT2020,
                   AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084,
                   rgb.get(), &clli, &decoder->diag),
               decoder->diag, "reconstruct full-capacity AVIF gain-map HDR");
  output.max_cll = clli.maxCLL;
  output.max_pall = clli.maxPALL;
  output.independent_formula_max_error =
      independent_formula_check(decoder->image, decoder->image->gainMap, *rgb);
  output.rec2020_pq_rgb.resize(
      static_cast<size_t>(decoder->image->width) * decoder->image->height * 3u);
  for (uint32_t y = 0; y < decoder->image->height; ++y) {
    const auto* source = reinterpret_cast<const uint16_t*>(
        rgb->pixels + static_cast<size_t>(y) * rgb->rowBytes);
    std::copy_n(source, static_cast<size_t>(decoder->image->width) * 3u,
                output.rec2020_pq_rgb.data() +
                    static_cast<size_t>(y) * decoder->image->width * 3u);
  }
  output.reconstruction_ms = elapsed_ms(reconstruction_start);

  if (decoder->image->exif.size > 0) {
    output.exif.assign(decoder->image->exif.data,
                       decoder->image->exif.data + decoder->image->exif.size);
  }
  if (decoder->image->xmp.size > 0) {
    output.xmp.assign(decoder->image->xmp.data,
                      decoder->image->xmp.data + decoder->image->xmp.size);
  }
  const auto orientation_start = Clock::now();
  const auto transformed = orientation::normalize_rgb16(
      output.rec2020_pq_rgb, decoder->image->width, decoder->image->height,
      output.info.original_orientation);
  output.info.width = transformed.width;
  output.info.height = transformed.height;
  output.info.orientation_normalized = true;
  orientation::set_exif_orientation_to_one(output.exif);
  orientation::set_xmp_orientation_to_one(output.xmp);
  output.orientation_ms = elapsed_ms(orientation_start);
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  return output;
}

GainMapRaster extract_avif_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel) {
  const auto begin = Clock::now();
  const auto bytes = read_file(path);
  auto decoder = parse_avif(bytes, true);
  const ItemGraph graph = read_item_graph(path);
  SourceInfo info = source_info(path, decoder->image, graph);
  const avifImage* image = decoder->image->gainMap->image;
  if (!image) throw std::runtime_error("AVIF gain-map item was not decoded");
  avifRGBImage rgb{};
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGB;
  rgb.depth = 16;
  rgb.isFloat = AVIF_FALSE;
  require_avif(avifRGBImageAllocatePixels(&rgb), decoder->diag,
                "allocate AVIF gain-map RGB raster");
  struct RgbFree { avifRGBImage* value; ~RgbFree() { avifRGBImageFreePixels(value); } } free{&rgb};
  require_avif(avifImageYUVToRGB(image, &rgb), decoder->diag,
               "decode AVIF gain-map RGB raster");
  std::vector<uint16_t> samples(static_cast<size_t>(image->width) * image->height * 3u);
  for (uint32_t y = 0; y < image->height; ++y) {
    check_gainmap_cancel(cancel);
    const auto* row = reinterpret_cast<const uint16_t*>(
        rgb.pixels + static_cast<size_t>(y) * rgb.rowBytes);
    std::copy_n(row, static_cast<size_t>(image->width) * 3u,
                samples.data() + static_cast<size_t>(y) * image->width * 3u);
  }
  auto result = make_raster(std::move(info), image->width, image->height,
                            decoder->image->gainMap->image->yuvFormat == AVIF_PIXEL_FORMAT_YUV400 ? 1u : 3u,
                            std::move(samples), "AVIF gain-map image", cancel);
  normalize_gainmap_orientation(result, result.info.original_orientation);
  result.decode_ms = elapsed_ms(begin);
  return result;
}

}  // namespace hdrbridge::gainmap
