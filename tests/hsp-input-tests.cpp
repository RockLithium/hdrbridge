#include "codecs/hsp.h"
#include "codecs/heif.h"
#include "image/orientation.h"
#include <libheif/heif.h>
#include <libheif/heif_properties.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class BitWriter {
 public:
  void write_bits(uint64_t value, size_t count) {
    for (size_t i = count; i > 0; --i) {
      const size_t bit_in_byte = 7u - (bit_pos_ & 7u);
      if ((bit_pos_ & 7u) == 0) data_.push_back(0);
      if ((value >> (i - 1u)) & 1u) {
        data_.back() |= static_cast<uint8_t>(1u << bit_in_byte);
      }
      ++bit_pos_;
    }
  }

  void write_ue(uint32_t value) {
    const uint32_t code = value + 1u;
    size_t bits = 0;
    for (uint32_t tmp = code; tmp > 0; tmp >>= 1) ++bits;
    const size_t zeros = bits - 1u;
    write_bits(0, zeros);
    write_bits(code, bits);
  }

  std::vector<uint8_t> finish_rbsp() {
    write_bits(1, 1);  // rbsp_stop_one_bit
    while ((bit_pos_ & 7u) != 0) write_bits(0, 1);
    return data_;
  }

 private:
  std::vector<uint8_t> data_;
  size_t bit_pos_ = 0;
};

std::vector<uint8_t> rbsp_to_ebsp(const std::vector<uint8_t>& rbsp) {
  std::vector<uint8_t> out;
  int zero_count = 0;
  for (uint8_t b : rbsp) {
    if (zero_count == 2 && b <= 3) {
      out.push_back(3);
      zero_count = 0;
    }
    out.push_back(b);
    zero_count = (b == 0) ? (zero_count + 1) : 0;
  }
  return out;
}

std::vector<uint8_t> make_sps_nal(uint8_t chroma_format_idc,
                                  uint32_t coded_w, uint32_t coded_h,
                                  uint32_t crop_left, uint32_t crop_right,
                                  uint32_t crop_top, uint32_t crop_bottom,
                                  uint16_t primaries, uint16_t transfer,
                                  uint16_t matrix, bool full_range) {
  BitWriter w;
  w.write_bits(0, 4);   // sps_video_parameter_set_id
  w.write_bits(0, 3);   // sps_max_sub_layers_minus1
  w.write_bits(1, 1);   // sps_temporal_id_nesting_flag
  // profile_tier_level (Main 10, level 6.0)
  w.write_bits(0, 2);
  w.write_bits(0, 1);
  w.write_bits(2, 5);
  w.write_bits(0x20000000u, 32);
  w.write_bits(0x900000000000ull, 48);
  w.write_bits(180, 8);
  w.write_ue(0);        // sps_seq_parameter_set_id
  w.write_ue(chroma_format_idc);
  if (chroma_format_idc == 3) {
    w.write_bits(0, 1);  // separate_colour_plane_flag
  }
  w.write_ue(coded_w);
  w.write_ue(coded_h);
  const bool has_crop = crop_left || crop_right || crop_top || crop_bottom;
  w.write_bits(has_crop ? 1u : 0u, 1);
  if (has_crop) {
    w.write_ue(crop_left);
    w.write_ue(crop_right);
    w.write_ue(crop_top);
    w.write_ue(crop_bottom);
  }
  w.write_ue(2);        // bit_depth_luma_minus8 = 2 (10-bit)
  w.write_ue(2);        // bit_depth_chroma_minus8 = 2 (10-bit)
  w.write_ue(4);        // log2_max_pic_order_cnt_lsb_minus4
  w.write_bits(1, 1);   // sps_sub_layer_ordering_info_present_flag
  w.write_ue(0); w.write_ue(0); w.write_ue(0);
  for (int i = 0; i < 6; ++i) w.write_ue(0);
  w.write_bits(0, 1);   // scaling_list_enabled_flag
  w.write_bits(0, 1);   // amp_enabled_flag
  w.write_bits(1, 1);   // sample_adaptive_offset_enabled_flag
  w.write_bits(0, 1);   // pcm_enabled_flag
  w.write_ue(0);        // num_short_term_ref_pic_sets
  w.write_bits(0, 1);   // long_term_ref_pics_present_flag
  w.write_bits(0, 1);   // sps_temporal_mvp_enabled_flag
  w.write_bits(0, 1);   // strong_intra_smoothing_enabled_flag
  const bool emit_vui = primaries != 0 || transfer != 0 || matrix != 0;
  w.write_bits(emit_vui ? 1u : 0u, 1);   // vui_parameters_present_flag
  if (emit_vui) {
    w.write_bits(0, 1);   // aspect_ratio_info_present_flag
    w.write_bits(0, 1);   // overscan_info_present_flag
    w.write_bits(1, 1);   // video_signal_type_present_flag
    w.write_bits(5, 3);   // video_format
    w.write_bits(full_range ? 1u : 0u, 1);
    w.write_bits(1, 1);   // colour_description_present_flag
    w.write_bits(primaries, 8);
    w.write_bits(transfer, 8);
    w.write_bits(matrix, 8);
  }

  const auto ebsp = rbsp_to_ebsp(w.finish_rbsp());
  std::vector<uint8_t> nal{0x42, 0x01};
  nal.insert(nal.end(), ebsp.begin(), ebsp.end());
  return nal;
}

