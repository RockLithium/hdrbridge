#include "hdrbridge_core.h"
#include "codecs/gainmap/heif.h"
#include "codecs/gainmap/jpeg.h"
#include "codecs/gainmap/icc_description.h"
#include "codecs/gainmap/iso_metadata.h"
#include "color/hdr_transfer.h"
#include <avif/avif.h>
#include <jxl/color_encoding.h>
#include <jxl/encode_cxx.h>
#include <lcms2.h>
#include <tiffio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using hdrbridge::gainmap::detail::adaptive_hdr_icc_reference_white_nits;
using hdrbridge::gainmap::detail::extract_jpeg_icc_profiles;
using hdrbridge::gainmap::detail::is_adaptive_hdr_icc;
using hdrbridge::gainmap::detail::jpeg_adaptive_hdr_icc_reference_white_nits;
using hdrbridge::gainmap::detail::parse_iso_gain_map_metadata;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct UniqueTempDir {
  std::filesystem::path path;
  explicit UniqueTempDir(const char* prefix) {
    static std::atomic<uint64_t> seq{0};
    const uint64_t tick = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t n = seq.fetch_add(1u, std::memory_order_relaxed);
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    path = std::filesystem::temp_directory_path() /
        (std::string(prefix) + "-" + std::to_string(pid) + "-" +
         std::to_string(tick) + "-" + std::to_string(n));
    std::filesystem::create_directories(path);
  }
  ~UniqueTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  UniqueTempDir(const UniqueTempDir&) = delete;
  UniqueTempDir& operator=(const UniqueTempDir&) = delete;
};

void write_be16(std::vector<uint8_t>& out, size_t pos, uint16_t val) {
  out[pos] = static_cast<uint8_t>(val >> 8);
  out[pos + 1] = static_cast<uint8_t>(val & 0xffu);
}

void write_be32(std::vector<uint8_t>& out, size_t pos, uint32_t val) {
  out[pos] = static_cast<uint8_t>((val >> 24) & 0xffu);
  out[pos + 1] = static_cast<uint8_t>((val >> 16) & 0xffu);
  out[pos + 2] = static_cast<uint8_t>((val >> 8) & 0xffu);
  out[pos + 3] = static_cast<uint8_t>(val & 0xffu);
}

struct TagSpec {
  const char* sig;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> build_synthetic_icc(const std::vector<TagSpec>& tags,
                                         const char* profile_class = "mntr",
                                         const char* color_space = "RGB ") {
  const size_t header_and_table = 132u + tags.size() * 12u;
  size_t total = header_and_table;
  for (const auto& tag : tags) {
    total += (tag.data.size() + 3u) & ~size_t(3u);
  }
  std::vector<uint8_t> icc(total, 0);
  write_be32(icc, 0, static_cast<uint32_t>(total));
  std::memcpy(icc.data() + 12, profile_class, 4);
  std::memcpy(icc.data() + 16, color_space, 4);
  std::memcpy(icc.data() + 36, "acsp", 4);
  write_be32(icc, 128, static_cast<uint32_t>(tags.size()));
  size_t data_pos = header_and_table;
  for (size_t i = 0; i < tags.size(); ++i) {
    const size_t entry = 132u + i * 12u;
    std::memcpy(icc.data() + entry, tags[i].sig, 4);
    write_be32(icc, entry + 4, static_cast<uint32_t>(data_pos));
    write_be32(icc, entry + 8, static_cast<uint32_t>(tags[i].data.size()));
    std::memcpy(icc.data() + data_pos, tags[i].data.data(), tags[i].data.size());
    data_pos += (tags[i].data.size() + 3u) & ~size_t(3u);
  }
  return icc;
}

std::vector<uint8_t> make_hagc_tag(uint8_t app_version,
                                   uint8_t min_app_version,
                                   uint8_t flags,
                                   uint16_t raw_custom_ref_white) {
  const bool has_custom = (flags & 0x80u) != 0;
  const uint32_t payload_size = has_custom ? 4u : 2u;
  std::vector<uint8_t> tag(12u + payload_size, 0);
  std::memcpy(tag.data(), "hagc", 4);
  write_be32(tag, 8, payload_size);
  tag[12] = static_cast<uint8_t>(((app_version & 0x07u) << 5u) |
                                 ((min_app_version & 0x07u) << 2u));
  tag[13] = flags;
  if (has_custom) {
    write_be16(tag, 14, raw_custom_ref_white);
  }
  return tag;
}

std::vector<uint8_t> make_lumi_tag(double nits) {
  std::vector<uint8_t> tag(20, 0);
  std::memcpy(tag.data(), "XYZ ", 4);
  const uint32_t fixed = static_cast<uint32_t>(std::llround(nits * 65536.0));
  write_be32(tag, 12, fixed);
  return tag;
}

void test_adaptive_icc_reference_white() {
  const std::vector<uint8_t> dummy_lut(16, 0);
  const auto lut_profile = [&](std::vector<TagSpec> tags) {
    tags.insert(tags.begin(), {{"A2B0", dummy_lut}, {"B2A0", dummy_lut}});
    return build_synthetic_icc(tags);
  };
  const auto legacy_tags = [&] {
    return std::vector<TagSpec>{{"hdgm", dummy_lut}, {"lumi", make_lumi_tag(100.0)}};
  };
  const auto legacy = lut_profile(legacy_tags());
  auto newer_tags = legacy_tags();
  newer_tags.push_back({"HAGC", make_hagc_tag(2, 1, 0xC0, 1500)});
  const auto newer = lut_profile(newer_tags);
  auto preferred_tags = legacy_tags();
  preferred_tags.push_back({"HAGC", make_hagc_tag(0, 0, 0xC0, 1500)});

  const struct Case {
    const char* name;
    std::vector<uint8_t> icc;
    bool allow_lumi;
    std::optional<double> expected_nits;
    std::optional<bool> adaptive = std::nullopt;
  } cases[] = {
      {"standard HAGC", lut_profile({{"hdgm", dummy_lut}, {"HAGC", make_hagc_tag(0, 0, 0x40, 0)}}),
       true, 203.0, true},
      {"custom HAGC", lut_profile({{"HAGC", make_hagc_tag(0, 0, 0xC0, 750)}}),
       false, 150.0, true},
      {"minimum reference white", lut_profile({{"HAGC", make_hagc_tag(0, 0, 0xC0, 0)}}),
       false, 0.2},
      {"maximum reference white", lut_profile({{"HAGC", make_hagc_tag(0, 0, 0xC0, 60000)}}),
       false, 10000.0},
      {"newer application version", lut_profile({{"HAGC", make_hagc_tag(1, 0, 0xC0, 1000)}}),
       false, 200.0},
      {"unsupported minimum version", newer, false, std::nullopt},
      {"unsupported version with luminance fallback", newer, true, 100.0},
      {"HAGC overrides luminance", lut_profile(preferred_tags), true, 300.0},
      {"legacy luminance enabled", legacy, true, 100.0},
      {"legacy luminance disabled", legacy, false, std::nullopt},
      {"plain ICC", build_synthetic_icc({{"rXYZ", dummy_lut}}), false, std::nullopt, false},
      {"RGB HAGC without LUT", build_synthetic_icc(
           {{"rXYZ", dummy_lut}, {"HAGC", make_hagc_tag(0, 0, 0xC0, 750)}}), false, 150.0, false},
      {"grayscale HAGC", build_synthetic_icc(
           {{"HAGC", make_hagc_tag(0, 0, 0xC0, 750)}}, "mntr", "GRAY"), true, std::nullopt},
  };
  for (const auto& test : cases) {
    const auto nits = adaptive_hdr_icc_reference_white_nits(
        test.icc.data(), test.icc.size(), test.allow_lumi);
    require(nits.has_value() == test.expected_nits.has_value() &&
                (!nits || std::abs(*nits - *test.expected_nits) < 1e-9),
            std::string(test.name) + ": reference white mismatch");
    if (test.adaptive.has_value()) {
      require(is_adaptive_hdr_icc(test.icc.data(), test.icc.size()) == *test.adaptive,
              std::string(test.name) + ": adaptive ICC classification mismatch");
    }
  }
}

void append_jpeg_segment(std::vector<uint8_t>& jpg,
                         uint8_t marker,
                         const std::vector<uint8_t>& payload) {
  jpg.push_back(0xffu);
  jpg.push_back(marker);
  const size_t seg_len = payload.size() + 2u;
  jpg.push_back(static_cast<uint8_t>(seg_len >> 8u));
  jpg.push_back(static_cast<uint8_t>(seg_len & 0xffu));
  jpg.insert(jpg.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> make_app2_icc_chunk(uint8_t seq, uint8_t total,
                                         const uint8_t* data, size_t size) {
  std::vector<uint8_t> payload(14u + size);
  std::memcpy(payload.data(), "ICC_PROFILE\0", 12u);
  payload[12] = seq;
  payload[13] = total;
  std::memcpy(payload.data() + 14u, data, size);
  return payload;
}

void test_jpeg_app2_reassembly_and_isolation() {
  const std::vector<uint8_t> dummy_lut(24, 0);
  const auto plain_icc = build_synthetic_icc({{"rXYZ", dummy_lut}});
  const auto hagc_150 = build_synthetic_icc({
      {"A2B0", dummy_lut},
      {"B2A0", dummy_lut},
      {"HAGC", make_hagc_tag(0, 0, 0xC0, 750)},  // 750 / 5 = 150.0 nits
  });
  const auto rogue_300 = build_synthetic_icc({
      {"A2B0", dummy_lut},
      {"B2A0", dummy_lut},
      {"HAGC", make_hagc_tag(0, 0, 0xC0, 1500)},  // 300.0 nits
  });

  // Base JPEG: plain ICC (1/1) + conflicting rogue ICC stuffed inside non-ICC APP15
  std::vector<uint8_t> base_jpg{0xffu, 0xd8u};
  append_jpeg_segment(base_jpg, 0xe2u, make_app2_icc_chunk(1, 1, plain_icc.data(), plain_icc.size()));
  append_jpeg_segment(base_jpg, 0xefu, rogue_300);  // must NOT be treated as APP2 ICC
  base_jpg.push_back(0xffu); base_jpg.push_back(0xdau);
  base_jpg.insert(base_jpg.end(), rogue_300.begin(), rogue_300.end());  // rogue scan bytes
  base_jpg.push_back(0xffu); base_jpg.push_back(0xd9u);

  // Gain-map JPEG: 150-nit HAGC ICC split out-of-order across two APP2 segments (2/2 then 1/2),
  // preceded by a plain 1/1 ICC so multi-profile JPEG is also verified.
  const size_t split = hagc_150.size() / 2u;
  std::vector<uint8_t> gain_jpg{0xffu, 0xd8u};
  append_jpeg_segment(gain_jpg, 0xe2u, make_app2_icc_chunk(1, 1, plain_icc.data(), plain_icc.size()));
  append_jpeg_segment(gain_jpg, 0xe2u, make_app2_icc_chunk(2, 2, hagc_150.data() + split, hagc_150.size() - split));
  append_jpeg_segment(gain_jpg, 0xe2u, make_app2_icc_chunk(1, 2, hagc_150.data(), split));
  gain_jpg.push_back(0xffu); gain_jpg.push_back(0xdau);
  gain_jpg.push_back(0xffu); gain_jpg.push_back(0xd9u);

  const auto gain_profiles = extract_jpeg_icc_profiles(gain_jpg.data(), gain_jpg.size());
  require(gain_profiles.size() == 2u && gain_profiles[1] == hagc_150,
          "multi-chunk out-of-order APP2 ICC reassembly failed");

  const auto rw = jpeg_adaptive_hdr_icc_reference_white_nits(
      base_jpg.data(), base_jpg.size(), gain_jpg.data(), gain_jpg.size(), false);
  require(rw.has_value() && std::abs(*rw - 150.0) < 1e-9,
          "chunked HAGC ICC in gain JPEG should resolve to 150.0 nits and ignore non-APP2 rogue bytes");

  // Real conflict between Base and Gain APP2 adaptive ICCs must throw
  std::vector<uint8_t> conflict_base{0xffu, 0xd8u};
  append_jpeg_segment(conflict_base, 0xe2u, make_app2_icc_chunk(1, 1, rogue_300.data(), rogue_300.size()));
  conflict_base.push_back(0xffu); conflict_base.push_back(0xdau);
  conflict_base.push_back(0xffu); conflict_base.push_back(0xd9u);
  bool threw = false;
  try {
    (void)jpeg_adaptive_hdr_icc_reference_white_nits(
        conflict_base.data(), conflict_base.size(), gain_jpg.data(), gain_jpg.size(), false);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "conflicting base vs gain APP2 adaptive ICCs should throw");
}

void append_s32(std::vector<uint8_t>& out, int32_t v) {
  const size_t pos = out.size();
  out.resize(pos + 4);
  write_be32(out, pos, static_cast<uint32_t>(v));
}

void append_u32(std::vector<uint8_t>& out, uint32_t v) {
  const size_t pos = out.size();
  out.resize(pos + 4);
  write_be32(out, pos, v);
}

void test_iso_metadata_parser() {
  std::vector<uint8_t> m1;
  m1.push_back(0); m1.push_back(0);
  m1.push_back(0); m1.push_back(0);
  m1.push_back(0x00);
  append_u32(m1, 0); append_u32(m1, 1);
  append_u32(m1, 3); append_u32(m1, 1);
  append_s32(m1, 0); append_u32(m1, 1);
  append_s32(m1, 25); append_u32(m1, 10);
  append_u32(m1, 1); append_u32(m1, 1);
  append_s32(m1, 0); append_u32(m1, 1);
  append_s32(m1, 0); append_u32(m1, 1);
  const auto p1 = parse_iso_gain_map_metadata(m1.data(), m1.size());
  require(!p1.multichannel && !p1.use_base_color_space, "1ch flags mismatch");
  require(std::abs(p1.alternate_hdr_headroom - 3.0) < 1e-9, "alt headroom mismatch");
  require(std::abs(p1.maximum[0] - 2.5) < 1e-9 && std::abs(p1.maximum[2] - 2.5) < 1e-9,
          "1ch gainMapMax replication mismatch");

  std::vector<uint8_t> m3;
  m3.push_back(0); m3.push_back(0);
  m3.push_back(0); m3.push_back(0);
  m3.push_back(0xC0);
  append_u32(m3, 0); append_u32(m3, 1);
  append_u32(m3, 2); append_u32(m3, 1);
  for (int ch = 0; ch < 3; ++ch) {
    append_s32(m3, ch); append_u32(m3, 10);
    append_s32(m3, 20 + ch); append_u32(m3, 10);
    append_u32(m3, 10 + ch); append_u32(m3, 10);
    append_s32(m3, 1); append_u32(m3, 100);
    append_s32(m3, 2); append_u32(m3, 100);
  }
  const auto p3 = parse_iso_gain_map_metadata(m3.data(), m3.size());
  require(p3.multichannel && p3.use_base_color_space, "3ch flags mismatch");
  require(std::abs(p3.maximum[0] - 2.0) < 1e-9 &&
          std::abs(p3.maximum[1] - 2.1) < 1e-9 &&
          std::abs(p3.maximum[2] - 2.2) < 1e-9,
          "3ch gainMapMax mismatch");
  require(std::abs(p3.gamma[2] - 1.2) < 1e-9, "3ch gamma mismatch");
}

struct AvifImgDeleter { void operator()(avifImage* img) const { if (img) avifImageDestroy(img); } };
struct AvifGainDeleter { void operator()(avifGainMap* gm) const { if (gm) avifGainMapDestroy(gm); } };
struct AvifEncDeleter { void operator()(avifEncoder* enc) const { if (enc) avifEncoderDestroy(enc); } };

using AvifImgPtr = std::unique_ptr<avifImage, AvifImgDeleter>;
using AvifGainPtr = std::unique_ptr<avifGainMap, AvifGainDeleter>;
using AvifEncPtr = std::unique_ptr<avifEncoder, AvifEncDeleter>;

void require_avif(avifResult res, const char* op) {
  if (res != AVIF_RESULT_OK) {
    throw std::runtime_error(std::string(op) + ": " + avifResultToString(res));
  }
}

struct TmapFixture {
  uint32_t map_width = 16;
  uint32_t map_height = 12;
  int map_depth = 10;
  bool use_base_color_space = false;
  avifColorPrimaries alternate_primaries = AVIF_COLOR_PRIMARIES_BT2020;
  avifColorPrimaries base_primaries = AVIF_COLOR_PRIMARIES_BT709;
  avifPixelFormat map_format = AVIF_PIXEL_FORMAT_YUV444;
  avifTransferCharacteristics base_transfer = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
  avifRange base_range = AVIF_RANGE_FULL;
  bool strip_colr_box = false;
  std::vector<uint8_t> base_icc{};
  std::vector<uint8_t> unassociated_icc{};
};

void write_tmap_fixture(const std::filesystem::path& path, const TmapFixture& fixture = {}) {
  constexpr uint32_t width = 32;
  constexpr uint32_t height = 24;
  AvifImgPtr base(avifImageCreate(width, height, 10, AVIF_PIXEL_FORMAT_YUV444));
  require(base != nullptr, "allocate synthetic AVIF base");
  base->colorPrimaries = fixture.base_primaries;
  base->transferCharacteristics = fixture.base_transfer;
  base->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
  base->yuvRange = fixture.base_range;

  std::vector<uint16_t> base_pixels(static_cast<size_t>(width) * height * 3u);
  std::vector<uint16_t> alternate_pixels(base_pixels.size());
  const auto pq16 = [](double nits) {
    return static_cast<uint16_t>(std::llround(hdrbridge::transfer::nits_to_pq(nits) * 65535.0));
  };
  const std::array<uint16_t, 3> base_highlight{65535u, 16000u, 8000u};
  const std::array<uint16_t, 3> alternate_highlight{pq16(800.0), pq16(50.0), pq16(50.0)};
  const uint16_t alternate_white = pq16(203.0);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t pixel = (static_cast<size_t>(y) * width + x) * 3u;
      for (size_t channel = 0; channel < 3; ++channel) {
        base_pixels[pixel + channel] = x >= width / 2u ? base_highlight[channel] : 48000u;
        alternate_pixels[pixel + channel] =
            x >= width / 2u ? alternate_highlight[channel] : alternate_white;
      }
    }
  }

  const auto rgb_pixels = [&](avifImage* image, std::vector<uint16_t>& pixels) {
    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, image);
    rgb.depth = 16;
    rgb.format = AVIF_RGB_FORMAT_RGB;
    rgb.pixels = reinterpret_cast<uint8_t*>(pixels.data());
    rgb.rowBytes = width * 3u * sizeof(uint16_t);
    rgb.avoidLibYUV = AVIF_TRUE;
    return rgb;
  };
  auto base_rgb = rgb_pixels(base.get(), base_pixels);
  auto alternate_rgb = rgb_pixels(base.get(), alternate_pixels);
  require_avif(avifImageRGBToYUV(base.get(), &base_rgb), "convert synthetic base to YUV");

