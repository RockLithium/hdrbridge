#include "codecs/hsp.h"
#include "codecs/heif.h"
#include "infrastructure/files.h"
#include "infrastructure/progress.h"
#include "metadata/exif_repack.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace hdrbridge::detail {
namespace {

constexpr uint16_t kHspCompressionCode = 51176;
constexpr size_t kMaxIfdEntries = 4096;
constexpr size_t kMaxStripCount = 4096;
constexpr size_t kMaxNalCount = 65536;
constexpr size_t kMaxMetadataBytes = 16u * 1024u * 1024u;
constexpr uint32_t kMaxImageDimension = 65535u;

bool fits(size_t total, size_t offset, size_t length) {
  return offset <= total && length <= total - offset;
}

uint16_t read_u16(const uint8_t* p, bool le) {
  return le ? static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8))
            : static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_u32(const uint8_t* p, bool le) {
  return le ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
            : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
}

void write_be16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>(value & 0xffu));
}

void write_be32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>(value & 0xffu));
}

size_t tiff_type_size(uint16_t type) {
  switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: case 13: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
  }
}

struct TiffEntry {
  uint16_t tag = 0;
  uint16_t type = 0;
  uint32_t count = 0;
  size_t value_offset = 0;
  size_t value_bytes = 0;
};

std::vector<uint32_t> read_tiff_uint_array(const std::vector<uint8_t>& bytes,
                                           const TiffEntry& entry,
                                           bool le,
                                           size_t max_count) {
  if (entry.count == 0 || entry.count > max_count) {
    throw std::runtime_error("invalid HSP TIFF array tag count");
  }
  if (entry.type != 3 && entry.type != 4) {
    throw std::runtime_error("unexpected HSP TIFF integer tag type");
  }
  const size_t elem_size = entry.type == 3 ? 2u : 4u;
  if (!fits(bytes.size(), entry.value_offset, static_cast<size_t>(entry.count) * elem_size)) {
    throw std::runtime_error("HSP TIFF array value out of range");
  }
  std::vector<uint32_t> values(entry.count);
  for (uint32_t i = 0; i < entry.count; ++i) {
    const uint8_t* ptr = bytes.data() + entry.value_offset + static_cast<size_t>(i) * elem_size;
    values[i] = entry.type == 3 ? static_cast<uint32_t>(read_u16(ptr, le))
                                : read_u32(ptr, le);
  }
  return values;
}

class BitReader {
 public:
  explicit BitReader(const std::vector<uint8_t>& data) : data_(data) {}

  uint32_t read_bits(size_t count) {
    if (count > 32) throw std::runtime_error("HEVC SPS bit read exceeds 32 bits");
    if (count > total_bits() - bit_pos_) {
      throw std::runtime_error("truncated HEVC SPS");
    }
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) {
      const size_t byte_index = bit_pos_ >> 3;
      const size_t bit_shift = 7u - (bit_pos_ & 7u);
      value = (value << 1) | ((data_[byte_index] >> bit_shift) & 1u);
      ++bit_pos_;
    }
    return value;
  }

  uint64_t read_bits64(size_t count) {
    if (count > 64) throw std::runtime_error("HEVC SPS bit read exceeds 64 bits");
    if (count > total_bits() - bit_pos_) {
      throw std::runtime_error("truncated HEVC SPS");
    }
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) {
      const size_t byte_index = bit_pos_ >> 3;
      const size_t bit_shift = 7u - (bit_pos_ & 7u);
      value = (value << 1) | ((data_[byte_index] >> bit_shift) & 1u);
      ++bit_pos_;
    }
    return value;
  }

  uint32_t read_ue() {
    size_t zeros = 0;
    while (read_bits(1) == 0) {
      ++zeros;
      if (zeros > 31) throw std::runtime_error("invalid HEVC SPS exp-golomb code");
    }
    if (zeros == 0) return 0;
    const uint32_t suffix = read_bits(zeros);
    return ((1u << zeros) - 1u) + suffix;
  }

  int32_t read_se() {
    const uint32_t code = read_ue();
    if (code == 0) return 0;
    const uint32_t mag = (code + 1u) >> 1;
    if ((code & 1u) != 0) {
      return mag <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
          ? static_cast<int32_t>(mag)
          : throw std::runtime_error("HEVC SPS se(v) overflow");
    }
    return -static_cast<int32_t>(mag);
  }

 private:
  size_t total_bits() const { return data_.size() * 8u; }

  const std::vector<uint8_t>& data_;
  size_t bit_pos_ = 0;
};

std::vector<uint8_t> ebsp_to_rbsp(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(size);
  size_t i = 0;
  while (i < size) {
    if (i + 2 < size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x03) {
      out.push_back(0x00);
      out.push_back(0x00);
      i += 3;
    } else {
      out.push_back(data[i]);
      ++i;
    }
  }
  return out;
}

void skip_scaling_list_data(BitReader& r) {
  for (uint32_t size_id = 0; size_id < 4; ++size_id) {
    const uint32_t matrix_count = size_id == 3 ? 2u : 6u;
    for (uint32_t matrix_id = 0; matrix_id < matrix_count; ++matrix_id) {
      const uint32_t pred_mode_flag = r.read_bits(1);
      if (!pred_mode_flag) {
        (void)r.read_ue();  // scaling_list_pred_matrix_id_delta
      } else {
        const uint32_t coef_num = std::min(64u, 1u << (4u + (size_id << 1)));
        if (size_id > 1) (void)r.read_se();  // scaling_list_dc_coef_minus8
        for (uint32_t i = 0; i < coef_num; ++i) {
          (void)r.read_se();  // scaling_list_delta_coef
        }
      }
    }
  }
}

