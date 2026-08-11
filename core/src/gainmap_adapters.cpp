#include "gainmap_adapters.h"

#include "jpeg_probe.h"
#include "orientation.h"

#include <avif/avif.h>
#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <libheif/heif_properties.h>
#include <lcms2.h>
#include <tiffio.h>
#include <ultrahdr_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace hdrbridge::gainmap {

namespace {

using Clock = std::chrono::steady_clock;
constexpr char kAppleHdrGainMapUrn[] =
    "urn:com:apple:photo:2020:aux:hdrgainmap";

uint32_t codec_worker_count() {
#if defined(__EMSCRIPTEN__)
  return 1u;
#else
  return std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
#endif
}

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
struct HeifContextDeleter {
  void operator()(heif_context* value) const { heif_context_free(value); }
};
struct HeifHandleDeleter {
  void operator()(heif_image_handle* value) const {
    heif_image_handle_release(value);
  }
};
struct HeifImageDeleter {
  void operator()(heif_image* value) const { heif_image_release(value); }
};
struct HeifOptionsDeleter {
  void operator()(heif_decoding_options* value) const {
    heif_decoding_options_free(value);
  }
};

using AvifDecoderPtr = std::unique_ptr<avifDecoder, AvifDecoderDeleter>;
using AvifRgbPtr = std::unique_ptr<avifRGBImage, AvifRgbDeleter>;
using HeifContextPtr = std::unique_ptr<heif_context, HeifContextDeleter>;
using HeifHandlePtr = std::unique_ptr<heif_image_handle, HeifHandleDeleter>;
using HeifImagePtr = std::unique_ptr<heif_image, HeifImageDeleter>;
using HeifOptionsPtr = std::unique_ptr<heif_decoding_options, HeifOptionsDeleter>;

struct TiffDeleter {
  void operator()(TIFF* value) const { if (value) TIFFClose(value); }
};
struct CmsProfileDeleter {
  void operator()(cmsHPROFILE value) const { if (value) cmsCloseProfile(value); }
};
using TiffPtr = std::unique_ptr<TIFF, TiffDeleter>;
using CmsProfilePtr = std::unique_ptr<std::remove_pointer_t<cmsHPROFILE>, CmsProfileDeleter>;

constexpr uint32_t kTiffGainMapMetadataTag = 52557;
constexpr uint16_t kTiffGainMapPhotometric = 52553;
constexpr uint32_t kTiffGainMapSubfileType = 32;

const TIFFFieldInfo kGainMapFields[] = {
    {kTiffGainMapMetadataTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED,
     FIELD_CUSTOM, TRUE, TRUE, const_cast<char*>("GainMapMetadata")}};

struct IsoGainMapMetadata {
  bool multichannel = false;
  bool use_base_color_space = true;
  double base_hdr_headroom = 0.0;
  double alternate_hdr_headroom = 0.0;
  std::array<double, 3> minimum{};
  std::array<double, 3> maximum{};
  std::array<double, 3> gamma{};
  std::array<double, 3> base_offset{};
  std::array<double, 3> alternate_offset{};
};

struct UhdrDecoderDeleter {
  void operator()(uhdr_codec_private_t* value) const {
    if (value) uhdr_release_decoder(value);
  }
};
using UhdrDecoderPtr = std::unique_ptr<uhdr_codec_private_t, UhdrDecoderDeleter>;

double elapsed_ms(Clock::time_point begin) {
  return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open gain-map input: " + path.string());
  const std::streamoff size = input.tellg();
  if (size < 0) throw std::runtime_error("cannot determine gain-map input size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  input.seekg(0);
  if (!bytes.empty() &&
      !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("cannot read gain-map input");
  }
  return bytes;
}

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

void require_heif(heif_error result, const char* operation) {
  if (result.code == heif_error_Ok) return;
  throw std::runtime_error(std::string(operation) + ": " +
                           (result.message ? result.message : "libheif error"));
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

struct ItemGraph {
  uint32_t base = 0;
  uint32_t gain = 0;
  uint32_t tone_map = 0;
};

uint8_t orientation_from_heif(const heif_context* context,
                              const heif_image_handle* handle) {
  const heif_item_id item = heif_image_handle_get_item_id(handle);
  const int rotation = heif_item_get_property_transform_rotation_ccw(context, item, 0);
  const auto mirror = heif_item_get_property_transform_mirror(context, item, 0);
  const bool mirrored = mirror != heif_transform_mirror_direction_invalid;
  const bool horizontal = mirror == heif_transform_mirror_direction_horizontal;
  if (rotation == 90) return mirrored ? (horizontal ? 7 : 5) : 8;
  if (rotation == 180) return mirrored ? (horizontal ? 4 : 2) : 3;
  if (rotation == 270) return mirrored ? (horizontal ? 5 : 7) : 6;
  return mirrored ? (horizontal ? 2 : 4) : 1;
}

ItemGraph read_item_graph(const heif_context* context) {
  const int count = heif_context_get_number_of_items(context);
  std::vector<heif_item_id> ids(static_cast<size_t>(std::max(count, 0)));
  const int got = ids.empty() ? 0 : heif_context_get_list_of_item_IDs(
      context, ids.data(), static_cast<int>(ids.size()));
  ItemGraph graph;
  for (int i = 0; i < got; ++i) {
    const heif_item_id id = ids[static_cast<size_t>(i)];
    if (heif_item_get_item_type(context, id) != heif_fourcc('t', 'm', 'a', 'p')) continue;
    graph.tone_map = id;
    for (int reference_index = 0;; ++reference_index) {
      uint32_t type = 0;
      heif_item_id* targets = nullptr;
      const size_t target_count = heif_context_get_item_references(
          context, id, reference_index, &type, &targets);
      if (target_count == 0) break;
      if (type == heif_fourcc('d', 'i', 'm', 'g') && target_count >= 2) {
        graph.base = targets[0];
        graph.gain = targets[1];
      }
      heif_release_item_references(context, &targets);
    }
  }
  return graph;
}

ItemGraph read_item_graph(const std::filesystem::path& path) {
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  require_heif(heif_context_read_from_file(context.get(), path.string().c_str(), nullptr),
               "read AVIF item graph");
  return read_item_graph(context.get());
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
  info.base_channels = 3;
  info.base_codec = "AV1";
  info.base_color_space = "Display P3";
  info.base_transfer = "SDR gamma";
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
  info.pixel_format = "AV1 " + info.chroma;
  info.color_signal_kind = "AVIF CICP + ICC / tmap metadata";
  info.bit_depth = image->depth;
  info.chroma = "base " + std::to_string(image->yuvFormat) +
                ", gain-map " + std::to_string(gain->image ? gain->image->yuvFormat : 0);
  info.primaries = static_cast<uint16_t>(image->colorPrimaries);
  info.transfer = static_cast<uint16_t>(image->transferCharacteristics);
  info.matrix = static_cast<uint16_t>(image->matrixCoefficients);
  info.full_range = image->yuvRange == AVIF_RANGE_FULL;
  info.range_known = true;
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

double inverse_srgb(double value) {
  return value <= 0.04045 ? value / 12.92
                          : std::pow((value + 0.055) / 1.055, 2.4);
}

double forward_pq(double nits) {
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 32.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 128.0;
  constexpr double c3 = 2392.0 / 128.0;
  const double linear = std::clamp(nits / 10000.0, 0.0, 1.0);
  const double powered = std::pow(linear, m1);
  return std::pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
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

uint16_t read_be16(const uint8_t*& cursor, const uint8_t* end) {
  if (end - cursor < 2) throw std::runtime_error("truncated TIFF gain-map metadata");
  const uint16_t value = static_cast<uint16_t>((cursor[0] << 8) | cursor[1]);
  cursor += 2;
  return value;
}

uint32_t read_be32(const uint8_t*& cursor, const uint8_t* end) {
  if (end - cursor < 4) throw std::runtime_error("truncated TIFF gain-map metadata");
  const uint32_t value = (static_cast<uint32_t>(cursor[0]) << 24) |
                         (static_cast<uint32_t>(cursor[1]) << 16) |
                         (static_cast<uint32_t>(cursor[2]) << 8) |
                         static_cast<uint32_t>(cursor[3]);
  cursor += 4;
  return value;
}

double unsigned_fraction(const uint8_t*& cursor, const uint8_t* end) {
  const uint32_t numerator = read_be32(cursor, end);
  const uint32_t denominator = read_be32(cursor, end);
  if (denominator == 0) throw std::runtime_error("zero denominator in TIFF gain-map metadata");
  return static_cast<double>(numerator) / denominator;
}

double signed_fraction(const uint8_t*& cursor, const uint8_t* end) {
  const int32_t numerator = static_cast<int32_t>(read_be32(cursor, end));
  const uint32_t denominator = read_be32(cursor, end);
  if (denominator == 0) throw std::runtime_error("zero denominator in TIFF gain-map metadata");
  return static_cast<double>(numerator) / denominator;
}

IsoGainMapMetadata parse_iso_gain_map_metadata(const void* data, size_t size) {
  if (!data || size < 21) throw std::runtime_error("ISO gain-map metadata is missing");
  const auto* cursor = static_cast<const uint8_t*>(data);
  const auto* end = cursor + size;
  const uint16_t minimum_version = read_be16(cursor, end);
  const uint16_t writer_version = read_be16(cursor, end);
  if (minimum_version != 0 || writer_version < minimum_version) {
    throw std::runtime_error("unsupported ISO gain-map metadata version");
  }
  if (cursor == end) throw std::runtime_error("truncated ISO gain-map flags");
  const uint8_t flags = *cursor++;
  if ((flags & 0x33) != 0) throw std::runtime_error("invalid reserved ISO gain-map metadata bits");
  if ((flags & 0x04) != 0) {
    throw std::runtime_error("HDR-base inverse-direction Apple gain map is not supported");
  }
  const bool common_denominator = (flags & 0x08) != 0;
  uint32_t denominator = 0;
  if (common_denominator) {
    denominator = read_be32(cursor, end);
    if (denominator == 0) throw std::runtime_error("zero common ISO gain-map denominator");
  }
  const auto read_unsigned = [&]() {
    const uint32_t numerator = read_be32(cursor, end);
    const uint32_t divisor = common_denominator ? denominator : read_be32(cursor, end);
    if (divisor == 0) throw std::runtime_error("zero ISO gain-map denominator");
    return static_cast<double>(numerator) / divisor;
  };
  const auto read_signed = [&]() {
    const int32_t numerator = static_cast<int32_t>(read_be32(cursor, end));
    const uint32_t divisor = common_denominator ? denominator : read_be32(cursor, end);
    if (divisor == 0) throw std::runtime_error("zero ISO gain-map denominator");
    return static_cast<double>(numerator) / divisor;
  };
  IsoGainMapMetadata metadata;
  metadata.multichannel = (flags & 0x80) != 0;
  metadata.use_base_color_space = (flags & 0x40) != 0;
  metadata.base_hdr_headroom = read_unsigned();
  metadata.alternate_hdr_headroom = read_unsigned();
  const size_t channels = metadata.multichannel ? 3u : 1u;
  for (size_t channel = 0; channel < channels; ++channel) {
    metadata.minimum[channel] = read_signed();
    metadata.maximum[channel] = read_signed();
    metadata.gamma[channel] = read_unsigned();
    metadata.base_offset[channel] = read_signed();
    metadata.alternate_offset[channel] = read_signed();
    if (metadata.maximum[channel] < metadata.minimum[channel] ||
        metadata.gamma[channel] <= 0.0) {
      throw std::runtime_error("invalid ISO gain-map channel metadata");
    }
  }
  for (size_t channel = channels; channel < 3; ++channel) {
    metadata.minimum[channel] = metadata.minimum[0];
    metadata.maximum[channel] = metadata.maximum[0];
    metadata.gamma[channel] = metadata.gamma[0];
    metadata.base_offset[channel] = metadata.base_offset[0];
    metadata.alternate_offset[channel] = metadata.alternate_offset[0];
  }
  if (cursor != end) throw std::runtime_error("unexpected trailing ISO gain-map metadata");
  return metadata;
}

IsoGainMapMetadata parse_tiff_gain_map_metadata(const void* data, uint32_t size) {
  // Adobe TIFF places a four-byte container version before the ISO 21496-1
  // GainMapMetadata syntax used by the AVIF tmap box.
  if (!data || size < 25) throw std::runtime_error("TIFF gain-map metadata is missing");
  const auto* cursor = static_cast<const uint8_t*>(data);
  const auto* end = cursor + size;
  const uint32_t container_version = read_be32(cursor, end);
  const uint16_t minimum_version = read_be16(cursor, end);
  const uint16_t writer_version = read_be16(cursor, end);
  if (container_version != 0 || minimum_version != 0 || writer_version < minimum_version) {
    throw std::runtime_error("unsupported TIFF/ISO gain-map metadata version");
  }
  const uint8_t flags = *cursor++;
  if ((flags & 0x3f) != 0) throw std::runtime_error("invalid reserved TIFF gain-map metadata bits");
  IsoGainMapMetadata metadata;
  metadata.multichannel = (flags & 0x80) != 0;
  metadata.use_base_color_space = (flags & 0x40) != 0;
  metadata.base_hdr_headroom = unsigned_fraction(cursor, end);
  metadata.alternate_hdr_headroom = unsigned_fraction(cursor, end);
  const size_t channel_count = metadata.multichannel ? 3u : 1u;
  for (size_t channel = 0; channel < channel_count; ++channel) {
    metadata.minimum[channel] = signed_fraction(cursor, end);
    metadata.maximum[channel] = signed_fraction(cursor, end);
    metadata.gamma[channel] = unsigned_fraction(cursor, end);
    metadata.base_offset[channel] = signed_fraction(cursor, end);
    metadata.alternate_offset[channel] = signed_fraction(cursor, end);
    if (metadata.maximum[channel] < metadata.minimum[channel] || metadata.gamma[channel] <= 0.0) {
      throw std::runtime_error("invalid TIFF gain-map channel metadata");
    }
  }
  for (size_t channel = channel_count; channel < 3; ++channel) {
    metadata.minimum[channel] = metadata.minimum[0];
    metadata.maximum[channel] = metadata.maximum[0];
    metadata.gamma[channel] = metadata.gamma[0];
    metadata.base_offset[channel] = metadata.base_offset[0];
    metadata.alternate_offset[channel] = metadata.alternate_offset[0];
  }
  if (cursor != end) throw std::runtime_error("unexpected trailing TIFF gain-map metadata");
  return metadata;
}

std::string profile_description(const void* data, uint32_t size) {
  CmsProfilePtr profile(cmsOpenProfileFromMem(data, size));
  if (!profile) return {};
  std::array<char, 256> description{};
  cmsGetProfileInfoASCII(profile.get(), cmsInfoDescription, "en", "US",
                         description.data(), static_cast<cmsUInt32Number>(description.size()));
  return description.data();
}

uint32_t read_be32_at(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

std::optional<double> adaptive_tmap_reference_white_nits(
    const std::filesystem::path& path) {
  // Adaptive HDR may attach a complex HDR output profile to the tmap item.
  // It is not a source profile: A2B0/B2A0 and hdgm describe HDR presentation.
  // We only consume its ICC luminance tag as the absolute reference-white
  // anchor for the ISO reconstruction and never run the base raster through it.
  const std::vector<uint8_t> bytes = read_file(path);
  std::optional<double> found;
  for (size_t offset = 0; offset + 132u <= bytes.size(); ++offset) {
    if (std::memcmp(bytes.data() + offset + 36u, "acsp", 4u) != 0) continue;
    const uint32_t profile_size = read_be32_at(bytes.data() + offset);
    if (profile_size < 132u || profile_size > bytes.size() - offset) continue;
    const uint32_t tag_count = read_be32_at(bytes.data() + offset + 128u);
    if (tag_count > (profile_size - 132u) / 12u) continue;
    bool has_a2b0 = false;
    bool has_b2a0 = false;
    bool has_hdgm = false;
    uint32_t lumi_offset = 0;
    uint32_t lumi_size = 0;
    for (uint32_t tag = 0; tag < tag_count; ++tag) {
      const uint8_t* entry = bytes.data() + offset + 132u + static_cast<size_t>(tag) * 12u;
      const uint32_t signature = read_be32_at(entry);
      const uint32_t data_offset = read_be32_at(entry + 4u);
      const uint32_t data_size = read_be32_at(entry + 8u);
      if (data_offset > profile_size || data_size > profile_size - data_offset) continue;
      has_a2b0 |= signature == 0x41324230u;  // A2B0
      has_b2a0 |= signature == 0x42324130u;  // B2A0
      has_hdgm |= signature == 0x6864676du;  // hdgm
      if (signature == 0x6c756d69u) {       // lumi
        lumi_offset = data_offset;
        lumi_size = data_size;
      }
    }
    if (!has_a2b0 || !has_b2a0 || !has_hdgm || lumi_size < 20u) continue;
    const uint8_t* lumi = bytes.data() + offset + lumi_offset;
    if (std::memcmp(lumi, "XYZ ", 4u) != 0) continue;
    const int32_t y_fixed = static_cast<int32_t>(read_be32_at(lumi + 12u));
    const double nits = static_cast<double>(y_fixed) / 65536.0;
    if (!std::isfinite(nits) || nits <= 0.0 || nits > 10000.0) continue;
    if (found && std::abs(*found - nits) > 1e-6) {
      throw std::runtime_error("Apple HEIC contains conflicting Adaptive HDR ICC luminance tags");
    }
    found = nits;
    offset += profile_size - 1u;
  }
  return found;
}

struct TiffGainMapData {
  SourceInfo info;
  IsoGainMapMetadata metadata;
  std::vector<uint16_t> base;
  std::vector<uint16_t> map;
  std::vector<uint8_t> xmp;
  std::vector<uint8_t> icc;
};

TiffPtr open_tiff_gain_map(const std::filesystem::path& path) {
#ifdef _WIN32
  TiffPtr tiff(TIFFOpenW(path.c_str(), "r"));
#else
  TiffPtr tiff(TIFFOpen(path.string().c_str(), "r"));
#endif
  if (!tiff) throw std::runtime_error("cannot open Adobe gain-map TIFF");
  if (TIFFMergeFieldInfo(tiff.get(), kGainMapFields,
                         static_cast<uint32_t>(std::size(kGainMapFields))) < 0) {
    throw std::runtime_error("cannot register TIFF gain-map metadata tag");
  }
  return tiff;
}

TiffGainMapData read_tiff_gain_map(const std::filesystem::path& path,
                                   bool pixels,
                                   std::atomic_bool* cancel) {
  auto tiff = open_tiff_gain_map(path);
  TiffGainMapData result;
  auto& info = result.info;
  info.path = path;
  info.format = "TIFF";
  info.container_brand = "TIFF/SubIFD";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "adobe-iso-tiff";
  info.codec = "RGB16 base + RGB16 ISO gain map SubIFD";
  info.profile = "ISO 21496-1 gain-map HDR reconstruction";
  info.bit_depth = 16;
  info.chroma = "4:4:4 RGB";
  uint16_t base_bits = 0, base_samples = 0, base_format = SAMPLEFORMAT_UINT;
  uint16_t base_photo = 0, base_planar = 0, base_orientation = ORIENTATION_TOPLEFT;
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &info.width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &info.height);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &base_bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &base_samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &base_format);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PHOTOMETRIC, &base_photo);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &base_planar);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_ORIENTATION, &base_orientation);
  if (!info.width || !info.height || base_bits != 16 || base_samples != 3 ||
      base_format != SAMPLEFORMAT_UINT || base_photo != PHOTOMETRIC_RGB ||
      base_planar != PLANARCONFIG_CONTIG) {
    throw std::runtime_error("Adobe gain-map TIFF base must be contiguous RGB16");
  }
  info.base_width = info.width;
  info.base_height = info.height;
  info.base_bit_depth = base_bits;
  info.base_channels = base_samples;
  info.base_codec = "TIFF RGB";
  info.base_transfer = "SDR gamma";
  info.primaries = 12;
  info.transfer = 13;
  info.matrix = 0;
  info.full_range = true;
  info.range_known = true;
  info.original_orientation = static_cast<uint8_t>(std::clamp<uint16_t>(base_orientation, 1, 8));
  uint32_t icc_size = 0;
  void* icc_data = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc_data) != 1 || !icc_data || !icc_size) {
    throw std::runtime_error("Adobe gain-map TIFF SDR base ICC profile is missing");
  }
  result.icc.assign(static_cast<uint8_t*>(icc_data), static_cast<uint8_t*>(icc_data) + icc_size);
  const std::string base_profile = profile_description(icc_data, icc_size);
  info.base_color_space = base_profile;
  if (base_profile.find("Display P3") == std::string::npos &&
      base_profile.find("P3") == std::string::npos) {
    throw std::runtime_error("Adobe gain-map TIFF base ICC is not validated Display P3");
  }
  info.profile = base_profile + " base + ISO 21496-1 gain map";
  info.icc_present = true;
  info.color_signal_kind = "TIFF ICC + ISO gain-map metadata";
  info.pixel_format = "RGB16 integer base + RGB16 gain map";