  AvifGainPtr gain(avifGainMapCreate());
  require(gain != nullptr, "allocate gain map");
  gain->image = avifImageCreate(fixture.map_width, fixture.map_height,
                                fixture.map_depth, fixture.map_format);
  require(gain->image != nullptr, "allocate gain-map image");
  gain->image->matrixCoefficients = static_cast<avifMatrixCoefficients>(
      fixture.map_format == AVIF_PIXEL_FORMAT_YUV400
          ? AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL
          : AVIF_MATRIX_COEFFICIENTS_BT601);
  gain->image->yuvRange = AVIF_RANGE_FULL;
  gain->useBaseColorSpace = fixture.use_base_color_space ? AVIF_TRUE : AVIF_FALSE;
  gain->altColorPrimaries = fixture.alternate_primaries;
  gain->altTransferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;
  gain->altMatrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
  gain->altYUVRange = AVIF_RANGE_FULL;
  avifDiagnostics diag{};
  require_avif(avifRGBImageComputeGainMap(
      &base_rgb, base->colorPrimaries, base->transferCharacteristics,
      &alternate_rgb, gain->altColorPrimaries, gain->altTransferCharacteristics,
      gain.get(), &diag), "compute synthetic gain map");
  gain->useBaseColorSpace = fixture.use_base_color_space ? AVIF_TRUE : AVIF_FALSE;
  base->gainMap = gain.release();

  if (!fixture.base_icc.empty()) {
    // Make the encoded base depend on the ICC for primaries and transfer.
    base->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    base->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    require_avif(avifImageSetProfileICC(base.get(), fixture.base_icc.data(), fixture.base_icc.size()),
                 "set synthetic base ICC profile");
  }

  AvifEncPtr encoder(avifEncoderCreate());
  require(encoder != nullptr, "allocate AVIF encoder");
  encoder->speed = 9;
  encoder->quality = encoder->qualityGainMap = 95;
  avifRWData encoded = AVIF_DATA_EMPTY;
  require_avif(avifEncoderWrite(encoder.get(), base.get(), &encoded), "encode synthetic tmap");
  std::vector<uint8_t> bytes(encoded.data, encoded.data + encoded.size);
  avifRWDataFree(&encoded);

  if (fixture.strip_colr_box) {
    bool patched = false;
    for (size_t i = 0; i + 8u <= bytes.size(); ++i) {
      if (std::memcmp(bytes.data() + i + 4u, "colr", 4u) == 0) {
        std::memcpy(bytes.data() + i + 4u, "free", 4u);
        patched = true;
      }
    }
    require(patched, "expected colr box in synthetic tmap fixture");
  }