void skip_short_term_ref_pic_sets(BitReader& r, uint32_t num_rps) {
  if (num_rps > 64) throw std::runtime_error("HEVC SPS short-term RPS count out of range");
  std::vector<uint32_t> num_delta_pocs(num_rps, 0);
  for (uint32_t st_idx = 0; st_idx < num_rps; ++st_idx) {
    const bool inter_pred = st_idx != 0 && r.read_bits(1) != 0;
    if (inter_pred) {
      const uint32_t ref_idx = st_idx - 1u;
      (void)r.read_bits(1);  // delta_rps_sign
      (void)r.read_ue();     // abs_delta_rps_minus1
      uint32_t count = 0;
      for (uint32_t j = 0; j <= num_delta_pocs[ref_idx]; ++j) {
        const uint32_t used = r.read_bits(1);
        const uint32_t use_delta = used ? 1u : r.read_bits(1);
        if (used || use_delta) ++count;
      }
      num_delta_pocs[st_idx] = count;
    } else {
      const uint32_t num_neg = r.read_ue();
      const uint32_t num_pos = r.read_ue();
      if (num_neg > 32 || num_pos > 32) {
        throw std::runtime_error("HEVC SPS short-term RPS picture count out of range");
      }
      num_delta_pocs[st_idx] = num_neg + num_pos;
      for (uint32_t i = 0; i < num_neg + num_pos; ++i) {
        (void)r.read_ue();
        (void)r.read_bits(1);
      }
    }
  }
}

struct SpsMetadata {
  uint8_t profile_space_tier_idc = 0;
  uint32_t compat_flags = 0;
  uint64_t constraint_flags = 0;
  uint8_t level_idc = 0;
  uint8_t max_sub_layers_minus1 = 0;
  uint8_t temporal_id_nesting = 0;
  uint8_t chroma_format_idc = 0;
  uint8_t bit_depth_luma = 0;
  uint8_t bit_depth_chroma = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t color_primaries = 9;
  uint16_t transfer_characteristics = 18;
  uint16_t matrix_coeffs = 9;
  bool full_range = false;
  bool has_vui_primaries = false;
  bool has_vui_transfer = false;
  bool has_vui_matrix = false;
  bool has_vui_range = false;
};

