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

struct GainMapAsset {
  SourceInfo info;
  uint32_t storage_width = 0;
  uint32_t storage_height = 0;
  uint32_t map_storage_width = 0;
  uint32_t map_storage_height = 0;
  uint16_t base_primaries = 12;
  uint16_t base_transfer = 13;
  uint16_t application_primaries = 12;
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

struct ResolvedHeifBaseColor {
  uint16_t primaries = 12;
  uint16_t transfer = 13;
  std::string description = "Display P3";
  std::string transfer_name = "SDR gamma";
  std::string signal_kind = "HEIF ICC / tmap metadata";
  bool icc_present = false;
  std::vector<uint8_t> icc;
  std::string error;
};

uint16_t read_be16_at(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t read_be32_at(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

uint64_t read_be64_at(const uint8_t* data) {
  return (static_cast<uint64_t>(read_be32_at(data)) << 32) |
         static_cast<uint64_t>(read_be32_at(data + 4));
}

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

std::string read_auxiliary_type(const heif_image_handle* handle) {
  const char* type = nullptr;
  if (heif_image_handle_get_auxiliary_type(handle, &type).code != heif_error_Ok || !type) {
    return {};
  }
  std::string result(type);
  heif_image_handle_release_auxiliary_type(handle, &type);
  return result;
}

IsoGainMapMetadata parse_heif_tmap_metadata(const std::vector<uint8_t>& raw_metadata) {
  // ISO 21496-1 HEIF tmap item payloads normally start with a 1-byte uint8
  // version (= 0) before GainMapMetadata. Some writers (e.g. Hasselblad X2D II)
  // omit that leading byte and start directly with minimum_version (= 0).
  if (raw_metadata.size() >= 2 && raw_metadata[0] == 0) {
    try {
      return parse_iso_gain_map_metadata(
          raw_metadata.data() + 1, raw_metadata.size() - 1);
    } catch (const std::exception&) {
      // Fall through to check a versionless GainMapMetadata payload.
    }
  }
  if (!raw_metadata.empty()) {
    return parse_iso_gain_map_metadata(raw_metadata.data(), raw_metadata.size());
  }
  throw std::runtime_error("HEIC tmap item-data is empty");
}

std::string primaries_name(uint16_t primaries) {
  switch (primaries) {
    case 1: return "BT.709 / sRGB primaries";
    case 9: return "BT.2020";
    case 12: return "Display P3";
    default: return "Display P3";
  }
}

uint16_t identify_icc_primaries(const void* icc_data, size_t icc_size,
                                std::string* out_description = nullptr) {
  CmsProfilePtr profile(cmsOpenProfileFromMem(
      icc_data, static_cast<cmsUInt32Number>(icc_size)));
  if (!profile) return 0;
  const std::string description = profile_description(profile.get());
  if (out_description) *out_description = description;
  if (uint16_t detected = hdrbridge::icc::identify_rgb_primaries(profile.get())) {
    return detected;
  }
  if (description.find("P3") != std::string::npos) return 12;
  if (description.find("2020") != std::string::npos ||
      description.find("2100") != std::string::npos) return 9;
  if (description.find("sRGB") != std::string::npos ||
      description.find("709") != std::string::npos) return 1;
  return 0;
}

ResolvedHeifBaseColor read_heif_base_color_optional(const heif_image_handle* primary) {
  ResolvedHeifBaseColor color;
  const size_t icc_size = heif_image_handle_get_raw_color_profile_size(primary);
  if (icc_size > 0) {
    color.icc.resize(icc_size);
    if (heif_image_handle_get_raw_color_profile(primary, color.icc.data()).code != heif_error_Ok) {
      color.error = "read HEIC SDR base ICC profile";
      return color;
    }
    color.icc_present = true;
    if (is_adaptive_hdr_icc(color.icc.data(), color.icc.size())) {
      color.description = "Adaptive HDR ICC";
      color.error = "HEIC base resolved to an Adaptive HDR ICC instead of the SDR base ICC";
      return color;
    }
    CmsProfilePtr profile(cmsOpenProfileFromMem(
        color.icc.data(), static_cast<cmsUInt32Number>(color.icc.size())));
    if (!profile) {
      color.description = "Unreadable ICC";
      color.error = "cannot parse HEIC SDR base ICC profile";
      return color;
    }
    color.description = profile_description(profile.get());
    color.primaries = identify_icc_primaries(color.icc.data(), color.icc.size());
    if (color.primaries == 0) {
      color.error = "HEIC SDR base ICC primaries are unsupported: " + color.description;
      return color;
    }
    const std::array<cmsToneCurve*, 3> curves{
        static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigRedTRCTag)),
        static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigGreenTRCTag)),
        static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigBlueTRCTag))};
    if (const auto* cicp = static_cast<const cmsVideoSignalType*>(
            cmsReadTag(profile.get(), cmsSigcicpTag))) {
      if (cicp->TransferCharacteristics == 16 || cicp->TransferCharacteristics == 18) {
        color.transfer = cicp->TransferCharacteristics;
        color.transfer_name = color.transfer == 16 ? "PQ / ST 2084" : "HLG / BT.2100";
        color.error = "HEIC base ICC has non-SDR transfer characteristics: " +
            std::to_string(color.transfer);
        return color;
      }
      if (cicp->TransferCharacteristics == 8) {
        color.transfer = 8;
        color.transfer_name = "Linear";
        const bool primaries_match =
            cicp->ColourPrimaries == 0 || cicp->ColourPrimaries == 2 ||
            cicp->ColourPrimaries == color.primaries;
        auto is_linear_trc = [](const cmsToneCurve* curve) -> bool {
          if (!curve) return false;
          for (float sample : {0.0f, 0.05f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
            if (std::abs(static_cast<double>(cmsEvalToneCurveFloat(curve, sample)) - sample) >
                0.002) {
              return false;
            }
          }
          return true;
        };
        if (!primaries_match || !is_linear_trc(curves[0]) || !is_linear_trc(curves[1]) ||
            !is_linear_trc(curves[2])) {
          color.error =
              "HEIC base ICC CICP linear transfer conflicts with profile colorimetry or TRC";
          return color;
        }
        color.signal_kind = "HEIF ICC / tmap metadata";
        return color;
      }
    }
    if (!curves[0] || !curves[1] || !curves[2]) {
      color.transfer = 0;
      color.transfer_name = "Unspecified";
      color.signal_kind = "HEIF ICC / tmap metadata";
      color.error = "HEIC SDR base ICC has no RGB transfer curves";
      return color;
    }
    if (const uint16_t sdr_trc = hdrbridge::icc::identify_sdr_transfer(profile.get())) {
      color.transfer = sdr_trc;
    }
    color.transfer_name = color.transfer == 14 ? "BT.2020 SDR" :
                          color.transfer == 1 ? "BT.709" : "SDR gamma";
    color.signal_kind = "HEIF ICC / tmap metadata";
    return color;
  }

  heif_color_profile_nclx* raw_nclx = nullptr;
  if (heif_image_handle_get_nclx_color_profile(primary, &raw_nclx).code == heif_error_Ok &&
      raw_nclx) {
    struct NclxGuard {
      heif_color_profile_nclx* value;
      ~NclxGuard() { if (value) heif_nclx_color_profile_free(value); }
    } nclx{raw_nclx};
    const uint16_t cp = static_cast<uint16_t>(raw_nclx->color_primaries);
    const uint16_t tc = static_cast<uint16_t>(raw_nclx->transfer_characteristics);
    const bool supported_sdr_transfer =
        tc == 1u || tc == 6u || tc == 13u || tc == 14u || tc == 15u;
    color.primaries = cp;
    color.transfer = tc;
    color.transfer_name = tc == 13u ? "sRGB" :
                          tc == 16u ? "PQ / ST 2084" :
                          tc == 18u ? "HLG / BT.2100" :
                          tc == 8u ? "Linear" :
                          supported_sdr_transfer ? "BT.709 / SDR" :
                          "Unsupported (" + std::to_string(tc) + ")";
    color.signal_kind = "HEIF NCLX / tmap metadata";
    const bool supported_primaries = cp == 1u || cp == 9u || cp == 12u;
    color.description = supported_primaries
        ? primaries_name(cp)
        : "Unsupported NCLX primaries (" + std::to_string(cp) + ")";
    if (!supported_primaries) {
      color.error = "HEIC SDR base color profile primaries are unsupported: " + std::to_string(cp);
    } else if (!supported_sdr_transfer) {
      color.error = "HEIC SDR base transfer characteristics are unsupported: " + std::to_string(tc);
    }
    return color;
  }

  color.description = "Unspecified";
  color.transfer_name = "Unspecified";
  color.error = "HEIC SDR base color profile is missing";
  return color;
}

