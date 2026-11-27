#include "codecs/gainmap/icc_description.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace hdrbridge::gainmap::detail {

namespace {

uint32_t read_be32_at(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

uint16_t read_be16_at(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

struct AdaptiveIccTags {
  bool valid = false;
  bool hagc_profile_allowed = false;
  bool has_a2b0 = false;
  bool has_b2a0 = false;
  bool has_hdgm = false;
  uint32_t hagc_offset = 0;
  uint32_t hagc_size = 0;
  uint32_t lumi_offset = 0;
  uint32_t lumi_size = 0;
};

AdaptiveIccTags parse_adaptive_icc_tags(const uint8_t* data, size_t size) {
  AdaptiveIccTags tags;
  if (!data || size < 132u || std::memcmp(data + 36u, "acsp", 4u) != 0) return tags;
  const uint32_t profile_size = read_be32_at(data);
  if (profile_size < 132u || profile_size > size) return tags;
  const uint32_t tag_count = read_be32_at(data + 128u);
  if (tag_count > (profile_size - 132u) / 12u) return tags;
  // ICC.1 HAGC §1.1: permitted when data colour space is RGB ('RGB ') and
  // profile class is Input ('scnr'), Display ('mntr'), or ColorSpace ('spac').
  const bool is_rgb = std::memcmp(data + 16u, "RGB ", 4u) == 0;
  const bool valid_class = std::memcmp(data + 12u, "scnr", 4u) == 0 ||
                           std::memcmp(data + 12u, "mntr", 4u) == 0 ||
                           std::memcmp(data + 12u, "spac", 4u) == 0;
  tags.hagc_profile_allowed = is_rgb && valid_class;
  tags.valid = true;
  for (uint32_t tag = 0; tag < tag_count; ++tag) {
    const uint8_t* entry = data + 132u + static_cast<size_t>(tag) * 12u;
    const uint32_t signature = read_be32_at(entry);
    const uint32_t data_offset = read_be32_at(entry + 4u);
    const uint32_t data_size = read_be32_at(entry + 8u);
    if (data_offset > profile_size || data_size > profile_size - data_offset) continue;
    tags.has_a2b0 |= signature == 0x41324230u;  // A2B0
    tags.has_b2a0 |= signature == 0x42324130u;  // B2A0
    tags.has_hdgm |= signature == 0x6864676du;  // hdgm
    if (signature == 0x48414743u) {             // HAGC (SMPTE ST 2094-50)
      tags.hagc_offset = data_offset;
      tags.hagc_size = data_size;
    } else if (signature == 0x6c756d69u) {      // lumi
      tags.lumi_offset = data_offset;
      tags.lumi_size = data_size;
    }
  }
  return tags;
}

}  // namespace

std::string profile_description(cmsHPROFILE profile) {
  std::array<char, 256> description{};
  cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US",
                         description.data(), static_cast<cmsUInt32Number>(description.size()));
  return description.data();
}

std::string profile_description(const void* data, uint32_t size) {
  const CmsProfilePtr profile(cmsOpenProfileFromMem(data, size));
  return profile ? profile_description(profile.get()) : std::string{};
}

bool is_adaptive_hdr_icc(const void* data, size_t size) {
  const auto tags = parse_adaptive_icc_tags(static_cast<const uint8_t*>(data), size);
  return tags.valid && tags.has_a2b0 && tags.has_b2a0 &&
         (tags.has_hdgm || tags.hagc_size > 0);
}

std::optional<double> adaptive_hdr_icc_reference_white_nits(const void* raw_data,
                                                            size_t size,
                                                            bool allow_lumi) {
  const auto* data = static_cast<const uint8_t*>(raw_data);
  const auto tags = parse_adaptive_icc_tags(data, size);
  if (!tags.valid) return std::nullopt;
  // iOS 27+ / SMPTE ST 2094-50 HAGC tag: header 'hagc' (0..3) + 4B reserved (4..7)
  // + 4B payload_size (8..11), followed by payload byte 0: General Info
  // (Application Version [7:5], Minimum Application Version [4:2], Reserved [1:0]),
  // byte 1: flags (bit 7 = has_custom_hdr_reference_white_flag).
  // A2B0/B2A0 LUTs are optional per ICC HAGC §2 and §1.1.
  if (tags.hagc_profile_allowed && tags.hagc_size >= 14u) {
    const uint8_t* hagc = data + tags.hagc_offset;
    if (std::memcmp(hagc, "hagc", 4u) == 0) {
      const uint32_t payload_size = read_be32_at(hagc + 8u);
      if (payload_size >= 2u && payload_size <= tags.hagc_size - 12u) {
        const uint8_t* payload = hagc + 12u;
        const uint8_t min_application_version = (payload[0] >> 2u) & 0x07u;
        if (min_application_version == 0u) {
          const bool has_custom_reference_white = (payload[1] & 0x80u) != 0;
          if (has_custom_reference_white) {
            if (payload_size >= 4u) {
              const uint16_t raw_custom = read_be16_at(payload + 2u);
              const uint32_t clamped = std::clamp<uint32_t>(raw_custom, 1u, 50000u);
              return static_cast<double>(clamped) / 5.0;
            }
          } else {
            return 203.0;
          }
        }
      }
    }
  }
  // Legacy iOS 18 Adaptive HDR ("Gain Map Preview") ICC luminance tag.
  if (allow_lumi && tags.has_a2b0 && tags.has_b2a0 && tags.has_hdgm &&
      tags.lumi_size >= 20u) {
    const uint8_t* lumi = data + tags.lumi_offset;
    if (std::memcmp(lumi, "XYZ ", 4u) == 0) {
      const int32_t y_fixed = static_cast<int32_t>(read_be32_at(lumi + 12u));
      const double nits = static_cast<double>(y_fixed) / 65536.0;
      if (std::isfinite(nits) && nits > 0.0 && nits <= 10000.0) {
        return nits;
      }
    }
  }
  return std::nullopt;
}

std::vector<std::vector<uint8_t>> extract_jpeg_icc_profiles(const uint8_t* data,
                                                            size_t size) {
  std::vector<std::vector<uint8_t>> profiles;
  if (!data || size < 4u || data[0] != 0xffu || data[1] != 0xd8u) return profiles;
  uint8_t pending_total = 0;
  std::vector<std::vector<uint8_t>> pending_chunks;
  size_t offset = 2u;
  while (offset + 1u < size) {
    while (offset < size && data[offset] != 0xffu) ++offset;
    while (offset < size && data[offset] == 0xffu) ++offset;
    if (offset >= size) break;
    const uint8_t marker = data[offset++];
    if (marker == 0xd9u || marker == 0xdau) break;
    if (marker == 0x01u || (marker >= 0xd0u && marker <= 0xd7u)) continue;
    if (offset + 2u > size) break;
    const size_t segment_size =
        (static_cast<size_t>(data[offset]) << 8u) | data[offset + 1u];
    if (segment_size < 2u || segment_size > size - offset) break;
    const uint8_t* payload = data + offset + 2u;
    const size_t payload_size = segment_size - 2u;
    if (marker == 0xe2u && payload_size >= 14u &&
        std::memcmp(payload, "ICC_PROFILE\0", 12u) == 0) {
      const uint8_t seq = payload[12];
      const uint8_t total = payload[13];
      if (seq >= 1u && total >= 1u && seq <= total) {
        if (pending_total != total || !pending_chunks[seq - 1u].empty()) {
          pending_total = total;
          pending_chunks.assign(total, {});
        }
        pending_chunks[seq - 1u].assign(payload + 14u, payload + payload_size);
        const bool complete = std::all_of(
            pending_chunks.begin(), pending_chunks.end(),
            [](const auto& chunk) { return !chunk.empty(); });
        if (complete) {
          std::vector<uint8_t> combined;
          for (const auto& chunk : pending_chunks) {
            combined.insert(combined.end(), chunk.begin(), chunk.end());
          }
          profiles.push_back(std::move(combined));
          pending_total = 0;
          pending_chunks.clear();
        }
      }
    }
    offset += segment_size;
  }
  return profiles;
}

std::optional<double> jpeg_adaptive_hdr_icc_reference_white_nits(
    const uint8_t* base_data,
    size_t base_size,
    const uint8_t* gain_data,
    size_t gain_size,
    bool allow_lumi) {
  std::optional<double> found;
  const auto scan_jpeg = [&](const uint8_t* data, size_t size) {
    for (const auto& profile : extract_jpeg_icc_profiles(data, size)) {
      if (const auto nits = adaptive_hdr_icc_reference_white_nits(
              profile.data(), profile.size(), allow_lumi)) {
        if (found && std::abs(*found - *nits) > 1e-6) {
          throw std::runtime_error(
              "Adaptive HDR ICC contains conflicting reference-white luminance tags");
        }
        found = *nits;
      }
    }
  };
  scan_jpeg(base_data, base_size);
  scan_jpeg(gain_data, gain_size);
  return found;
}

}  // namespace hdrbridge::gainmap::detail