void push_u16_le(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xffu));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
}

void push_u32_le(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xffu));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
}

struct SyntheticHspOptions {
  uint8_t chroma_format_idc = 1;
  uint32_t coded_w = 5984;
  uint32_t coded_h = 4000;
  uint32_t crop_right = 0;
  uint32_t crop_bottom = 0;
  uint16_t primaries = 9;
  uint16_t transfer = 18;
  uint16_t matrix = 9;
  bool full_range = false;
  uint16_t orientation = 1;
  bool include_gps = true;
  bool include_makernote = true;
  std::string xmp;
};

std::vector<uint8_t> make_synthetic_hsp(const SyntheticHspOptions& opts) {
  std::vector<uint8_t> out{'I', 'I', 42, 0};
  push_u32_le(out, 8);  // IFD0 offset

  struct TagSpec {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    std::vector<uint8_t> data;
    uint32_t inline_val = 0;
  };

  auto str_bytes = [](const std::string& s) {
    std::vector<uint8_t> b(s.begin(), s.end());
    b.push_back(0);
    return b;
  };

  std::vector<TagSpec> ifd0;
  ifd0.push_back({0x0103, 3, 1, {}, 51176});
  ifd0.push_back({0x010f, 2, 10, str_bytes("Panasonic")});
  const size_t strip_entry_placeholder_idx = ifd0.size();
  ifd0.push_back({0x0111, 4, 1, {}, 0});
  ifd0.push_back({0x0112, 3, 1, {}, opts.orientation});
  if (!opts.xmp.empty()) {
    std::vector<uint8_t> xmp_bytes(opts.xmp.begin(), opts.xmp.end());
    ifd0.push_back({0x02bc, 7, static_cast<uint32_t>(xmp_bytes.size()), std::move(xmp_bytes)});
  }
  const size_t exif_ptr_idx = ifd0.size();
  ifd0.push_back({0x8769, 4, 1, {}, 0});
  size_t gps_ptr_idx = static_cast<size_t>(-1);
  if (opts.include_gps) {
    gps_ptr_idx = ifd0.size();
    ifd0.push_back({0x8825, 4, 1, {}, 0});
  }
  const size_t len_entry_idx = ifd0.size();
  ifd0.push_back({0x9005, 4, 1, {}, 0});

  const size_t ifd0_pos = out.size();
  out.resize(ifd0_pos + 2u + ifd0.size() * 12u + 4u, 0);
  auto write_ifd = [&](size_t dir_pos, std::vector<TagSpec>& entries) {
    out[dir_pos] = static_cast<uint8_t>(entries.size() & 0xffu);
    out[dir_pos + 1] = static_cast<uint8_t>((entries.size() >> 8) & 0xffu);
    for (size_t i = 0; i < entries.size(); ++i) {
      const size_t epos = dir_pos + 2u + i * 12u;
      const auto& e = entries[i];
      out[epos] = static_cast<uint8_t>(e.tag & 0xffu);
      out[epos + 1] = static_cast<uint8_t>((e.tag >> 8) & 0xffu);
      out[epos + 2] = static_cast<uint8_t>(e.type & 0xffu);
      out[epos + 3] = static_cast<uint8_t>((e.type >> 8) & 0xffu);
      out[epos + 4] = static_cast<uint8_t>(e.count & 0xffu);
      out[epos + 5] = static_cast<uint8_t>((e.count >> 8) & 0xffu);
      out[epos + 6] = static_cast<uint8_t>((e.count >> 16) & 0xffu);
      out[epos + 7] = static_cast<uint8_t>((e.count >> 24) & 0xffu);
      uint32_t val = e.inline_val;
      if (!e.data.empty()) {
        if (e.data.size() <= 4) {
          std::memcpy(out.data() + epos + 8u, e.data.data(), e.data.size());
          continue;
        }
        if (out.size() & 1u) out.push_back(0);
        val = static_cast<uint32_t>(out.size());
        out.insert(out.end(), e.data.begin(), e.data.end());
      }
      out[epos + 8] = static_cast<uint8_t>(val & 0xffu);
      out[epos + 9] = static_cast<uint8_t>((val >> 8) & 0xffu);
      out[epos + 10] = static_cast<uint8_t>((val >> 16) & 0xffu);
      out[epos + 11] = static_cast<uint8_t>((val >> 24) & 0xffu);
    }
  };

  // Emit ExifIFD
  std::vector<TagSpec> exif_ifd;
  exif_ifd.push_back({0x829a, 5, 1, {10, 0, 0, 0, 0, 1, 0, 0}});
  if (opts.include_makernote) {
    exif_ifd.push_back({0x927c, 7, 12, {'P','a','n','a','s','o','n','i','c',0,0,0}});
  }
  const size_t exif_pos = out.size();
  out.resize(exif_pos + 2u + exif_ifd.size() * 12u + 4u, 0);
  ifd0[exif_ptr_idx].inline_val = static_cast<uint32_t>(exif_pos);
  write_ifd(exif_pos, exif_ifd);

  // Emit GPSIFD
  if (opts.include_gps) {
    std::vector<TagSpec> gps_ifd;
    gps_ifd.push_back({0x0001, 2, 2, {'N', 0}});
    const size_t gps_pos = out.size();
    out.resize(gps_pos + 2u + gps_ifd.size() * 12u + 4u, 0);
    ifd0[gps_ptr_idx].inline_val = static_cast<uint32_t>(gps_pos);
    write_ifd(gps_pos, gps_ifd);
  }

  // Emit Annex B stream (3-byte start code test + 4-byte start codes)
  const uint32_t stream_off = static_cast<uint32_t>(out.size());
  auto append_nal = [&](const std::vector<uint8_t>& nal, bool three_byte) {
    if (three_byte) out.insert(out.end(), {0, 0, 1});
    else out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), nal.begin(), nal.end());
  };
  append_nal({0x40, 0x01, 0x0c, 0x01, 0xff, 0xff}, true);  // VPS
  append_nal(make_sps_nal(opts.chroma_format_idc, opts.coded_w, opts.coded_h, 0,
                          opts.crop_right, 0, opts.crop_bottom, opts.primaries,
                          opts.transfer, opts.matrix, opts.full_range), false);  // SPS
  append_nal({0x44, 0x01, 0xc0, 0x72}, false);              // PPS
  append_nal({0x28, 0x01, 0xaa, 0xbb, 0xcc}, false);        // IDR_N_LP slice

  ifd0[strip_entry_placeholder_idx].inline_val = stream_off;
  ifd0[len_entry_idx].inline_val = static_cast<uint32_t>(out.size() - stream_off);
  write_ifd(ifd0_pos, ifd0);
  return out;
}