void validate_heif_hdr_base_color(const ResolvedHeifBaseColor& color) {
  if (!color.error.empty()) {
    throw std::runtime_error(color.error);
  }
}

struct HeifColrBox {
  uint16_t nclx_primaries = 0;
  std::vector<uint8_t> icc;
};

template <typename Callback>
void iterate_bmff_boxes(const uint8_t* data, size_t start, size_t end, Callback&& on_box) {
  size_t pos = start;
  while (pos + 8u <= end) {
    const uint32_t size32 = read_be32_at(data + pos);
    const uint8_t* type = data + pos + 4u;
    size_t hdr = 8u;
    size_t box_end = end;
    if (size32 == 1u) {
      if (pos + 16u > end) break;
      const uint64_t size64 = read_be64_at(data + pos + 8u);
      hdr = 16u;
      if (size64 < hdr || size64 > static_cast<uint64_t>(end - pos)) break;
      box_end = pos + static_cast<size_t>(size64);
    } else if (size32 != 0u) {
      if (size32 < hdr || size32 > end - pos) break;
      box_end = pos + size32;
    }
    on_box(type, pos + hdr, box_end);
    if (box_end <= pos) break;
    pos = box_end;
  }
}

std::vector<HeifColrBox> read_heif_item_colr_boxes(const std::vector<uint8_t>& bytes,
                                                   heif_item_id target_item) {
  std::vector<HeifColrBox> result;
  if (target_item == 0u) return result;
  struct RawBoxSlice {
    std::array<uint8_t, 4> type{};
    size_t payload_start = 0;
    size_t payload_end = 0;
  };
  iterate_bmff_boxes(bytes.data(), 0u, bytes.size(),
      [&](const uint8_t* type, size_t meta_hdr, size_t meta_end) {
        if (std::memcmp(type, "meta", 4u) != 0 || meta_hdr + 4u > meta_end) return;
        iterate_bmff_boxes(bytes.data(), meta_hdr + 4u, meta_end,
            [&](const uint8_t* mtype, size_t iprp_hdr, size_t iprp_end) {
              if (std::memcmp(mtype, "iprp", 4u) != 0) return;
              std::vector<RawBoxSlice> ipco_props;
              iterate_bmff_boxes(bytes.data(), iprp_hdr, iprp_end,
                  [&](const uint8_t* ptype, size_t ipco_hdr, size_t ipco_end) {
                    if (std::memcmp(ptype, "ipco", 4u) != 0) return;
                    iterate_bmff_boxes(bytes.data(), ipco_hdr, ipco_end,
                        [&](const uint8_t* ctype, size_t prop_hdr, size_t prop_end) {
                          RawBoxSlice slice;
                          std::memcpy(slice.type.data(), ctype, 4u);
                          slice.payload_start = prop_hdr;
                          slice.payload_end = prop_end;
                          ipco_props.push_back(slice);
                        });
                  });
              if (ipco_props.empty()) return;
              iterate_bmff_boxes(bytes.data(), iprp_hdr, iprp_end,
                  [&](const uint8_t* ptype, size_t ipma_hdr, size_t ipma_end) {
                    if (std::memcmp(ptype, "ipma", 4u) != 0 || ipma_hdr + 8u > ipma_end) return;
                    const uint8_t version = bytes[ipma_hdr];
                    const uint32_t flags = (static_cast<uint32_t>(bytes[ipma_hdr + 1u]) << 16u) |
                                           (static_cast<uint32_t>(bytes[ipma_hdr + 2u]) << 8u) |
                                           static_cast<uint32_t>(bytes[ipma_hdr + 3u]);
                    const uint32_t entry_count = read_be32_at(bytes.data() + ipma_hdr + 4u);
                    size_t cursor = ipma_hdr + 8u;
                    for (uint32_t e = 0; e < entry_count; ++e) {
                      const size_t id_bytes = version >= 1u ? 4u : 2u;
                      if (cursor + id_bytes + 1u > ipma_end) return;
                      const uint32_t iid = version >= 1u
                          ? read_be32_at(bytes.data() + cursor)
                          : read_be16_at(bytes.data() + cursor);
                      cursor += id_bytes;
                      const uint8_t assoc_count = bytes[cursor++];
                      const size_t prop_bytes = (flags & 1u) ? 2u : 1u;
                      if (cursor + static_cast<size_t>(assoc_count) * prop_bytes > ipma_end) return;
                      if (iid == target_item) {
                        for (uint8_t a = 0; a < assoc_count; ++a) {
                          const uint16_t prop_idx = (flags & 1u)
                              ? static_cast<uint16_t>(read_be16_at(bytes.data() + cursor) & 0x7FFFu)
                              : static_cast<uint16_t>(bytes[cursor] & 0x7Fu);
                          cursor += prop_bytes;
                          if (prop_idx == 0u || prop_idx > ipco_props.size()) continue;
                          const auto& prop = ipco_props[prop_idx - 1u];
                          if (std::memcmp(prop.type.data(), "colr", 4u) != 0) continue;
                          const size_t colr_size = prop.payload_end - prop.payload_start;
                          if (colr_size < 4u) continue;
                          const uint8_t* colr = bytes.data() + prop.payload_start;
                          HeifColrBox parsed;
                          if ((std::memcmp(colr, "rICC", 4u) == 0 ||
                               std::memcmp(colr, "prof", 4u) == 0) &&
                              colr_size > 4u) {
                            parsed.icc.assign(colr + 4u, colr + colr_size);
                            result.push_back(std::move(parsed));
                          } else if (std::memcmp(colr, "nclx", 4u) == 0 && colr_size >= 10u) {
                            parsed.nclx_primaries = read_be16_at(colr + 4u);
                            result.push_back(std::move(parsed));
                          }
                        }
                      } else {
                        cursor += static_cast<size_t>(assoc_count) * prop_bytes;
                      }
                    }
                  });
            });
      });
  return result;
}