  if (!fixture.unassociated_icc.empty()) {
    const size_t offset = bytes.size();
    const auto box_size = static_cast<uint32_t>(8u + fixture.unassociated_icc.size());
    bytes.resize(offset + 8u);
    write_be32(bytes, offset, box_size);
    std::memcpy(bytes.data() + offset + 4u, "free", 4u);
    bytes.insert(bytes.end(), fixture.unassociated_icc.begin(), fixture.unassociated_icc.end());
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(out.good(), "write synthetic tmap image");
}

std::array<double, 3> sample_nits_at(const hdrbridge::gainmap::ReconstructedHdr& hdr,
                                     uint32_t x, uint32_t y) {
  const size_t p = (static_cast<size_t>(y) * hdr.info.width + x) * 3u;
  return {
      hdrbridge::transfer::pq_to_nits(hdr.rec2020_pq_rgb[p + 0] / 65535.0),
      hdrbridge::transfer::pq_to_nits(hdr.rec2020_pq_rgb[p + 1] / 65535.0),
      hdrbridge::transfer::pq_to_nits(hdr.rec2020_pq_rgb[p + 2] / 65535.0),
  };
}

void check_extraction_without_reconstruction(const std::filesystem::path& input,
                                             const std::filesystem::path& output,
                                             std::string_view expected_error,
                                             uint32_t width, uint32_t height, uint32_t channels,
                                             uint16_t expected_transfer = 0,
                                             const char* output_format = "png") {
  const auto info = hdrbridge::inspect(input);
  const auto name = input.filename().string();
  require(info.asset_kind == "gain-map-hdr" && info.gain_map_present,
          name + ": Inspector rejected the gain map");
  if (expected_transfer) {
    require(info.transfer == expected_transfer, name + ": Inspector reported the wrong transfer");
  }

  auto unexpected_output = output;
  unexpected_output.replace_extension(".unexpected.png");
  hdrbridge::ConversionOptions options;
  options.mode = "png-pq16";
  std::string error;
  try {
    (void)hdrbridge::convert(input, unexpected_output, options);
  } catch (const std::runtime_error& failure) {
    error = failure.what();
  }
  require(error.find(expected_error) != std::string::npos &&
              !std::filesystem::exists(unexpected_output),
          name + ": expected HDR reconstruction rejection, got: " + error);

  options.mode = "gainmap-extract";
  options.gainmap_export_format = output_format;
  const auto result = hdrbridge::convert(input, output, options);
  const auto& verified = result.verification;
  require(result.success && verified.passed && verified.width == width &&
              verified.height == height && verified.gain_map_channels == channels,
          name + ": gain map export dimensions or channels mismatch");
}

void test_synthetic_iso_tmap_heif() {
  UniqueTempDir temp_dir("hdrbridge-gainmap-heif-tests");
  const auto& dir = temp_dir.path;
  const std::vector<uint8_t> dummy_lut(16, 0);
  const auto unassociated_300_icc = build_synthetic_icc({
      {"A2B0", dummy_lut}, {"B2A0", dummy_lut},
      {"HAGC", make_hagc_tag(0, 0, 0xC0, 1500)},
  });

  const struct Case {
    const char* name;
    TmapFixture fixture;
    double tolerance_red;
    double tolerance_other;
  } samples[] = {
      {"nclx-rgb10.heif", {.unassociated_icc = unassociated_300_icc}, 35.0, 15.0},
      {"nclx-rgb8-odd-scale.heif", {.map_width = 11, .map_height = 8, .map_depth = 8}, 40.0, 20.0},
  };
  for (const auto& test : samples) {
    const auto path = dir / test.name;
    write_tmap_fixture(path, test.fixture);
    const auto info = hdrbridge::inspect(path);
    require(info.asset_kind == "gain-map-hdr" && info.gain_map_family == "iso-heif-tmap" &&
                !info.icc_present && info.native_color_present &&
                info.resolved_signaling_source == "Native" &&
                info.primaries == 1 && info.gain_map_channels == 3 &&
                !info.gain_map_uses_base_color_space &&
                info.gain_map_width == test.fixture.map_width && info.gain_map_height == test.fixture.map_height,
            std::string(test.name) + ": ISO tmap Inspector mismatch");

    // Ignoring tmap primaries shifts the green highlight to about 107 nits.
    // An unrelated 300-nit ICC would instead raise the red highlight to about 1182 nits.
    const auto hdr = hdrbridge::gainmap::reconstruct_heif_gainmap(path);
    require(hdr.info.width == 32 && hdr.info.height == 24, "reconstructed HEIF dimensions mismatch");
    const auto white = sample_nits_at(hdr, 6, 12);
    require(std::all_of(white.begin(), white.end(),
                        [](double nits) { return std::abs(nits - 203.0) < 20.0; }),
            std::string(test.name) + ": SDR patch nits mismatch");
    const auto highlight = sample_nits_at(hdr, 26, 12);
    require(std::abs(highlight[0] - 800.0) < test.tolerance_red &&
                std::abs(highlight[1] - 50.0) < test.tolerance_other &&
                std::abs(highlight[2] - 50.0) < test.tolerance_other,
            std::string(test.name) + ": tmap highlight RGB nits mismatch");
  }
  hdrbridge::ConversionOptions options;
  options.mode = "png-pq16";
  bool saw_iso_tmap_progress = false;
  bool saw_apple_progress = false;
  const auto callback = [&](int, const std::string& message) {
    if (message.find("ISO tmap HEIF/HEIC") != std::string::npos ||
        message.find("HEIF/HEIC base/gain-map/tmap") != std::string::npos) {
      saw_iso_tmap_progress = true;
    }
    if (message.find("Apple") != std::string::npos) {
      saw_apple_progress = true;
    }
  };
  require(hdrbridge::convert(dir / samples[0].name, dir / "hdr.png", options, callback).verification.passed,
          "synthetic ISO tmap HEIF HDR export failed");
  require(saw_iso_tmap_progress && !saw_apple_progress,
          "synthetic ISO tmap HEIF progress must not report Apple branding");

  const struct RejectedCase {
    const char* name;
    TmapFixture fixture;
    const char* error;
    uint16_t expected_transfer = 0;
  } rejected[] = {
      {"alternate-601.heif", {.alternate_primaries = AVIF_COLOR_PRIMARIES_BT601},
       "tmap NCLX primaries are unsupported"},
      {"base-601.heif", {.use_base_color_space = true, .base_primaries = AVIF_COLOR_PRIMARIES_BT601},
       "base color profile primaries are unsupported"},
      {"base-pq.heif", {.use_base_color_space = true, .base_primaries = AVIF_COLOR_PRIMARIES_BT2020,
                       .base_transfer = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084},
       "base transfer characteristics are unsupported", 16},
  };
  for (const auto& test : rejected) {
    const auto path = dir / test.name;
    write_tmap_fixture(path, test.fixture);
    auto extracted = path;
    extracted.replace_extension(".png");
    check_extraction_without_reconstruction(path, extracted, test.error, 16, 12, 3,
                                             test.expected_transfer);
  }
}

void test_avif_gainmap_extract_mono_and_rgb() {
  UniqueTempDir temp_dir("hdrbridge-avif-gm-extract-tests");
  const struct Case {
    const char* name;
    avifPixelFormat format;
    uint32_t channels;
  } cases[] = {
      {"mono", AVIF_PIXEL_FORMAT_YUV400, 1},
      {"rgb", AVIF_PIXEL_FORMAT_YUV444, 3},
  };
  for (const auto& test : cases) {
    const auto name = std::string(test.name);
    const auto input = temp_dir.path / (name + ".avif");
    write_tmap_fixture(input, {.map_format = test.format});
    hdrbridge::ConversionOptions options;
    options.mode = "gainmap-extract";
    options.gainmap_export_format = "png";
    const auto result = hdrbridge::convert(input, temp_dir.path / (name + ".png"), options);
    const auto& verified = result.verification;
    require(result.success && verified.passed && verified.width == 16 && verified.height == 12 &&
                verified.gain_map_channels == test.channels,
            name + ": AVIF gain-map PNG export mismatch");
  }
}

void test_tiff_gainmap_extract_decoupling() {
  UniqueTempDir temp_dir("hdrbridge-tiff-gm-extract-tests");
  const auto& dir = temp_dir.path;

  const auto tiff_path = dir / "no-icc-alt-tiff.tif";
  {
    TIFF* tiff = TIFFOpenW(tiff_path.c_str(), "w");
    require(tiff != nullptr, "create synthetic gain-map TIFF");
    static const TIFFFieldInfo kFields[] = {
        {52557u, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED,
         FIELD_CUSTOM, TRUE, TRUE, const_cast<char*>("GainMapMetadata")}};
    TIFFMergeFieldInfo(tiff, kFields, 1);

    // Base directory (no ICC profile, 16x12 RGB16)
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 16u);
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 12u);
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, uint16_t(16));
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, uint16_t(3));
    TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, uint16_t(SAMPLEFORMAT_UINT));
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, uint16_t(PHOTOMETRIC_RGB));
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, uint16_t(PLANARCONFIG_CONTIG));
    TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 12u);
    uint16_t subifd_count = 1;
    uint64_t subifd_offsets[1] = {0};
    TIFFSetField(tiff, TIFFTAG_SUBIFD, subifd_count, subifd_offsets);
    std::vector<uint16_t> base_row(16u * 3u, 32768u);
    for (uint32_t y = 0; y < 12u; ++y) {
      require(TIFFWriteScanline(tiff, base_row.data(), y, 0) == 1, "write TIFF base row");
    }
    require(TIFFWriteDirectory(tiff) == 1, "finalize TIFF base IFD");

    // Gain Map SubIFD (8x6 RGB16, useBaseColorSpace=false)
    std::vector<uint8_t> tiff_meta(4, 0);  // 4-byte container_version = 0
    tiff_meta.push_back(0); tiff_meta.push_back(0);  // min_version
    tiff_meta.push_back(0); tiff_meta.push_back(0);  // writer_version
    tiff_meta.push_back(0x80u);  // 3ch RGB, useBaseColorSpace = false
    append_u32(tiff_meta, 0); append_u32(tiff_meta, 1);
    append_u32(tiff_meta, 2); append_u32(tiff_meta, 1);
    for (int c = 0; c < 3; ++c) {
      append_s32(tiff_meta, 0); append_u32(tiff_meta, 1);
      append_s32(tiff_meta, 2); append_u32(tiff_meta, 1);
      append_u32(tiff_meta, 1); append_u32(tiff_meta, 1);
      append_s32(tiff_meta, 0); append_u32(tiff_meta, 1);
      append_s32(tiff_meta, 0); append_u32(tiff_meta, 1);
    }
    TIFFSetField(tiff, TIFFTAG_SUBFILETYPE, uint32_t(32));
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 8u);
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 6u);
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, uint16_t(16));
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, uint16_t(3));
    TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, uint16_t(SAMPLEFORMAT_UINT));
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, uint16_t(52553));
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, uint16_t(PLANARCONFIG_CONTIG));
    TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 6u);
    require(TIFFMergeFieldInfo(tiff, kFields, 1) == 0, "register GainMapMetadata tag on SubIFD");
    TIFFSetField(tiff, 52557u, static_cast<uint32_t>(tiff_meta.size()), tiff_meta.data());
    std::vector<uint16_t> gm_row(8u * 3u);
    for (uint32_t x = 0; x < 8u; ++x) {
      gm_row[x * 3u + 0] = static_cast<uint16_t>(10000u + x * 1000u);
      gm_row[x * 3u + 1] = static_cast<uint16_t>(20000u + x * 1000u);
      gm_row[x * 3u + 2] = static_cast<uint16_t>(30000u + x * 1000u);
    }
    for (uint32_t y = 0; y < 6u; ++y) {
      require(TIFFWriteScanline(tiff, gm_row.data(), y, 0) == 1, "write TIFF gainmap row");
    }
    require(TIFFWriteDirectory(tiff) == 1, "finalize TIFF SubIFD");
    TIFFClose(tiff);
  }

  const auto output = dir / "extracted-gm.tiff";
  check_extraction_without_reconstruction(tiff_path, output, "SDR base ICC profile is missing",
                                           8, 6, 3, 0, "tiff");
  std::unique_ptr<TIFF, decltype(&TIFFClose)> extracted(TIFFOpenW(output.c_str(), "r"), TIFFClose);
  std::array<uint16_t, 8 * 3> row{};
  require(extracted && TIFFReadScanline(extracted.get(), row.data(), 0, 0) == 1 &&
              row[0] == 10000u && row[1] == 20000u && row[2] == 30000u,
          "extracted TIFF gain-map RGB samples mismatch");
}