#ifdef TIFFTAG_XMLPACKET
  uint32_t xmp_size = 0;
  void* xmp_data = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_XMLPACKET, &xmp_size, &xmp_data) == 1 && xmp_data && xmp_size) {
    result.xmp.assign(static_cast<uint8_t*>(xmp_data), static_cast<uint8_t*>(xmp_data) + xmp_size);
    info.xmp_present = true;
  }
#endif
  uint16_t subifd_count = 0;
  toff_t* subifd_offsets = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_SUBIFD, &subifd_count, &subifd_offsets) != 1 ||
      subifd_count != 1 || !subifd_offsets) {
    throw std::runtime_error("Adobe gain-map TIFF requires exactly one gain-map SubIFD");
  }
  const toff_t gain_map_offset = subifd_offsets[0];
  if (pixels) {
    result.base.resize(static_cast<size_t>(info.width) * info.height * 3u);
    for (uint32_t y = 0; y < info.height; ++y) {
      if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
      if (TIFFReadScanline(tiff.get(), result.base.data() + static_cast<size_t>(y) * info.width * 3u, y, 0) < 0) {
        throw std::runtime_error("Adobe gain-map TIFF base scanline decode failed");
      }
    }
  }
  if (!TIFFSetSubDirectory(tiff.get(), gain_map_offset)) {
    throw std::runtime_error("cannot open Adobe gain-map TIFF SubIFD");
  }
  uint32_t map_width = 0, map_height = 0;
  uint16_t map_bits = 0, map_samples = 0, map_format = SAMPLEFORMAT_UINT;
  uint16_t map_photo = 0, map_planar = 0;
  uint32_t subfile_type = 0;
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &map_width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &map_height);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &map_bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &map_samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &map_format);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PHOTOMETRIC, &map_photo);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &map_planar);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SUBFILETYPE, &subfile_type);
  if (!map_width || !map_height || map_bits != 16 || map_samples != 3 ||
      map_format != SAMPLEFORMAT_UINT || map_photo != kTiffGainMapPhotometric ||
      map_planar != PLANARCONFIG_CONTIG || (subfile_type & kTiffGainMapSubfileType) == 0) {
    throw std::runtime_error("Adobe gain-map TIFF SubIFD has an unsupported pixel layout");
  }
  info.gain_map_width = map_width;
  info.gain_map_height = map_height;
  info.gain_map_channels = map_samples;
  info.gain_map_scale_x = info.base_width / static_cast<double>(map_width);
  info.gain_map_scale_y = info.base_height / static_cast<double>(map_height);
  uint32_t metadata_size = 0;
  void* metadata_data = nullptr;
  if (TIFFGetField(tiff.get(), kTiffGainMapMetadataTag, &metadata_size, &metadata_data) != 1) {
    throw std::runtime_error("Adobe gain-map TIFF ISO metadata tag is missing");
  }
  result.metadata = parse_tiff_gain_map_metadata(metadata_data, metadata_size);
  info.base_hdr_headroom = result.metadata.base_hdr_headroom;
  info.alternate_hdr_headroom = result.metadata.alternate_hdr_headroom;
  info.hdr_capacity_min = std::exp2(info.base_hdr_headroom);
  info.hdr_capacity_max = std::exp2(info.alternate_hdr_headroom);
  info.gain_map_uses_base_color_space = result.metadata.use_base_color_space;
  info.gain_map_min = result.metadata.minimum;
  info.gain_map_max = result.metadata.maximum;
  info.gain_map_gamma = result.metadata.gamma;
  info.base_offset = result.metadata.base_offset;
  info.alternate_offset = result.metadata.alternate_offset;
  if (!result.metadata.use_base_color_space) {
    throw std::runtime_error("TIFF gain map using alternate color space is not safely reconstructable without its profile");
  }
  if (pixels) {
    result.map.resize(static_cast<size_t>(map_width) * map_height * 3u);
    for (uint32_t y = 0; y < map_height; ++y) {
      if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
      if (TIFFReadScanline(tiff.get(), result.map.data() + static_cast<size_t>(y) * map_width * 3u, y, 0) < 0) {
        throw std::runtime_error("Adobe gain-map TIFF SubIFD scanline decode failed");
      }
    }
  }
  if (info.original_orientation >= 5) {
    std::swap(info.width, info.height);
    std::swap(info.base_width, info.base_height);
    info.gain_map_scale_x = info.base_width / static_cast<double>(map_width);
    info.gain_map_scale_y = info.base_height / static_cast<double>(map_height);
  }
  info.orientation_normalized = true;
  return result;
}