uint16_t resolve_tmap_application_primaries(const std::vector<uint8_t>& bytes,
                                            heif_item_id tone_map_item,
                                            const IsoGainMapMetadata& metadata,
                                            uint16_t base_primaries) {
  if (metadata.use_base_color_space) return base_primaries;
  if (tone_map_item == 0u) {
    throw std::runtime_error("HEIC tmap item is missing for alternate color space");
  }
  const auto colr_boxes = read_heif_item_colr_boxes(bytes, tone_map_item);
  std::optional<std::string> unsupported_reason;
  for (const auto& box : colr_boxes) {
    if (!box.icc.empty()) {
      if (is_adaptive_hdr_icc(box.icc.data(), box.icc.size())) continue;
      std::string desc;
      if (const uint16_t cp = identify_icc_primaries(box.icc.data(), box.icc.size(), &desc)) {
        return cp;
      }
      unsupported_reason = "HEIC tmap ICC primaries are unsupported" +
          (desc.empty() ? std::string{} : ": " + desc);
    }
  }
  for (const auto& box : colr_boxes) {
    if (box.nclx_primaries == 1u || box.nclx_primaries == 9u || box.nclx_primaries == 12u) {
      return box.nclx_primaries;
    }
    if (box.nclx_primaries != 0u) {
      unsupported_reason = "HEIC tmap NCLX primaries are unsupported: " +
          std::to_string(box.nclx_primaries);
    }
  }
  throw std::runtime_error(unsupported_reason.value_or(
      "HEIC tmap alternate color profile is missing when useBaseColorSpace=false"));
}