std::vector<uint8_t> encode_minimal_jxl_codestream(uint32_t width, uint32_t height,
                                                 uint32_t channels, bool custom_icc_base) {
  auto enc = JxlEncoderMake(nullptr);
  require(enc != nullptr, "allocate JxlEncoder");
  JxlBasicInfo basic{};
  JxlEncoderInitBasicInfo(&basic);
  basic.xsize = width;
  basic.ysize = height;
  basic.bits_per_sample = 8;
  basic.num_color_channels = channels;
  basic.uses_original_profile = JXL_TRUE;
  require(JxlEncoderSetBasicInfo(enc.get(), &basic) == JXL_ENC_SUCCESS, "JXL basic info");
  if (custom_icc_base) {
    cmsCIExyY wp{0.3127, 0.3290, 1.0};
    cmsCIExyYTRIPLE adobe{{0.64, 0.33, 1.0}, {0.21, 0.71, 1.0}, {0.15, 0.06, 1.0}};
    cmsToneCurve* gamma = cmsBuildGamma(nullptr, 2.2);
    cmsToneCurve* curves[3]{gamma, gamma, gamma};
    cmsHPROFILE prof = cmsCreateRGBProfile(&wp, &adobe, curves);
    require(prof != nullptr, "create custom AdobeRGB ICC");
    cmsUInt32Number icc_sz = 0;
    require(cmsSaveProfileToMem(prof, nullptr, &icc_sz) && icc_sz > 0, "size custom ICC");
    std::vector<uint8_t> custom_icc(icc_sz);
    require(cmsSaveProfileToMem(prof, custom_icc.data(), &icc_sz), "serialize custom ICC");
    cmsFreeToneCurve(gamma);
    cmsCloseProfile(prof);
    require(JxlEncoderSetICCProfile(enc.get(), custom_icc.data(), custom_icc.size()) == JXL_ENC_SUCCESS,
            "JXL set custom ICC");
  } else {
    JxlColorEncoding color{};
    JxlColorEncodingSetToLinearSRGB(&color, channels == 1 ? JXL_TRUE : JXL_FALSE);
    require(JxlEncoderSetColorEncoding(enc.get(), &color) == JXL_ENC_SUCCESS, "JXL set color");
  }
  auto* frame_settings = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
  require(frame_settings != nullptr, "JXL frame settings");
  require(JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE) == JXL_ENC_SUCCESS, "JXL lossless");
  JxlPixelFormat fmt{channels, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint8_t> px(static_cast<size_t>(width) * height * channels, 128u);
  require(JxlEncoderAddImageFrame(frame_settings, &fmt, px.data(), px.size()) == JXL_ENC_SUCCESS,
          "JXL add frame");
  JxlEncoderCloseInput(enc.get());
  std::vector<uint8_t> out(4096);
  uint8_t* next = out.data();
  size_t avail = out.size();
  while (JxlEncoderProcessOutput(enc.get(), &next, &avail) == JXL_ENC_NEED_MORE_OUTPUT) {
    const size_t used = static_cast<size_t>(next - out.data());
    out.resize(out.size() * 2u);
    next = out.data() + used;
    avail = out.size() - used;
  }
  out.resize(static_cast<size_t>(next - out.data()));
  return out;
}

void append_bmff_box(std::vector<uint8_t>& out, const char* type4, const std::vector<uint8_t>& payload) {
  const size_t pos = out.size();
  out.resize(pos + 8u + payload.size());
  write_be32(out, pos, static_cast<uint32_t>(8u + payload.size()));
  std::memcpy(out.data() + pos + 4u, type4, 4u);
  std::memcpy(out.data() + pos + 8u, payload.data(), payload.size());
}

void test_jxl_gainmap_extract_decoupling() {
  UniqueTempDir temp_dir("hdrbridge-jxl-gm-extract-tests");
  const auto& dir = temp_dir.path;

  const auto base_cs = encode_minimal_jxl_codestream(16, 12, 3, /*custom_icc_base=*/true);
  const auto map_cs = encode_minimal_jxl_codestream(8, 6, 1, /*custom_icc_base=*/false);

  std::vector<uint8_t> iso_meta;
  iso_meta.push_back(0); iso_meta.push_back(0);
  iso_meta.push_back(0); iso_meta.push_back(0);
  iso_meta.push_back(0x40);  // 1ch, useBaseColorSpace=true
  append_u32(iso_meta, 0); append_u32(iso_meta, 1);
  append_u32(iso_meta, 2); append_u32(iso_meta, 1);
  append_s32(iso_meta, 0); append_u32(iso_meta, 1);
  append_s32(iso_meta, 2); append_u32(iso_meta, 1);
  append_u32(iso_meta, 1); append_u32(iso_meta, 1);
  append_s32(iso_meta, 0); append_u32(iso_meta, 1);
  append_s32(iso_meta, 0); append_u32(iso_meta, 1);

  std::vector<uint8_t> jhgm;
  jhgm.push_back(0);  // bundle version
  jhgm.push_back(static_cast<uint8_t>(iso_meta.size() >> 8));
  jhgm.push_back(static_cast<uint8_t>(iso_meta.size() & 0xffu));
  jhgm.insert(jhgm.end(), iso_meta.begin(), iso_meta.end());
  jhgm.push_back(0);  // color encoding size = 0
  append_u32(jhgm, 0);  // icc size = 0
  jhgm.insert(jhgm.end(), map_cs.begin(), map_cs.end());

  std::vector<uint8_t> jxl_file;
  append_bmff_box(jxl_file, "JXL ", {0x0d, 0x0a, 0x87, 0x0a});
  std::vector<uint8_t> ftyp(12);
  std::memcpy(ftyp.data(), "jxl ", 4);
  write_be32(ftyp, 4, 0);
  std::memcpy(ftyp.data() + 8, "jxl ", 4);
  append_bmff_box(jxl_file, "ftyp", ftyp);
  append_bmff_box(jxl_file, "jxlc", base_cs);
  append_bmff_box(jxl_file, "jhgm", jhgm);

  const auto jxl_path = dir / "custom-icc-base-jhgm.jxl";
  {
    std::ofstream out(jxl_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(jxl_file.data()),
              static_cast<std::streamsize>(jxl_file.size()));
    require(out.good(), "write synthetic JXL");
  }

  check_extraction_without_reconstruction(jxl_path, dir / "gm.png",
                                           "unsupported jhgm SDR base color encoding", 8, 6, 1);
}

std::vector<uint8_t> build_bt709_icc_with_optional_cicp(
    double gamma_val, std::optional<uint8_t> cicp_transfer,
    bool strip_rgb_trc = false) {
  const cmsCIExyY d65{0.3127, 0.3290, 1.0};
  const cmsCIExyYTRIPLE bt709_primaries{{0.640, 0.330, 1.0}, {0.300, 0.600, 1.0}, {0.150, 0.060, 1.0}};
  cmsToneCurve* trc = cmsBuildGamma(nullptr, gamma_val);
  require(trc != nullptr, "build gamma TRC");
  cmsToneCurve* curves[3]{trc, trc, trc};
  cmsHPROFILE prof = cmsCreateRGBProfile(&d65, &bt709_primaries, curves);
  cmsFreeToneCurve(trc);
  require(prof != nullptr, "create BT.709 RGB ICC profile");
  if (strip_rgb_trc) {
    require(cmsWriteTag(prof, cmsSigRedTRCTag, nullptr) != 0, "remove rTRC tag");
    require(cmsWriteTag(prof, cmsSigGreenTRCTag, nullptr) != 0, "remove gTRC tag");
    require(cmsWriteTag(prof, cmsSigBlueTRCTag, nullptr) != 0, "remove bTRC tag");
  }
  if (cicp_transfer) {
    cmsVideoSignalType cicp{};
    cicp.ColourPrimaries = 1;
    cicp.TransferCharacteristics = *cicp_transfer;
    cicp.MatrixCoefficients = 0;
    cicp.VideoFullRangeFlag = 1;
    require(cmsWriteTag(prof, cmsSigcicpTag, &cicp) != 0, "write cicp tag to ICC");
  }
  cmsUInt32Number size = 0;
  require(cmsSaveProfileToMem(prof, nullptr, &size) != 0 && size > 0, "measure ICC profile");
  std::vector<uint8_t> icc(size);
  require(cmsSaveProfileToMem(prof, icc.data(), &size) != 0, "serialize ICC profile");
  cmsCloseProfile(prof);
  return icc;
}

void test_heif_base_icc_validation() {
  UniqueTempDir temp_dir("hdrbridge-heif-icc-cicp-tests");
  const auto& dir = temp_dir.path;
  TmapFixture fixture{.map_depth = 8, .use_base_color_space = true,
                      .base_transfer = AVIF_TRANSFER_CHARACTERISTICS_LINEAR};
  const auto linear = dir / "linear.heif";
  fixture.base_icc = build_bt709_icc_with_optional_cicp(1.0, std::nullopt);
  write_tmap_fixture(linear, fixture);
  const auto cicp = dir / "linear-cicp.heif";
  fixture.base_icc = build_bt709_icc_with_optional_cicp(1.0, 8);
  write_tmap_fixture(cicp, fixture);

  const auto info = hdrbridge::inspect(cicp);
  require(info.asset_kind == "gain-map-hdr" && info.transfer == 8 &&
              info.icc_present && info.resolved_signaling_source == "ICC",
          "linear ICC with CICP transfer 8 must inspect as linear gain-map HDR with ICC signaling");
  const auto without_tag = hdrbridge::gainmap::reconstruct_heif_gainmap(linear);
  const auto with_tag = hdrbridge::gainmap::reconstruct_heif_gainmap(cicp);
  require(!with_tag.rec2020_pq_rgb.empty() && without_tag.rec2020_pq_rgb == with_tag.rec2020_pq_rgb,
          "linear ICC reconstruction must match with and without CICP transfer 8");

  const struct Case {
    const char* name;
    double gamma;
    std::optional<uint8_t> cicp_transfer;
    bool strip_trc;
    const char* error;
  } rejected[] = {
      {"nonlinear-cicp8", 2.2, 8, false, "ICC CICP linear transfer conflicts"},
      {"pq-cicp16", 1.0, 16, false, "ICC has non-SDR transfer characteristics"},
      {"missing-trc", 2.2, std::nullopt, true, "ICC has no RGB transfer curves"},
  };
  for (const auto& test : rejected) {
    const auto name = std::string(test.name);
    const auto path = dir / (name + ".heif");
    fixture.base_icc = build_bt709_icc_with_optional_cicp(
        test.gamma, test.cicp_transfer, test.strip_trc);
    write_tmap_fixture(path, fixture);
    check_extraction_without_reconstruction(path, dir / (name + ".png"),
                                             test.error, 16, 12, 3);
  }
}

