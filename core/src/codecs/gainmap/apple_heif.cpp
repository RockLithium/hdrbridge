#include "codecs/gainmap/apple_heif.h"
#include "codecs/gainmap/bmff_items.h"
#include "codecs/gainmap/icc_description.h"
#include "codecs/gainmap/iso_metadata.h"
#include "codecs/gainmap/decode_support.h"
#include "codecs/gainmap/extract_support.h"
#include "codecs/gainmap/reconstruction_math.h"
#include "color/icc_rgb_primaries.h"
#include "image/orientation.h"
#include <libheif/heif_items.h>
#include <libheif/heif_properties.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <thread>

namespace hdrbridge::gainmap {

using namespace detail;

namespace {

// Container-specific Apple inputs are kept separate until they have been
// decoded to this neutral gain-map asset.  Adobe tmap and Ultra HDR metadata
// are deliberately not represented by this type.
struct GainMapAsset {
  SourceInfo info;
  uint32_t storage_width = 0;
  uint32_t storage_height = 0;
  uint32_t map_storage_width = 0;
  uint32_t map_storage_height = 0;
  std::vector<uint16_t> base_rgb16;
  std::vector<uint16_t> gain_map_rgb16;
  std::vector<uint8_t> base_icc;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  double container_parse_ms = 0.0;
  double base_decode_ms = 0.0;
  double gain_map_decode_ms = 0.0;
  double reference_white_nits = 203.0;
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

AppleAuxiliaryProbe probe_apple_heif(const std::filesystem::path& path) {
  AppleAuxiliaryProbe probe;
  probe.container = "HEIF/HEIC";
  const auto bytes = read_file(path);
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  const heif_error read = heif_context_read_from_memory_without_copy(
      context.get(), bytes.data(), bytes.size(), nullptr);
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

SourceInfo inspect_apple_heif_gainmap(const std::filesystem::path& path) {
  const AppleAuxiliaryProbe probe = probe_apple_heif(path);
  if (!probe.detected) throw std::runtime_error("Apple HDR auxiliary gain map was not found");
  const auto bytes = read_file(path);
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  require_heif(heif_context_read_from_memory_without_copy(
                   context.get(), bytes.data(), bytes.size(), nullptr),
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

static std::vector<uint16_t> extract_apple_heif_auxiliary_plane(
    const std::filesystem::path& path,
    const AppleAuxiliaryProbe& probe,
    std::atomic_bool* cancel) {
  if (!probe.detected || !probe.extraction_supported || probe.item_id == 0) {
    throw std::runtime_error("Apple HEIF auxiliary descriptor is not extractable");
  }
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  const auto bytes = read_file(path);
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  require_heif(heif_context_read_from_memory_without_copy(
                   context.get(), bytes.data(), bytes.size(), nullptr),
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

static GainMapAsset extract_apple_heif_gainmap_asset(
    const std::filesystem::path& path, std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  const auto parse_start = Clock::now();
  const int worker_count = static_cast<int>(codec_worker_count());
  const auto bytes = read_file(path);
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  heif_context_set_max_decoding_threads(context.get(), codec_background_workers());
  require_heif(heif_context_read_from_memory_without_copy(
                   context.get(), bytes.data(), bytes.size(), nullptr),
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
  const auto codec_threads = [&](const heif_image_handle* handle) {
    heif_image_tiling tiling{};
    tiling.version = 1;
    const bool tiled = heif_image_handle_get_image_tiling(handle, 0, &tiling).code == heif_error_Ok &&
        (tiling.num_columns > 1 || tiling.num_rows > 1);
    // A tiled image already uses the context's worker pool.
    return tiled ? 1 : worker_count;
  };
  options->num_codec_threads = codec_threads(primary.get());
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
  options->num_codec_threads = codec_threads(auxiliary.get());
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

GainMapRaster extract_heif_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel) {
  const auto begin = Clock::now();
  auto asset = extract_apple_heif_gainmap_asset(path, cancel);
  const uint32_t width = asset.map_storage_width;
  const uint32_t height = asset.map_storage_height;
  std::vector<uint16_t> map = std::move(asset.gain_map_rgb16);
  auto info = std::move(asset.info);
  auto result = make_raster(std::move(info), width, height, 1u, std::move(map),
                            "Apple auxiliary HDR gain-map image", cancel);
  normalize_gainmap_orientation(result, result.info.original_orientation);
  result.decode_ms = elapsed_ms(begin);
  return result;
}

}  // namespace hdrbridge::gainmap
