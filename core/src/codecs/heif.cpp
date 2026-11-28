#include "codecs/heif.h"
#include "codecs/heif_helpers.h"
#include "codecs/gainmap/heif.h"
#include "color/gamut.h"
#include "color/pq.h"
#include "color/signaling.h"
#include "image/orientation.h"
#include "infrastructure/files.h"
#include "infrastructure/big_endian.h"
#include "infrastructure/timing.h"
#include "infrastructure/progress.h"
#include "infrastructure/pixel_workers.h"
#include "metadata/source_metadata.h"
#include <libheif/heif_properties.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace hdrbridge::detail {

void require_heif(const heif_error& e, const char* operation) {
  if (e.code != heif_error_Ok) {
    throw std::runtime_error(std::string(operation) + ": " + (e.message ? e.message : "libheif error"));
  }
}

static void inspect_container_bits(const std::vector<uint8_t>& bytes, SourceInfo& info) {
  if (bytes.size() >= 12 && std::memcmp(bytes.data() + 4, "ftyp", 4) == 0) {
    info.container_brand.assign(reinterpret_cast<const char*>(bytes.data() + 8), 4);
    const uint32_t size = be32(bytes.data());
    if (size >= 16 && size <= bytes.size()) {
      for (size_t offset = 16; offset + 4 <= size; offset += 4) {
        if (std::memcmp(bytes.data() + offset, "tmap", 4) == 0) {
          info.gain_map_present = true;
          info.asset_kind = "gain-map-hdr";
        }
      }
    }
  }
  for (size_t i = 4; i + 26 < bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, "hvcC", 4) != 0) continue;
    const uint32_t box_size = be32(bytes.data() + i - 4);
    if (box_size < 30 || i + box_size - 4 > bytes.size()) continue;
    const uint8_t* h = bytes.data() + i + 4;
    const uint8_t profile_idc = h[1] & 0x1f;
    info.profile = profile_idc == 4 ? "HEVC Range Extensions" : "HEVC profile " + std::to_string(profile_idc);
    const uint8_t chroma = h[16] & 0x03;
    info.chroma = chroma == 1 ? "4:2:0" : chroma == 2 ? "4:2:2" : chroma == 3 ? "4:4:4" : "monochrome";
    info.bit_depth = static_cast<uint32_t>(8 + (h[17] & 0x07));
    info.codec = "HEVC";
    break;
  }
  for (size_t i = 4; i + 8 < bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, "av1C", 4) != 0) continue;
    const uint32_t box_size = be32(bytes.data() + i - 4);
    if (box_size < 12 || i + box_size - 4 > bytes.size()) continue;
    const uint8_t* config = bytes.data() + i + 4;
    const uint8_t profile = static_cast<uint8_t>((config[1] >> 5) & 0x07);
    const uint8_t flags = config[2];
    const bool high_bitdepth = (flags & 0x40) != 0;
    const bool twelve_bit = (flags & 0x20) != 0;
    const bool monochrome = (flags & 0x10) != 0;
    const bool subsampling_x = (flags & 0x08) != 0;
    const bool subsampling_y = (flags & 0x04) != 0;
    info.codec = "AV1";
    info.profile = profile == 0 ? "AV1 Main" : profile == 1 ? "AV1 High" : "AV1 Professional";
    info.bit_depth = high_bitdepth ? (twelve_bit ? 12u : 10u) : 8u;
    info.chroma = monochrome ? "monochrome" : !subsampling_x && !subsampling_y ? "4:4:4" :
                  subsampling_x && !subsampling_y ? "4:2:2" : "4:2:0";
    break;
  }
}

OpenedHeif open_heif_bytes(std::vector<uint8_t> bytes) {
  ContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  heif_context_set_max_decoding_threads(context.get(), codec_background_workers());
  require_heif(heif_context_read_from_memory_without_copy(
                   context.get(), bytes.data(), bytes.size(), nullptr),
               "read HEIF");
  heif_image_handle* raw = nullptr;
  require_heif(heif_context_get_primary_image_handle(context.get(), &raw), "get primary image");
  return {std::move(bytes), std::move(context), HandlePtr(raw)};
}

OpenedHeif open_heif(const std::filesystem::path& path) {
  return open_heif_bytes(read_file(path));
}