std::optional<double> resolve_heif_reference_white_nits(
    const std::vector<uint8_t>& bytes,
    const ItemGraph& graph,
    const ResolvedHeifBaseColor& base_color) {
  std::optional<double> found;
  const auto check_icc = [&](const std::vector<uint8_t>& icc) {
    if (icc.empty()) return;
    if (const auto nits = adaptive_hdr_icc_reference_white_nits(
            icc.data(), icc.size(), /*allow_lumi=*/true)) {
      if (found && std::abs(*found - *nits) > 1e-6) {
        throw std::runtime_error(
            "Adaptive HDR ICC contains conflicting reference-white luminance tags");
      }
      found = *nits;
    }
  };
  for (const auto& box : read_heif_item_colr_boxes(bytes, graph.tone_map)) {
    check_icc(box.icc);
  }
  check_icc(base_color.icc);
  for (const auto& box : read_heif_item_colr_boxes(bytes, graph.gain)) {
    check_icc(box.icc);
  }
  return found;
}

std::array<double, 3> convert_gainmap_base_primaries(const std::array<double, 3>& rgb,
                                                     uint16_t from_primaries,
                                                     uint16_t to_primaries) {
  if (from_primaries == to_primaries) return rgb;
  const auto rec2020 = rec2020_from_gainmap_base(rgb, from_primaries);
  if (to_primaries == 12) {
    return {1.3435783 * rec2020[0] - 0.2821797 * rec2020[1] - 0.0613986 * rec2020[2],
           -0.0652975 * rec2020[0] + 1.0757879 * rec2020[1] - 0.0104904 * rec2020[2],
            0.0028218 * rec2020[0] - 0.0195985 * rec2020[1] + 1.0167767 * rec2020[2]};
  }
  if (to_primaries == 1) {
    return {1.6604910 * rec2020[0] - 0.5876411 * rec2020[1] - 0.0728499 * rec2020[2],
           -0.1245505 * rec2020[0] + 1.1328999 * rec2020[1] - 0.0083494 * rec2020[2],
           -0.0181508 * rec2020[0] - 0.1005789 * rec2020[1] + 1.1187297 * rec2020[2]};
  }
  return rec2020;
}

double inverse_bt709_sdr(double value) {
  constexpr double alpha = 1.09929682680944;
  constexpr double beta = 0.018053968510807;
  return value < 4.5 * beta ? value / 4.5
                            : std::pow((value + alpha - 1.0) / alpha, 1.0 / 0.45);
}

std::array<std::vector<double>, 3> build_base_inverse_trc(const ResolvedHeifBaseColor& color) {
  std::array<std::vector<double>, 3> inverse_trc;
  if (!color.icc.empty()) {
    CmsProfilePtr profile(cmsOpenProfileFromMem(
        color.icc.data(), static_cast<cmsUInt32Number>(color.icc.size())));
    if (!profile) throw std::runtime_error("cannot parse HEIC SDR base ICC profile");
    std::array<cmsToneCurve*, 3> curves{
        static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigRedTRCTag)),
        static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigGreenTRCTag)),
        static_cast<cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigBlueTRCTag))};
    if (!curves[0] || !curves[1] || !curves[2]) {
      throw std::runtime_error("HEIC SDR base ICC has no RGB transfer curves");
    }
    for (size_t channel = 0; channel < 3; ++channel) {
      inverse_trc[channel].resize(65536u);
      for (size_t code = 0; code < 65536u; ++code) {
        inverse_trc[channel][code] = cmsEvalToneCurveFloat(
            curves[channel], static_cast<float>(code / 65535.0));
      }
    }
    return inverse_trc;
  }
  const bool bt709 = color.transfer == 1 || color.transfer == 6 ||
                     color.transfer == 14 || color.transfer == 15;
  inverse_trc[0].resize(65536u);
  for (size_t code = 0; code < 65536u; ++code) {
    const double v = code / 65535.0;
    inverse_trc[0][code] = color.transfer == 8 ? v :
                           (bt709 ? inverse_bt709_sdr(v) : inverse_srgb(v));
  }
  inverse_trc[1] = inverse_trc[0];
  inverse_trc[2] = inverse_trc[0];
  return inverse_trc;
}

void load_heif_gainmap_metadata(const heif_context* context,
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
                   "read HEIF metadata");
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