double sample_map_bilinear(const std::vector<uint16_t>& map, uint32_t map_width,
                           uint32_t map_height, uint32_t base_width,
                           uint32_t base_height, uint32_t x, uint32_t y,
                           size_t channel) {
  if (map_width == base_width && map_height == base_height) {
    return map[(static_cast<size_t>(y) * map_width + x) * 3u + channel] / 65535.0;
  }
  const double mx = (static_cast<double>(x) + 0.5) * map_width / base_width - 0.5;
  const double my = (static_cast<double>(y) + 0.5) * map_height / base_height - 0.5;
  const int x0 = std::clamp(static_cast<int>(std::floor(mx)), 0, static_cast<int>(map_width) - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor(my)), 0, static_cast<int>(map_height) - 1);
  const int x1 = std::min(x0 + 1, static_cast<int>(map_width) - 1);
  const int y1 = std::min(y0 + 1, static_cast<int>(map_height) - 1);
  const double fx = std::clamp(mx - std::floor(mx), 0.0, 1.0);
  const double fy = std::clamp(my - std::floor(my), 0.0, 1.0);
  const auto at = [&](int px, int py) {
    return map[(static_cast<size_t>(py) * map_width + static_cast<size_t>(px)) * 3u + channel] / 65535.0;
  };
  return (at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx) * (1.0 - fy) +
         (at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx) * fy;
}