void test_heif_nclx_matrix_range_and_absent_signaling() {
  UniqueTempDir temp_dir("hdrbridge-heif-nclx-tests");
  const auto limited_path = temp_dir.path / "nclx-12-13-6-limited.heif";
  write_tmap_fixture(limited_path, {
      .base_primaries = AVIF_COLOR_PRIMARIES_SMPTE432,
      .base_transfer = AVIF_TRANSFER_CHARACTERISTICS_SRGB,
      .base_range = AVIF_RANGE_LIMITED});
  const auto limited_info = hdrbridge::inspect(limited_path);
  require(limited_info.gain_map_family == "iso-heif-tmap" &&
              limited_info.primaries == 12 && limited_info.transfer == 13 &&
              limited_info.matrix == 6 && !limited_info.full_range &&
              limited_info.range_known && limited_info.native_color_present &&
              limited_info.native_primaries == 12 && limited_info.native_transfer == 13 &&
              limited_info.native_matrix == 6 && !limited_info.native_full_range &&
              limited_info.native_range_known &&
              limited_info.resolved_signaling_source == "Native",
          "Gain-map HEIF must preserve base NCLX primaries/transfer/matrix and limited range");

  const auto stripped_path = temp_dir.path / "nclx-stripped.heif";
  write_tmap_fixture(stripped_path, {
      .base_primaries = AVIF_COLOR_PRIMARIES_SMPTE432,
      .base_transfer = AVIF_TRANSFER_CHARACTERISTICS_SRGB,
      .base_range = AVIF_RANGE_LIMITED,
      .strip_colr_box = true});
  const auto stripped_info = hdrbridge::inspect(stripped_path);
  require(!stripped_info.icc_present && !stripped_info.native_color_present &&
              stripped_info.primaries == 0 && stripped_info.transfer == 0 &&
              stripped_info.matrix == 0 && !stripped_info.range_known &&
              !stripped_info.native_range_known &&
              stripped_info.resolved_signaling_source == "Unknown",
          "Gain-map HEIF without color properties must report absent native signal and Unknown source");
}

void test_apple_jpeg_ignores_base_comment_urn() {
  UniqueTempDir temp_dir("hdrbridge-uhdr-comment-tests");
  const auto source_heif = temp_dir.path / "source.heif";
  write_tmap_fixture(source_heif, {.base_primaries = AVIF_COLOR_PRIMARIES_BT709});
  const auto uhdr_path = temp_dir.path / "iso-uhdr.jpg";
  hdrbridge::ConversionOptions options;
  options.mode = "ultrahdr";
  require(hdrbridge::convert(source_heif, uhdr_path, options).verification.passed,
          "generate ISO Ultra HDR fixture");

  std::ifstream in(uhdr_path, std::ios::binary | std::ios::ate);
  require(in.good(), "open generated ISO Ultra HDR JPEG");
  const auto size = static_cast<size_t>(in.tellg());
  std::vector<uint8_t> bytes(size);
  in.seekg(0);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  require(bytes.size() >= 2 && bytes[0] == 0xffu && bytes[1] == 0xd8u, "valid JPEG SOI");

  const std::string comment = "urn:com:apple:photo:2020:aux:hdrgainmap";
  std::vector<uint8_t> commented{0xffu, 0xd8u};
  append_jpeg_segment(commented, 0xfeu,
                      std::vector<uint8_t>(comment.begin(), comment.end()));
  commented.insert(commented.end(), bytes.begin() + 2, bytes.end());
  const auto commented_path = temp_dir.path / "iso-uhdr-commented.jpg";
  {
    std::ofstream out(commented_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(commented.data()),
              static_cast<std::streamsize>(commented.size()));
    require(out.good(), "write commented ISO Ultra HDR JPEG");
  }

  require(!hdrbridge::gainmap::probe_apple_jpeg(commented_path).detected,
          "ISO Ultra HDR JPEG with Apple URN in base COM comment must not route as Apple HDR JPEG");
  const auto info = hdrbridge::inspect(commented_path);
  require(info.gain_map_family == "iso-ultrahdr-jpeg" && info.format == "Ultra HDR JPEG",
          "commented ISO Ultra HDR JPEG must inspect as iso-ultrahdr-jpeg");
  hdrbridge::ConversionOptions out_opts;
  out_opts.mode = "png-pq16";
  require(hdrbridge::convert(commented_path, temp_dir.path / "out.png", out_opts).verification.passed,
          "commented ISO Ultra HDR JPEG must convert cleanly via Ultra HDR path");
}