static uint8_t heif_container_orientation(const OpenedHeif& opened) {
  const heif_item_id item_id = heif_image_handle_get_item_id(opened.handle.get());
  const int rotation = heif_item_get_property_transform_rotation_ccw(
      opened.context.get(), item_id, 0);
  const auto mirror = heif_item_get_property_transform_mirror(
      opened.context.get(), item_id, 0);
  const bool mirrored = mirror != heif_transform_mirror_direction_invalid;
  const bool horizontal_axis = mirror == heif_transform_mirror_direction_horizontal;
  if (rotation == 90) {
    if (mirrored) return horizontal_axis ? 7 : 5;
    return 8;
  }
  if (rotation == 180) {
    if (mirrored) return horizontal_axis ? 4 : 2;
    return 3;
  }
  if (rotation == 270) {
    if (mirrored) return horizontal_axis ? 5 : 7;
    return 6;
  }
  if (mirrored) return horizontal_axis ? 2 : 4;
  return 1;
}

static void load_metadata(const heif_image_handle* handle, SourceInfo& info,
                   std::vector<uint8_t>* exif, std::vector<uint8_t>* xmp) {
  info.exif_status = "absent";
  info.xmp_status = "absent";
  const int count = heif_image_handle_get_number_of_metadata_blocks(handle, nullptr);
  std::vector<heif_item_id> ids(static_cast<size_t>(std::max(count, 0)));
  const int got = ids.empty() ? 0 : heif_image_handle_get_list_of_metadata_block_IDs(handle, nullptr, ids.data(), count);
  for (int i = 0; i < got; ++i) {
    const char* type = heif_image_handle_get_metadata_type(handle, ids[static_cast<size_t>(i)]);
    const char* content = heif_image_handle_get_metadata_content_type(handle, ids[static_cast<size_t>(i)]);
    const bool is_exif = type && std::strcmp(type, "Exif") == 0;
    const bool is_xmp = (type && std::strcmp(type, "mime") == 0 && content &&
                         (std::strstr(content, "rdf") || std::strstr(content, "xml")));
    info.exif_present = info.exif_present || is_exif;
    info.xmp_present = info.xmp_present || is_xmp;
    if (is_exif) info.exif_status = "present";
    if (is_xmp) info.xmp_status = "present";
    auto* target = is_exif ? exif : is_xmp ? xmp : nullptr;
    if (target && target->empty()) {
      target->resize(heif_image_handle_get_metadata_size(handle, ids[static_cast<size_t>(i)]));
      if (!target->empty()) {
        const heif_error error = heif_image_handle_get_metadata(
            handle, ids[static_cast<size_t>(i)], target->data());
        if (error.code != heif_error_Ok) {
          target->clear();
          if (is_exif) info.exif_status = "read-error";
          if (is_xmp) info.xmp_status = "read-error";
        }
      }
    }
  }
}