void populate_heif_gainmap_info(SourceInfo& info,
                                const std::filesystem::path& path,
                                const AppleAuxiliaryProbe& probe,
                                const ItemGraph& graph,
                                const IsoGainMapMetadata& metadata,
                                const ResolvedHeifBaseColor& base_color,
                                const heif_context* context,
                                const heif_image_handle* primary,
                                uint32_t storage_width,
                                uint32_t storage_height,
                                uint32_t map_width,
                                uint32_t map_height,
                                const std::vector<uint8_t>& exif,
                                const std::vector<uint8_t>& xmp) {
  info.path = path;
  info.format = probe.is_apple ? "Apple HDR HEIC" : "HEIF/HEIC";
  info.container_brand = "HEIF/HEIC + tmap";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = probe.is_apple ? "apple-auxiliary-tmap" : "iso-heif-tmap";
  info.base_item_id = graph.base;
  info.gain_map_item_id = graph.gain;
  info.tone_map_item_id = graph.tone_map;
  info.auxiliary_type = probe.auxiliary_type;
  info.codec = probe.is_apple
      ? "HEVC SDR base + Apple auxiliary HEVC gain map + ISO tmap metadata"
      : "HEVC SDR base + HEVC gain map + ISO tmap metadata";
  info.color_signal_kind = base_color.signal_kind;
  const uint32_t gain_channels = metadata.multichannel ? 3u : 1u;
  info.pixel_format = gain_channels == 3u
      ? "HEVC SDR base + RGB gain map"
      : "HEVC SDR base + monochrome auxiliary gain map";
  info.bit_depth = static_cast<uint32_t>(std::max(
      heif_image_handle_get_luma_bits_per_pixel(primary), 0));
  info.chroma = gain_channels == 3u
      ? "SDR RGB base + RGB gain map"
      : "SDR RGB base + monochrome gain map";
  info.original_orientation = orientation_from_heif(context, primary);
  if (info.original_orientation == 1 && orientation::has_exif_orientation(exif)) {
    info.original_orientation = orientation::read_exif_orientation(exif);
  }
  info.orientation_normalized = true;
  info.width = info.original_orientation >= 5 ? storage_height : storage_width;
  info.height = info.original_orientation >= 5 ? storage_width : storage_height;
  info.base_width = info.width;
  info.base_height = info.height;
  info.base_bit_depth = info.bit_depth;
  info.base_channels = 3;
  info.base_codec = "HEVC";
  info.base_color_space = base_color.description;
  info.base_transfer = base_color.transfer_name;
  info.primaries = base_color.primaries;
  info.transfer = base_color.transfer;
  info.matrix = 0;
  info.full_range = true;
  info.range_known = true;
  info.gain_map_width = map_width;
  info.gain_map_height = map_height;
  info.gain_map_channels = gain_channels;
  if (map_width) info.gain_map_scale_x = info.base_width / static_cast<double>(map_width);
  if (map_height) info.gain_map_scale_y = info.base_height / static_cast<double>(map_height);
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
  info.icc_present = base_color.icc_present;
  if (base_color.icc_present) {
    info.profile = base_color.description + (probe.is_apple
        ? " SDR base + Apple HDR gain map"
        : " SDR base + ISO tmap gain map");
  } else {
    info.profile = base_color.description + " SDR base + ISO tmap gain map";
  }
  info.exif_present = !exif.empty();
  info.xmp_present = !xmp.empty();
}