SpsMetadata parse_hevc_sps(const std::vector<uint8_t>& nal) {
  if (nal.size() < 4) throw std::runtime_error("invalid HEVC SPS NAL");
  const auto rbsp = ebsp_to_rbsp(nal.data() + 2, nal.size() - 2);
  BitReader r(rbsp);
  SpsMetadata sps;

  (void)r.read_bits(4);  // sps_video_parameter_set_id
  sps.max_sub_layers_minus1 = static_cast<uint8_t>(r.read_bits(3));
  if (sps.max_sub_layers_minus1 > 6) {
    throw std::runtime_error("invalid HEVC SPS max_sub_layers_minus1");
  }
  sps.temporal_id_nesting = static_cast<uint8_t>(r.read_bits(1));

  // profile_tier_level(1, max_sub_layers_minus1)
  sps.profile_space_tier_idc = static_cast<uint8_t>(r.read_bits(8));
  sps.compat_flags = r.read_bits(32);
  sps.constraint_flags = r.read_bits64(48);
  sps.level_idc = static_cast<uint8_t>(r.read_bits(8));

  std::array<uint8_t, 8> sub_profile{};
  std::array<uint8_t, 8> sub_level{};
  for (uint8_t i = 0; i < sps.max_sub_layers_minus1; ++i) {
    sub_profile[i] = static_cast<uint8_t>(r.read_bits(1));
    sub_level[i] = static_cast<uint8_t>(r.read_bits(1));
  }
  if (sps.max_sub_layers_minus1 > 0) {
    for (uint8_t i = sps.max_sub_layers_minus1; i < 8; ++i) {
      (void)r.read_bits(2);
    }
  }
  for (uint8_t i = 0; i < sps.max_sub_layers_minus1; ++i) {
    if (sub_profile[i]) {
      (void)r.read_bits(8);
      (void)r.read_bits(32);
      (void)r.read_bits64(48);
    }
    if (sub_level[i]) {
      (void)r.read_bits(8);
    }
  }

  (void)r.read_ue();  // sps_seq_parameter_set_id
  const uint32_t chroma = r.read_ue();
  if (chroma > 3) throw std::runtime_error("invalid HEVC SPS chroma_format_idc");
  sps.chroma_format_idc = static_cast<uint8_t>(chroma);
  uint32_t separate_colour_plane = 0;
  if (chroma == 3) separate_colour_plane = r.read_bits(1);

  const uint32_t coded_w = r.read_ue();
  const uint32_t coded_h = r.read_ue();
  if (coded_w == 0 || coded_h == 0 || coded_w > kMaxImageDimension || coded_h > kMaxImageDimension) {
    throw std::runtime_error("HEVC SPS dimensions out of range");
  }

  uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
  if (r.read_bits(1) != 0) {  // conformance_window_flag
    crop_left = r.read_ue();
    crop_right = r.read_ue();
    crop_top = r.read_ue();
    crop_bottom = r.read_ue();
  }
  const uint32_t sub_width_c = (chroma == 1 || chroma == 2) ? 2u : 1u;
  const uint32_t sub_height_c = (chroma == 1) ? 2u : 1u;
  (void)separate_colour_plane;
  const uint64_t crop_x = (static_cast<uint64_t>(crop_left) + crop_right) * sub_width_c;
  const uint64_t crop_y = (static_cast<uint64_t>(crop_top) + crop_bottom) * sub_height_c;
  if (crop_x >= coded_w || crop_y >= coded_h) {
    throw std::runtime_error("HEVC SPS conformance window exceeds coded dimensions");
  }
  sps.width = coded_w - static_cast<uint32_t>(crop_x);
  sps.height = coded_h - static_cast<uint32_t>(crop_y);

  const uint32_t luma_minus8 = r.read_ue();
  const uint32_t chroma_minus8 = r.read_ue();
  if (luma_minus8 > 8 || chroma_minus8 > 8) {
    throw std::runtime_error("HEVC SPS bit depth out of range");
  }
  sps.bit_depth_luma = static_cast<uint8_t>(luma_minus8 + 8u);
  sps.bit_depth_chroma = static_cast<uint8_t>(chroma_minus8 + 8u);
  if (sps.chroma_format_idc < 1 || sps.chroma_format_idc > 3) {
    throw std::runtime_error("unsupported HSP HEVC chroma format (expected 4:2:0, 4:2:2, or 4:4:4)");
  }

  const uint32_t log2_max_poc_lsb = r.read_ue() + 4u;
  if (log2_max_poc_lsb > 16u) throw std::runtime_error("invalid HEVC SPS POC bits");
  const bool sub_layer_ordering = r.read_bits(1) != 0;
  for (uint8_t i = sub_layer_ordering ? 0 : sps.max_sub_layers_minus1;
       i <= sps.max_sub_layers_minus1; ++i) {
    (void)r.read_ue();
    (void)r.read_ue();
    (void)r.read_ue();
  }
  for (int i = 0; i < 6; ++i) (void)r.read_ue();

  if (r.read_bits(1) != 0) {  // scaling_list_enabled_flag
    if (r.read_bits(1) != 0) {  // sps_scaling_list_data_present_flag
      skip_scaling_list_data(r);
    }
  }
  (void)r.read_bits(1);  // amp_enabled_flag
  (void)r.read_bits(1);  // sample_adaptive_offset_enabled_flag
  if (r.read_bits(1) != 0) {  // pcm_enabled_flag
    (void)r.read_bits(4);
    (void)r.read_bits(4);
    (void)r.read_ue();
    (void)r.read_ue();
    (void)r.read_bits(1);
  }

  const uint32_t num_short_term_rps = r.read_ue();
  skip_short_term_ref_pic_sets(r, num_short_term_rps);
  if (r.read_bits(1) != 0) {  // long_term_ref_pics_present_flag
    const uint32_t num_lt = r.read_ue();
    if (num_lt > 32) throw std::runtime_error("HEVC SPS long-term RPS count out of range");
    for (uint32_t i = 0; i < num_lt; ++i) {
      (void)r.read_bits(log2_max_poc_lsb);
      (void)r.read_bits(1);
    }
  }
  (void)r.read_bits(1);  // sps_temporal_mvp_enabled_flag
  (void)r.read_bits(1);  // strong_intra_smoothing_enabled_flag

  if (r.read_bits(1) != 0) {  // vui_parameters_present_flag
    if (r.read_bits(1) != 0) {  // aspect_ratio_info_present_flag
      const uint32_t aspect_idc = r.read_bits(8);
      if (aspect_idc == 255u) {
        (void)r.read_bits(16);
        (void)r.read_bits(16);
      }
    }
    if (r.read_bits(1) != 0) {  // overscan_info_present_flag
      (void)r.read_bits(1);
    }
    if (r.read_bits(1) != 0) {  // video_signal_type_present_flag
      (void)r.read_bits(3);  // video_format
      sps.full_range = r.read_bits(1) != 0;
      sps.has_vui_range = true;
      if (r.read_bits(1) != 0) {  // colour_description_present_flag
        const uint16_t primaries = static_cast<uint16_t>(r.read_bits(8));
        const uint16_t transfer = static_cast<uint16_t>(r.read_bits(8));
        const uint16_t matrix = static_cast<uint16_t>(r.read_bits(8));
        if (primaries != 2u) {
          sps.color_primaries = primaries;
          sps.has_vui_primaries = true;
        }
        if (transfer != 2u) {
          sps.transfer_characteristics = transfer;
          sps.has_vui_transfer = true;
        }
        if (matrix != 2u) {
          sps.matrix_coeffs = matrix;
          sps.has_vui_matrix = true;
        }
      }
    }
  }
  return sps;
}

std::vector<std::vector<uint8_t>> split_annexb_nals(const std::vector<uint8_t>& stream) {
  std::vector<std::vector<uint8_t>> nals;
  size_t pos = 0;
  auto find_start_code = [&](size_t from, size_t& sc_start, size_t& sc_len) -> bool {
    for (size_t i = from; i + 2 < stream.size(); ++i) {
      if (stream[i] == 0x00 && stream[i + 1] == 0x00 && stream[i + 2] == 0x01) {
        if (i > from && stream[i - 1] == 0x00) {
          sc_start = i - 1;
          sc_len = 4;
        } else {
          sc_start = i;
          sc_len = 3;
        }
        return true;
      }
    }
    return false;
  };

  size_t cur_start = 0, cur_len = 0;
  if (!find_start_code(0, cur_start, cur_len)) return nals;
  pos = cur_start + cur_len;
  while (pos < stream.size()) {
    size_t next_start = 0, next_len = 0;
    const bool has_next = find_start_code(pos, next_start, next_len);
    const size_t nal_end = has_next ? next_start : stream.size();
    if (nal_end > pos) {
      if (nals.size() >= kMaxNalCount) {
        throw std::runtime_error("HSP HEVC NAL unit count exceeds safety limit");
      }
      nals.emplace_back(stream.begin() + pos, stream.begin() + nal_end);
    }
    if (!has_next) break;
    pos = next_start + next_len;
  }
  return nals;
}