SourceInfo inspect_opened(const std::filesystem::path& path, const OpenedHeif& opened) {
  SourceInfo info;
  info.path = path;
  // ispe reports the storage raster before irot/imir. The ordinary width API
  // reports presentation dimensions, which must not be used when decoding
  // with transformations disabled.
  const int storage_width = heif_image_handle_get_ispe_width(opened.handle.get());
  const int storage_height = heif_image_handle_get_ispe_height(opened.handle.get());
  info.width = static_cast<uint32_t>(storage_width > 0 ? storage_width
      : heif_image_handle_get_width(opened.handle.get()));
  info.height = static_cast<uint32_t>(storage_height > 0 ? storage_height
      : heif_image_handle_get_height(opened.handle.get()));
  info.bit_depth = static_cast<uint32_t>(std::max(heif_image_handle_get_luma_bits_per_pixel(opened.handle.get()), 0));

  heif_image_tiling tiling{};
  tiling.version = 1;
  const heif_error tile_error = heif_image_handle_get_image_tiling(opened.handle.get(), 1, &tiling);
  if (tile_error.code == heif_error_Ok) {
    info.grid_columns = tiling.num_columns;
    info.grid_rows = tiling.num_rows;
    info.tile_width = tiling.tile_width;
    info.tile_height = tiling.tile_height;
    info.is_grid = tiling.num_columns > 1 || tiling.num_rows > 1;
  }

  heif_color_profile_nclx* raw_nclx = nullptr;
  const heif_error color_error = heif_image_handle_get_nclx_color_profile(opened.handle.get(), &raw_nclx);
  NclxPtr nclx(raw_nclx);
  if (color_error.code == heif_error_Ok && nclx) {
    info.native_color_present = true;
    info.native_primaries = static_cast<uint16_t>(nclx->color_primaries);
    info.native_transfer = static_cast<uint16_t>(nclx->transfer_characteristics);
    info.native_matrix = static_cast<uint16_t>(nclx->matrix_coefficients);
    info.native_full_range = nclx->full_range_flag != 0;
    info.native_range_known = true;
    info.native_color_description = "CICP / NCLX";
  }
  std::vector<uint8_t> source_exif;
  load_metadata(opened.handle.get(), info, &source_exif, nullptr);
  const size_t raw_profile_size = heif_image_handle_get_raw_color_profile_size(opened.handle.get());
  std::vector<uint8_t> raw_profile(raw_profile_size);
  if (!raw_profile.empty()) {
    const heif_error raw_error = heif_image_handle_get_raw_color_profile(
        opened.handle.get(), raw_profile.data());
    if (raw_error.code != heif_error_Ok) raw_profile.clear();
  }
  store_icc_signal(info, parse_icc_signal(raw_profile.data(), raw_profile.size()));
  info.original_orientation = heif_container_orientation(opened);
  const bool exif_orientation = orientation::has_exif_orientation(source_exif);
  if (info.original_orientation == 1 && exif_orientation) {
    info.original_orientation = orientation::read_exif_orientation(source_exif);
  }
  info.orientation_status = (info.original_orientation != 1 || exif_orientation)
      ? "present" : "absent";
  if ((storage_width <= 0 || storage_height <= 0) && info.original_orientation >= 5) {
    std::swap(info.width, info.height);
  }
  info.orientation_normalized = true;
  inspect_container_bits(opened.bytes, info);
  info.format = info.container_brand == "avif" ? "AVIF" : "HEIF/HIF";
  resolve_color_signaling(info, false);
  info.color_signal_kind = info.resolved_signaling_source;
  info.pixel_format = info.chroma == "monochrome"
      ? "monochrome" : "YCbCr " + info.chroma;
  if (!info.icc_description.empty()) info.profile = info.icc_description;
  if (info.codec.empty()) info.codec = "HEVC/HEIF";
  if (info.profile.empty()) info.profile = "unknown";
  if (info.chroma.empty()) info.chroma = "unknown";
  return info;
}