bool contains_ascii(const std::vector<uint8_t>& bytes, const char* text) {
  const size_t length = std::strlen(text);
  return length > 0 && std::search(bytes.begin(), bytes.end(), text, text + length) != bytes.end();
}

std::vector<uint8_t> item_data(const heif_context* context, heif_item_id item) {
  uint8_t* raw = nullptr;
  size_t size = 0;
  require_heif(heif_item_get_item_data(context, item, nullptr, &raw, &size),
               "read HEIF metadata item");
  std::vector<uint8_t> result;
  if (raw && size) result.assign(raw, raw + size);
  heif_release_item_data(context, &raw);
  return result;
}

void load_heif_apple_metadata(const heif_context* context,
                              const heif_image_handle* primary,
                              std::vector<uint8_t>& exif,
                              std::vector<uint8_t>& xmp) {
  const int metadata_count = heif_image_handle_get_number_of_metadata_blocks(primary, nullptr);
  std::vector<heif_item_id> metadata_ids(static_cast<size_t>(std::max(metadata_count, 0)));
  const int got = metadata_ids.empty() ? 0 : heif_image_handle_get_list_of_metadata_block_IDs(
      primary, nullptr, metadata_ids.data(), metadata_count);
  for (int index = 0; index < got; ++index) {
    const heif_item_id id = metadata_ids[static_cast<size_t>(index)];
    const char* type = heif_image_handle_get_metadata_type(primary, id);
    const char* content = heif_image_handle_get_metadata_content_type(primary, id);
    const bool is_exif = type && std::strcmp(type, "Exif") == 0;
    const bool is_xmp = type && std::strcmp(type, "mime") == 0 && content &&
                        (std::strstr(content, "rdf") || std::strstr(content, "xml"));
    auto* target = is_exif ? &exif : is_xmp ? &xmp : nullptr;
    if (!target || !target->empty()) continue;
    target->resize(heif_image_handle_get_metadata_size(primary, id));
    if (!target->empty()) {
      require_heif(heif_image_handle_get_metadata(primary, id, target->data()),
                   "read Apple HEIF metadata");
    }
  }
  // Apple attaches HDR gain-map XMP to the auxiliary item instead of the
  // primary image. Preserve it by scanning only MIME metadata items.
  const int item_count = heif_context_get_number_of_items(context);
  std::vector<heif_item_id> ids(static_cast<size_t>(std::max(item_count, 0)));
  const int item_got = ids.empty() ? 0 : heif_context_get_list_of_item_IDs(
      context, ids.data(), static_cast<int>(ids.size()));
  for (int index = 0; index < item_got && xmp.empty(); ++index) {
    const heif_item_id id = ids[static_cast<size_t>(index)];
    if (heif_item_get_item_type(context, id) != heif_item_type_mime) continue;
    const char* content = heif_item_get_mime_item_content_type(context, id);
    if (!content || (!std::strstr(content, "rdf") && !std::strstr(content, "xml"))) continue;
    auto candidate = item_data(context, id);
    if (contains_ascii(candidate, "HDRGainMapVersion") ||
        contains_ascii(candidate, kAppleHdrGainMapUrn)) {
      xmp = std::move(candidate);
    }
  }
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

double sample_mono_map_bilinear(const std::vector<uint16_t>& map,
                                uint32_t map_width, uint32_t map_height,
                                uint32_t base_width, uint32_t base_height,
                                uint32_t x, uint32_t y) {
  if (map_width == base_width && map_height == base_height) {
    return map[static_cast<size_t>(y) * map_width + x] / 65535.0;
  }
  const double mx = (static_cast<double>(x) + 0.5) * map_width / base_width - 0.5;
  const double my = (static_cast<double>(y) + 0.5) * map_height / base_height - 0.5;
  const int x0 = std::clamp(static_cast<int>(std::floor(mx)), 0, static_cast<int>(map_width) - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor(my)), 0, static_cast<int>(map_height) - 1);
  const int x1 = std::min(x0 + 1, static_cast<int>(map_width) - 1);
  const int y1 = std::min(y0 + 1, static_cast<int>(map_height) - 1);
  const double fx = std::clamp(mx - std::floor(mx), 0.0, 1.0);
  const double fy = std::clamp(my - std::floor(my), 0.0, 1.0);
  const auto at = [&](int px, int py) {
    return map[static_cast<size_t>(py) * map_width + static_cast<size_t>(px)] / 65535.0;
  };
  return (at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx) * (1.0 - fy) +
         (at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx) * fy;
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

bool is_adobe_gainmap_tiff(const std::filesystem::path& path) {
  try {
    (void)read_tiff_gain_map(path, false, nullptr);
    return true;
  } catch (...) {
    return false;
  }
}

SourceInfo inspect_adobe_gainmap_tiff(const std::filesystem::path& path) {
  return read_tiff_gain_map(path, false, nullptr).info;
}

ReconstructedHdr reconstruct_adobe_gainmap_tiff(
    const std::filesystem::path& path, std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  const auto decode_start = Clock::now();
  TiffGainMapData tiff = read_tiff_gain_map(path, true, cancel);
  ReconstructedHdr output;
  output.info = tiff.info;
  output.xmp = std::move(tiff.xmp);
  output.decode_ms = elapsed_ms(decode_start);

  const uint32_t width = output.info.original_orientation >= 5
      ? output.info.height : output.info.width;
  const uint32_t height = output.info.original_orientation >= 5
      ? output.info.width : output.info.height;
  if (tiff.base.size() != static_cast<size_t>(width) * height * 3u) {
    throw std::runtime_error("Adobe gain-map TIFF base dimensions are inconsistent");
  }
  CmsProfilePtr profile(cmsOpenProfileFromMem(tiff.icc.data(),
                                               static_cast<cmsUInt32Number>(tiff.icc.size())));
  if (!profile) throw std::runtime_error("cannot parse Adobe gain-map TIFF base ICC");
  auto* red_curve = static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigRedTRCTag));
  auto* green_curve = static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigGreenTRCTag));
  auto* blue_curve = static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigBlueTRCTag));
  if (!red_curve || !green_curve || !blue_curve) {
    throw std::runtime_error("Adobe gain-map TIFF base ICC has no RGB transfer curves");
  }
  const std::array<cmsToneCurve*, 3> curves{red_curve, green_curve, blue_curve};
  const auto reconstruction_start = Clock::now();
  output.rec2020_pq_rgb.resize(tiff.base.size());
  const double base_headroom = tiff.metadata.base_hdr_headroom;
  const double alternate_headroom = tiff.metadata.alternate_hdr_headroom;
  const double full_headroom = std::max(base_headroom, alternate_headroom);
  double weight = 0.0;
  if (alternate_headroom != base_headroom) {
    weight = std::clamp((full_headroom - base_headroom) /
                            (alternate_headroom - base_headroom), 0.0, 1.0);
    if (alternate_headroom < base_headroom) weight = -weight;
  }
  double maximum_linear = 0.0;
  long double sum_max_linear = 0.0;
  for (uint32_t y = 0; y < height; ++y) {
    if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
    for (uint32_t x = 0; x < width; ++x) {
      const size_t pixel = static_cast<size_t>(y) * width + x;
      std::array<double, 3> p3_linear{};
      for (size_t channel = 0; channel < 3; ++channel) {
        const float encoded_base = tiff.base[pixel * 3u + channel] / 65535.0f;
        const double base_linear = cmsEvalToneCurveFloat(curves[channel], encoded_base);
        const double encoded_map = sample_map_bilinear(
            tiff.map, output.info.gain_map_width, output.info.gain_map_height,
            width, height, x, y, channel);
        const double log_gain = tiff.metadata.minimum[channel] +
            (tiff.metadata.maximum[channel] - tiff.metadata.minimum[channel]) *
                std::pow(encoded_map, 1.0 / tiff.metadata.gamma[channel]);
        p3_linear[channel] =
            (base_linear + tiff.metadata.base_offset[channel]) *
                std::exp2(log_gain * weight) -
            tiff.metadata.alternate_offset[channel];
      }
      const std::array<double, 3> rec2020{
          0.7538330 * p3_linear[0] + 0.1985974 * p3_linear[1] + 0.0475696 * p3_linear[2],
          0.0457438 * p3_linear[0] + 0.9417772 * p3_linear[1] + 0.0124789 * p3_linear[2],
         -0.0012103 * p3_linear[0] + 0.0176017 * p3_linear[1] + 0.9836086 * p3_linear[2]};
      const double pixel_max = std::max({rec2020[0], rec2020[1], rec2020[2], 0.0});
      maximum_linear = std::max(maximum_linear, pixel_max);
      sum_max_linear += pixel_max;
      for (size_t channel = 0; channel < 3; ++channel) {
        output.rec2020_pq_rgb[pixel * 3u + channel] = static_cast<uint16_t>(std::llround(
            forward_pq(std::max(rec2020[channel], 0.0) * 203.0) * 65535.0));
      }
    }
  }
  output.max_cll = static_cast<uint16_t>(std::clamp(
      std::llround(maximum_linear * 203.0), 0ll, 65535ll));
  const double average_linear = static_cast<double>(
      sum_max_linear / static_cast<long double>(static_cast<uint64_t>(width) * height));
  output.max_pall = static_cast<uint16_t>(std::clamp(
      std::llround(average_linear * 203.0), 0ll, 65535ll));
  output.reconstruction_ms = elapsed_ms(reconstruction_start);

  const auto orientation_start = Clock::now();
  const auto transformed = orientation::normalize_rgb16(
      output.rec2020_pq_rgb, width, height, output.info.original_orientation);
  output.info.width = transformed.width;
  output.info.height = transformed.height;
  output.info.orientation_normalized = true;
  orientation::set_xmp_orientation_to_one(output.xmp);
  output.orientation_ms = elapsed_ms(orientation_start);
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  return output;
}

