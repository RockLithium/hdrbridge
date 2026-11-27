#include "hdrbridge_core.h"
#include "codecs/gainmap/apple_heif.h"
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
  base->yuvRange = AVIF_RANGE_FULL;

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
                !info.icc_present && info.primaries == 1 && info.gain_map_channels == 3 &&
                !info.gain_map_uses_base_color_space &&
                info.gain_map_width == test.fixture.map_width && info.gain_map_height == test.fixture.map_height,
            std::string(test.name) + ": ISO tmap Inspector mismatch");

    // Ignoring tmap primaries shifts the green highlight to about 107 nits.
    // An unrelated 300-nit ICC would instead raise the red highlight to about 1182 nits.
    const auto hdr = hdrbridge::gainmap::reconstruct_apple_heif_gainmap(path);
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
  require(hdrbridge::convert(dir / samples[0].name, dir / "hdr.png", options).verification.passed,
          "synthetic ISO tmap HEIF HDR export failed");

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
  require(info.asset_kind == "gain-map-hdr" && info.transfer == 8,
          "linear ICC with CICP transfer 8 must inspect as linear gain-map HDR");
  const auto without_tag = hdrbridge::gainmap::reconstruct_apple_heif_gainmap(linear);
  const auto with_tag = hdrbridge::gainmap::reconstruct_apple_heif_gainmap(cicp);
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
    std::cout << "PASS gainmap-heif-hagc-tests\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