std::vector<uint8_t> build_hvcc_box_payload(const std::vector<std::vector<uint8_t>>& vps,
                                            const std::vector<std::vector<uint8_t>>& sps_list,
                                            const std::vector<std::vector<uint8_t>>& pps,
                                            const SpsMetadata& sps) {
  std::vector<uint8_t> hvcc;
  hvcc.push_back(1);  // configurationVersion
  hvcc.push_back(sps.profile_space_tier_idc);
  write_be32(hvcc, sps.compat_flags);
  for (int shift = 40; shift >= 0; shift -= 8) {
    hvcc.push_back(static_cast<uint8_t>((sps.constraint_flags >> shift) & 0xffu));
  }
  hvcc.push_back(sps.level_idc);
  write_be16(hvcc, 0xf000u);  // min_spatial_segmentation_idc = 0
  hvcc.push_back(0xfcu);      // parallelismType = 0
  hvcc.push_back(static_cast<uint8_t>(0xfcu | (sps.chroma_format_idc & 0x03u)));
  hvcc.push_back(static_cast<uint8_t>(0xf8u | ((sps.bit_depth_luma - 8u) & 0x07u)));
  hvcc.push_back(static_cast<uint8_t>(0xf8u | ((sps.bit_depth_chroma - 8u) & 0x07u)));
  write_be16(hvcc, 0);        // avgFrameRate = 0
  const uint8_t num_layers = static_cast<uint8_t>((sps.max_sub_layers_minus1 + 1u) & 0x07u);
  hvcc.push_back(static_cast<uint8_t>(0x03u | (num_layers << 3) | ((sps.temporal_id_nesting & 1u) << 2)));

  struct NalGroup {
    uint8_t type;
    const std::vector<std::vector<uint8_t>>* list;
  };
  const std::array<NalGroup, 3> groups{{{32, &vps}, {33, &sps_list}, {34, &pps}}};
  hvcc.push_back(static_cast<uint8_t>(groups.size()));
  for (const auto& group : groups) {
    if (group.list->size() > 65535u) {
      throw std::runtime_error("too many HEVC parameter sets in HSP stream");
    }
    hvcc.push_back(static_cast<uint8_t>(0x80u | group.type));  // array_completeness = 1
    write_be16(hvcc, static_cast<uint16_t>(group.list->size()));
    for (const auto& nal : *group.list) {
      if (nal.size() > 65535u) {
        throw std::runtime_error("HEVC parameter set exceeds 64 KB");
      }
      write_be16(hvcc, static_cast<uint16_t>(nal.size()));
      hvcc.insert(hvcc.end(), nal.begin(), nal.end());
    }
  }
  return hvcc;
}

std::vector<uint8_t> make_box(const char type[4], const std::vector<uint8_t>& payload) {
  if (payload.size() > std::numeric_limits<uint32_t>::max() - 8u) {
    throw std::runtime_error("HEIF box size overflow");
  }
  std::vector<uint8_t> box;
  box.reserve(8u + payload.size());
  write_be32(box, static_cast<uint32_t>(8u + payload.size()));
  box.insert(box.end(), type, type + 4);
  box.insert(box.end(), payload.begin(), payload.end());
  return box;
}

std::vector<uint8_t> make_fullbox(const char type[4], uint8_t version, uint32_t flags,
                                  const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> full;
  full.reserve(4u + payload.size());
  write_be32(full, (static_cast<uint32_t>(version) << 24) | (flags & 0x00ffffffu));
  full.insert(full.end(), payload.begin(), payload.end());
  return make_box(type, full);
}