AppleAuxiliaryProbe probe_apple_heif(const std::filesystem::path& path) {
  AppleAuxiliaryProbe probe;
  probe.container = "HEIF/HEIC";
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  const heif_error read = heif_context_read_from_file(context.get(), path.string().c_str(), nullptr);
  if (read.code != heif_error_Ok) return probe;
  heif_image_handle* raw_primary = nullptr;
  if (heif_context_get_primary_image_handle(context.get(), &raw_primary).code != heif_error_Ok) return probe;
  HeifHandlePtr primary(raw_primary);
  const int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA | LIBHEIF_AUX_IMAGE_FILTER_OMIT_DEPTH;
  const int count = heif_image_handle_get_number_of_auxiliary_images(primary.get(), filter);
  std::vector<heif_item_id> ids(static_cast<size_t>(std::max(count, 0)));
  const int got = ids.empty() ? 0 : heif_image_handle_get_list_of_auxiliary_image_IDs(
      primary.get(), filter, ids.data(), static_cast<int>(ids.size()));
  for (int i = 0; i < got; ++i) {
    heif_image_handle* raw_auxiliary = nullptr;
    if (heif_image_handle_get_auxiliary_image_handle(
            primary.get(), ids[static_cast<size_t>(i)], &raw_auxiliary).code != heif_error_Ok) {
      continue;
    }
    HeifHandlePtr auxiliary(raw_auxiliary);
    const char* type = nullptr;
    if (heif_image_handle_get_auxiliary_type(auxiliary.get(), &type).code == heif_error_Ok && type) {
      const std::string auxiliary_type(type);
      heif_image_handle_release_auxiliary_type(auxiliary.get(), &type);
      if (auxiliary_type == kAppleHdrGainMapUrn) {
        probe.detected = true;
        probe.auxiliary_type = auxiliary_type;
        probe.item_id = ids[static_cast<size_t>(i)];
        probe.width = static_cast<uint32_t>(heif_image_handle_get_width(auxiliary.get()));
        probe.height = static_cast<uint32_t>(heif_image_handle_get_height(auxiliary.get()));
        probe.bit_depth = static_cast<uint32_t>(std::max(
            heif_image_handle_get_luma_bits_per_pixel(auxiliary.get()), 0));
        probe.extraction_supported = true;
        return probe;
      }
    }
  }
  return probe;
}

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