void test_apple_jpeg_missing_icc_keeps_unspecified_signaling() {
  UniqueTempDir temp_dir("hdrbridge-apple-jpeg-no-icc-tests");
  const auto source_heif = temp_dir.path / "source.heif";
  write_tmap_fixture(source_heif, {.base_primaries = AVIF_COLOR_PRIMARIES_BT709});
  const auto uhdr_path = temp_dir.path / "iso-uhdr.jpg";
  hdrbridge::ConversionOptions options;
  options.mode = "ultrahdr";
  require(hdrbridge::convert(source_heif, uhdr_path, options).verification.passed,
          "generate base Ultra HDR JPEG");

  std::ifstream in(uhdr_path, std::ios::binary | std::ios::ate);
  require(in.good(), "open Ultra HDR JPEG");
  std::vector<uint8_t> bytes(static_cast<size_t>(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

  // Strip base APP2 ICC_PROFILE segment (if any) and append Apple XMP inside the
  // secondary gain-map JPEG's APP1 segment, keeping MPF offsets updated.
  size_t mpf_offset = std::string::npos;
  size_t gain_soi = std::string::npos;
  for (size_t i = 2; i + 4 <= bytes.size(); ++i) {
    if (bytes[i] == 0xffu && bytes[i + 1u] == 0xe2u &&
        i + 8 <= bytes.size() && std::memcmp(bytes.data() + i + 4u, "MPF\0", 4u) == 0) {
      mpf_offset = i + 4u;
    }
    if (i > 4 && bytes[i] == 0xffu && bytes[i + 1u] == 0xd8u && bytes[i + 2u] == 0xffu) {
      gain_soi = i;
    }
  }
  require(mpf_offset != std::string::npos && gain_soi != std::string::npos,
          "expected MPF segment and gain-map SOI in Ultra HDR JPEG");

  // Neutralize any base APP2 ICC_PROFILE segment before gain_soi (preserving MPF
  // offsets) and append Apple XMP inside the secondary gain-map JPEG's APP1
  // segment, updating the MPF gain-map image size field.
  for (size_t i = 2; i + 16u <= gain_soi; ++i) {
    if (bytes[i] == 0xffu && bytes[i + 1u] == 0xe2u &&
        std::memcmp(bytes.data() + i + 4u, "ICC_PROFILE\0", 12u) == 0) {
      bytes[i + 4u] = 'X';
    }
  }
  const std::string xmp_prefix("http://ns.adobe.com/xap/1.0/\0", 29);
  const std::string xmp_xml =
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
      "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" "
      "xmlns:apdi=\"http://ns.apple.com/pixeldatainfo/1.0/\" "
      "xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">"
      "<apdi:AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap</apdi:AuxiliaryImageType>"
      "<HDRGainMap:HDRGainMapVersion>131072</HDRGainMap:HDRGainMapVersion>"
      "</rdf:Description></rdf:RDF></x:xmpmeta>";
  std::vector<uint8_t> gain_xmp(xmp_prefix.begin(), xmp_prefix.end());
  gain_xmp.insert(gain_xmp.end(), xmp_xml.begin(), xmp_xml.end());

  std::vector<uint8_t> patched;
  patched.insert(patched.end(), bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(gain_soi + 2u));
  append_jpeg_segment(patched, 0xe1u, gain_xmp);
  patched.insert(patched.end(), bytes.begin() + static_cast<ptrdiff_t>(gain_soi + 2u), bytes.end());
  const uint32_t added = static_cast<uint32_t>(4u + gain_xmp.size());
  // Update MPF TIFF image-size fields for the grown gain-map JPEG (little or big endian).
  const size_t tiff_base = mpf_offset + 4u;
  const bool big_endian = bytes[tiff_base] == 'M' && bytes[tiff_base + 1u] == 'M';
  auto read32 = [&](const std::vector<uint8_t>& buf, size_t off) -> uint32_t {
    if (big_endian) {
      return (uint32_t(buf[off]) << 24) | (uint32_t(buf[off + 1]) << 16) |
             (uint32_t(buf[off + 2]) << 8) | uint32_t(buf[off + 3]);
    }
    return uint32_t(buf[off]) | (uint32_t(buf[off + 1]) << 8) |
           (uint32_t(buf[off + 2]) << 16) | (uint32_t(buf[off + 3]) << 24);
  };
  auto write32 = [&](std::vector<uint8_t>& buf, size_t off, uint32_t v) {
    if (big_endian) {
      buf[off] = static_cast<uint8_t>(v >> 24); buf[off + 1] = static_cast<uint8_t>(v >> 16);
      buf[off + 2] = static_cast<uint8_t>(v >> 8); buf[off + 3] = static_cast<uint8_t>(v);
    } else {
      buf[off] = static_cast<uint8_t>(v); buf[off + 1] = static_cast<uint8_t>(v >> 8);
      buf[off + 2] = static_cast<uint8_t>(v >> 16); buf[off + 3] = static_cast<uint8_t>(v >> 24);
    }
  };
  const uint32_t gain_orig_size = static_cast<uint32_t>(bytes.size() - gain_soi);
  for (size_t i = tiff_base; i + 4 <= tiff_base + 256 && i + 4 <= patched.size(); ++i) {
    if (read32(patched, i) == gain_orig_size) {
      write32(patched, i, gain_orig_size + added);
    }
  }

  const auto apple_no_icc_path = temp_dir.path / "apple-no-icc.jpg";
  {
    std::ofstream out(apple_no_icc_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(patched.data()),
              static_cast<std::streamsize>(patched.size()));
    require(out.good(), "write Apple no-ICC MPF JPEG fixture");
  }

  const auto info = hdrbridge::inspect(apple_no_icc_path);
  require(info.format == "Apple HDR JPEG" && info.gain_map_family == "apple-mpf-iso" &&
              !info.icc_present && info.primaries == 0 && info.transfer == 0 &&
              !info.range_known && info.resolved_signaling_source == "Unknown" &&
              info.base_color_space == "Unspecified" && info.base_transfer == "Unspecified",
          "Apple JPEG without ICC must preserve unspecified color signaling");
  hdrbridge::ConversionOptions recon_opts;
  recon_opts.mode = "png-pq16";
  require(hdrbridge::convert(apple_no_icc_path, temp_dir.path / "apple-no-icc.png", recon_opts)
              .verification.passed,
          "Apple JPEG without ICC must still reconstruct via single-open Apple path");
}

void test_standard_ultrahdr_ignores_hagc_reference_white() {
  UniqueTempDir temp_dir("hdrbridge-uhdr-ignore-hagc-tests");
  const auto source_heif = temp_dir.path / "source.heif";
  write_tmap_fixture(source_heif, {.base_primaries = AVIF_COLOR_PRIMARIES_BT709});
  const auto uhdr_path = temp_dir.path / "iso-uhdr.jpg";
  hdrbridge::ConversionOptions encode_opts;
  encode_opts.mode = "ultrahdr";
  require(hdrbridge::convert(source_heif, uhdr_path, encode_opts).verification.passed,
          "generate ISO Ultra HDR JPEG");

  hdrbridge::ConversionOptions recon_opts;
  recon_opts.mode = "png-pq16";
  const auto baseline = hdrbridge::convert(uhdr_path, temp_dir.path / "baseline.png", recon_opts);
  require(baseline.verification.passed, "decode baseline ISO Ultra HDR JPEG");

  std::ifstream in(uhdr_path, std::ios::binary | std::ios::ate);
  require(in.good(), "open ISO Ultra HDR JPEG");
  std::vector<uint8_t> bytes(static_cast<size_t>(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

  size_t mpf_offset = std::string::npos;
  size_t gain_soi = std::string::npos;
  for (size_t i = 2; i + 4 <= bytes.size(); ++i) {
    if (bytes[i] == 0xffu && bytes[i + 1u] == 0xe2u &&
        i + 8 <= bytes.size() && std::memcmp(bytes.data() + i + 4u, "MPF\0", 4u) == 0) {
      mpf_offset = i + 4u;
    }
    if (i > 4 && bytes[i] == 0xffu && bytes[i + 1u] == 0xd8u && bytes[i + 2u] == 0xffu) {
      gain_soi = i;
    }
  }
  require(mpf_offset != std::string::npos && gain_soi != std::string::npos,
          "expected MPF segment and gain-map SOI");

  const std::vector<uint8_t> dummy_lut(16, 0);
  const auto plain_icc = build_synthetic_icc({{"rXYZ", dummy_lut}});
  const auto hagc_300 = build_synthetic_icc({
      {"rXYZ", dummy_lut}, {"HAGC", make_hagc_tag(0, 0, 0xC0, 1500)}});
  const auto hagc_400 = build_synthetic_icc({
      {"rXYZ", dummy_lut}, {"HAGC", make_hagc_tag(0, 0, 0xC0, 2000)}});

  const auto patch_gainmap_iccs = [&](const std::vector<std::vector<uint8_t>>& chunks,
                                      const std::filesystem::path& out_path) {
    std::vector<uint8_t> patched;
    patched.insert(patched.end(), bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(gain_soi + 2u));
    uint32_t added = 0;
    for (const auto& chunk : chunks) {
      append_jpeg_segment(patched, 0xe2u, chunk);
      added += static_cast<uint32_t>(4u + chunk.size());
    }
    patched.insert(patched.end(), bytes.begin() + static_cast<ptrdiff_t>(gain_soi + 2u), bytes.end());
    const size_t tiff_base = mpf_offset + 4u;
    const bool big_endian = bytes[tiff_base] == 'M' && bytes[tiff_base + 1u] == 'M';
    auto read32 = [&](const std::vector<uint8_t>& buf, size_t off) -> uint32_t {
      if (big_endian) {
        return (uint32_t(buf[off]) << 24) | (uint32_t(buf[off + 1]) << 16) |
               (uint32_t(buf[off + 2]) << 8) | uint32_t(buf[off + 3]);
      }
      return uint32_t(buf[off]) | (uint32_t(buf[off + 1]) << 8) |
             (uint32_t(buf[off + 2]) << 16) | uint32_t(buf[off + 3]) << 24;
    };
    auto write32 = [&](std::vector<uint8_t>& buf, size_t off, uint32_t v) {
      if (big_endian) {
        buf[off] = static_cast<uint8_t>(v >> 24); buf[off + 1] = static_cast<uint8_t>(v >> 16);
        buf[off + 2] = static_cast<uint8_t>(v >> 8); buf[off + 3] = static_cast<uint8_t>(v);
      } else {
        buf[off] = static_cast<uint8_t>(v); buf[off + 1] = static_cast<uint8_t>(v >> 8);
        buf[off + 2] = static_cast<uint8_t>(v >> 16); buf[off + 3] = static_cast<uint8_t>(v >> 24);
      }
    };
    const uint32_t gain_orig_size = static_cast<uint32_t>(bytes.size() - gain_soi);
    for (size_t i = tiff_base; i + 4 <= tiff_base + 256 && i + 4 <= patched.size(); ++i) {
      if (read32(patched, i) == gain_orig_size) {
        write32(patched, i, gain_orig_size + added);
      }
    }
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(patched.data()),
              static_cast<std::streamsize>(patched.size()));
    require(out.good(), "write patched ISO Ultra HDR JPEG");
  };

  const auto plain_icc_path = temp_dir.path / "iso-uhdr-plain-icc.jpg";
  const auto with_hagc_path = temp_dir.path / "iso-uhdr-hagc.jpg";
  patch_gainmap_iccs({make_app2_icc_chunk(1, 1, plain_icc.data(), plain_icc.size())},
                     plain_icc_path);
  patch_gainmap_iccs({make_app2_icc_chunk(1, 1, hagc_300.data(), hagc_300.size()),
                      make_app2_icc_chunk(1, 1, hagc_400.data(), hagc_400.size())},
                     with_hagc_path);

  const auto plain_result = hdrbridge::convert(plain_icc_path, temp_dir.path / "plain-icc.png", recon_opts);
  const auto info = hdrbridge::inspect(with_hagc_path);
  require(info.gain_map_family == "iso-ultrahdr-jpeg",
          "Ultra HDR JPEG with HAGC ICC must remain iso-ultrahdr-jpeg");
  const auto result = hdrbridge::convert(with_hagc_path, temp_dir.path / "with-hagc.png", recon_opts);
  require(plain_result.verification.passed && result.verification.passed &&
              std::abs(result.verification.max_channel_nits -
                       plain_result.verification.max_channel_nits) < 1e-6 &&
              std::abs(result.verification.max_channel_nits -
                       baseline.verification.max_channel_nits) < 1.0,
          "Standard Ultra HDR JPEG must ignore HAGC tags and stay at 203-nit reference white");
}

struct MpfLocations {
  size_t mpf_offset = std::string::npos;
  size_t gain_soi = std::string::npos;
};

MpfLocations find_mpf_and_gainmap(const std::vector<uint8_t>& bytes) {
  MpfLocations loc;
  for (size_t i = 2; i + 4 <= bytes.size(); ++i) {
    if (bytes[i] == 0xffu && bytes[i + 1u] == 0xe2u &&
        i + 8 <= bytes.size() && std::memcmp(bytes.data() + i + 4u, "MPF\0", 4u) == 0) {
      loc.mpf_offset = i + 4u;
    }
    if (i > 4 && bytes[i] == 0xffu && bytes[i + 1u] == 0xd8u && bytes[i + 2u] == 0xffu) {
      loc.gain_soi = i;
    }
  }
  require(loc.mpf_offset != std::string::npos && loc.gain_soi != std::string::npos,
          "expected MPF segment and gain-map SOI");
  return loc;
}

std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  require(in.good(), "open binary file");
  std::vector<uint8_t> bytes(static_cast<size_t>(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(in.good(), "read binary file");
  return bytes;
}

void write_binary_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  require(out.good(), "write binary file");
}

void update_mpf_gain_size(std::vector<uint8_t>& bytes,
                          size_t mpf_offset,
                          uint32_t old_size,
                          uint32_t new_size) {
  const size_t tiff_base = mpf_offset + 4u;
  const bool big_endian = bytes[tiff_base] == 'M' && bytes[tiff_base + 1u] == 'M';
  const auto read32 = [&](size_t off) -> uint32_t {
    if (big_endian) {
      return (uint32_t(bytes[off]) << 24) | (uint32_t(bytes[off + 1]) << 16) |
             (uint32_t(bytes[off + 2]) << 8) | uint32_t(bytes[off + 3]);
    }
    return uint32_t(bytes[off]) | (uint32_t(bytes[off + 1]) << 8) |
           (uint32_t(bytes[off + 2]) << 16) | uint32_t(bytes[off + 3]) << 24;
  };
  const auto write32 = [&](size_t off, uint32_t v) {
    if (big_endian) {
      bytes[off] = static_cast<uint8_t>(v >> 24); bytes[off + 1] = static_cast<uint8_t>(v >> 16);
      bytes[off + 2] = static_cast<uint8_t>(v >> 8); bytes[off + 3] = static_cast<uint8_t>(v);
    } else {
      bytes[off] = static_cast<uint8_t>(v); bytes[off + 1] = static_cast<uint8_t>(v >> 8);
      bytes[off + 2] = static_cast<uint8_t>(v >> 16); bytes[off + 3] = static_cast<uint8_t>(v >> 24);
    }
  };
  for (size_t i = tiff_base; i + 4 <= tiff_base + 256 && i + 4 <= bytes.size(); ++i) {
    if (read32(i) == old_size) {
      write32(i, new_size);
    }
  }
}

void test_apple_jpeg_xmp_custom_prefixes_and_hagc() {
  UniqueTempDir temp_dir("hdrbridge-apple-xmp-prefix-tests");
  const auto source_heif = temp_dir.path / "source.heif";
  write_tmap_fixture(source_heif, {.base_primaries = AVIF_COLOR_PRIMARIES_BT709});
  const auto uhdr_path = temp_dir.path / "iso-uhdr.jpg";
  hdrbridge::ConversionOptions encode_opts;
  encode_opts.mode = "ultrahdr";
  require(hdrbridge::convert(source_heif, uhdr_path, encode_opts).verification.passed,
          "generate base Ultra HDR JPEG");

  const auto bytes = read_binary_file(uhdr_path);
  const auto loc = find_mpf_and_gainmap(bytes);

  const std::vector<uint8_t> dummy_lut(16, 0);
  const auto hagc_304 = build_synthetic_icc({
      {"rXYZ", dummy_lut}, {"HAGC", make_hagc_tag(0, 0, 0xC0, 1520)}});  // 304.0 nits
  const auto hagc_chunk = make_app2_icc_chunk(1, 1, hagc_304.data(), hagc_304.size());

  const auto make_apple_xmp_segment = [](int variant) {
    const std::string xmp_prefix("http://ns.adobe.com/xap/1.0/\0", 29);
    const std::string xmp_xml = variant == 1
        ? "<meta:xmpmeta xmlns:meta=\"adobe:ns:meta/\">"
          "<r:RDF xmlns:r=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
          "<r:Description r:about=\"\" "
          "xmlns:p=\"http://ns.apple.com/pixeldatainfo/1.0/\" "
          "xmlns:g=\"http://ns.apple.com/HDRGainMap/1.0/\">"
          "<p:AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap</p:AuxiliaryImageType>"
          "<g:HDRGainMapVersion>131072</g:HDRGainMapVersion>"
          "</r:Description></r:RDF></meta:xmpmeta>"
        : variant == 2
        ? "<meta:xmpmeta xmlns:meta=\"adobe:ns:meta/\">"
          "<r:RDF xmlns:r=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
          "<r:Description r:about=\"\" "
          "xmlns:p=\"http://ns.&#x61;pple.com/pixeldatainfo/1.0/\" "
          "xmlns:g=\"http://ns.&#x61;pple.com/HDRGainMap/1.0/\">"
          "<p:AuxiliaryImageType>urn:com:&#x61;pple:photo:2020:aux:hdrgainmap</p:AuxiliaryImageType>"
          "<g:HDRGainMapVersion>131072</g:HDRGainMapVersion>"
          "</r:Description></r:RDF></meta:xmpmeta>"
        : variant == 3
        ? "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
          "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
          "<rdf:Description rdf:about=\"\" "
          "xmlns:apdi=\"http://ns.apple.com/pixeldatainfo/1.0/\">"
          "<apdi:AuxiliaryImageType rdf:resource=\"urn:com:apple:photo:2020:aux:hdrgainmap\"/>"
          "</rdf:Description></rdf:RDF></x:xmpmeta>"
        : "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
          "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
          "<rdf:Description rdf:about=\"\" "
          "xmlns:apdi=\"http://ns.apple.com/pixeldatainfo/1.0/\" "
          "xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">"
          "<apdi:AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap</apdi:AuxiliaryImageType>"
          "<HDRGainMap:HDRGainMapVersion>131072</HDRGainMap:HDRGainMapVersion>"
          "</rdf:Description></rdf:RDF></x:xmpmeta>";
    std::vector<uint8_t> payload(xmp_prefix.begin(), xmp_prefix.end());
    payload.insert(payload.end(), xmp_xml.begin(), xmp_xml.end());
    return payload;
  };

  const auto build_variant = [&](int variant, const std::filesystem::path& out_path) {
    const auto xmp_seg = make_apple_xmp_segment(variant);
    std::vector<uint8_t> patched;
    patched.insert(patched.end(), bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(loc.gain_soi + 2u));
    append_jpeg_segment(patched, 0xe1u, xmp_seg);
    append_jpeg_segment(patched, 0xe2u, hagc_chunk);
    patched.insert(patched.end(), bytes.begin() + static_cast<ptrdiff_t>(loc.gain_soi + 2u), bytes.end());
    const uint32_t added = static_cast<uint32_t>(8u + xmp_seg.size() + hagc_chunk.size());
    const uint32_t gain_orig_size = static_cast<uint32_t>(bytes.size() - loc.gain_soi);
    update_mpf_gain_size(patched, loc.mpf_offset, gain_orig_size, gain_orig_size + added);
    write_binary_file(out_path, patched);
  };

  const auto std_path = temp_dir.path / "apple-std-prefix.jpg";
  const auto custom_path = temp_dir.path / "apple-custom-prefix.jpg";
  const auto entity_path = temp_dir.path / "apple-charref.jpg";
  const auto resource_path = temp_dir.path / "apple-rdf-resource.jpg";
  build_variant(0, std_path);
  build_variant(1, custom_path);
  build_variant(2, entity_path);
  build_variant(3, resource_path);

  const auto std_info = hdrbridge::inspect(std_path);
  const auto custom_info = hdrbridge::inspect(custom_path);
  const auto entity_info = hdrbridge::inspect(entity_path);
  const auto resource_info = hdrbridge::inspect(resource_path);
  require(std_info.gain_map_family == "apple-mpf-iso" &&
              custom_info.gain_map_family == "apple-mpf-iso" &&
              custom_info.format == "Apple HDR JPEG" &&
              entity_info.gain_map_family == "apple-mpf-iso" &&
              entity_info.format == "Apple HDR JPEG" &&
              resource_info.gain_map_family == "apple-mpf-iso" &&
              resource_info.format == "Apple HDR JPEG",
          "Apple XMP detection must classify by XML namespace URIs, character references, and rdf:resource");

  hdrbridge::ConversionOptions recon_opts;
  recon_opts.mode = "png-pq16";
  const auto std_out = hdrbridge::convert(std_path, temp_dir.path / "std.png", recon_opts);
  const auto custom_out = hdrbridge::convert(custom_path, temp_dir.path / "custom.png", recon_opts);
  const auto entity_out = hdrbridge::convert(entity_path, temp_dir.path / "entity.png", recon_opts);
  const auto resource_out = hdrbridge::convert(resource_path, temp_dir.path / "resource.png", recon_opts);
  require(std_out.verification.passed && custom_out.verification.passed &&
              entity_out.verification.passed && resource_out.verification.passed &&
              std::abs(std_out.verification.max_channel_nits -
                       custom_out.verification.max_channel_nits) < 1e-6 &&
              std::abs(std_out.verification.max_channel_nits -
                       entity_out.verification.max_channel_nits) < 1e-6 &&
              std::abs(std_out.verification.max_channel_nits -
                       resource_out.verification.max_channel_nits) < 1e-6,
          "Custom-prefix, entity-encoded, and rdf:resource Apple XMP must honor the exact same HAGC luminance");

  // Corrupt ISO 21496-1 minimum_version inside the Apple gain-map JPEG and verify
  // inspect/convert fail instead of silently downgrading to SDR.
  auto broken = read_binary_file(resource_path);
  const char iso_urn[] = "urn:iso:std:iso:ts:21496:-1";
  const size_t iso_urn_len = sizeof(iso_urn);  // includes trailing NUL
  bool patched_iso_version = false;
  for (size_t i = loc.gain_soi; i + iso_urn_len + 2u <= broken.size(); ++i) {
    if (std::memcmp(broken.data() + i, iso_urn, iso_urn_len) == 0) {
      broken[i + iso_urn_len] = 0x00u;
      broken[i + iso_urn_len + 1u] = 0x7fu;
      patched_iso_version = true;
      break;
    }
  }
  require(patched_iso_version, "locate ISO 21496-1 APP2 metadata in gain-map JPEG");
  const auto broken_path = temp_dir.path / "apple-corrupt-iso-version.jpg";
  write_binary_file(broken_path, broken);
  bool inspect_threw = false;
  try {
    (void)hdrbridge::inspect(broken_path);
  } catch (const std::runtime_error&) {
    inspect_threw = true;
  }
  bool convert_threw = false;
  try {
    (void)hdrbridge::convert(broken_path, temp_dir.path / "broken.png", recon_opts);
  } catch (const std::runtime_error&) {
    convert_threw = true;
  }
  require(inspect_threw && convert_threw,
          "Corrupted gain-map JPEG metadata must throw instead of falling back to SDR");
}

void test_portrait_jpeg_swaps_gainmap_dimensions_and_scale() {
  UniqueTempDir temp_dir("hdrbridge-portrait-uhdr-tests");
  const auto source_heif = temp_dir.path / "source.heif";
  write_tmap_fixture(source_heif, {.base_primaries = AVIF_COLOR_PRIMARIES_BT709});
  const auto uhdr_path = temp_dir.path / "iso-uhdr-half.jpg";
  hdrbridge::ConversionOptions encode_opts;
  encode_opts.mode = "ultrahdr";
  encode_opts.gainmap_scale = 2;
  require(hdrbridge::convert(source_heif, uhdr_path, encode_opts).verification.passed,
          "generate half-res Ultra HDR JPEG");

  const auto bytes = read_binary_file(uhdr_path);
  const std::vector<uint8_t> exif_payload{
      'E', 'x', 'i', 'f', 0x00, 0x00,
      'M', 'M', 0x00, 0x2a, 0x00, 0x00, 0x00, 0x08,
      0x00, 0x01,
      0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00};
  std::vector<uint8_t> patched;
  patched.push_back(0xffu);
  patched.push_back(0xd8u);
  append_jpeg_segment(patched, 0xe1u, exif_payload);
  patched.insert(patched.end(), bytes.begin() + 2, bytes.end());
  const auto portrait_path = temp_dir.path / "portrait-uhdr.jpg";
  write_binary_file(portrait_path, patched);

  const auto info = hdrbridge::inspect(portrait_path);
  require(info.original_orientation == 8 &&
              info.width == 24 && info.height == 32 &&
              info.base_width == 24 && info.base_height == 32 &&
              info.gain_map_width == 12 && info.gain_map_height == 16 &&
              std::abs(info.gain_map_scale_x - 2.0) < 1e-9 &&
              std::abs(info.gain_map_scale_y - 2.0) < 1e-9,
          "Portrait JPEG must swap gain-map dimensions and report true 2.0x/2.0x scale");
}

void test_heif_unrecognized_icc_curve_keeps_unspecified_transfer() {
  UniqueTempDir temp_dir("hdrbridge-heif-icc-trc-tests");
  const auto heif_path = temp_dir.path / "gamma22-icc.heif";
  write_tmap_fixture(heif_path, {
      .map_depth = 8,
      .use_base_color_space = true,
      .base_icc = build_bt709_icc_with_optional_cicp(2.2, std::nullopt)});

  const auto info = hdrbridge::inspect(heif_path);
  require(info.icc_present && info.primaries == 1 && info.transfer == 0 &&
              info.base_transfer == "ICC RGB transfer curve" &&
              info.resolved_signaling_source == "ICC",
          "Unrecognized ICC SDR curve in HEIF must not fall back to transfer 13 / sRGB");

  hdrbridge::ConversionOptions recon_opts;
  recon_opts.mode = "png-pq16";
  require(hdrbridge::convert(heif_path, temp_dir.path / "gamma22-out.png", recon_opts)
              .verification.passed,
          "HEIF with unrecognized ICC SDR curve must still reconstruct using the ICC TRC");
}

void test_acr_rdf_seq_xmp_jpeg_without_iso_app2() {
  UniqueTempDir temp_dir("hdrbridge-acr-xmp-jpeg-tests");
  const auto source_heif = temp_dir.path / "source.heif";
  write_tmap_fixture(source_heif, {.base_primaries = AVIF_COLOR_PRIMARIES_SMPTE432});
  const auto uhdr_path = temp_dir.path / "uhdr-rgb.jpg";
  hdrbridge::ConversionOptions encode_opts;
  encode_opts.mode = "ultrahdr";
  encode_opts.multi_channel_gainmap = true;
  encode_opts.gainmap_scale = 1;
  require(hdrbridge::convert(source_heif, uhdr_path, encode_opts).verification.passed,
          "generate RGB Ultra HDR JPEG fixture");

  const auto original = read_binary_file(uhdr_path);
  size_t sec_soi = std::string::npos;
  for (size_t i = 2; i + 3 < original.size(); ++i) {
    if (original[i] == 0xffu && original[i + 1] == 0xd9u &&
        original[i + 2] == 0xffu && original[i + 3] == 0xd8u) {
      sec_soi = i + 2;
      break;
    }
  }
  require(sec_soi != std::string::npos, "locate secondary JPEG SOI");

  auto make_xmp_app1 = [](const std::string& version, const std::string& max_attr,
                          const std::string& max_elem) {
    constexpr char kXmpNs[] = "http://ns.adobe.com/xap/1.0/";
    const std::string xml =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description rdf:about=\"\" xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" "
        "hdrgm:Version=\"" + version + "\" " + max_attr +
        "hdrgm:BaseRenditionIsHDR=\"False\" hdrgm:HDRCapacityMin=\"0\" hdrgm:HDRCapacityMax=\"2.3\" "
        "hdrgm:OffsetSDR=\"0.015625\" hdrgm:OffsetHDR=\"0.015625\">"
        "<hdrgm:GainMapMin><rdf:Seq><rdf:li>-2.5</rdf:li><rdf:li>-0.7</rdf:li><rdf:li>-0.5</rdf:li></rdf:Seq></hdrgm:GainMapMin>" +
        max_elem +
        "<hdrgm:Gamma><rdf:Seq><rdf:li>1.3</rdf:li><rdf:li>0.7</rdf:li><rdf:li>0.65</rdf:li></rdf:Seq></hdrgm:Gamma>"
        "</rdf:Description></rdf:RDF></x:xmpmeta>";
    std::vector<uint8_t> payload(kXmpNs, kXmpNs + sizeof(kXmpNs));
    payload.insert(payload.end(), xml.begin(), xml.end());
    return payload;
  };
  auto rebuild_stream = [&](size_t begin, size_t end, bool strip_all_app1_app2,
                            const std::vector<uint8_t>& app1, const std::vector<uint8_t>& app2) {
    std::vector<uint8_t> out{0xffu, 0xd8u};
    if (!app1.empty()) append_jpeg_segment(out, 0xe1u, app1);
    if (!app2.empty()) append_jpeg_segment(out, 0xe2u, app2);
    for (size_t pos = begin + 2; pos < end;) {
      if (original[pos] != 0xffu) break;
      const uint8_t m = original[pos + 1];
      if (m == 0xdau) {
        out.insert(out.end(), original.begin() + static_cast<std::ptrdiff_t>(pos),
                   original.begin() + static_cast<std::ptrdiff_t>(end));
        break;
      }
      const size_t len = (static_cast<size_t>(original[pos + 2]) << 8) | original[pos + 3];
      const bool drop = strip_all_app1_app2
          ? (m == 0xe1u || m == 0xe2u)
          : (m == 0xe2u && len > 6u && std::memcmp(&original[pos + 4], "MPF\0", 4u) == 0);
      if (!drop) {
        out.insert(out.end(), original.begin() + static_cast<std::ptrdiff_t>(pos),
                   original.begin() + static_cast<std::ptrdiff_t>(pos + 2 + len));
      }
      pos += 2 + len;
    }
    return out;
  };

  const auto app1_v1 = make_xmp_app1(
      "1.0", "", "<hdrgm:GainMapMax><rdf:Seq><rdf:li>2.1</rdf:li><rdf:li>2.0</rdf:li><rdf:li>1.9</rdf:li></rdf:Seq></hdrgm:GainMapMax>");
  const auto prim_base = rebuild_stream(0, sec_soi, false, {}, {});
  const auto sec_v1 = rebuild_stream(sec_soi, original.size(), true, app1_v1, {});
  auto concat = [](std::initializer_list<const std::vector<uint8_t>*> parts) {
    std::vector<uint8_t> out;
    for (const auto* p : parts) out.insert(out.end(), p->begin(), p->end());
    return out;
  };

  const auto acr_path = temp_dir.path / "acr-no-iso.jpg";
  write_binary_file(acr_path, concat({&prim_base, &sec_v1}));
  const auto info = hdrbridge::inspect(acr_path);
  require(info.asset_kind == "gain-map-hdr" &&
              info.gain_map_family == "adobe-hdrgm-xmp" &&
              info.profile.find("Adobe HDR Gain Map XMP") != std::string::npos &&
              info.codec.find("Adobe HDR Gain Map XMP") != std::string::npos &&
              info.codec.find("ISO 21496-1") == std::string::npos &&
              info.gain_map_channels == 3u &&
              std::abs(info.gain_map_min[0] - (-2.5)) < 1e-4 &&
              std::abs(info.gain_map_max[2] - 1.9) < 1e-4 &&
              std::abs(info.gain_map_gamma[1] - 0.7) < 1e-4 &&
              std::abs(info.alternate_hdr_headroom - 2.3) < 1e-4,
          "Inspect must report Adobe XMP source metadata (not native ISO APP2) for ACR XMP-only JPEG");

  const auto sec_v2 = rebuild_stream(
      sec_soi, original.size(), true,
      make_xmp_app1("2.0", "", "<hdrgm:GainMapMax><rdf:Seq><rdf:li>2.1</rdf:li><rdf:li>2.0</rdf:li><rdf:li>1.9</rdf:li></rdf:Seq></hdrgm:GainMapMax>"),
      {});
  const auto v2_path = temp_dir.path / "acr-v2.jpg";
  write_binary_file(v2_path, concat({&prim_base, &sec_v2}));
  bool v2_rejected = false;
  try { (void)hdrbridge::inspect(v2_path); } catch (const std::exception& err) {
    v2_rejected = std::string(err.what()).find("unsupported Adobe hdrgm:Version") != std::string::npos;
  }
  require(v2_rejected, "Unsupported hdrgm:Version=\"2.0\" must be rejected cleanly");

  hdrbridge::ConversionOptions png_opts;
  png_opts.mode = "png-pq16";
  const auto full_list_res = hdrbridge::convert(acr_path, temp_dir.path / "acr-out.png", png_opts);
  require(full_list_res.verification.passed, "ACR <rdf:Seq> XMP JPEG reconstructs HDR PQ16");

  // Hybrid scalar GainMapMax vs single-element <rdf:Seq> GainMapMax with per-channel Gamma.
  const auto sec_seq = rebuild_stream(sec_soi, original.size(), true,
      make_xmp_app1("1.0", "", "<hdrgm:GainMapMax><rdf:Seq><rdf:li>2.0</rdf:li></rdf:Seq></hdrgm:GainMapMax>"), {});
  const auto sec_scalar = rebuild_stream(sec_soi, original.size(), true,
      make_xmp_app1("1.0", "hdrgm:GainMapMax=\"2.0\" ", ""), {});
  const auto hybrid_seq_path = temp_dir.path / "acr-hybrid-seq.jpg";
  const auto hybrid_scalar_path = temp_dir.path / "acr-hybrid-scalar.jpg";
  write_binary_file(hybrid_seq_path, concat({&prim_base, &sec_seq}));
  write_binary_file(hybrid_scalar_path, concat({&prim_base, &sec_scalar}));
  const auto hybrid_scalar_info = hdrbridge::inspect(hybrid_scalar_path);
  require(hybrid_scalar_info.gain_map_family == "adobe-hdrgm-xmp" &&
              std::abs(hybrid_scalar_info.gain_map_gamma[0] - 1.3) < 1e-4 &&
              std::abs(hybrid_scalar_info.gain_map_gamma[1] - 0.7) < 1e-4 &&
              std::abs(hybrid_scalar_info.gain_map_gamma[2] - 0.65) < 1e-4,
          "Hybrid scalar/list XMP must preserve per-channel Gamma");
  require(hdrbridge::convert(hybrid_seq_path, temp_dir.path / "hybrid-seq.png", png_opts).sha256 ==
              hdrbridge::convert(hybrid_scalar_path, temp_dir.path / "hybrid-scalar.png", png_opts).sha256,
          "Scalar vs single-element <rdf:Seq> GainMapMax outputs match byte-for-byte");

  // MPF [primary, plain_sec, real_sec], primary-only hdrgm + plain_sec, and unindexed no-MPF 3-stream.
  const auto plain_sec = rebuild_stream(sec_soi, original.size(), true, {}, {});
  const auto prim_hdrgm = rebuild_stream(0, sec_soi, false, app1_v1, {});
  constexpr size_t kMpfPayloadBytes = 4u + 8u + 2u + 3u * 12u + 4u + 3u * 16u;
  const uint32_t prim_mpf_size = static_cast<uint32_t>(prim_hdrgm.size() + 4u + kMpfPayloadBytes);
  const size_t tiff_file_off = 2u + 4u + app1_v1.size() + 8u;
  const uint32_t plain_off = static_cast<uint32_t>(prim_mpf_size - tiff_file_off);
  const uint32_t real_off = static_cast<uint32_t>(plain_off + plain_sec.size());
  std::vector<uint8_t> mpf(kMpfPayloadBytes, 0);
  std::memcpy(mpf.data(), "MPF\0MM", 6u);
  write_be16(mpf, 6, 42u); write_be32(mpf, 8, 8u); write_be16(mpf, 12, 3u);
  auto put_ifd = [&](size_t idx, uint16_t tag, uint16_t typ, uint32_t cnt, uint32_t val) {
    const size_t p = 14u + idx * 12u;
    write_be16(mpf, p, tag); write_be16(mpf, p + 2, typ);
    write_be32(mpf, p + 4, cnt); write_be32(mpf, p + 8, val);
  };
  put_ifd(0, 0xB000u, 7u, 4u, 0x30313030u);
  put_ifd(1, 0xB001u, 4u, 1u, 3u);
  put_ifd(2, 0xB002u, 7u, 48u, 50u);
  auto put_mp = [&](size_t idx, uint32_t attr, uint32_t sz, uint32_t off) {
    const size_t p = 54u + idx * 16u;
    write_be32(mpf, p, attr); write_be32(mpf, p + 4, sz); write_be32(mpf, p + 8, off);
  };
  put_mp(0, 0x20030000u, prim_mpf_size, 0u);
  put_mp(1, 0x00000000u, static_cast<uint32_t>(plain_sec.size()), plain_off);
  put_mp(2, 0x00050000u, static_cast<uint32_t>(sec_v1.size()), real_off);

  const auto prim_mpf = rebuild_stream(0, sec_soi, false, app1_v1, mpf);
  const auto three_img_path = temp_dir.path / "acr-mpf-unrelated-second.jpg";
  write_binary_file(three_img_path, concat({&prim_mpf, &plain_sec, &sec_v1}));
  const auto three_img_res = hdrbridge::convert(three_img_path, temp_dir.path / "three-img.png", png_opts);
  require(hdrbridge::inspect(three_img_path).gain_map_family == "adobe-hdrgm-xmp" &&
              three_img_res.verification.passed &&
              three_img_res.sha256 == full_list_res.sha256,
          "MPF with unrelated second JPEG selects real third gain map identically");

  const auto checker_only_path = temp_dir.path / "acr-checker-only.jpg";
  write_binary_file(checker_only_path, concat({&prim_hdrgm, &plain_sec}));
  require(hdrbridge::inspect(checker_only_path).asset_kind == "non-HDR",
          "Unrelated secondary JPEG with primary hdrgm XMP stays SDR");

  const auto no_mpf_path = temp_dir.path / "acr-no-mpf.jpg";
  write_binary_file(no_mpf_path, concat({&prim_hdrgm, &plain_sec, &sec_v1}));
  require(hdrbridge::inspect(no_mpf_path).asset_kind == "non-HDR",
          "Unindexed real gain map behind unrelated JPEG without MPF stays SDR");

  hdrbridge::ConversionOptions extract_opts;
  extract_opts.mode = "gainmap-extract";
  extract_opts.gainmap_export_format = "original";
  require(hdrbridge::convert(acr_path, temp_dir.path / "acr-gm.jpg", extract_opts)
              .verification.passed,
          "ACR <rdf:Seq> XMP JPEG must support gain-map extraction");
}

}  // namespace

int main() {
  try {
    test_adaptive_icc_reference_white();
    test_jpeg_app2_reassembly_and_isolation();
    test_iso_metadata_parser();
    test_synthetic_iso_tmap_heif();
    test_avif_gainmap_extract_mono_and_rgb();
    test_tiff_gainmap_extract_decoupling();
    test_jxl_gainmap_extract_decoupling();
    test_heif_base_icc_validation();
    test_heif_nclx_matrix_range_and_absent_signaling();
    test_apple_jpeg_ignores_base_comment_urn();
    test_apple_jpeg_missing_icc_keeps_unspecified_signaling();
    test_standard_ultrahdr_ignores_hagc_reference_white();
    test_apple_jpeg_xmp_custom_prefixes_and_hagc();
    test_portrait_jpeg_swaps_gainmap_dimensions_and_scale();
    test_heif_unrecognized_icc_curve_keeps_unspecified_transfer();
    test_acr_rdf_seq_xmp_jpeg_without_iso_app2();
    std::cout << "PASS gainmap-heif-hagc-tests\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