HeifHandlePtr open_gain_map_handle(const heif_context* context,
                                  const heif_image_handle* primary,
                                  heif_item_id gain_item_id) {
  heif_image_handle* raw_gain = nullptr;
  if (heif_context_get_image_handle(const_cast<heif_context*>(context), gain_item_id,
                                    &raw_gain).code == heif_error_Ok &&
      raw_gain) {
    return HeifHandlePtr(raw_gain);
  }
  require_heif(heif_image_handle_get_auxiliary_image_handle(
                   primary, gain_item_id, &raw_gain),
               "extract HEIC gain-map image handle");
  return HeifHandlePtr(raw_gain);
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
  if (heif_context_get_primary_image_handle(context.get(), &raw_primary).code != heif_error_Ok) {
    return probe;
  }
  HeifHandlePtr primary(raw_primary);
  const heif_item_id primary_id = heif_image_handle_get_item_id(primary.get());

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
    const std::string auxiliary_type = read_auxiliary_type(auxiliary.get());
    if (auxiliary_type == kAppleHdrGainMapUrn) {
      probe.detected = true;
      probe.is_apple = true;
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

  const ItemGraph graph = read_item_graph(context.get());
  if (graph.tone_map && graph.base == primary_id && graph.gain) {
    heif_image_handle* raw_gain = nullptr;
    if (heif_context_get_image_handle(context.get(), graph.gain, &raw_gain).code == heif_error_Ok &&
        raw_gain) {
      HeifHandlePtr gain(raw_gain);
      const std::string auxiliary_type = read_auxiliary_type(gain.get());
      probe.detected = true;
      probe.is_apple = auxiliary_type == kAppleHdrGainMapUrn;
      probe.auxiliary_type = auxiliary_type.empty() ? "tmap" : auxiliary_type;
      probe.item_id = graph.gain;
      probe.width = static_cast<uint32_t>(heif_image_handle_get_width(gain.get()));
      probe.height = static_cast<uint32_t>(heif_image_handle_get_height(gain.get()));
      probe.bit_depth = static_cast<uint32_t>(std::max(
          heif_image_handle_get_luma_bits_per_pixel(gain.get()), 0));
      probe.extraction_supported = true;
    }
  }
  return probe;
}

struct LoadedHeifContainer {
  AppleAuxiliaryProbe probe;
  std::vector<uint8_t> bytes;
  HeifContextPtr context;
  HeifHandlePtr primary;
  HeifHandlePtr gain_handle;
  ItemGraph graph;
  IsoGainMapMetadata metadata;
  ResolvedHeifBaseColor base_color;
  SourceInfo info;
  uint32_t storage_width = 0;
  uint32_t storage_height = 0;
  double parse_ms = 0.0;
};

LoadedHeifContainer open_heif_gainmap_container(const std::filesystem::path& path,
                                                std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  const auto parse_start = Clock::now();
  LoadedHeifContainer container;
  container.probe = probe_apple_heif(path);
  if (!container.probe.detected || container.probe.item_id == 0) {
    throw std::runtime_error("HEIC HDR gain map was not found");
  }
  container.bytes = read_file(path);
  container.context.reset(heif_context_alloc());
  if (!container.context) throw std::bad_alloc();
  heif_context_set_max_decoding_threads(container.context.get(), codec_background_workers());
  require_heif(heif_context_read_from_memory_without_copy(
                   container.context.get(), container.bytes.data(), container.bytes.size(), nullptr),
               "read HDR gain-map HEIC");
  heif_image_handle* raw_primary = nullptr;
  require_heif(heif_context_get_primary_image_handle(container.context.get(), &raw_primary),
               "get HDR gain-map HEIC primary image");
  container.primary.reset(raw_primary);
  container.gain_handle = open_gain_map_handle(
      container.context.get(), container.primary.get(), container.probe.item_id);

  container.graph = read_item_graph(container.context.get());
  if (!container.graph.tone_map ||
      container.graph.base != heif_image_handle_get_item_id(container.primary.get()) ||
      container.graph.gain != container.probe.item_id) {
    throw std::runtime_error("HEIC tmap/base/gain item relationship is inconsistent");
  }
  const auto raw_metadata = item_data(container.context.get(), container.graph.tone_map);
  container.metadata = parse_heif_tmap_metadata(raw_metadata);
  container.base_color = read_heif_base_color_optional(container.primary.get());
  container.storage_width = static_cast<uint32_t>(heif_image_handle_get_ispe_width(container.primary.get()));
  container.storage_height = static_cast<uint32_t>(heif_image_handle_get_ispe_height(container.primary.get()));
  if (!container.storage_width || !container.storage_height) {
    container.storage_width = static_cast<uint32_t>(heif_image_handle_get_width(container.primary.get()));
    container.storage_height = static_cast<uint32_t>(heif_image_handle_get_height(container.primary.get()));
  }
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  load_heif_gainmap_metadata(container.context.get(), container.primary.get(), exif, xmp);
  populate_heif_gainmap_info(container.info, path, container.probe, container.graph,
                             container.metadata, container.base_color,
                             container.context.get(), container.primary.get(),
                             container.storage_width, container.storage_height,
                             container.probe.width, container.probe.height, exif, xmp);
  container.info.exif_present = !exif.empty();
  container.info.xmp_present = !xmp.empty();
  container.parse_ms = elapsed_ms(parse_start);
  return container;
}

HeifOptionsPtr make_heif_decode_options(const heif_image_handle* handle,
                                        std::atomic_bool* cancel) {
  HeifOptionsPtr options(heif_decoding_options_alloc());
  if (!options) throw std::bad_alloc();
  options->convert_hdr_to_8bit = 0;
  options->ignore_transformations = 1;
  options->output_image_nclx_profile_passthrough = 1;
  heif_image_tiling tiling{};
  tiling.version = 1;
  const bool tiled = heif_image_handle_get_image_tiling(handle, 0, &tiling).code == heif_error_Ok &&
      (tiling.num_columns > 1 || tiling.num_rows > 1);
  options->num_codec_threads = tiled ? 1 : static_cast<int>(codec_worker_count());
  options->progress_user_data = cancel;
  options->cancel_decoding = [](void* user) -> int {
    return user && static_cast<std::atomic_bool*>(user)->load() ? 1 : 0;
  };
  return options;
}

void decode_heif_gainmap_planes(const heif_image_handle* gain_handle,
                                bool rgb_map,
                                std::atomic_bool* cancel,
                                SourceInfo& info,
                                uint32_t& out_width,
                                uint32_t& out_height,
                                std::vector<uint16_t>& out_pixels) {
  auto options = make_heif_decode_options(gain_handle, cancel);
  heif_image* raw_map = nullptr;
  heif_channel map_channel = heif_channel_interleaved;
  if (rgb_map) {
    require_heif(heif_decode_image(gain_handle, &raw_map, heif_colorspace_RGB,
                                   heif_chroma_interleaved_RRGGBB_LE, options.get()),
                 "decode HEIC RGB gain map");
  } else {
    const heif_error mono_err = heif_decode_image(
        gain_handle, &raw_map, heif_colorspace_monochrome,
        heif_chroma_monochrome, options.get());
    if (mono_err.code != heif_error_Ok || !raw_map) {
      if (raw_map) {
        heif_image_release(raw_map);
        raw_map = nullptr;
      }
      const heif_error native_err = heif_decode_image(
          gain_handle, &raw_map, heif_colorspace_undefined,
          heif_chroma_undefined, options.get());
      if (native_err.code == heif_error_Ok && raw_map &&
          heif_image_has_channel(raw_map, heif_channel_Y)) {
        map_channel = heif_channel_Y;
      } else if (native_err.code == heif_error_Ok && raw_map &&
                 heif_image_has_channel(raw_map, heif_channel_R)) {
        map_channel = heif_channel_R;
      } else {
        if (raw_map) {
          heif_image_release(raw_map);
          raw_map = nullptr;
        }
        require_heif(heif_decode_image(gain_handle, &raw_map, heif_colorspace_RGB,
                                       heif_chroma_interleaved_RRGGBB_LE, options.get()),
                     "decode HEIC monochrome gain map");
        map_channel = heif_channel_interleaved;
      }
    } else {
      map_channel = heif_channel_Y;
    }
  }
  HeifImagePtr map(raw_map);
  int map_stride = 0;
  const uint8_t* map_plane = heif_image_get_plane_readonly(map.get(), map_channel, &map_stride);
  out_width = static_cast<uint32_t>(heif_image_get_width(map.get(), map_channel));
  out_height = static_cast<uint32_t>(heif_image_get_height(map.get(), map_channel));
  const int map_bits = heif_image_get_bits_per_pixel_range(map.get(), map_channel);
  const int storage_bpp = heif_image_get_bits_per_pixel(map.get(), map_channel);
  const int source_channels = map_channel == heif_channel_interleaved ? 3 : 1;
  const bool storage_16bit = (storage_bpp / source_channels) > 8;
  const uint32_t map_channels = rgb_map ? 3u : 1u;
  if (!map_plane || !out_width || !out_height) {
    throw std::runtime_error("HEIC gain-map decode plane is invalid");
  }
  const uint32_t map_max = map_bits > 0 && map_bits < 16 ? (1u << map_bits) - 1u : 65535u;
  out_pixels.resize(static_cast<size_t>(out_width) * out_height * map_channels);
  for (uint32_t y = 0; y < out_height; ++y) {
    const uint8_t* row8 = map_plane + static_cast<size_t>(y) * map_stride;
    const auto* row16 = reinterpret_cast<const uint16_t*>(row8);
    if (rgb_map || source_channels == 1) {
      const size_t row_samples = static_cast<size_t>(out_width) * map_channels;
      for (size_t s = 0; s < row_samples; ++s) {
        const uint32_t value = storage_16bit ? row16[s] : row8[s];
        out_pixels[static_cast<size_t>(y) * row_samples + s] =
            static_cast<uint16_t>((value * 65535u + map_max / 2u) / map_max);
      }
    } else {
      for (uint32_t x = 0; x < out_width; ++x) {
        const size_t src_idx = static_cast<size_t>(x) * static_cast<size_t>(source_channels);
        const uint32_t value = storage_16bit ? row16[src_idx] : row8[src_idx];
        out_pixels[static_cast<size_t>(y) * out_width + x] =
            static_cast<uint16_t>((value * 65535u + map_max / 2u) / map_max);
      }
    }
  }
  info.gain_map_channels = map_channels;
  info.gain_map_width = info.original_orientation >= 5 ? out_height : out_width;
  info.gain_map_height = info.original_orientation >= 5 ? out_width : out_height;
  if (info.gain_map_width) {
    info.gain_map_scale_x = info.base_width / static_cast<double>(info.gain_map_width);
  }
  if (info.gain_map_height) {
    info.gain_map_scale_y = info.base_height / static_cast<double>(info.gain_map_height);
  }
}

GainMapAsset extract_apple_heif_hdr_asset(
    const std::filesystem::path& path,
    std::atomic_bool* cancel) {
  auto container = open_heif_gainmap_container(path, cancel);
  validate_heif_hdr_base_color(container.base_color);

  GainMapAsset asset;
  asset.info = std::move(container.info);
  asset.storage_width = container.storage_width;
  asset.storage_height = container.storage_height;
  asset.base_primaries = container.base_color.primaries;
  asset.base_transfer = container.base_color.transfer;
  asset.base_icc = container.base_color.icc;
  asset.reference_white_nits =
      resolve_heif_reference_white_nits(container.bytes, container.graph, container.base_color)
          .value_or(203.0);
  asset.application_primaries = resolve_tmap_application_primaries(
      container.bytes, container.graph.tone_map, container.metadata, container.base_color.primaries);
  load_heif_gainmap_metadata(container.context.get(), container.primary.get(), asset.exif, asset.xmp);
  asset.container_parse_ms = container.parse_ms;

  const auto base_decode_start = Clock::now();
  auto base_options = make_heif_decode_options(container.primary.get(), cancel);
  heif_image* raw_base = nullptr;
  require_heif(heif_decode_image(container.primary.get(), &raw_base, heif_colorspace_RGB,
                                 heif_chroma_interleaved_RRGGBB_LE, base_options.get()),
               "decode HEIC SDR base");
  HeifImagePtr base(raw_base);
  int base_stride = 0;
  const uint8_t* base_plane = heif_image_get_plane_readonly(
      base.get(), heif_channel_interleaved, &base_stride);
  if (!base_plane || base_stride < static_cast<int>(asset.storage_width * 6u)) {
    throw std::runtime_error("HEIC SDR base decode plane is invalid");
  }
  asset.base_rgb16.resize(static_cast<size_t>(asset.storage_width) * asset.storage_height * 3u);
  const int decoded_base_bits = heif_image_get_bits_per_pixel_range(
      base.get(), heif_channel_interleaved);
  if (decoded_base_bits <= 0 || decoded_base_bits > 16) {
    throw std::runtime_error("HEIC SDR base has an invalid decoded bit depth");
  }
  asset.info.bit_depth = static_cast<uint32_t>(decoded_base_bits);
  asset.info.base_bit_depth = asset.info.bit_depth;
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
  decode_heif_gainmap_planes(
      container.gain_handle.get(), container.metadata.multichannel, cancel,
      asset.info, asset.map_storage_width, asset.map_storage_height, asset.gain_map_rgb16);
  asset.gain_map_decode_ms = elapsed_ms(map_decode_start);
  return asset;
}

SourceInfo inspect_apple_heif_gainmap(const std::filesystem::path& path) {
  return open_heif_gainmap_container(path, nullptr).info;
}

ReconstructedHdr reconstruct_apple_heif_gainmap(
    const std::filesystem::path& path, std::atomic_bool* cancel) {
  const auto decode_start = Clock::now();
  GainMapAsset asset = extract_apple_heif_hdr_asset(path, cancel);
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

  const auto inverse_trc = build_base_inverse_trc(ResolvedHeifBaseColor{
      asset.base_primaries, asset.base_transfer, asset.info.base_color_space,
      asset.info.base_transfer, asset.info.color_signal_kind, !asset.base_icc.empty(),
      asset.base_icc});
  const auto reconstruction_start = Clock::now();
  const uint32_t width = asset.storage_width;
  const uint32_t height = asset.storage_height;
  const uint32_t map_channels = asset.info.gain_map_channels == 3u ? 3u : 1u;
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
  const bool shared_gain = map_channels == 1u &&
      output.info.gain_map_min[0] == output.info.gain_map_min[1] &&
      output.info.gain_map_min[0] == output.info.gain_map_min[2] &&
      output.info.gain_map_max[0] == output.info.gain_map_max[1] &&
      output.info.gain_map_max[0] == output.info.gain_map_max[2] &&
      output.info.gain_map_gamma[0] == output.info.gain_map_gamma[1] &&
      output.info.gain_map_gamma[0] == output.info.gain_map_gamma[2];
  struct WorkerState {
    std::vector<std::array<double, 3>> map_row;
    std::vector<std::array<double, 3>> app_row;
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
    state.app_row.resize(width);
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
            static_cast<size_t>(y0) * asset.map_storage_width * map_channels;
        const auto* map1 = asset.gain_map_rgb16.data() +
            static_cast<size_t>(y1) * asset.map_storage_width * map_channels;
        for (uint32_t x = 0; x < width; ++x) {
          const XSample& sx = x_samples[x];
          if (map_channels == 1u) {
            const double top = map0[sx.lower] * (1.0 - sx.fraction) +
                               map0[sx.upper] * sx.fraction;
            const double bottom = map1[sx.lower] * (1.0 - sx.fraction) +
                                  map1[sx.upper] * sx.fraction;
            const double sample = (top * (1.0 - fy) + bottom * fy) / 65535.0;
            state.map_row[x] = {sample, sample, sample};
          } else {
            for (size_t channel = 0; channel < 3; ++channel) {
              const double top = map0[static_cast<size_t>(sx.lower) * 3u + channel] * (1.0 - sx.fraction) +
                                 map0[static_cast<size_t>(sx.upper) * 3u + channel] * sx.fraction;
              const double bottom = map1[static_cast<size_t>(sx.lower) * 3u + channel] * (1.0 - sx.fraction) +
                                    map1[static_cast<size_t>(sx.upper) * 3u + channel] * sx.fraction;
              state.map_row[x][channel] = (top * (1.0 - fy) + bottom * fy) / 65535.0;
            }
          }
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
                    std::pow(state.map_row[x][channel], 1.0 / output.info.gain_map_gamma[channel]);
            return std::exp2(log_gain * weight);
          };
          if (shared_gain) {
            factors.fill(compute_factor(0));
          } else {
            for (size_t channel = 0; channel < 3; ++channel) {
              factors[channel] = compute_factor(channel);
            }
          }
          const std::array<double, 3> base_linear{
              inverse_trc[0][asset.base_rgb16[pixel * 3u]],
              inverse_trc[1][asset.base_rgb16[pixel * 3u + 1u]],
              inverse_trc[2][asset.base_rgb16[pixel * 3u + 2u]]};
          const auto app_base = convert_gainmap_base_primaries(
              base_linear, asset.base_primaries, asset.application_primaries);
          for (size_t channel = 0; channel < 3; ++channel) {
            state.app_row[x][channel] =
                (app_base[channel] + output.info.base_offset[channel]) *
                    factors[channel] - output.info.alternate_offset[channel];
          }
        }
        state.gain_cpu_ms += elapsed_ms(gain_start);

        const auto color_start = Clock::now();
        for (uint32_t x = 0; x < width; ++x) {
          const size_t pixel = row_pixel + x;
          const auto rec2020 = rec2020_from_gainmap_base(
              state.app_row[x], asset.application_primaries);
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
  auto container = open_heif_gainmap_container(path, cancel);
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint16_t> map;
  decode_heif_gainmap_planes(
      container.gain_handle.get(), container.metadata.multichannel, cancel,
      container.info, width, height, map);
  const uint32_t channels = container.info.gain_map_channels == 3u ? 3u : 1u;
  auto info = std::move(container.info);
  const char* encoding = info.gain_map_family == "apple-auxiliary-tmap"
      ? "Apple auxiliary HDR gain-map image"
      : "HEIC ISO tmap gain-map image";
  auto result = make_raster(std::move(info), width, height, channels, std::move(map),
                            encoding, cancel);
  normalize_gainmap_orientation(result, result.info.original_orientation);
  result.decode_ms = elapsed_ms(begin);
  return result;
}

}  // namespace hdrbridge::gainmap