SourceInfo inspect_apple_heif_gainmap(const std::filesystem::path& path) {
  const AppleAuxiliaryProbe probe = probe_apple_heif(path);
  if (!probe.detected) throw std::runtime_error("Apple HDR auxiliary gain map was not found");
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  require_heif(heif_context_read_from_file(context.get(), path.string().c_str(), nullptr),
               "inspect Apple HDR HEIC");
  heif_image_handle* raw_primary = nullptr;
  require_heif(heif_context_get_primary_image_handle(context.get(), &raw_primary),
               "inspect Apple HDR HEIC primary image");
  HeifHandlePtr primary(raw_primary);
  const ItemGraph graph = read_item_graph(path);
  if (!graph.tone_map || graph.base != heif_image_handle_get_item_id(primary.get()) ||
      graph.gain != probe.item_id) {
    throw std::runtime_error("Apple HEIC tmap/base/gain item relationship is inconsistent");
  }
  const auto raw_metadata = item_data(context.get(), graph.tone_map);
  if (raw_metadata.size() < 2 || raw_metadata[0] != 0) {
    throw std::runtime_error("unsupported Apple HEIC tmap item-data version");
  }
  const IsoGainMapMetadata metadata = parse_iso_gain_map_metadata(
      raw_metadata.data() + 1, raw_metadata.size() - 1);
  SourceInfo info;
  info.path = path;
  info.format = "Apple HDR HEIC";
  info.container_brand = "HEIF/HEIC + tmap";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "apple-auxiliary-tmap";
  info.base_item_id = graph.base;
  info.gain_map_item_id = graph.gain;
  info.tone_map_item_id = graph.tone_map;
  info.auxiliary_type = probe.auxiliary_type;
  info.codec = "HEVC SDR base + Apple auxiliary HEVC gain map + ISO tmap metadata";
  info.color_signal_kind = "HEIF ICC / auxiliary tmap metadata";
  info.pixel_format = "HEVC SDR base + monochrome auxiliary gain map";
  info.bit_depth = static_cast<uint32_t>(std::max(
      heif_image_handle_get_luma_bits_per_pixel(primary.get()), 0));
  info.chroma = "SDR RGB base + monochrome gain map";
  info.original_orientation = orientation_from_heif(context.get(), primary.get());
  info.orientation_normalized = true;
  uint32_t storage_width = static_cast<uint32_t>(heif_image_handle_get_ispe_width(primary.get()));
  uint32_t storage_height = static_cast<uint32_t>(heif_image_handle_get_ispe_height(primary.get()));
  if (!storage_width || !storage_height) {
    storage_width = static_cast<uint32_t>(heif_image_handle_get_width(primary.get()));
    storage_height = static_cast<uint32_t>(heif_image_handle_get_height(primary.get()));
  }
  info.width = info.original_orientation >= 5 ? storage_height : storage_width;
  info.height = info.original_orientation >= 5 ? storage_width : storage_height;
  info.base_width = info.width;
  info.base_height = info.height;
  info.base_bit_depth = info.bit_depth;
  info.base_channels = 3;
  info.base_codec = "HEVC";
  info.base_color_space = "Display P3";
  info.base_transfer = "SDR gamma";
  info.primaries = 12;
  info.transfer = 13;
  info.matrix = 0;
  info.full_range = true;
  info.range_known = true;
  const uint32_t map_width = probe.width;
  const uint32_t map_height = probe.height;
  // libheif reports auxiliary presentation dimensions; normalize explicitly
  // only when the values are storage-oriented.
  info.gain_map_width = map_width;
  info.gain_map_height = map_height;
  info.gain_map_channels = 1;
  if (map_width) info.gain_map_scale_x =
      info.base_width / static_cast<double>(map_width);
  if (map_height) info.gain_map_scale_y =
      info.base_height / static_cast<double>(map_height);
  info.base_hdr_headroom = metadata.base_hdr_headroom;
  info.alternate_hdr_headroom = metadata.alternate_hdr_headroom;
  info.hdr_capacity_min = std::exp2(info.base_hdr_headroom);
  info.hdr_capacity_max = std::exp2(info.alternate_hdr_headroom);
  info.gain_map_min = metadata.minimum;
  info.gain_map_max = metadata.maximum;
  info.gain_map_gamma = metadata.gamma;
  info.base_offset = metadata.base_offset;
  info.alternate_offset = metadata.alternate_offset;
  info.gain_map_uses_base_color_space = metadata.use_base_color_space;
  const size_t icc_size = heif_image_handle_get_raw_color_profile_size(primary.get());
  if (icc_size) {
    info.icc_present = true;
    std::vector<uint8_t> icc(icc_size);
    require_heif(heif_image_handle_get_raw_color_profile(primary.get(), icc.data()),
                 "inspect Apple HEIC SDR base ICC profile");
    info.profile = profile_description(icc.data(), static_cast<uint32_t>(icc.size())) +
                   " SDR base + Apple HDR gain map";
    info.base_color_space = profile_description(
        icc.data(), static_cast<uint32_t>(icc.size()));
  }
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  load_heif_apple_metadata(context.get(), primary.get(), exif, xmp);
  info.exif_present = !exif.empty();
  info.xmp_present = !xmp.empty();
  return info;
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

std::vector<uint16_t> extract_apple_heif_auxiliary_plane(
    const std::filesystem::path& path,
    const AppleAuxiliaryProbe& probe,
    std::atomic_bool* cancel) {
  if (!probe.detected || !probe.extraction_supported || probe.item_id == 0) {
    throw std::runtime_error("Apple HEIF auxiliary descriptor is not extractable");
  }
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  require_heif(heif_context_read_from_file(context.get(), path.string().c_str(), nullptr),
               "read Apple gain-map HEIF");
  heif_image_handle* raw_primary = nullptr;
  require_heif(heif_context_get_primary_image_handle(context.get(), &raw_primary),
               "get Apple gain-map primary image");
  HeifHandlePtr primary(raw_primary);
  heif_image_handle* raw_auxiliary = nullptr;
  require_heif(heif_image_handle_get_auxiliary_image_handle(
                   primary.get(), probe.item_id, &raw_auxiliary),
               "extract Apple HDR gain-map auxiliary handle");
  HeifHandlePtr auxiliary(raw_auxiliary);
  HeifOptionsPtr options(heif_decoding_options_alloc());
  if (!options) throw std::bad_alloc();
  options->convert_hdr_to_8bit = 0;
  options->ignore_transformations = 1;
  heif_image* raw_image = nullptr;
  require_heif(heif_decode_image(auxiliary.get(), &raw_image,
                                 heif_colorspace_monochrome,
                                 heif_chroma_monochrome, options.get()),
               "decode Apple HDR gain-map auxiliary plane");
  HeifImagePtr image(raw_image);
  int stride = 0;
  const uint8_t* plane = heif_image_get_plane_readonly(image.get(), heif_channel_Y, &stride);
  if (!plane) throw std::runtime_error("Apple HDR gain-map auxiliary plane is missing");
  const uint32_t width = static_cast<uint32_t>(heif_image_get_width(image.get(), heif_channel_Y));
  const uint32_t height = static_cast<uint32_t>(heif_image_get_height(image.get(), heif_channel_Y));
  const int bits = heif_image_get_bits_per_pixel_range(image.get(), heif_channel_Y);
  const uint32_t source_max = bits >= 16 ? 65535u : (1u << std::max(bits, 1)) - 1u;
  std::vector<uint16_t> result(static_cast<size_t>(width) * height);
  for (uint32_t y = 0; y < height; ++y) {
    if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
    if (bits <= 8) {
      const uint8_t* row = plane + static_cast<size_t>(y) * stride;
      for (uint32_t x = 0; x < width; ++x) {
        result[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(
            (static_cast<uint32_t>(row[x]) * 65535u + source_max / 2u) / source_max);
      }
    } else {
      const auto* row = reinterpret_cast<const uint16_t*>(plane + static_cast<size_t>(y) * stride);
      for (uint32_t x = 0; x < width; ++x) {
        result[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(
            (static_cast<uint32_t>(row[x]) * 65535u + source_max / 2u) / source_max);
      }
    }
  }
  // Apple auxiliary images are decoded as storage rasters. Apply the primary
  // item's presentation transform to the auxiliary plane so a future
  // reconstruction adapter cannot misregister portrait base and gain map.
  orientation::normalize_plane16(result, width, height,
                                 orientation_from_heif(context.get(), primary.get()));
  return result;
}

GainMapAsset extract_apple_heif_gainmap_asset(
    const std::filesystem::path& path, std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  const auto parse_start = Clock::now();
  const int worker_count = static_cast<int>(codec_worker_count());
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
#if defined(__EMSCRIPTEN__)
  heif_context_set_max_decoding_threads(context.get(), 0);
#else
  heif_context_set_max_decoding_threads(context.get(), worker_count);
#endif
  require_heif(heif_context_read_from_file(context.get(), path.string().c_str(), nullptr),
               "read Apple HDR HEIC");
  heif_image_handle* raw_primary = nullptr;
  require_heif(heif_context_get_primary_image_handle(context.get(), &raw_primary),
               "get Apple HDR HEIC primary image");
  HeifHandlePtr primary(raw_primary);
  const int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA |
                     LIBHEIF_AUX_IMAGE_FILTER_OMIT_DEPTH;
  const int auxiliary_count = heif_image_handle_get_number_of_auxiliary_images(
      primary.get(), filter);
  std::vector<heif_item_id> auxiliary_ids(
      static_cast<size_t>(std::max(auxiliary_count, 0)));
  const int auxiliary_got = auxiliary_ids.empty() ? 0 :
      heif_image_handle_get_list_of_auxiliary_image_IDs(
          primary.get(), filter, auxiliary_ids.data(),
          static_cast<int>(auxiliary_ids.size()));
  HeifHandlePtr auxiliary;
  heif_item_id auxiliary_id = 0;
  for (int index = 0; index < auxiliary_got; ++index) {
    heif_image_handle* raw_candidate = nullptr;
    if (heif_image_handle_get_auxiliary_image_handle(
            primary.get(), auxiliary_ids[static_cast<size_t>(index)],
            &raw_candidate).code != heif_error_Ok) {
      continue;
    }
    HeifHandlePtr candidate(raw_candidate);
    const char* type = nullptr;
    if (heif_image_handle_get_auxiliary_type(candidate.get(), &type).code !=
            heif_error_Ok || !type) {
      continue;
    }
    const bool is_hdr_gain_map = std::strcmp(type, kAppleHdrGainMapUrn) == 0;
    heif_image_handle_release_auxiliary_type(candidate.get(), &type);
    if (is_hdr_gain_map) {
      auxiliary_id = auxiliary_ids[static_cast<size_t>(index)];
      auxiliary = std::move(candidate);
      break;
    }
  }
  if (!auxiliary) {
    throw std::runtime_error("Apple HDR auxiliary gain map was not found");
  }

  const ItemGraph graph = read_item_graph(context.get());
  if (!graph.tone_map || graph.base != heif_image_handle_get_item_id(primary.get()) ||
      graph.gain != auxiliary_id) {
    throw std::runtime_error("Apple HEIC tmap/base/gain item relationship is inconsistent");
  }
  const auto raw_metadata = item_data(context.get(), graph.tone_map);
  // The HEIF tmap item carries a one-byte item-data version before the
  // ISO 21496-1 GainMapMetadata payload. libheif's newer convenience API
  // strips this byte; the generic item API used by our fixed dependency does
  // not, so reject unknown versions and parse the remaining bytes explicitly.
  if (raw_metadata.size() < 2 || raw_metadata[0] != 0) {
    throw std::runtime_error("unsupported Apple HEIC tmap item-data version");
  }
  const IsoGainMapMetadata metadata = parse_iso_gain_map_metadata(
      raw_metadata.data() + 1, raw_metadata.size() - 1);
  if (!metadata.use_base_color_space) {
    throw std::runtime_error("Apple HEIC gain map requires an unsupported alternate application color space");
  }

  GainMapAsset asset;
  asset.reference_white_nits = adaptive_tmap_reference_white_nits(path).value_or(203.0);
  auto& info = asset.info;
  info.path = path;
  info.format = "Apple HDR HEIC";
  info.container_brand = "HEIF/HEIC + tmap";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "apple-auxiliary-tmap";
  info.base_item_id = graph.base;
  info.gain_map_item_id = auxiliary_id;
  info.tone_map_item_id = graph.tone_map;
  info.auxiliary_type = kAppleHdrGainMapUrn;
  info.codec = "HEVC SDR base + Apple auxiliary HEVC gain map + ISO tmap metadata";
  info.bit_depth = static_cast<uint32_t>(std::max(
      heif_image_handle_get_luma_bits_per_pixel(primary.get()), 0));
  info.chroma = "SDR RGB base + monochrome gain map";
  info.original_orientation = orientation_from_heif(context.get(), primary.get());
  info.orientation_normalized = true;
  info.base_hdr_headroom = metadata.base_hdr_headroom;
  info.alternate_hdr_headroom = metadata.alternate_hdr_headroom;
  info.gain_map_min = metadata.minimum;
  info.gain_map_max = metadata.maximum;
  info.gain_map_gamma = metadata.gamma;
  info.base_offset = metadata.base_offset;
  info.alternate_offset = metadata.alternate_offset;
  info.gain_map_uses_base_color_space = metadata.use_base_color_space;

  asset.storage_width = static_cast<uint32_t>(heif_image_handle_get_ispe_width(primary.get()));
  asset.storage_height = static_cast<uint32_t>(heif_image_handle_get_ispe_height(primary.get()));
  if (!asset.storage_width || !asset.storage_height) {
    asset.storage_width = static_cast<uint32_t>(heif_image_handle_get_width(primary.get()));
    asset.storage_height = static_cast<uint32_t>(heif_image_handle_get_height(primary.get()));
  }
  info.width = info.original_orientation >= 5 ? asset.storage_height : asset.storage_width;
  info.height = info.original_orientation >= 5 ? asset.storage_width : asset.storage_height;

  const size_t icc_size = heif_image_handle_get_raw_color_profile_size(primary.get());
  if (icc_size) {
    asset.base_icc.resize(icc_size);
    require_heif(heif_image_handle_get_raw_color_profile(primary.get(), asset.base_icc.data()),
                 "read Apple HEIC SDR base ICC profile");
    info.profile = profile_description(asset.base_icc.data(),
                                       static_cast<uint32_t>(asset.base_icc.size())) +
                   " SDR base + Apple HDR gain map";
  }
  if (asset.base_icc.empty()) {
    throw std::runtime_error("Apple HEIC SDR base ICC profile is missing");
  }
  load_heif_apple_metadata(context.get(), primary.get(), asset.exif, asset.xmp);
  info.exif_present = !asset.exif.empty();
  info.xmp_present = !asset.xmp.empty();
  asset.container_parse_ms = elapsed_ms(parse_start);

  HeifOptionsPtr options(heif_decoding_options_alloc());
  if (!options) throw std::bad_alloc();
  options->convert_hdr_to_8bit = 0;
  options->ignore_transformations = 1;
  options->output_image_nclx_profile_passthrough = 1;
  options->num_codec_threads = worker_count;
  options->progress_user_data = cancel;
  options->cancel_decoding = [](void* user) -> int {
    return user && static_cast<std::atomic_bool*>(user)->load() ? 1 : 0;
  };
  const auto base_decode_start = Clock::now();
  heif_image* raw_base = nullptr;
  require_heif(heif_decode_image(primary.get(), &raw_base, heif_colorspace_RGB,
                                 heif_chroma_interleaved_RRGGBB_LE, options.get()),
               "decode Apple HEIC SDR base");
  HeifImagePtr base(raw_base);
  int base_stride = 0;
  const uint8_t* base_plane = heif_image_get_plane_readonly(
      base.get(), heif_channel_interleaved, &base_stride);
  if (!base_plane || base_stride < static_cast<int>(asset.storage_width * 6u)) {
    throw std::runtime_error("Apple HEIC SDR base decode plane is invalid");
  }
  asset.base_rgb16.resize(static_cast<size_t>(asset.storage_width) * asset.storage_height * 3u);
  const int decoded_base_bits = heif_image_get_bits_per_pixel_range(
      base.get(), heif_channel_interleaved);
  if (decoded_base_bits <= 0 || decoded_base_bits > 16) {
    throw std::runtime_error("Apple HEIC SDR base has an invalid decoded bit depth");
  }
  info.bit_depth = static_cast<uint32_t>(decoded_base_bits);
  const uint32_t source_max = decoded_base_bits < 16
      ? (1u << decoded_base_bits) - 1u : 65535u;
  for (uint32_t y = 0; y < asset.storage_height; ++y) {
    if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
    const auto* source = reinterpret_cast<const uint16_t*>(
        base_plane + static_cast<size_t>(y) * base_stride);
    auto* target = asset.base_rgb16.data() + static_cast<size_t>(y) * asset.storage_width * 3u;
    for (size_t sample = 0; sample < static_cast<size_t>(asset.storage_width) * 3u; ++sample) {
      target[sample] = source_max == 65535u ? source[sample] : static_cast<uint16_t>(
          (static_cast<uint32_t>(source[sample]) * 65535u + source_max / 2u) / source_max);
    }
  }
  asset.base_decode_ms = elapsed_ms(base_decode_start);

  const auto map_decode_start = Clock::now();
  heif_image* raw_map = nullptr;
  require_heif(heif_decode_image(auxiliary.get(), &raw_map, heif_colorspace_monochrome,
                                 heif_chroma_monochrome, options.get()),
               "decode Apple HEIC auxiliary gain map");
  HeifImagePtr map(raw_map);
  int map_stride = 0;
  const uint8_t* map_plane = heif_image_get_plane_readonly(map.get(), heif_channel_Y, &map_stride);
  asset.map_storage_width = static_cast<uint32_t>(heif_image_get_width(map.get(), heif_channel_Y));
  asset.map_storage_height = static_cast<uint32_t>(heif_image_get_height(map.get(), heif_channel_Y));
  const int map_bits = heif_image_get_bits_per_pixel_range(map.get(), heif_channel_Y);
  if (!map_plane || !asset.map_storage_width || !asset.map_storage_height) {
    throw std::runtime_error("Apple HEIC auxiliary gain-map decode plane is invalid");
  }
  const uint32_t map_max = map_bits > 0 && map_bits < 16 ? (1u << map_bits) - 1u : 65535u;
  asset.gain_map_rgb16.resize(
      static_cast<size_t>(asset.map_storage_width) * asset.map_storage_height);
  for (uint32_t y = 0; y < asset.map_storage_height; ++y) {
    const uint8_t* row8 = map_plane + static_cast<size_t>(y) * map_stride;
    const auto* row16 = reinterpret_cast<const uint16_t*>(row8);
    for (uint32_t x = 0; x < asset.map_storage_width; ++x) {
      const uint32_t value = map_bits <= 8 ? row8[x] : row16[x];
      asset.gain_map_rgb16[static_cast<size_t>(y) * asset.map_storage_width + x] =
          static_cast<uint16_t>((value * 65535u + map_max / 2u) / map_max);
    }
  }
  info.gain_map_width = info.original_orientation >= 5
      ? asset.map_storage_height : asset.map_storage_width;
  info.gain_map_height = info.original_orientation >= 5
      ? asset.map_storage_width : asset.map_storage_height;
  asset.gain_map_decode_ms = elapsed_ms(map_decode_start);
  return asset;
}

ReconstructedHdr reconstruct_apple_heif_gainmap(
    const std::filesystem::path& path, std::atomic_bool* cancel) {
  const auto decode_start = Clock::now();
  GainMapAsset asset = extract_apple_heif_gainmap_asset(path, cancel);
  ReconstructedHdr output;
  output.info = asset.info;
  output.exif = std::move(asset.exif);
  output.xmp = std::move(asset.xmp);
  if (contains_ascii(output.xmp, "HDRGainMapVersion") ||
      contains_ascii(output.xmp, kAppleHdrGainMapUrn)) {
    // This packet describes the source auxiliary relationship. Copying it to
    // a new direct-HDR container would leave stale gain-map signaling.
    output.xmp.clear();
  }
  output.decode_ms = elapsed_ms(decode_start);
  output.base_decode_ms = asset.base_decode_ms;
  output.gain_map_decode_ms = asset.gain_map_decode_ms;

  CmsProfilePtr profile(cmsOpenProfileFromMem(
      asset.base_icc.data(), static_cast<cmsUInt32Number>(asset.base_icc.size())));
  if (!profile) throw std::runtime_error("cannot parse Apple HEIC SDR base ICC profile");
  const std::string description = profile_description(
      asset.base_icc.data(), static_cast<uint32_t>(asset.base_icc.size()));
  if (description.find("P3") == std::string::npos) {
    throw std::runtime_error("Apple HEIC SDR base ICC is not a validated Display P3 profile");
  }
  if (cmsIsTag(profile.get(), cmsSigAToB0Tag) ||
      cmsIsTag(profile.get(), cmsSigBToA0Tag) ||
      cmsIsTag(profile.get(), static_cast<cmsTagSignature>(0x6864676du))) {
    throw std::runtime_error(
        "Apple HEIC base resolved to the Adaptive HDR tmap ICC instead of the SDR Display P3 ICC");
  }
  std::array<cmsToneCurve*, 3> curves{
      static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigRedTRCTag)),
      static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigGreenTRCTag)),
      static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigBlueTRCTag))};
  if (!curves[0] || !curves[1] || !curves[2]) {
    throw std::runtime_error("Apple HEIC SDR base ICC has no RGB transfer curves");
  }
  std::array<std::vector<double>, 3> inverse_trc;
  for (size_t channel = 0; channel < 3; ++channel) {
    inverse_trc[channel].resize(65536u);
    for (size_t code = 0; code < 65536u; ++code) {
      inverse_trc[channel][code] = cmsEvalToneCurveFloat(
          curves[channel], static_cast<float>(code / 65535.0));
    }
  }
  const auto reconstruction_start = Clock::now();
  const uint32_t width = asset.storage_width;
  const uint32_t height = asset.storage_height;
  output.rec2020_pq_rgb.resize(asset.base_rgb16.size());
  const double base_headroom = output.info.base_hdr_headroom;
  const double alternate_headroom = output.info.alternate_hdr_headroom;
  const double full_headroom = std::max(base_headroom, alternate_headroom);
  double weight = 0.0;
  if (alternate_headroom != base_headroom) {
    weight = std::clamp((full_headroom - base_headroom) /
                            (alternate_headroom - base_headroom), 0.0, 1.0);
    if (alternate_headroom < base_headroom) weight = -weight;
  }
  struct XSample {
    uint32_t lower = 0;
    uint32_t upper = 0;
    double fraction = 0.0;
  };
  std::vector<XSample> x_samples(width);
  for (uint32_t x = 0; x < width; ++x) {
    const double coordinate = (static_cast<double>(x) + 0.5) *
        asset.map_storage_width / width - 0.5;
    const double floor_coordinate = std::floor(coordinate);
    const int lower = std::clamp(static_cast<int>(floor_coordinate), 0,
                                 static_cast<int>(asset.map_storage_width) - 1);
    x_samples[x].lower = static_cast<uint32_t>(lower);
    x_samples[x].upper = static_cast<uint32_t>(std::min(
        lower + 1, static_cast<int>(asset.map_storage_width) - 1));
    x_samples[x].fraction = std::clamp(coordinate - floor_coordinate, 0.0, 1.0);
  }
  const bool shared_gain = output.info.gain_map_min[0] == output.info.gain_map_min[1] &&
      output.info.gain_map_min[0] == output.info.gain_map_min[2] &&
      output.info.gain_map_max[0] == output.info.gain_map_max[1] &&
      output.info.gain_map_max[0] == output.info.gain_map_max[2] &&
      output.info.gain_map_gamma[0] == output.info.gain_map_gamma[1] &&
      output.info.gain_map_gamma[0] == output.info.gain_map_gamma[2];
  struct WorkerState {
    std::vector<double> map_row;
    std::vector<std::array<double, 3>> p3_row;
    double maximum_linear = 0.0;
    long double sum_max_linear = 0.0;
    double upsample_cpu_ms = 0.0;
    double gain_cpu_ms = 0.0;
    double color_cpu_ms = 0.0;
  };
  const uint32_t worker_count = std::min<uint32_t>(codec_worker_count(), height);
  std::vector<WorkerState> states(worker_count);
  for (auto& state : states) {
    state.map_row.resize(width);
    state.p3_row.resize(width);
  }
  std::atomic<uint32_t> next_row{0};
  const auto process_rows = [&](uint32_t worker_index) {
    WorkerState& state = states[worker_index];
    while (true) {
      const uint32_t row_begin = next_row.fetch_add(4u);
      if (row_begin >= height) break;
      const uint32_t row_end = std::min(row_begin + 4u, height);
      for (uint32_t y = row_begin; y < row_end; ++y) {
        if (cancel && cancel->load()) return;
        const auto upsample_start = Clock::now();
        const double map_y = (static_cast<double>(y) + 0.5) *
            asset.map_storage_height / height - 0.5;
        const double map_y_floor = std::floor(map_y);
        const int y0 = std::clamp(static_cast<int>(map_y_floor), 0,
                                  static_cast<int>(asset.map_storage_height) - 1);
        const int y1 = std::min(y0 + 1, static_cast<int>(asset.map_storage_height) - 1);
        const double fy = std::clamp(map_y - map_y_floor, 0.0, 1.0);
        const auto* map0 = asset.gain_map_rgb16.data() +
            static_cast<size_t>(y0) * asset.map_storage_width;
        const auto* map1 = asset.gain_map_rgb16.data() +
            static_cast<size_t>(y1) * asset.map_storage_width;
        for (uint32_t x = 0; x < width; ++x) {
          const XSample& sx = x_samples[x];
          const double top = map0[sx.lower] * (1.0 - sx.fraction) +
                             map0[sx.upper] * sx.fraction;
          const double bottom = map1[sx.lower] * (1.0 - sx.fraction) +
                                map1[sx.upper] * sx.fraction;
          state.map_row[x] = (top * (1.0 - fy) + bottom * fy) / 65535.0;
        }
        state.upsample_cpu_ms += elapsed_ms(upsample_start);

        const auto gain_start = Clock::now();
        const size_t row_pixel = static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
          const size_t pixel = row_pixel + x;
          std::array<double, 3> factors{};
          const auto compute_factor = [&](size_t channel) {
            const double log_gain = output.info.gain_map_min[channel] +
                (output.info.gain_map_max[channel] - output.info.gain_map_min[channel]) *
                    std::pow(state.map_row[x], 1.0 / output.info.gain_map_gamma[channel]);
            return std::exp2(log_gain * weight);
          };
          if (shared_gain) {
            factors.fill(compute_factor(0));
          } else {
            for (size_t channel = 0; channel < 3; ++channel) {
              factors[channel] = compute_factor(channel);
            }
          }
          for (size_t channel = 0; channel < 3; ++channel) {
            const uint16_t base_code = asset.base_rgb16[pixel * 3u + channel];
            state.p3_row[x][channel] =
                (inverse_trc[channel][base_code] + output.info.base_offset[channel]) *
                    factors[channel] - output.info.alternate_offset[channel];
          }
        }
        state.gain_cpu_ms += elapsed_ms(gain_start);

        const auto color_start = Clock::now();
        for (uint32_t x = 0; x < width; ++x) {
          const size_t pixel = row_pixel + x;
          const auto& p3 = state.p3_row[x];
          const std::array<double, 3> rec2020{
              0.7538330 * p3[0] + 0.1985974 * p3[1] + 0.0475696 * p3[2],
              0.0457438 * p3[0] + 0.9417772 * p3[1] + 0.0124789 * p3[2],
             -0.0012103 * p3[0] + 0.0176017 * p3[1] + 0.9836086 * p3[2]};
          const double pixel_max = std::max({rec2020[0], rec2020[1], rec2020[2], 0.0});
          state.maximum_linear = std::max(state.maximum_linear, pixel_max);
          state.sum_max_linear += pixel_max;
          for (size_t channel = 0; channel < 3; ++channel) {
            output.rec2020_pq_rgb[pixel * 3u + channel] =
                static_cast<uint16_t>(std::llround(forward_pq(
                    std::clamp(rec2020[channel] * asset.reference_white_nits,
                               0.0, 10000.0)) * 65535.0));
          }
        }
        state.color_cpu_ms += elapsed_ms(color_start);
      }
    }
  };
  std::vector<std::thread> workers;
  workers.reserve(worker_count > 0 ? worker_count - 1u : 0u);
  for (uint32_t worker = 1; worker < worker_count; ++worker) {
    workers.emplace_back(process_rows, worker);
  }
  process_rows(0);
  for (auto& worker : workers) worker.join();
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  double maximum_linear = 0.0;
  long double sum_max_linear = 0.0;
  double upsample_cpu_ms = 0.0;
  double gain_cpu_ms = 0.0;
  double color_cpu_ms = 0.0;
  for (const auto& state : states) {
    maximum_linear = std::max(maximum_linear, state.maximum_linear);
    sum_max_linear += state.sum_max_linear;
    upsample_cpu_ms += state.upsample_cpu_ms;
    gain_cpu_ms += state.gain_cpu_ms;
    color_cpu_ms += state.color_cpu_ms;
  }
  output.reconstruction_ms = elapsed_ms(reconstruction_start);
  const double profiled_cpu_ms = upsample_cpu_ms + gain_cpu_ms + color_cpu_ms;
  if (profiled_cpu_ms > 0.0) {
    output.gain_map_upsample_ms = output.reconstruction_ms * upsample_cpu_ms / profiled_cpu_ms;
    output.gain_apply_ms = output.reconstruction_ms * gain_cpu_ms / profiled_cpu_ms;
    output.color_conversion_ms = output.reconstruction_ms * color_cpu_ms / profiled_cpu_ms;
  }
  output.max_cll = static_cast<uint16_t>(std::clamp(
      std::llround(maximum_linear * asset.reference_white_nits), 0ll, 65535ll));
  output.max_pall = static_cast<uint16_t>(std::clamp(std::llround(
      static_cast<double>(sum_max_linear /
          static_cast<long double>(static_cast<uint64_t>(width) * height)) *
              asset.reference_white_nits),
      0ll, 65535ll));
  const auto orientation_start = Clock::now();
  const auto transformed = orientation::normalize_rgb16(
      output.rec2020_pq_rgb, width, height, output.info.original_orientation);
  output.info.width = transformed.width;
  output.info.height = transformed.height;
  output.info.orientation_normalized = true;
  orientation::set_exif_orientation_to_one(output.exif);
  orientation::set_xmp_orientation_to_one(output.xmp);
  output.orientation_ms = elapsed_ms(orientation_start);
  return output;
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
              half_to_float_local(row[static_cast<size_t>(x) * 4u]) * 203.0,
              half_to_float_local(row[static_cast<size_t>(x) * 4u + 1u]) * 203.0,
              half_to_float_local(row[static_cast<size_t>(x) * 4u + 2u]) * 203.0};
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

}  // namespace hdrbridge::gainmap