static DecodedImage decode_direct_hdr_opened(const std::filesystem::path& path,
                                             OpenedHeif opened,
                                             const ProgressCallback& progress,
                                             std::atomic_bool* cancel) {
  const auto decode_start = Clock::now();
  DecodedImage result;
  result.info = inspect_opened(path, opened);
  if (result.info.gain_map_present) {
    throw std::runtime_error("gain-map HEIF/AVIF detected; direct primary-image decode would discard HDR intent");
  }
  const bool direct_pq = result.info.transfer == 16 &&
                         (result.info.primaries == 1 || result.info.primaries == 9 ||
                          result.info.primaries == 12);
  const bool direct_hlg = result.info.transfer == 18 &&
                          (result.info.primaries == 1 || result.info.primaries == 9 ||
                           result.info.primaries == 12);
  if (!direct_pq && !direct_hlg) {
    return result;
  }
  if (result.info.color_signaling_conflict) {
    report(progress, 7,
           "Color signaling conflict: preserving container/codec YUV decode while using the resolved HDR RGB interpretation");
  } else if (result.info.resolved_signaling_source == "ICC") {
    report(progress, 7,
           "HDR transfer/gamut resolved from ICC; codec/VUI remains responsible for stored YUV-to-RGB decoding");
  } else if (result.info.resolved_signaling_source == "Native + ICC") {
    report(progress, 7, "Matching native and ICC HDR signaling resolved");
  }
  load_metadata(opened.handle.get(), result.info, &result.exif, &result.xmp);
  check_cancel(cancel);

  OptionsPtr options(heif_decoding_options_alloc());
  if (!options) throw std::bad_alloc();
  options->convert_hdr_to_8bit = 0;
  // Decode storage raster exactly once, then apply the canonical transform to
  // RGB ourselves. This keeps base/gain/auxiliary planes on one policy and
  // prevents metadata-driven double rotation on output.
  options->ignore_transformations = 1;
  options->output_image_nclx_profile_passthrough = 1;
  // Tile workers share the budget; avoid starting another full pool per tile.
  options->num_codec_threads = result.info.is_grid ? 1 : default_worker_count();
#if defined(__EMSCRIPTEN_PTHREADS__)
  // Large AV1 frames leave less room for per-thread codec buffers in WASM.
  const uint64_t frame_pixels = static_cast<uint64_t>(result.info.width) * result.info.height;
  if (result.info.codec == "AV1" && frame_pixels > 141'000'000) {
    const int thread_limit = frame_pixels > 152'000'000 ? 4 : 8;
    options->num_codec_threads = std::min(options->num_codec_threads, thread_limit);
  }
#endif
  options->progress_user_data = cancel;
  options->cancel_decoding = [](void* user) -> int {
    return user && static_cast<std::atomic_bool*>(user)->load() ? 1 : 0;
  };

  heif_image* raw_image = nullptr;
  report(progress, 12, "Decoding " + result.info.codec + " image to RGB16");
  require_heif(heif_decode_image(opened.handle.get(), &raw_image, heif_colorspace_RGB,
                                 heif_chroma_interleaved_RRGGBB_LE, options.get()), "decode HEIF RGB16");
  ImagePtr image(raw_image);
  check_cancel(cancel);

  int stride = 0;
  const uint8_t* plane = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &stride);
  if (!plane || stride < static_cast<int>(result.info.width * 6u)) throw std::runtime_error("invalid RGB16 decode plane");
  const size_t pixel_count = static_cast<size_t>(result.info.width) * result.info.height;
  result.rgb.resize(pixel_count * 3u);
  for (uint32_t y = 0; y < result.info.height; ++y) {
    check_cancel(cancel);
    const auto* source = reinterpret_cast<const uint16_t*>(plane + static_cast<size_t>(y) * stride);
    auto* target = result.rgb.data() + static_cast<size_t>(y) * result.info.width * 3u;
    const size_t samples = static_cast<size_t>(result.info.width) * 3u;
    if (result.info.bit_depth > 0 && result.info.bit_depth < 16) {
      const uint32_t source_max = (1u << result.info.bit_depth) - 1u;
      for (size_t i = 0; i < samples; ++i) {
        target[i] = static_cast<uint16_t>((static_cast<uint32_t>(source[i]) * 65535u + source_max / 2u) / source_max);
      }
    } else {
      std::memcpy(target, source, samples * sizeof(uint16_t));
    }
  }
  result.timings.decode_ms = elapsed_ms(decode_start);
  // RGB and metadata are now owned by result. Release codec contexts before
  // starting the next parallel stage so their workers cannot retain the pool.
  image.reset();
  opened.handle.reset();
  opened.context.reset();
  const auto orientation_start = Clock::now();
  uint8_t canonical_transform = result.info.original_orientation;
  if (canonical_transform == 1) {
    canonical_transform = orientation::read_exif_orientation(result.exif);
    result.info.original_orientation = canonical_transform;
  }
  if (canonical_transform != 1) {
    const auto transformed = orientation::normalize_rgb16(
        result.rgb, result.info.width, result.info.height, canonical_transform);
    result.info.width = transformed.width;
    result.info.height = transformed.height;
  }
  orientation::set_exif_orientation_to_one(result.exif);
  orientation::set_xmp_orientation_to_one(result.xmp);
  result.info.orientation_normalized = true;
  result.timings.orientation_ms = elapsed_ms(orientation_start);
  const auto color_start = Clock::now();
  if (direct_hlg) {
    struct TransferErrors {
      long double squared = 0.0;
      double maximum = 0.0;
      long double pq_squared = 0.0;
      double pq_maximum = 0.0;
    };
    std::vector<TransferErrors> errors(default_worker_count());
    const size_t workers = parallel_indexed_blocks(pixel_count,
        [&](size_t worker, size_t begin, size_t end) {
      auto& local = errors[worker];
      for (size_t p = begin; p < end; ++p) {
      if ((p & 16383u) == 0) check_cancel(cancel);
      const std::array<double, 3> source_signal{
          result.rgb[p * 3u] / 65535.0,
          result.rgb[p * 3u + 1u] / 65535.0,
          result.rgb[p * 3u + 2u] / 65535.0};
      auto nits = transfer::hlg_to_linear_nits(source_signal);
      if (result.info.primaries == 12) {
        nits = {0.7538330 * nits[0] + 0.1985974 * nits[1] + 0.0475696 * nits[2],
                0.0457438 * nits[0] + 0.9417772 * nits[1] + 0.0124789 * nits[2],
               -0.0012103 * nits[0] + 0.0176017 * nits[1] + 0.9836086 * nits[2]};
      } else if (result.info.primaries == 1) {
        nits = {0.6274040 * nits[0] + 0.3292830 * nits[1] + 0.0433130 * nits[2],
                0.0690970 * nits[0] + 0.9195400 * nits[1] + 0.0113620 * nits[2],
                0.0163910 * nits[0] + 0.0880130 * nits[1] + 0.8955950 * nits[2]};
      }
      for (size_t c = 0; c < 3; ++c) {
        const double ideal_pq = transfer::nits_to_pq(nits[c]);
        result.rgb[p * 3u + c] = static_cast<uint16_t>(std::llround(
            ideal_pq * 65535.0));
        const double pq_error = result.rgb[p * 3u + c] / 65535.0 - ideal_pq;
        local.pq_squared += static_cast<long double>(pq_error) * pq_error;
        local.pq_maximum = std::max(local.pq_maximum, std::abs(pq_error));
      }
      const auto reconstructed_signal = transfer::linear_nits_to_hlg({
          transfer::pq_to_nits(result.rgb[p * 3u] / 65535.0),
          transfer::pq_to_nits(result.rgb[p * 3u + 1u] / 65535.0),
          transfer::pq_to_nits(result.rgb[p * 3u + 2u] / 65535.0)});
      for (size_t c = 0; c < 3; ++c) {
        const double error = reconstructed_signal[c] - source_signal[c];
        local.squared += static_cast<long double>(error) * error;
        local.maximum = std::max(local.maximum, std::abs(error));
      }
      }
    });
    TransferErrors combined;
    for (size_t worker = 0; worker < workers; ++worker) {
      combined.squared += errors[worker].squared;
      combined.maximum = std::max(combined.maximum, errors[worker].maximum);
      combined.pq_squared += errors[worker].pq_squared;
      combined.pq_maximum = std::max(combined.pq_maximum, errors[worker].pq_maximum);
    }
    result.transfer_conversion_rmse = std::sqrt(static_cast<double>(
        combined.squared / static_cast<long double>(pixel_count * 3u)));
    result.transfer_conversion_max_abs_error = combined.maximum;
    result.source_to_canonical_rmse = std::sqrt(static_cast<double>(
        combined.pq_squared / static_cast<long double>(pixel_count * 3u)));
    result.source_to_canonical_max_abs_error = combined.pq_maximum;
  } else if (result.info.primaries == 12) {
    convert_p3_pq_to_rec2020_pq(result.rgb);
  } else if (result.info.primaries == 1) {
    convert_rec709_pq_to_rec2020_pq(result.rgb);
  }
  result.timings.color_conversion_ms = elapsed_ms(color_start);
  report(progress, 42, direct_hlg
      ? "BT.2100 HLG OETF/OOTF reconstructed to linear HDR (1000-nit reference)"
      : "High-precision Rec.2020/PQ RGB ready");
  return result;
}