bool contains_bytes(const std::vector<uint8_t>& hay, const std::string& needle) {
  return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

bool contains_u16_le(const std::vector<uint8_t>& hay, uint16_t val) {
  const uint8_t pat[2]{static_cast<uint8_t>(val & 0xffu), static_cast<uint8_t>(val >> 8)};
  return std::search(hay.begin(), hay.end(), pat, pat + 2) != hay.end();
}

}  // namespace

int main() {
  try {
    // 1. SPS VUI, cropping, orientation irot/imir, GPS, XMP, and MakerNote drop
    {
      SyntheticHspOptions opts;
      opts.coded_w = 6016;
      opts.coded_h = 4000;
      opts.crop_right = 8;  // 6016 - 8*2 = 6000
      opts.crop_bottom = 0;
      opts.primaries = 12;  // Display P3
      opts.transfer = 16;   // PQ
      opts.matrix = 9;
      opts.full_range = true;
      opts.orientation = 5;  // 90 CCW + vertical mirror
      opts.include_gps = true;
      opts.include_makernote = true;
      opts.xmp = "<?xpacket begin=''?><x:xmpmeta><xmp:Rating>5</xmp:Rating></x:xmpmeta>";

      const auto hsp = make_synthetic_hsp(opts);
      const auto heif = hdrbridge::detail::build_heif_from_hsp_bytes(hsp);
      const auto info = hdrbridge::detail::inspect_hsp_bytes("synthetic.hsp", hsp);

      require(info.format == "HSP" && info.container_brand == "HSP",
              "HSP inspect must report HSP container instead of internal HEIF brand");
      require(info.native_color_present && info.native_color_description == "HEVC SPS VUI" &&
                  info.color_signal_kind == "HEVC SPS VUI" &&
                  info.resolved_signaling_source == "HEVC SPS VUI",
              "explicit SPS VUI color tags must be reported as HEVC SPS VUI");

      // Post-transform width/height because orientation 5 swaps axes!
      require(info.width == 4000 && info.height == 6000, "cropped & rotated dimensions mismatch");
      require(info.original_orientation == 5, "orientation 5 not preserved via irot/imir");
      require(info.native_primaries == 12 && info.native_transfer == 16 &&
                  info.native_matrix == 9 && info.native_full_range,
              "SPS VUI color tags mismatch");
      require(info.exif_present && info.xmp_present, "EXIF or valid XMP missing");

      // Check libheif metadata directly for GPS preserved, Orientation=1, MakerNote dropped
      heif_context* ctx = heif_context_alloc();
      require(ctx != nullptr, "heif_context_alloc failed");
      const auto err = heif_context_read_from_memory_without_copy(ctx, heif.data(), heif.size(), nullptr);
      require(err.code == heif_error_Ok, "libheif failed to read built HEIF");
      heif_image_handle* handle = nullptr;
      require(heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok, "get handle failed");

      const int meta_count = heif_image_handle_get_number_of_metadata_blocks(handle, "Exif");
      require(meta_count == 1, "expected 1 Exif block");
      heif_item_id exif_id = 0;
      heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id, 1);
      std::vector<uint8_t> exif(heif_image_handle_get_metadata_size(handle, exif_id));
      require(heif_image_handle_get_metadata(handle, exif_id, exif.data()).code == heif_error_Ok, "get exif failed");
      require(hdrbridge::orientation::read_exif_orientation(exif) == 1, "repacked EXIF orientation must be 1");
      require(contains_bytes(exif, "Panasonic"), "Make missing from repacked EXIF");
      require(!contains_bytes(exif, std::string("Panasonic\0\0\0", 12)) &&
                  !contains_u16_le(exif, 0x927c),
              "MakerNote leaked into repacked EXIF");
      require(contains_u16_le(exif, 0x8825), "GPSIFD tag 0x8825 was not preserved");

      heif_image_handle_release(handle);
      heif_context_free(ctx);
    }

    // 2. All orientations 1..8 roundtrip through irot/imir
    for (uint16_t orient = 1; orient <= 8; ++orient) {
      SyntheticHspOptions opts;
      opts.orientation = orient;
      opts.include_gps = false;
      opts.include_makernote = false;
      const auto heif = hdrbridge::detail::build_heif_from_hsp_bytes(make_synthetic_hsp(opts));
      const auto info = hdrbridge::detail::inspect_heif_bytes("orient.hsp", heif);
      require(info.original_orientation == orient,
              "orientation roundtrip mismatch for " + std::to_string(orient));
    }

    // 2b. 4:2:2 and 4:4:4 chroma formats are accepted (with cropping)
    for (const auto& [chroma_idc, expected_chroma] :
         std::array<std::pair<uint8_t, std::string>, 2>{{{2, "4:2:2"}, {3, "4:4:4"}}}) {
      SyntheticHspOptions opts;
      opts.chroma_format_idc = chroma_idc;
      opts.coded_w = 1924;
      opts.coded_h = 1080;
      opts.crop_right = 2;  // 1924 - 2*subWidthC (4 for 4:2:2, 2 for 4:4:4)
      opts.include_gps = false;
      opts.include_makernote = false;
      const auto heif = hdrbridge::detail::build_heif_from_hsp_bytes(make_synthetic_hsp(opts));
      const auto info = hdrbridge::detail::inspect_heif_bytes("chroma.hsp", heif);
      require(info.chroma == expected_chroma,
              "expected chroma " + expected_chroma + ", got " + info.chroma);
      const uint32_t expected_w = chroma_idc == 2 ? 1920u : 1922u;
      require(info.width == expected_w && info.height == 1080u,
              "cropped dimensions mismatch for " + expected_chroma);
    }

    // 2c. Missing VUI falls back cleanly to HSP defaults in Inspector
    {
      SyntheticHspOptions opts;
      opts.primaries = 0;
      opts.transfer = 0;
      opts.matrix = 0;
      opts.full_range = false;
      opts.include_gps = false;
      opts.include_makernote = false;
      const auto hsp = make_synthetic_hsp(opts);
      const auto info = hdrbridge::detail::inspect_hsp_bytes("novui.hsp", hsp);
      require(info.format == "HSP" && info.container_brand == "HSP",
              "HSP inspect must keep containerBrand=HSP when VUI is absent");
      require(!info.native_color_present && info.primaries == 9 && info.transfer == 18 &&
                  info.matrix == 9 && !info.full_range &&
                  info.color_signal_kind == "HSP default (BT.2020 HLG)" &&
                  info.resolved_signaling_source == "HSP default",
              "missing SPS VUI must report HSP default signaling rather than CICP / NCLX");
    }

    // 2d. Corrupt EXIF directory pointer must mark Read error without failing inspection/build
    {
      auto bad_exif = make_synthetic_hsp({});
      bool corrupted = false;
      const uint16_t count = static_cast<uint16_t>(bad_exif[8] | (bad_exif[9] << 8));
      for (uint16_t i = 0; i < count; ++i) {
        const size_t pos = 10u + static_cast<size_t>(i) * 12u;
        const uint16_t tag = static_cast<uint16_t>(bad_exif[pos] | (bad_exif[pos + 1] << 8));
        if (tag == 0x8769) {
          bad_exif[pos + 8] = 0xff;
          bad_exif[pos + 9] = 0xff;
          bad_exif[pos + 10] = 0xff;
          bad_exif[pos + 11] = 0x7f;
          corrupted = true;
          break;
        }
      }
      require(corrupted, "failed to locate ExifIFD tag to corrupt");
      const auto info = hdrbridge::detail::inspect_hsp_bytes("badexif.hsp", bad_exif);
      require(info.format == "HSP" && info.container_brand == "HSP" &&
                  info.exif_status == "read-error" && !info.exif_present,
              "corrupt EXIF pointer should yield exif_status=read-error without throwing");
      require(info.width == 5984 && info.height == 4000,
              "corrupt EXIF pointer must preserve image dimensions and pixel container build");
    }

    // 3. Invalid XMP is ignored safely
    {
      SyntheticHspOptions opts;
      opts.xmp = "NOT_VALID_XML_PAYLOAD";
      const auto heif = hdrbridge::detail::build_heif_from_hsp_bytes(make_synthetic_hsp(opts));
      const auto info = hdrbridge::detail::inspect_heif_bytes("badxmp.hsp", heif);
      require(!info.xmp_present, "invalid XMP should be ignored");
    }

    // 4. Security / bounds negative cases must throw
    auto expect_throw = [](const std::vector<uint8_t>& bad, const std::string& label) {
      bool threw = false;
      try {
        (void)hdrbridge::detail::build_heif_from_hsp_bytes(bad);
      } catch (const std::exception&) {
        threw = true;
      }
      require(threw, "expected exception for " + label);
    };

    expect_throw({}, "empty buffer");
    expect_throw(std::vector<uint8_t>(32, 0), "bad TIFF header");

    {
      auto bad_comp = make_synthetic_hsp({});
      // Replace compression 51176 with 6
      for (size_t i = 10; i + 12 <= 160; i += 12) {
        if (bad_comp[i] == 0x03 && bad_comp[i + 1] == 0x01) {
          bad_comp[i + 8] = 6;
          bad_comp[i + 9] = 0;
          break;
        }
      }
      expect_throw(bad_comp, "non-HSP compression");
    }

    {
      auto bad_len = make_synthetic_hsp({});
      // Corrupt IFD0 entry count to 5000 (> 4096)
      bad_len[8] = 0x88;
      bad_len[9] = 0x13;
      expect_throw(bad_len, "excessive IFD0 count");
    }

    {
      auto trunc = make_synthetic_hsp({});
      // Truncate past the 10-byte trailing Make string so the HEVC stream itself is short of 0x9005.
      trunc.resize(trunc.size() - 16);
      expect_throw(trunc, "truncated HEVC stream via 0x9005");
    }

    {
      SyntheticHspOptions mono_opts;
      mono_opts.chroma_format_idc = 0;
      bool threw_mono = false;
      try {
        (void)hdrbridge::detail::build_heif_from_hsp_bytes(make_synthetic_hsp(mono_opts));
      } catch (const std::exception&) {
        threw_mono = true;
      }
      require(threw_mono, "expected exception for monochrome 4:0:0 HSP stream");
    }

    return 0;
  } catch (const std::exception& error) {
    std::cerr << "HSP input test failed: " << error.what() << '\n';
    return 1;
  }
}