void append_bytes(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

std::vector<uint8_t> extract_valid_xmp(const std::vector<uint8_t>& bytes,
                                       const TiffEntry& entry) {
  if (entry.value_bytes == 0 || entry.value_bytes > kMaxMetadataBytes ||
      !fits(bytes.size(), entry.value_offset, entry.value_bytes)) {
    return {};
  }
  size_t len = entry.value_bytes;
  while (len > 0 && bytes[entry.value_offset + len - 1] == 0) --len;
  if (len == 0) return {};
  const uint8_t* ptr = bytes.data() + entry.value_offset;
  size_t start = 0;
  if (len >= 3 && ptr[0] == 0xef && ptr[1] == 0xbb && ptr[2] == 0xbf) start = 3;
  while (start < len && (ptr[start] == ' ' || ptr[start] == '\t' ||
                         ptr[start] == '\r' || ptr[start] == '\n')) {
    ++start;
  }
  if (start >= len || ptr[start] != '<') return {};
  return std::vector<uint8_t>(ptr, ptr + len);
}

// Map EXIF Orientation 1..8 to ISO/IEC 23008-12 irot (CCW 90-deg steps) and
// imir (0 = vertical flip, 1 = horizontal flip) matching heif.cpp.
struct HeifTransform {
  int irot_steps = -1;  // -1 = none, 1 = 90 CCW, 2 = 180, 3 = 270 CCW
  int imir_axis = -1;   // -1 = none, 0 = vertical, 1 = horizontal
};

HeifTransform heif_transform_for_exif_orientation(uint8_t orientation) {
  switch (orientation) {
    case 2: return {-1, 1};
    case 3: return {2, -1};
    case 4: return {-1, 0};
    case 5: return {1, 0};
    case 6: return {3, -1};
    case 7: return {1, 1};
    case 8: return {1, -1};
    default: return {-1, -1};
  }
}

struct HspHeifPackage {
  std::vector<uint8_t> heif;
  bool exif_read_error = false;
  bool has_vui_primaries = false;
  bool has_vui_transfer = false;
  bool has_vui_matrix = false;
  bool has_vui_range = false;
  uint16_t color_primaries = 9;
  uint16_t transfer_characteristics = 18;
  uint16_t matrix_coeffs = 9;
};

void apply_hsp_source_info(SourceInfo& info, const HspHeifPackage& pkg) {
  info.format = "HSP";
  info.container_brand = "HSP";
  if (pkg.exif_read_error) {
    info.exif_present = false;
    info.exif_status = "read-error";
  }
  const bool full_vui =
      pkg.has_vui_primaries && pkg.has_vui_transfer && pkg.has_vui_matrix && pkg.has_vui_range;
  const bool any_vui_color =
      pkg.has_vui_primaries || pkg.has_vui_transfer || pkg.has_vui_matrix;
  if (full_vui) {
    info.native_color_present = true;
    info.native_color_description = "HEVC SPS VUI";
    info.color_signal_kind = "HEVC SPS VUI";
    info.resolved_signaling_source = "HEVC SPS VUI";
  } else if (any_vui_color || pkg.has_vui_range) {
    info.native_color_present = any_vui_color;
    info.native_primaries = pkg.has_vui_primaries ? pkg.color_primaries : 0;
    info.native_transfer = pkg.has_vui_transfer ? pkg.transfer_characteristics : 0;
    info.native_matrix = pkg.has_vui_matrix ? pkg.matrix_coeffs : 0;
    info.native_range_known = pkg.has_vui_range;
    info.native_color_description = any_vui_color
        ? "HEVC SPS VUI (partial) + HSP default"
        : "HEVC SPS VUI range + HSP default (BT.2020 HLG)";
    info.color_signal_kind = "HEVC SPS VUI + HSP default";
    info.resolved_signaling_source = "HEVC SPS VUI + HSP default";
  } else {
    info.native_color_present = false;
    info.native_color_description = "Absent (Panasonic HSP default BT.2020 HLG)";
    info.native_primaries = 0;
    info.native_transfer = 0;
    info.native_matrix = 0;
    info.native_range_known = false;
    info.color_signal_kind = "HSP default (BT.2020 HLG)";
    info.resolved_signaling_source = "HSP default";
  }
}

HspHeifPackage build_hsp_heif_package(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 16) {
    throw std::runtime_error("file is too small to be a Panasonic HSP image");
  }
  const bool le = bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == 42 && bytes[3] == 0;
  const bool be = bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0 && bytes[3] == 42;
  if (!le && !be) {
    throw std::runtime_error("not a Panasonic HSP file (invalid TIFF header)");
  }

  const uint32_t ifd0_offset = read_u32(bytes.data() + 4, le);
  if (ifd0_offset < 8 || !fits(bytes.size(), ifd0_offset, 2)) {
    throw std::runtime_error("HSP IFD0 offset out of range");
  }
  const uint16_t entry_count = read_u16(bytes.data() + ifd0_offset, le);
  if (entry_count == 0 || entry_count > kMaxIfdEntries ||
      !fits(bytes.size(), static_cast<size_t>(ifd0_offset) + 2u,
            static_cast<size_t>(entry_count) * 12u + 4u)) {
    throw std::runtime_error("HSP IFD0 directory out of range");
  }

  bool has_compression = false;
  uint32_t compression = 0;
  bool has_strip_offsets = false;
  TiffEntry strip_offsets{};
  bool has_strip_counts = false;
  TiffEntry strip_counts{};
  bool has_panasonic_len = false;
  TiffEntry panasonic_len{};
  bool has_exif_ifd = false;
  bool has_gps_ifd = false;
  bool has_camera_ifd0 = false;
  bool exif_tag_corrupt = false;
  bool has_xmp = false;
  TiffEntry xmp_entry{};
  uint8_t orientation = 1;

  for (uint16_t i = 0; i < entry_count; ++i) {
    const size_t pos = static_cast<size_t>(ifd0_offset) + 2u + static_cast<size_t>(i) * 12u;
    const uint16_t tag = read_u16(bytes.data() + pos, le);
    const uint16_t type = read_u16(bytes.data() + pos + 2u, le);
    const uint32_t count = read_u32(bytes.data() + pos + 4u, le);
    const size_t elem_size = tiff_type_size(type);
    const bool is_exif_tag = tag == 0x8769 || tag == 0x8825 ||
        tag == 0x010e || tag == 0x010f || tag == 0x0110 || tag == 0x0131 ||
        tag == 0x0132 || tag == 0x013b || tag == 0x8298;
    if (elem_size == 0 || count > std::numeric_limits<size_t>::max() / elem_size) {
      if (is_exif_tag) exif_tag_corrupt = true;
      continue;
    }
    const size_t value_bytes = static_cast<size_t>(count) * elem_size;
    size_t value_offset = pos + 8u;
    if (value_bytes > 4u) {
      const uint32_t ptr = read_u32(bytes.data() + pos + 8u, le);
      if (!fits(bytes.size(), ptr, value_bytes)) {
        if (is_exif_tag) exif_tag_corrupt = true;
        continue;
      }
      value_offset = ptr;
    }
    const TiffEntry entry{tag, type, count, value_offset, value_bytes};
    if (tag == 0x0103 && count == 1 && (type == 3 || type == 4)) {
      has_compression = true;
      compression = type == 3 ? read_u16(bytes.data() + value_offset, le)
                              : read_u32(bytes.data() + value_offset, le);
    } else if (tag == 0x0111) {
      has_strip_offsets = true;
      strip_offsets = entry;
    } else if (tag == 0x0117) {
      has_strip_counts = true;
      strip_counts = entry;
    } else if (tag == 0x9005 && count == 1 && (type == 3 || type == 4)) {
      has_panasonic_len = true;
      panasonic_len = entry;
    } else if (tag == 0x0112 && count == 1 && (type == 3 || type == 4)) {
      const uint32_t val = type == 3 ? read_u16(bytes.data() + value_offset, le)
                                     : read_u32(bytes.data() + value_offset, le);
      if (val >= 1 && val <= 8) orientation = static_cast<uint8_t>(val);
    } else if (tag == 0x8769) {
      has_exif_ifd = true;
    } else if (tag == 0x8825) {
      has_gps_ifd = true;
    } else if (tag == 0x010e || tag == 0x010f || tag == 0x0110 || tag == 0x0131 ||
               tag == 0x0132 || tag == 0x013b || tag == 0x8298) {
      has_camera_ifd0 = true;
    } else if (tag == 0x02bc) {
      has_xmp = true;
      xmp_entry = entry;
    }
  }

  if (!has_compression || compression != kHspCompressionCode) {
    throw std::runtime_error("not a Panasonic HSP file (expected TIFF compression 51176)");
  }
  if (!has_strip_offsets) {
    throw std::runtime_error("HSP file missing StripOffsets");
  }

  const auto offsets = read_tiff_uint_array(bytes, strip_offsets, le, kMaxStripCount);
  std::vector<uint32_t> lengths;
  if (offsets.size() == 1 && has_panasonic_len) {
    lengths = read_tiff_uint_array(bytes, panasonic_len, le, 1);
  } else if (has_strip_counts) {
    lengths = read_tiff_uint_array(bytes, strip_counts, le, kMaxStripCount);
    if (lengths.size() != offsets.size()) {
      throw std::runtime_error("HSP StripByteCounts count does not match StripOffsets");
    }
  } else if (offsets.size() == 1) {
    const size_t remaining = bytes.size() - offsets[0];
    if (remaining > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("HSP stream length overflow");
    }
    lengths.push_back(static_cast<uint32_t>(remaining));
  } else {
    throw std::runtime_error("multi-strip HSP missing StripByteCounts");
  }

  size_t total_stream_bytes = 0;
  for (size_t i = 0; i < offsets.size(); ++i) {
    if (offsets[i] == 0 || lengths[i] == 0 || !fits(bytes.size(), offsets[i], lengths[i])) {
      throw std::runtime_error("HSP HEVC stream offset or length out of bounds");
    }
    if (total_stream_bytes > std::numeric_limits<size_t>::max() - lengths[i]) {
      throw std::runtime_error("HSP HEVC stream size overflow");
    }
    total_stream_bytes += lengths[i];
  }

  std::vector<uint8_t> annexb;
  annexb.reserve(total_stream_bytes);
  for (size_t i = 0; i < offsets.size(); ++i) {
    annexb.insert(annexb.end(), bytes.begin() + offsets[i],
                  bytes.begin() + offsets[i] + lengths[i]);
  }

  const auto nals = split_annexb_nals(annexb);
  std::vector<std::vector<uint8_t>> vps, sps_list, pps, slices;
  for (const auto& nal : nals) {
    if (nal.size() < 2) continue;
    const uint8_t nal_type = static_cast<uint8_t>((nal[0] >> 1) & 0x3fu);
    if (nal_type == 32) vps.push_back(nal);
    else if (nal_type == 33) sps_list.push_back(nal);
    else if (nal_type == 34) pps.push_back(nal);
    else if (nal_type <= 31) slices.push_back(nal);
  }
  if (vps.empty() || sps_list.empty() || pps.empty()) {
    throw std::runtime_error("HSP HEVC stream lacks VPS/SPS/PPS parameter sets");
  }
  if (slices.empty()) {
    throw std::runtime_error("HSP HEVC stream contains no image slices");
  }

  const SpsMetadata sps = parse_hevc_sps(sps_list.back());
  const auto hvcc = build_hvcc_box_payload(vps, sps_list, pps, sps);

  // Build HEVC mdat payload (4-byte big-endian length prefix per VCL NAL).
  std::vector<uint8_t> image_payload;
  for (const auto& slice : slices) {
    if (slice.size() > std::numeric_limits<uint32_t>::max() - 4u) {
      throw std::runtime_error("HSP HEVC slice exceeds 4 GiB");
    }
    write_be32(image_payload, static_cast<uint32_t>(slice.size()));
    append_bytes(image_payload, slice);
  }

  // Repack TIFF EXIF (preserves IFD0 + ExifIFD + GPSIFD, drops MakerNote 0x927c).
  // Use post-transform dimensions for EXIF pixel tags while keeping Orientation=1
  // inside EXIF and carrying source rotation via HEIF irot/imir properties.
  // Malformed EXIF metadata must not block image inspection or pixel decoding.
  std::vector<uint8_t> exif_item;
  bool exif_read_error = exif_tag_corrupt;
  if (has_exif_ifd || has_gps_ifd || has_camera_ifd0) {
    try {
      const bool swaps = orientation >= 5;
      const uint32_t exif_w = swaps ? sps.height : sps.width;
      const uint32_t exif_h = swaps ? sps.width : sps.height;
      size_t remaining_meta = kMaxMetadataBytes;
      const auto repacked_tiff = metadata::repack(
          [&](size_t offset, size_t count) -> std::vector<uint8_t> {
            if (count > remaining_meta || !fits(bytes.size(), offset, count)) {
              throw std::runtime_error("HSP EXIF metadata offset or size is invalid");
            }
            remaining_meta -= count;
            return std::vector<uint8_t>(bytes.begin() + offset,
                                        bytes.begin() + offset + count);
          },
          exif_w, exif_h, true);
      if (!repacked_tiff.empty()) {
        write_be32(exif_item, 6u);
        const char exif_hdr[] = {'E', 'x', 'i', 'f', '\0', '\0'};
        exif_item.insert(exif_item.end(), exif_hdr, exif_hdr + 6);
        append_bytes(exif_item, repacked_tiff);
      }
    } catch (const std::exception&) {
      exif_item.clear();
      exif_read_error = true;
    }
  }

  std::vector<uint8_t> xmp_item;
  if (has_xmp) {
    xmp_item = extract_valid_xmp(bytes, xmp_entry);
  }

  // Assemble ISO/IEC 23008-12 HEIF container in memory.
  const std::string brand = sps.bit_depth_luma <= 8 ? "heic" : "heix";
  std::vector<uint8_t> ftyp_payload;
  ftyp_payload.insert(ftyp_payload.end(), brand.begin(), brand.end());
  write_be32(ftyp_payload, 0);
  const char mif1[] = {'m', 'i', 'f', '1'};
  const char miaf[] = {'m', 'i', 'a', 'f'};
  ftyp_payload.insert(ftyp_payload.end(), mif1, mif1 + 4);
  ftyp_payload.insert(ftyp_payload.end(), brand.begin(), brand.end());
  ftyp_payload.insert(ftyp_payload.end(), miaf, miaf + 4);
  const auto ftyp = make_box("ftyp", ftyp_payload);

  std::vector<uint8_t> hdlr_payload(4, 0);
  const char pict[] = {'p', 'i', 'c', 't'};
  hdlr_payload.insert(hdlr_payload.end(), pict, pict + 4);
  hdlr_payload.resize(hdlr_payload.size() + 13, 0);
  const auto hdlr = make_fullbox("hdlr", 0, 0, hdlr_payload);

  std::vector<uint8_t> pitm_payload;
  write_be16(pitm_payload, 1);
  const auto pitm = make_fullbox("pitm", 0, 0, pitm_payload);

  // Properties in ipco: 1=hvcC, 2=colr, 3=ispe, 4=pixi, [5=irot], [6=imir]
  std::vector<uint8_t> ipco_payload;
  std::vector<uint8_t> prop_assocs;
  append_bytes(ipco_payload, make_box("hvcC", hvcc));
  prop_assocs.push_back(0x81);  // essential: hvcC

  std::vector<uint8_t> colr_payload{'n', 'c', 'l', 'x'};
  write_be16(colr_payload, sps.color_primaries);
  write_be16(colr_payload, sps.transfer_characteristics);
  write_be16(colr_payload, sps.matrix_coeffs);
  colr_payload.push_back(sps.full_range ? 0x80u : 0x00u);
  append_bytes(ipco_payload, make_box("colr", colr_payload));
  prop_assocs.push_back(0x02);

  std::vector<uint8_t> ispe_payload;
  write_be32(ispe_payload, sps.width);
  write_be32(ispe_payload, sps.height);
  append_bytes(ipco_payload, make_fullbox("ispe", 0, 0, ispe_payload));
  prop_assocs.push_back(0x03);

  std::vector<uint8_t> pixi_payload{3, sps.bit_depth_luma, sps.bit_depth_chroma, sps.bit_depth_chroma};
  append_bytes(ipco_payload, make_fullbox("pixi", 0, 0, pixi_payload));
  prop_assocs.push_back(0x04);

  const HeifTransform transform = heif_transform_for_exif_orientation(orientation);
  uint8_t next_prop_idx = 5;
  if (transform.irot_steps >= 0) {
    append_bytes(ipco_payload, make_box("irot", {static_cast<uint8_t>(transform.irot_steps & 0x03)}));
    prop_assocs.push_back(static_cast<uint8_t>(0x80u | next_prop_idx++));
  }
  if (transform.imir_axis >= 0) {
    append_bytes(ipco_payload, make_box("imir", {static_cast<uint8_t>(transform.imir_axis & 0x01)}));
    prop_assocs.push_back(static_cast<uint8_t>(0x80u | next_prop_idx++));
  }

  std::vector<uint8_t> ipma_payload;
  write_be32(ipma_payload, 1);  // entry_count = 1
  write_be16(ipma_payload, 1);  // item_ID = 1
  ipma_payload.push_back(static_cast<uint8_t>(prop_assocs.size()));
  append_bytes(ipma_payload, prop_assocs);

  std::vector<uint8_t> iprp_payload;
  append_bytes(iprp_payload, make_box("ipco", ipco_payload));
  append_bytes(iprp_payload, make_fullbox("ipma", 0, 0, ipma_payload));
  const auto iprp = make_box("iprp", iprp_payload);

  // Items: 1=image, [2=Exif], [next=XMP]
  struct HeifItem {
    uint16_t id;
    std::vector<uint8_t> payload;
  };
  std::vector<HeifItem> items;
  items.push_back({1, std::move(image_payload)});

  std::vector<std::vector<uint8_t>> infe_boxes;
  {
    std::vector<uint8_t> p;
    write_be16(p, 1);
    write_be16(p, 0);
    const char t[] = {'h', 'v', 'c', '1', '\0'};
    p.insert(p.end(), t, t + 5);
    infe_boxes.push_back(make_fullbox("infe", 2, 0, p));
  }
  uint16_t next_id = 2;
  std::vector<uint8_t> iref_payload;
  if (!exif_item.empty()) {
    const uint16_t id = next_id++;
    items.push_back({id, std::move(exif_item)});
    std::vector<uint8_t> p;
    write_be16(p, id);
    write_be16(p, 0);
    const char t[] = {'E', 'x', 'i', 'f', '\0'};
    p.insert(p.end(), t, t + 5);
    infe_boxes.push_back(make_fullbox("infe", 2, 0, p));
    std::vector<uint8_t> cdsc;
    write_be16(cdsc, id);
    write_be16(cdsc, 1);
    write_be16(cdsc, 1);
    append_bytes(iref_payload, make_box("cdsc", cdsc));
  }
  if (!xmp_item.empty()) {
    const uint16_t id = next_id++;
    items.push_back({id, std::move(xmp_item)});
    std::vector<uint8_t> p;
    write_be16(p, id);
    write_be16(p, 0);
    const char t[] = "mime\0application/rdf+xml";
    p.insert(p.end(), t, t + sizeof(t));
    infe_boxes.push_back(make_fullbox("infe", 2, 0, p));
    std::vector<uint8_t> cdsc;
    write_be16(cdsc, id);
    write_be16(cdsc, 1);
    write_be16(cdsc, 1);
    append_bytes(iref_payload, make_box("cdsc", cdsc));
  }

  std::vector<uint8_t> iinf_payload;
  write_be16(iinf_payload, static_cast<uint16_t>(infe_boxes.size()));
  for (const auto& b : infe_boxes) append_bytes(iinf_payload, b);
  const auto iinf = make_fullbox("iinf", 0, 0, iinf_payload);
  const auto iref = iref_payload.empty() ? std::vector<uint8_t>{}
                                         : make_fullbox("iref", 0, 0, iref_payload);

  auto build_meta = [&](const std::vector<uint32_t>& offsets) {
    std::vector<uint8_t> iloc_payload{0x44, 0x00};
    write_be16(iloc_payload, static_cast<uint16_t>(items.size()));
    for (size_t i = 0; i < items.size(); ++i) {
      write_be16(iloc_payload, items[i].id);
      write_be16(iloc_payload, 0);  // data_reference_index
      write_be16(iloc_payload, 1);  // extent_count
      write_be32(iloc_payload, offsets[i]);
      write_be32(iloc_payload, static_cast<uint32_t>(items[i].payload.size()));
    }
    const auto iloc = make_fullbox("iloc", 0, 0, iloc_payload);
    std::vector<uint8_t> meta_payload;
    append_bytes(meta_payload, hdlr);
    append_bytes(meta_payload, pitm);
    append_bytes(meta_payload, iloc);
    append_bytes(meta_payload, iinf);
    if (!iref.empty()) append_bytes(meta_payload, iref);
    append_bytes(meta_payload, iprp);
    return make_fullbox("meta", 0, 0, meta_payload);
  };

  std::vector<uint32_t> item_offsets(items.size(), 0);
  const auto dummy_meta = build_meta(item_offsets);
  size_t cursor = ftyp.size() + dummy_meta.size() + 8u;
  for (size_t i = 0; i < items.size(); ++i) {
    if (cursor > std::numeric_limits<uint32_t>::max() - items[i].payload.size()) {
      throw std::runtime_error("HEIF mdat offset overflow");
    }
    item_offsets[i] = static_cast<uint32_t>(cursor);
    cursor += items[i].payload.size();
  }
  const auto meta = build_meta(item_offsets);

  std::vector<uint8_t> mdat_payload;
  mdat_payload.reserve(cursor - (ftyp.size() + meta.size() + 8u));
  for (const auto& item : items) append_bytes(mdat_payload, item.payload);
  const auto mdat = make_box("mdat", mdat_payload);

  HspHeifPackage pkg;
  pkg.heif.reserve(ftyp.size() + meta.size() + mdat.size());
  append_bytes(pkg.heif, ftyp);
  append_bytes(pkg.heif, meta);
  append_bytes(pkg.heif, mdat);
  pkg.exif_read_error = exif_read_error;
  pkg.has_vui_primaries = sps.has_vui_primaries;
  pkg.has_vui_transfer = sps.has_vui_transfer;
  pkg.has_vui_matrix = sps.has_vui_matrix;
  pkg.has_vui_range = sps.has_vui_range;
  pkg.color_primaries = sps.color_primaries;
  pkg.transfer_characteristics = sps.transfer_characteristics;
  pkg.matrix_coeffs = sps.matrix_coeffs;
  return pkg;
}

}  // namespace

std::vector<uint8_t> build_heif_from_hsp_bytes(const std::vector<uint8_t>& bytes) {
  return build_hsp_heif_package(bytes).heif;
}

SourceInfo inspect_hsp_bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  auto pkg = build_hsp_heif_package(bytes);
  auto info = inspect_heif_bytes(path, std::move(pkg.heif));
  apply_hsp_source_info(info, pkg);
  return info;
}

SourceInfo inspect_hsp_input(const std::filesystem::path& path) {
  return inspect_hsp_bytes(path, read_file(path));
}

DecodedImage decode_hsp_input(const std::filesystem::path& path,
                              const ProgressCallback& progress,
                              std::atomic_bool* cancel) {
  check_cancel(cancel);
  report(progress, 3, "Reading Panasonic HSP container");
  auto pkg = build_hsp_heif_package(read_file(path));
  check_cancel(cancel);
  auto decoded = decode_direct_hdr_bytes(path, std::move(pkg.heif), progress, cancel);
  apply_hsp_source_info(decoded.info, pkg);
  return decoded;
}

}  // namespace hdrbridge::detail