DecodedImage decode_direct_hdr(const std::filesystem::path& path,
                               const ProgressCallback& progress,
                               std::atomic_bool* cancel) {
  report(progress, 5, "Inspecting HEIF container");
  return decode_direct_hdr_opened(path, open_heif(path), progress, cancel);
}

DecodedImage decode_direct_hdr_bytes(const std::filesystem::path& path,
                                     std::vector<uint8_t> bytes,
                                     const ProgressCallback& progress,
                                     std::atomic_bool* cancel) {
  report(progress, 5, "Inspecting HEIF container");
  return decode_direct_hdr_opened(path, open_heif_bytes(std::move(bytes)), progress, cancel);
}

SourceInfo inspect_heif_bytes(const std::filesystem::path& path, std::vector<uint8_t> bytes) {
  auto opened = open_heif_bytes(std::move(bytes));
  auto info = inspect_opened(path, opened);
  if (info.original_orientation >= 5) std::swap(info.width, info.height);
  return info;
}

SourceInfo inspect_heif_input(const std::filesystem::path& path) {
  const auto gainmap = gainmap::probe_heif_gainmap(path);
  if (gainmap.detected) {
    return gainmap::inspect_heif_gainmap(path);
  }
  auto opened = open_heif(path);
  auto info = inspect_opened(path, opened);
  if (info.original_orientation >= 5) std::swap(info.width, info.height);
  return info;
}

}  // namespace hdrbridge::detail
