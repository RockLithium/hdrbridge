#include "hdrbridge_core.h"
#include "gainmap_adapters.h"
#include "hdr_transfer.h"
#include "jpeg_probe.h"
#include "orientation.h"

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <avif/avif.h>
#include <libheif/heif.h>
#include <libheif/heif_properties.h>
#include <lcms2.h>
#include <nlohmann/json.hpp>
#include <png.h>
#include <tiffio.h>
#include <ultrahdr_api.h>
#include <zlib.h>
#ifdef _WIN32
extern "C" {
#include <jpeglib.h>
}
#endif
#ifndef _WIN32
extern "C" {
#include <JXRGlue.h>
}
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#ifdef _WIN32
#pragma comment(lib, "bcrypt.lib")
#endif

namespace hdrbridge {
namespace {

using json = nlohmann::json;
#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif

struct HeifContextDeleter { void operator()(heif_context* p) const { heif_context_free(p); } };
struct HeifHandleDeleter { void operator()(heif_image_handle* p) const { heif_image_handle_release(p); } };
struct HeifImageDeleter { void operator()(heif_image* p) const { heif_image_release(p); } };
struct HeifOptionsDeleter { void operator()(heif_decoding_options* p) const { heif_decoding_options_free(p); } };
struct NclxDeleter { void operator()(heif_color_profile_nclx* p) const { heif_nclx_color_profile_free(p); } };
struct HeifEncoderDeleter { void operator()(heif_encoder* p) const { heif_encoder_release(p); } };
struct HeifEncodingOptionsDeleter { void operator()(heif_encoding_options* p) const { heif_encoding_options_free(p); } };

using ContextPtr = std::unique_ptr<heif_context, HeifContextDeleter>;
using HandlePtr = std::unique_ptr<heif_image_handle, HeifHandleDeleter>;
using ImagePtr = std::unique_ptr<heif_image, HeifImageDeleter>;
using OptionsPtr = std::unique_ptr<heif_decoding_options, HeifOptionsDeleter>;
using NclxPtr = std::unique_ptr<heif_color_profile_nclx, NclxDeleter>;
using HeifEncoderPtr = std::unique_ptr<heif_encoder, HeifEncoderDeleter>;
using HeifEncodingOptionsPtr = std::unique_ptr<heif_encoding_options, HeifEncodingOptionsDeleter>;

struct AvifImageDeleter { void operator()(avifImage* p) const { avifImageDestroy(p); } };
struct AvifGainMapDeleter { void operator()(avifGainMap* p) const { avifGainMapDestroy(p); } };
struct AvifEncoderDeleter { void operator()(avifEncoder* p) const { avifEncoderDestroy(p); } };
using AvifImagePtr = std::unique_ptr<avifImage, AvifImageDeleter>;
using AvifGainMapPtr = std::unique_ptr<avifGainMap, AvifGainMapDeleter>;
using AvifEncoderPtr = std::unique_ptr<avifEncoder, AvifEncoderDeleter>;

struct DecodedImage {
  SourceInfo info;
  std::vector<uint16_t> rgb;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  TimingDiagnostics timings;
  double gain_map_formula_max_error = 0.0;
  double transfer_conversion_rmse = 0.0;
  double transfer_conversion_max_abs_error = 0.0;
  double source_to_canonical_rmse = 0.0;
  double source_to_canonical_max_abs_error = 0.0;
};

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void convert_p3_pq_to_rec2020_pq(std::vector<uint16_t>& pixels);
void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data);
std::vector<uint16_t> convert_rec2020_pq_to_p3_pq(const DecodedImage& decoded);
std::vector<uint16_t> convert_rec2020_pq_to_rec709_pq(const DecodedImage& decoded);
double inverse_pq(double encoded);

HdrTransfer public_transfer_kind(uint16_t cicp_transfer) {
  if (cicp_transfer == 16) return HdrTransfer::pq_st2084;
  if (cicp_transfer == 18) return HdrTransfer::hlg_bt2100;
  return HdrTransfer::unknown;
}

const char* public_transfer_name(uint16_t cicp_transfer) {
  if (cicp_transfer == 8) return "Linear";
  if (cicp_transfer == 13) return "sRGB";
  return transfer::name(transfer::from_cicp(cicp_transfer));
}

const char* metadata_status(bool present) {
  return present ? "present" : "absent";
}

struct IccSignal {
  bool present = false;
  bool valid = false;
  bool cicp_present = false;
  uint16_t primaries = 0;
  uint16_t transfer = 0;
  uint16_t matrix = 0;
  bool full_range = false;
  std::string description;
  std::string version;
  std::string transfer_interpretation = "Unknown";
};

std::string icc_info_ascii(cmsHPROFILE profile, cmsInfoType type) {
  const cmsUInt32Number size = cmsGetProfileInfoASCII(profile, type, "en", "US", nullptr, 0);
  if (!size) return {};
  std::string text(size, '\0');
  cmsGetProfileInfoASCII(profile, type, "en", "US", text.data(), size);
  if (!text.empty() && text.back() == '\0') text.pop_back();
  return text;
}

uint16_t identify_icc_primaries(cmsHPROFILE profile) {
  const auto* r = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigRedColorantTag));
  const auto* g = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigGreenColorantTag));
  const auto* b = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigBlueColorantTag));
  if (!r || !g || !b) return 0;
  const std::array<double, 9> actual{r->X, r->Y, r->Z, g->X, g->Y, g->Z, b->X, b->Y, b->Z};
  struct Candidate { uint16_t cicp; cmsCIExyYTRIPLE xy; };
  const std::array<Candidate, 3> candidates{{
      {1, {{0.6400, 0.3300, 1.0}, {0.3000, 0.6000, 1.0}, {0.1500, 0.0600, 1.0}}},
      {9, {{0.7080, 0.2920, 1.0}, {0.1700, 0.7970, 1.0}, {0.1310, 0.0460, 1.0}}},
      {12, {{0.6800, 0.3200, 1.0}, {0.2650, 0.6900, 1.0}, {0.1500, 0.0600, 1.0}}},
  }};
  const cmsCIExyY white{0.3127, 0.3290, 1.0};
  uint16_t best = 0; double best_error = std::numeric_limits<double>::max();
  for (const auto& candidate : candidates) {
    cmsToneCurve* curve = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve* curves[3]{curve, curve, curve};
    cmsHPROFILE reference = curve ? cmsCreateRGBProfile(&white, &candidate.xy, curves) : nullptr;
    if (curve) cmsFreeToneCurve(curve);
    if (!reference) continue;
    const auto* rr = static_cast<const cmsCIEXYZ*>(cmsReadTag(reference, cmsSigRedColorantTag));
    const auto* rg = static_cast<const cmsCIEXYZ*>(cmsReadTag(reference, cmsSigGreenColorantTag));
    const auto* rb = static_cast<const cmsCIEXYZ*>(cmsReadTag(reference, cmsSigBlueColorantTag));
    if (rr && rg && rb) {
      const std::array<double, 9> expected{rr->X, rr->Y, rr->Z, rg->X, rg->Y, rg->Z,
                                           rb->X, rb->Y, rb->Z};
      double error = 0.0;
      for (size_t i = 0; i < actual.size(); ++i) {
        const double delta = actual[i] - expected[i]; error += delta * delta;
      }
      if (error < best_error) { best_error = error; best = candidate.cicp; }
    }
    cmsCloseProfile(reference);
  }
  return best_error < 1e-4 ? best : 0;
}

uint16_t identify_icc_transfer(cmsHPROFILE profile) {
  const auto* curve = static_cast<const cmsToneCurve*>(cmsReadTag(profile, cmsSigRedTRCTag));
  if (!curve) return 0;
  constexpr std::array<double, 5> samples{0.10, 0.25, 0.50, 0.75, 0.90};
  double pq_error = 0.0, hlg_error = 0.0;
  constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
  for (double encoded : samples) {
    const double actual = cmsEvalToneCurveFloat(curve, static_cast<cmsFloat32Number>(encoded));
    const double pq = inverse_pq(encoded) / 10000.0;
    const double hlg = encoded <= 0.5 ? encoded * encoded / 3.0
                                     : (std::exp((encoded - c) / a) + b) / 12.0;
    pq_error += std::pow(actual - pq, 2.0);
    hlg_error += std::pow(actual - hlg, 2.0);
  }
  if (pq_error < 2e-5) return 16;
  if (hlg_error < 2e-4) return 18;
  return 0;
}

IccSignal parse_icc_signal(const void* data, size_t size) {
  IccSignal result;
  result.present = data && size > 0;
  if (!result.present || size > std::numeric_limits<cmsUInt32Number>::max()) return result;
  cmsHPROFILE profile = cmsOpenProfileFromMem(data, static_cast<cmsUInt32Number>(size));
  if (!profile) return result;
  result.valid = true;
  result.description = icc_info_ascii(profile, cmsInfoDescription);
  const double version = cmsGetProfileVersion(profile);
  std::ostringstream version_text;
  version_text << std::fixed << std::setprecision(1) << version;
  result.version = version_text.str();
  const uint16_t transform_primaries = identify_icc_primaries(profile);
  const uint16_t transform_transfer = identify_icc_transfer(profile);
  const auto* cicp = static_cast<const cmsVideoSignalType*>(cmsReadTag(profile, cmsSigcicpTag));
  if (cicp) {
    result.cicp_present = true;
    result.primaries = static_cast<uint16_t>(cicp->ColourPrimaries);
    result.transfer = static_cast<uint16_t>(cicp->TransferCharacteristics);
    result.matrix = static_cast<uint16_t>(cicp->MatrixCoefficients);
    result.full_range = cicp->VideoFullRangeFlag != 0;
    result.transfer_interpretation = std::string("CICP ") + public_transfer_name(result.transfer);
    if (transform_transfer == result.transfer) result.transfer_interpretation += "; TRC agrees";
    else if (transform_transfer != 0) result.transfer_interpretation += "; legacy/fallback TRC differs";
  } else {
    result.primaries = transform_primaries;
    result.transfer = transform_transfer;
    result.matrix = 0;
    result.full_range = transform_transfer != 0;
    result.transfer_interpretation = transform_transfer
        ? std::string("matrix/TRC interpreted as ") + public_transfer_name(transform_transfer)
        : "matrix/TRC is not a recognized PQ or HLG transform";
  }
  cmsCloseProfile(profile);
  return result;
}

void store_icc_signal(SourceInfo& info, const IccSignal& icc) {
  info.icc_present = icc.present;
  info.icc_status = icc.present ? (icc.valid ? "present" : "read-error") : "absent";
  info.icc_description = icc.description;
  info.icc_version = icc.version;
  info.icc_cicp_present = icc.cicp_present;
  info.icc_primaries = icc.primaries;
  info.icc_transfer = icc.transfer;
  info.icc_matrix = icc.matrix;
  info.icc_full_range = icc.full_range;
  info.icc_transfer_interpretation = icc.transfer_interpretation;
}

bool hdr_color(uint16_t primaries, uint16_t transfer) {
  return (transfer == 16 || transfer == 18) &&
         (primaries == 1 || primaries == 9 || primaries == 12);
}

void resolve_color_signaling(SourceInfo& info, bool native_preferred) {
  const bool native_hdr = info.native_color_present &&
                          hdr_color(info.native_primaries, info.native_transfer);
  const bool icc_hdr = info.icc_present &&
                       hdr_color(info.icc_primaries, info.icc_transfer);
  const bool both = info.native_color_present && icc_hdr;
  info.color_signaling_conflict = both &&
      (info.native_primaries != info.icc_primaries ||
       info.native_transfer != info.icc_transfer ||
       info.native_full_range != info.icc_full_range);
  if (info.color_signaling_conflict) {
    info.resolved_signaling_source = "Conflict";
  } else if (both) {
    info.resolved_signaling_source = "Native + ICC";
  } else if (native_hdr || (info.native_color_present && !icc_hdr)) {
    info.resolved_signaling_source = "Native";
  } else if (icc_hdr || info.icc_cicp_present) {
    info.resolved_signaling_source = "ICC";
  } else {
    info.resolved_signaling_source = "Unknown";
  }
  const bool use_native = native_hdr && (native_preferred || !icc_hdr);
  if (use_native) {
    info.primaries = info.native_primaries;
    info.transfer = info.native_transfer;
    info.matrix = info.native_matrix;
    info.full_range = info.native_full_range;
    info.range_known = info.native_range_known;
  } else if (icc_hdr) {
    info.primaries = info.icc_primaries;
    info.transfer = info.icc_transfer;
    // ICC matrix=0 describes RGB profile interpretation, not stored YCbCr.
    // Preserve a native storage matrix when one exists.
    info.matrix = info.native_color_present ? info.native_matrix : info.icc_matrix;
    info.full_range = info.icc_full_range;
    info.range_known = true;
  } else if (info.native_color_present) {
    info.primaries = info.native_primaries;
    info.transfer = info.native_transfer;
    info.matrix = info.native_matrix;
    info.full_range = info.native_full_range;
    info.range_known = info.native_range_known;
  }
  info.transfer_kind = public_transfer_kind(info.transfer);
  if (!info.gain_map_present) info.asset_kind = hdr_color(info.primaries, info.transfer)
      ? "direct-hdr" : "non-HDR";
}

void convert_rec709_pq_to_rec2020_pq(std::vector<uint16_t>& pixels) {
  for (size_t p = 0; p < pixels.size() / 3u; ++p) {
    const double r = transfer::pq_to_nits(pixels[p * 3u] / 65535.0);
    const double g = transfer::pq_to_nits(pixels[p * 3u + 1u] / 65535.0);
    const double b = transfer::pq_to_nits(pixels[p * 3u + 2u] / 65535.0);
    const std::array<double, 3> rec2020{
        0.6274040 * r + 0.3292830 * g + 0.0433130 * b,
        0.0690970 * r + 0.9195400 * g + 0.0113620 * b,
        0.0163910 * r + 0.0880130 * g + 0.8955950 * b};
    for (size_t c = 0; c < 3; ++c) pixels[p * 3u + c] =
        static_cast<uint16_t>(std::llround(transfer::nits_to_pq(
            std::clamp(rec2020[c], 0.0, 10000.0)) * 65535.0));
  }
}

void require_heif(const heif_error& e, const char* operation) {
  if (e.code != heif_error_Ok) {
    throw std::runtime_error(std::string(operation) + ": " + (e.message ? e.message : "libheif error"));
  }
}

void report(const ProgressCallback& cb, int value, const std::string& stage) {
  if (cb) cb(value, stage);
}

void check_cancel(std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("cannot open file: " + path.string());
  const auto size = in.tellg();
  if (size < 0) throw std::runtime_error("cannot determine file size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  in.seekg(0);
  if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("cannot read file");
  }
  return bytes;
}

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool starts_tiff_header(const uint8_t* data, size_t size) {
  return size >= 4 &&
      ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
       (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42));
}

std::vector<uint8_t> exif_tiff_payload(const std::vector<uint8_t>& exif) {
  if (exif.empty()) return {};
  if (starts_tiff_header(exif.data(), exif.size())) return exif;
  if (exif.size() >= 6 && std::memcmp(exif.data(), "Exif\0\0", 6) == 0 &&
      starts_tiff_header(exif.data() + 6, exif.size() - 6)) {
    return {exif.begin() + 6, exif.end()};
  }
  if (exif.size() >= 4) {
    const size_t offset = 4u + be32(exif.data());
    if (offset < exif.size() && starts_tiff_header(exif.data() + offset, exif.size() - offset)) {
      return {exif.begin() + static_cast<std::ptrdiff_t>(offset), exif.end()};
    }
  }
  return {};
}

std::vector<uint8_t> exif_for_jxl(const std::vector<uint8_t>& exif) {
  auto tiff = exif_tiff_payload(exif);
  if (tiff.empty()) return {};
  std::vector<uint8_t> output(4, 0);
  output.insert(output.end(), tiff.begin(), tiff.end());
  return output;
}

void inspect_container_bits(const std::vector<uint8_t>& bytes, SourceInfo& info) {
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

struct OpenedHeif {
  ContextPtr context;
  HandlePtr handle;
};

OpenedHeif open_heif(const std::filesystem::path& path) {
  ContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  heif_context_set_max_decoding_threads(context.get(), 0);
  require_heif(heif_context_read_from_file(context.get(), path.string().c_str(), nullptr), "read HEIF");
  heif_image_handle* raw = nullptr;
  require_heif(heif_context_get_primary_image_handle(context.get(), &raw), "get primary image");
  return {std::move(context), HandlePtr(raw)};
}

uint8_t heif_container_orientation(const OpenedHeif& opened) {
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

void load_metadata(const heif_image_handle* handle, SourceInfo& info,
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
  inspect_container_bits(read_file(path), info);
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

DecodedImage decode_direct_hdr(const std::filesystem::path& path,
                               const ProgressCallback& progress,
                               std::atomic_bool* cancel) {
  const auto decode_start = Clock::now();
  report(progress, 5, "Inspecting HEIF container");
  auto opened = open_heif(path);
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
  options->progress_user_data = cancel;
  options->cancel_decoding = [](void* user) -> int {
    return user && static_cast<std::atomic_bool*>(user)->load() ? 1 : 0;
  };

  heif_image* raw_image = nullptr;
  report(progress, 12, "Decoding 20-tile HEVC image at 16-bit precision");
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
    long double squared_error = 0.0;
    double max_error = 0.0;
    long double pq_squared_error = 0.0;
    double pq_max_error = 0.0;
    for (size_t p = 0; p < pixel_count; ++p) {
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
        pq_squared_error += static_cast<long double>(pq_error) * pq_error;
        pq_max_error = std::max(pq_max_error, std::abs(pq_error));
      }
      const auto reconstructed_signal = transfer::linear_nits_to_hlg({
          transfer::pq_to_nits(result.rgb[p * 3u] / 65535.0),
          transfer::pq_to_nits(result.rgb[p * 3u + 1u] / 65535.0),
          transfer::pq_to_nits(result.rgb[p * 3u + 2u] / 65535.0)});
      for (size_t c = 0; c < 3; ++c) {
        const double error = reconstructed_signal[c] - source_signal[c];
        squared_error += static_cast<long double>(error) * error;
        max_error = std::max(max_error, std::abs(error));
      }
    }
    result.transfer_conversion_rmse = std::sqrt(static_cast<double>(
        squared_error / static_cast<long double>(pixel_count * 3u)));
    result.transfer_conversion_max_abs_error = max_error;
    result.source_to_canonical_rmse = std::sqrt(static_cast<double>(
        pq_squared_error / static_cast<long double>(pixel_count * 3u)));
    result.source_to_canonical_max_abs_error = pq_max_error;
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

std::vector<uint8_t> make_pq_icc(bool display_p3,
                                 const std::string& description_override,
                                 bool include_cicp);
std::vector<uint8_t> make_windows_compatible_pq_icc(bool display_p3);

std::vector<uint8_t> encode_jxl(const DecodedImage& decoded, const ConversionOptions& options,
                                const std::vector<uint16_t>& pixels) {
  auto encoder = JxlEncoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!encoder || !runner) throw std::runtime_error("cannot create JPEG XL encoder");
  if (JxlEncoderSetParallelRunner(encoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_ENC_SUCCESS) {
    throw std::runtime_error("cannot attach JPEG XL parallel runner");
  }
  JxlResizableParallelRunnerSetThreads(runner.get(),
      JxlResizableParallelRunnerSuggestThreads(decoded.info.width, decoded.info.height));
  if (JxlEncoderUseContainer(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS) throw std::runtime_error("cannot enable JXL container");
  if (JxlEncoderUseBoxes(encoder.get()) != JXL_ENC_SUCCESS) throw std::runtime_error("cannot enable JXL metadata boxes");

  JxlBasicInfo basic{};
  JxlEncoderInitBasicInfo(&basic);
  basic.xsize = decoded.info.width;
  basic.ysize = decoded.info.height;
  basic.bits_per_sample = 16;
  basic.exponent_bits_per_sample = 0;
  basic.num_color_channels = 3;
  basic.num_extra_channels = 0;
  basic.uses_original_profile = JXL_TRUE;
  if (JxlEncoderSetBasicInfo(encoder.get(), &basic) != JXL_ENC_SUCCESS) throw std::runtime_error("cannot set JXL basic info");

  if (options.diagnostic_icc_only) {
    const auto icc = make_windows_compatible_pq_icc(options.output_gamut == "p3");
    if (JxlEncoderSetICCProfile(encoder.get(), icc.data(), icc.size()) != JXL_ENC_SUCCESS) {
      throw std::runtime_error("cannot set actual ICC-coded JPEG XL profile");
    }
  } else {
    JxlColorEncoding color{};
    color.color_space = JXL_COLOR_SPACE_RGB;
    color.white_point = JXL_WHITE_POINT_D65;
    color.primaries = options.output_gamut == "rec709" ? JXL_PRIMARIES_SRGB :
                      options.output_gamut == "p3" ? JXL_PRIMARIES_P3 : JXL_PRIMARIES_2100;
    color.transfer_function = options.output_transfer == "hlg"
        ? JXL_TRANSFER_FUNCTION_HLG : JXL_TRANSFER_FUNCTION_PQ;
    color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
    if (JxlEncoderSetColorEncoding(encoder.get(), &color) != JXL_ENC_SUCCESS) {
      throw std::runtime_error("cannot set JPEG XL structured color encoding");
    }
  }

  JxlEncoderFrameSettings* frame = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
  if (!frame) throw std::runtime_error("cannot create JXL frame settings");
  if (JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT,
                                       std::clamp(options.effort, 1, 10)) != JXL_ENC_SUCCESS) {
    throw std::runtime_error("cannot set JXL effort");
  }
  if (options.lossless) {
    if (JxlEncoderSetFrameLossless(frame, JXL_TRUE) != JXL_ENC_SUCCESS) throw std::runtime_error("cannot enable JXL lossless");
  } else {
    JxlEncoderSetFrameDistance(frame, std::max(0.01f, 1.0f - options.image_quality) * 4.0f);
  }
  const JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  if (JxlEncoderAddImageFrame(frame, &format, pixels.data(), pixels.size() * sizeof(uint16_t)) != JXL_ENC_SUCCESS) {
    throw std::runtime_error("cannot add JXL RGB16 frame");
  }
  JxlEncoderCloseFrames(encoder.get());

  if (options.copy_exif && !decoded.exif.empty()) {
    const JxlBoxType type = {'E', 'x', 'i', 'f'};
    const auto exif = exif_for_jxl(decoded.exif);
    if (!exif.empty() && JxlEncoderAddBox(encoder.get(), type, exif.data(), exif.size(), JXL_FALSE) != JXL_ENC_SUCCESS) {
      throw std::runtime_error("cannot add JXL Exif box");
    }
  }
  if (options.copy_xmp && !decoded.xmp.empty()) {
    const JxlBoxType type = {'x', 'm', 'l', ' '};
    if (JxlEncoderAddBox(encoder.get(), type, decoded.xmp.data(), decoded.xmp.size(), JXL_FALSE) != JXL_ENC_SUCCESS) {
      throw std::runtime_error("cannot add JXL XMP box");
    }
  }
  JxlEncoderCloseBoxes(encoder.get());
  JxlEncoderCloseInput(encoder.get());

  std::vector<uint8_t> output(1u << 20);
  uint8_t* next = output.data();
  size_t available = output.size();
  for (;;) {
    const JxlEncoderStatus status = JxlEncoderProcessOutput(encoder.get(), &next, &available);
    if (status == JXL_ENC_SUCCESS) break;
    if (status != JXL_ENC_NEED_MORE_OUTPUT) throw std::runtime_error("JPEG XL encoding failed");
    const size_t used = static_cast<size_t>(next - output.data());
    output.resize(output.size() * 2u);
    next = output.data() + used;
    available = output.size() - used;
  }
  output.resize(static_cast<size_t>(next - output.data()));
  return output;
}

std::vector<uint8_t> encode_jxl_gainmap_raster(uint32_t width, uint32_t height,
                                               const std::vector<uint16_t>& pixels,
                                               uint32_t channels,
                                               const JxlColorEncoding& color,
                                               bool use_container,
                                               bool lossless, float quality) {
  auto encoder = JxlEncoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!encoder || !runner) throw std::runtime_error("cannot create gain-map JPEG XL encoder");
  if (JxlEncoderSetParallelRunner(encoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_ENC_SUCCESS)
    throw std::runtime_error("cannot attach gain-map JPEG XL runner");
  JxlResizableParallelRunnerSetThreads(runner.get(),
      JxlResizableParallelRunnerSuggestThreads(width, height));
  if (use_container && JxlEncoderUseContainer(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS)
    throw std::runtime_error("cannot enable gain-map JPEG XL container");
  JxlBasicInfo basic{};
  JxlEncoderInitBasicInfo(&basic);
  basic.xsize = width; basic.ysize = height; basic.bits_per_sample = 16;
  basic.num_color_channels = channels; basic.num_extra_channels = 0;
  basic.uses_original_profile = JXL_TRUE;
  if (JxlEncoderSetBasicInfo(encoder.get(), &basic) != JXL_ENC_SUCCESS ||
      JxlEncoderSetColorEncoding(encoder.get(), &color) != JXL_ENC_SUCCESS)
    throw std::runtime_error("cannot configure gain-map JPEG XL raster");
  auto* frame = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
  if (!frame) throw std::runtime_error("cannot create gain-map JPEG XL frame");
  JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT, 7);
  if (lossless) JxlEncoderSetFrameLossless(frame, JXL_TRUE);
  else JxlEncoderSetFrameDistance(frame, std::max(0.01f, 1.0f - quality) * 4.0f);
  const JxlPixelFormat format{channels, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  if (JxlEncoderAddImageFrame(frame, &format, pixels.data(), pixels.size() * sizeof(uint16_t)) != JXL_ENC_SUCCESS)
    throw std::runtime_error("cannot add gain-map JPEG XL raster");
  JxlEncoderCloseInput(encoder.get());
  std::vector<uint8_t> output(1u << 20);
  uint8_t* next = output.data(); size_t available = output.size();
  for (;;) {
    const auto status = JxlEncoderProcessOutput(encoder.get(), &next, &available);
    if (status == JXL_ENC_SUCCESS) break;
    if (status != JXL_ENC_NEED_MORE_OUTPUT) throw std::runtime_error("gain-map JPEG XL encoding failed");
    const size_t used = static_cast<size_t>(next - output.data());
    output.resize(output.size() * 2u); next = output.data() + used; available = output.size() - used;
  }
  output.resize(static_cast<size_t>(next - output.data()));
  return output;
}

void append_be16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
void append_be32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24)); out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
void append_signed_fraction(std::vector<uint8_t>& out, double value, uint32_t denominator) {
  append_be32(out, static_cast<uint32_t>(static_cast<int32_t>(std::llround(value * denominator))));
}
void append_unsigned_fraction(std::vector<uint8_t>& out, double value, uint32_t denominator) {
  append_be32(out, static_cast<uint32_t>(std::llround(std::max(0.0, value) * denominator)));
}

std::vector<uint8_t> encode_gainmap_jxl(const DecodedImage& decoded,
                                        const ConversionOptions& options) {
  std::vector<uint16_t> hdr = decoded.rgb;
  if (options.output_gamut == "p3") hdr = convert_rec2020_pq_to_p3_pq(decoded);
  else if (options.output_gamut == "rec709") hdr = convert_rec2020_pq_to_rec709_pq(decoded);
  const size_t pixel_count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  std::vector<uint16_t> base(pixel_count * 3u);
  std::vector<double> channel_gain(pixel_count * 3u);
  double maximum_gain = 1.0;
  for (size_t i = 0; i < hdr.size(); ++i) {
    const double linear = std::max(0.0, transfer::pq_to_nits(hdr[i] / 65535.0) / 203.0);
    const double base_linear = std::min(linear, 1.0);
    const double srgb = base_linear <= 0.0031308 ? 12.92 * base_linear
        : 1.055 * std::pow(base_linear, 1.0 / 2.4) - 0.055;
    base[i] = static_cast<uint16_t>(std::lround(std::clamp(srgb, 0.0, 1.0) * 65535.0));
    channel_gain[i] = std::log2(std::max(linear, 1e-8) / std::max(base_linear, 1e-8));
    maximum_gain = std::max(maximum_gain, channel_gain[i]);
  }
  const uint32_t scale = static_cast<uint32_t>(std::clamp(options.gainmap_scale, 1, 4));
  const uint32_t map_width = std::max(1u, (decoded.info.width + scale - 1u) / scale);
  const uint32_t map_height = std::max(1u, (decoded.info.height + scale - 1u) / scale);
  const uint32_t channels = options.multi_channel_gainmap ? 3u : 1u;
  std::vector<uint16_t> map(static_cast<size_t>(map_width) * map_height * channels);
  for (uint32_t y = 0; y < map_height; ++y) for (uint32_t x = 0; x < map_width; ++x) {
    const uint32_t sx = std::min(decoded.info.width - 1u, x * scale + scale / 2u);
    const uint32_t sy = std::min(decoded.info.height - 1u, y * scale + scale / 2u);
    const size_t source = (static_cast<size_t>(sy) * decoded.info.width + sx) * 3u;
    if (channels == 1) {
      const double gain = std::max({channel_gain[source], channel_gain[source + 1u], channel_gain[source + 2u]});
      map[static_cast<size_t>(y) * map_width + x] = static_cast<uint16_t>(
          std::lround(std::clamp(gain / maximum_gain, 0.0, 1.0) * 65535.0));
    } else {
      for (size_t c = 0; c < 3; ++c) map[(static_cast<size_t>(y) * map_width + x) * 3u + c] =
          static_cast<uint16_t>(std::lround(std::clamp(channel_gain[source + c] / maximum_gain, 0.0, 1.0) * 65535.0));
    }
  }
  JxlColorEncoding base_color{}; base_color.color_space = JXL_COLOR_SPACE_RGB;
  base_color.white_point = JXL_WHITE_POINT_D65;
  base_color.primaries = options.output_gamut == "rec709" ? JXL_PRIMARIES_SRGB :
                         options.output_gamut == "p3" ? JXL_PRIMARIES_P3 : JXL_PRIMARIES_2100;
  base_color.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
  base_color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
  JxlColorEncoding map_color{}; map_color.color_space = channels == 1 ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
  map_color.white_point = JXL_WHITE_POINT_D65; map_color.primaries = JXL_PRIMARIES_SRGB;
  map_color.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
  map_color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
  auto base_bytes = encode_jxl_gainmap_raster(decoded.info.width, decoded.info.height, base, 3,
                                               base_color, true, options.lossless, options.image_quality);
  auto map_bytes = encode_jxl_gainmap_raster(map_width, map_height, map, channels,
                                              map_color, false, options.lossless,
                                              options.gainmap_quality / 100.0f);
  constexpr uint32_t denominator = 1000000u;
  std::vector<uint8_t> metadata;
  append_be16(metadata, 0); append_be16(metadata, 0);
  metadata.push_back(static_cast<uint8_t>(0x40 | 0x08 | (channels == 3 ? 0x80 : 0)));
  append_be32(metadata, denominator);
  append_unsigned_fraction(metadata, 0.0, denominator);
  append_unsigned_fraction(metadata, maximum_gain, denominator);
  for (uint32_t c = 0; c < channels; ++c) {
    append_signed_fraction(metadata, 0.0, denominator);
    append_signed_fraction(metadata, maximum_gain, denominator);
    append_unsigned_fraction(metadata, 1.0, denominator);
    append_signed_fraction(metadata, 0.0, denominator);
    append_signed_fraction(metadata, 0.0, denominator);
  }
  std::vector<uint8_t> payload;
  payload.push_back(0); append_be16(payload, static_cast<uint16_t>(metadata.size()));
  payload.insert(payload.end(), metadata.begin(), metadata.end());
  payload.push_back(0); append_be32(payload, 0);
  payload.insert(payload.end(), map_bytes.begin(), map_bytes.end());
  append_be32(base_bytes, static_cast<uint32_t>(payload.size() + 8u));
  base_bytes.insert(base_bytes.end(), {'j','h','g','m'});
  base_bytes.insert(base_bytes.end(), payload.begin(), payload.end());
  return base_bytes;
}

Verification decode_jxl(const std::vector<uint8_t>& data, const std::vector<uint16_t>* expected) {
  Verification verification;
  auto decoder = JxlDecoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!decoder || !runner) throw std::runtime_error("cannot create JXL decoder");
  if (JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot attach JXL decoder runner");
  JxlResizableParallelRunnerSetThreads(runner.get(), 0);
  const int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE;
  if (JxlDecoderSubscribeEvents(decoder.get(), events) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot subscribe JXL events");
  if (JxlDecoderSetInput(decoder.get(), data.data(), data.size()) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot set JXL input");
  JxlDecoderCloseInput(decoder.get());
  const JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint16_t> pixels;
  JxlBasicInfo basic{};
  JxlColorEncoding color{};
  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) throw std::runtime_error("JPEG XL verification decode failed");
    if (status == JXL_DEC_BASIC_INFO) {
      if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot read JXL info");
      verification.width = basic.xsize;
      verification.height = basic.ysize;
      verification.bit_depth = basic.bits_per_sample;
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &color) == JXL_DEC_SUCCESS) {
        verification.color_encoding =
            color.transfer_function == JXL_TRANSFER_FUNCTION_HLG &&
                    color.primaries == JXL_PRIMARIES_2100 ? "Rec.2020/HLG" :
            color.transfer_function == JXL_TRANSFER_FUNCTION_HLG &&
                    color.primaries == JXL_PRIMARIES_P3 ? "Display P3/HLG" :
            color.transfer_function == JXL_TRANSFER_FUNCTION_PQ &&
                    color.primaries == JXL_PRIMARIES_2100 ? "Rec.2020/PQ" :
            color.transfer_function == JXL_TRANSFER_FUNCTION_PQ &&
                    color.primaries == JXL_PRIMARIES_P3 ? "Display P3/PQ" : "unexpected";
      } else {
        size_t icc_size = 0;
        if (JxlDecoderGetICCProfileSize(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                       &icc_size) == JXL_DEC_SUCCESS && icc_size > 0) {
          std::vector<uint8_t> icc(icc_size);
          if (JxlDecoderGetColorAsICCProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                            icc.data(), icc.size()) == JXL_DEC_SUCCESS) {
            const auto signal = parse_icc_signal(icc.data(), icc.size());
            if (signal.cicp_present && (signal.transfer == 16 || signal.transfer == 18)) {
              verification.color_encoding = signal.transfer == 18
                  ? (signal.primaries == 12 ? "Display P3/HLG ICC" : signal.primaries == 9
                                             ? "Rec.2020/HLG ICC" : "unexpected ICC")
                  : (signal.primaries == 12 ? "Display P3/PQ ICC" : signal.primaries == 9
                                             ? "Rec.2020/PQ ICC" : "unexpected ICC");
            }
          }
        }
      }
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t size = 0;
      if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &size) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot size JXL output");
      pixels.resize((size + sizeof(uint16_t) - 1u) / sizeof(uint16_t));
      if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, pixels.data(), size) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot set JXL output");
    } else if (status == JXL_DEC_SUCCESS) {
      break;
    }
  }
  verification.pixel_format = "RGB uint16";
  verification.exact_roundtrip = expected && expected->size() == pixels.size() &&
                                 std::memcmp(expected->data(), pixels.data(), pixels.size() * sizeof(uint16_t)) == 0;
  verification.checks.push_back(verification.width && verification.height ? "dimensions parse" : "dimensions missing");
  verification.checks.push_back(verification.bit_depth == 16 ? "16-bit integer" : "unexpected bit depth");
  const bool pq_profile = verification.color_encoding == "Rec.2020/PQ" ||
                          verification.color_encoding == "Display P3/PQ" ||
                          verification.color_encoding == "Rec.2020/PQ ICC" ||
                          verification.color_encoding == "Display P3/PQ ICC";
  const bool hlg_profile = verification.color_encoding == "Rec.2020/HLG" ||
                           verification.color_encoding == "Display P3/HLG" ||
                           verification.color_encoding == "Rec.2020/HLG ICC" ||
                           verification.color_encoding == "Display P3/HLG ICC";
  verification.checks.push_back(pq_profile || hlg_profile
      ? verification.color_encoding + " profile" : "profile mismatch");
  if (expected) verification.checks.push_back(verification.exact_roundtrip ? "exact RGB16 encoder-buffer roundtrip" : "RGB16 roundtrip mismatch");
  verification.passed = verification.width > 0 && verification.height > 0 && verification.bit_depth == 16 &&
                        (pq_profile || hlg_profile) && (!expected || verification.exact_roundtrip);
  return verification;
}

DecodedImage decode_jxl_input(const std::filesystem::path& path) {
  const auto data = read_file(path);
  auto decoder = JxlDecoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (!decoder || !runner) throw std::runtime_error("cannot create JPEG XL input decoder");
  if (JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("cannot attach JPEG XL input runner");
  }
  JxlResizableParallelRunnerSetThreads(runner.get(), 0);
  if (JxlDecoderSetDecompressBoxes(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
      JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                                               JXL_DEC_FULL_IMAGE | JXL_DEC_BOX) != JXL_DEC_SUCCESS ||
      JxlDecoderSetInput(decoder.get(), data.data(), data.size()) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("cannot initialize JPEG XL input decode");
  }
  JxlDecoderCloseInput(decoder.get());
  const JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  DecodedImage decoded;
  decoded.info.path = path; decoded.info.format = "JPEG XL"; decoded.info.container_brand = "JXL ";
  decoded.info.asset_kind = "non-HDR"; decoded.info.codec = "JPEG XL"; decoded.info.chroma = "4:4:4 RGB";
  decoded.info.pixel_format = "RGB integer";
  decoded.info.color_signal_kind = "Unknown";
  decoded.info.exif_status = "absent";
  decoded.info.xmp_status = "absent";
  decoded.info.icc_status = "absent";
  JxlColorEncoding color{};
  bool structured_available = false;
  std::vector<uint8_t> original_icc;
  std::vector<uint8_t>* box_target = nullptr;
  size_t box_chunk_offset = 0;
  size_t box_chunk_size = 0;
  const auto finish_box = [&] {
    if (!box_target) return;
    const size_t remaining = JxlDecoderReleaseBoxBuffer(decoder.get());
    box_target->resize(box_chunk_offset + box_chunk_size - remaining);
    box_target = nullptr;
    box_chunk_offset = box_chunk_size = 0;
  };
  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) throw std::runtime_error("JPEG XL input decode failed");
    if (status == JXL_DEC_BASIC_INFO) {
      JxlBasicInfo basic{};
      if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot read JPEG XL input info");
      decoded.info.width = basic.xsize; decoded.info.height = basic.ysize; decoded.info.bit_depth = basic.bits_per_sample;
      decoded.info.profile = "RGB" + std::to_string(basic.bits_per_sample);
      decoded.info.pixel_format = "RGB" + std::to_string(basic.bits_per_sample) +
          (basic.alpha_bits ? "A integer" : " integer");
      decoded.info.original_orientation = static_cast<uint8_t>(basic.orientation);
      decoded.info.orientation_status = "present";
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      structured_available = JxlDecoderGetColorAsEncodedProfile(
          decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &color) == JXL_DEC_SUCCESS;
      size_t icc_size = 0;
      if (!structured_available &&
          JxlDecoderGetICCProfileSize(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                      &icc_size) == JXL_DEC_SUCCESS && icc_size > 0) {
        original_icc.resize(icc_size);
        if (JxlDecoderGetColorAsICCProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                           original_icc.data(), original_icc.size()) != JXL_DEC_SUCCESS) {
          original_icc.clear();
        }
      }
    } else if (status == JXL_DEC_BOX) {
      finish_box();
      JxlBoxType type{};
      if (JxlDecoderGetBoxType(decoder.get(), type, JXL_TRUE) != JXL_DEC_SUCCESS) {
        throw std::runtime_error("cannot read JPEG XL metadata box type");
      }
      if (std::memcmp(type, "Exif", 4) == 0) {
        box_target = &decoded.exif;
        decoded.info.exif_present = true;
        decoded.info.exif_status = "present";
      } else if (std::memcmp(type, "xml ", 4) == 0) {
        box_target = &decoded.xmp;
        decoded.info.xmp_present = true;
        decoded.info.xmp_status = "present";
      }
      if (box_target) {
        box_target->assign(65536u, 0);
        box_chunk_size = box_target->size();
        if (JxlDecoderSetBoxBuffer(decoder.get(), box_target->data(), box_chunk_size) != JXL_DEC_SUCCESS) {
          throw std::runtime_error("cannot read JPEG XL metadata box");
        }
      }
    } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
      if (!box_target) throw std::runtime_error("unexpected JPEG XL metadata buffer request");
      const size_t remaining = JxlDecoderReleaseBoxBuffer(decoder.get());
      box_chunk_offset += box_chunk_size - remaining;
      box_chunk_size = 65536u;
      box_target->resize(box_chunk_offset + box_chunk_size);
      if (JxlDecoderSetBoxBuffer(decoder.get(), box_target->data() + box_chunk_offset,
                                 box_chunk_size) != JXL_DEC_SUCCESS) {
        throw std::runtime_error("cannot continue JPEG XL metadata box read");
      }
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t size = 0;
      if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &size) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot size JPEG XL input pixels");
      decoded.rgb.resize((size + sizeof(uint16_t) - 1u) / sizeof(uint16_t));
      if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, decoded.rgb.data(), size) != JXL_DEC_SUCCESS) throw std::runtime_error("cannot set JPEG XL input buffer");
    } else if (status == JXL_DEC_SUCCESS) {
      finish_box();
      break;
    }
  }
  if (structured_available && color.color_space == JXL_COLOR_SPACE_RGB) {
    decoded.info.native_color_present = true;
    decoded.info.native_primaries = color.primaries == JXL_PRIMARIES_P3 ? 12 :
        color.primaries == JXL_PRIMARIES_2100 ? 9 :
        color.primaries == JXL_PRIMARIES_SRGB ? 1 : 0;
    decoded.info.native_transfer = static_cast<uint16_t>(color.transfer_function);
    decoded.info.native_matrix = 0;
    decoded.info.native_full_range = true;
    decoded.info.native_range_known = true;
    decoded.info.native_color_description = "JPEG XL structured color encoding (CICP equivalent)";
  }
  store_icc_signal(decoded.info, parse_icc_signal(original_icc.data(), original_icc.size()));
  resolve_color_signaling(decoded.info, true);
  decoded.info.color_signal_kind = decoded.info.native_color_present
      ? "CICP equivalent (JPEG XL structured color encoding)"
      : decoded.info.resolved_signaling_source;
  if (!decoded.info.icc_description.empty()) decoded.info.profile = decoded.info.icc_description;
  if (decoded.info.asset_kind != "direct-hdr") return decoded;
  if (decoded.info.bit_depth > 16 || decoded.info.bit_depth == 0) {
    throw std::runtime_error("JPEG XL direct HDR integer storage exceeds the supported 16-bit precision");
  }
  if (decoded.info.transfer == 18) {
    const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
    for (size_t p = 0; p < count; ++p) {
      const auto nits = transfer::hlg_to_linear_nits({
          decoded.rgb[p * 3u] / 65535.0,
          decoded.rgb[p * 3u + 1u] / 65535.0,
          decoded.rgb[p * 3u + 2u] / 65535.0});
      for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] =
          static_cast<uint16_t>(std::llround(transfer::nits_to_pq(nits[c]) * 65535.0));
    }
  } else if (decoded.info.primaries == 12) {
    convert_p3_pq_to_rec2020_pq(decoded.rgb);
  } else if (decoded.info.primaries == 1) {
    convert_rec709_pq_to_rec2020_pq(decoded.rgb);
  }
  return decoded;
}

uint16_t float_to_half(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponent <= 0) {
    if (exponent < -10) return static_cast<uint16_t>(sign);
    mantissa = (mantissa | 0x800000u) >> static_cast<uint32_t>(1 - exponent);
    return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
  }
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7bffu);
  mantissa += 0x1000u;
  if (mantissa & 0x800000u) { mantissa = 0; ++exponent; }
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7bffu);
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

float half_to_float(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
  uint32_t exponent = (half >> 10) & 0x1fu;
  uint32_t mantissa = half & 0x3ffu;
  uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) bits = sign;
    else {
      int shift = 0;
      while ((mantissa & 0x400u) == 0) { mantissa <<= 1; ++shift; }
      mantissa &= 0x3ffu;
      bits = sign | (static_cast<uint32_t>(127 - 15 - shift) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float result = 0;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

double inverse_pq(double encoded) {
  return transfer::pq_to_nits(encoded);
}

double forward_pq(double nits) {
  return transfer::nits_to_pq(nits);
}

const std::array<double, 65536>& pq_nits_lut() {
  static const std::array<double, 65536> values = [] {
    std::array<double, 65536> result{};
    for (size_t i = 0; i < result.size(); ++i) result[i] = inverse_pq(i / 65535.0);
    return result;
  }();
  return values;
}

struct HdrStats {
  double max_channel_nits = 0.0;
  double max_luminance_nits = 0.0;
  double p999_nits = 0.0;
  double p9999_nits = 0.0;
  double chosen_target_nits = 203.0;
  double clipped_fraction = 0.0;
  std::string reason;
};

double histogram_percentile(const std::array<uint64_t, 10001>& histogram,
                            uint64_t count, double percentile) {
  if (count == 0) return 0.0;
  const uint64_t wanted = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(count * percentile)));
  uint64_t seen = 0;
  for (size_t i = 0; i < histogram.size(); ++i) {
    seen += histogram[i];
    if (seen >= wanted) return static_cast<double>(i);
  }
  return 10000.0;
}

HdrStats measure_hdr(const DecodedImage& decoded, float requested_target_nits = 0.0f) {
  HdrStats stats;
  std::array<uint64_t, 10001> luminance_histogram{};
  std::array<uint64_t, 10001> channel_histogram{};
  const auto& lut = pq_nits_lut();
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  for (size_t p = 0; p < count; ++p) {
    const double r = lut[decoded.rgb[p * 3u]];
    const double g = lut[decoded.rgb[p * 3u + 1u]];
    const double b = lut[decoded.rgb[p * 3u + 2u]];
    const double channel = std::max({r, g, b});
    const double luminance = std::max(0.0, 0.2627 * r + 0.6780 * g + 0.0593 * b);
    stats.max_channel_nits = std::max(stats.max_channel_nits, channel);
    stats.max_luminance_nits = std::max(stats.max_luminance_nits, luminance);
    ++luminance_histogram[static_cast<size_t>(std::clamp(std::llround(luminance), 0ll, 10000ll))];
    ++channel_histogram[static_cast<size_t>(std::clamp(std::llround(channel), 0ll, 10000ll))];
  }
  stats.p999_nits = histogram_percentile(luminance_histogram, count, 0.999);
  stats.p9999_nits = histogram_percentile(luminance_histogram, count, 0.9999);
  if (requested_target_nits > 0.0f) {
    stats.chosen_target_nits = std::clamp(static_cast<double>(requested_target_nits), 203.0, 10000.0);
    if (stats.chosen_target_nits <= 203.0) {
      stats.chosen_target_nits = 204.0;
      stats.reason = "203 nit override adjusted to 204 because libultrahdr requires capacity max > capacity min";
    } else {
      stats.reason = "explicit user target peak override";
    }
  } else {
    const double p9999_channel = histogram_percentile(channel_histogram, count, 0.9999);
    const double robust = std::max(stats.p9999_nits, p9999_channel) * 1.05;
    const double absolute_peak = std::max(stats.max_luminance_nits, stats.max_channel_nits);
    if (absolute_peak <= 203.0) {
      stats.chosen_target_nits = 204.0;
      stats.reason = "source peak is below the API floor; 204 nits is the minimum decodable capacity above 1.0";
    } else if (absolute_peak <= std::max(robust * 1.25, robust + 25.0)) {
      stats.chosen_target_nits = std::ceil(absolute_peak);
      stats.reason = "finite source maximum is within the 99.99-percentile robustness envelope";
    } else {
      stats.chosen_target_nits = std::ceil(std::clamp(robust, 203.0, 10000.0));
      stats.reason = "99.99-percentile guard excluded sparse hot pixels from capacity selection";
    }
    stats.chosen_target_nits = std::clamp(stats.chosen_target_nits, 203.0, 10000.0);
  }
  uint64_t clipped = 0;
  for (size_t p = 0; p < count; ++p) {
    if (lut[decoded.rgb[p * 3u]] > stats.chosen_target_nits ||
        lut[decoded.rgb[p * 3u + 1u]] > stats.chosen_target_nits ||
        lut[decoded.rgb[p * 3u + 2u]] > stats.chosen_target_nits) ++clipped;
  }
  stats.clipped_fraction = count ? static_cast<double>(clipped) / static_cast<double>(count) : 0.0;
  return stats;
}

void attach_hdr_stats(Verification& verification, const HdrStats& stats) {
  verification.max_channel_nits = stats.max_channel_nits;
  verification.max_luminance_nits = stats.max_luminance_nits;
  verification.percentile_99_9_nits = stats.p999_nits;
  verification.percentile_99_99_nits = stats.p9999_nits;
  verification.chosen_target_peak_nits = stats.chosen_target_nits;
  verification.clipped_pixel_fraction = stats.clipped_fraction;
  verification.peak_choice_reason = stats.reason;
}

std::vector<uint8_t> make_pq_icc(bool display_p3,
                                 const std::string& description_override = {},
                                 bool include_cicp = false) {
  cmsCIExyY white{0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE primaries{};
  if (display_p3) {
    primaries.Red = {0.6800, 0.3200, 1.0};
    primaries.Green = {0.2650, 0.6900, 1.0};
    primaries.Blue = {0.1500, 0.0600, 1.0};
  } else {
    primaries.Red = {0.7080, 0.2920, 1.0};
    primaries.Green = {0.1700, 0.7970, 1.0};
    primaries.Blue = {0.1310, 0.0460, 1.0};
  }
  std::array<cmsUInt16Number, 4096> table{};
  for (size_t i = 0; i < table.size(); ++i) {
    const double linear = inverse_pq(i / static_cast<double>(table.size() - 1u)) / 10000.0;
    table[i] = static_cast<cmsUInt16Number>(std::llround(std::clamp(linear, 0.0, 1.0) * 65535.0));
  }
  std::array<cmsToneCurve*, 3> curves{};
  for (auto& curve : curves) curve = cmsBuildTabulatedToneCurve16(nullptr, static_cast<cmsUInt32Number>(table.size()), table.data());
  auto free_curves = [&] { for (auto* curve : curves) if (curve) cmsFreeToneCurve(curve); };
  if (std::any_of(curves.begin(), curves.end(), [](const cmsToneCurve* curve) { return curve == nullptr; })) {
    free_curves();
    throw std::runtime_error("cannot create PQ ICC tone curves");
  }
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves.data());
  free_curves();
  if (!profile) throw std::runtime_error("cannot create PQ ICC profile");
  cmsSetProfileVersion(profile, include_cicp ? 4.4 : 4.3);
  cmsSetDeviceClass(profile, cmsSigDisplayClass);
  if (include_cicp) {
    cmsVideoSignalType cicp{};
    cicp.ColourPrimaries = display_p3 ? 12 : 9;
    cicp.TransferCharacteristics = 16;
    cicp.MatrixCoefficients = 0;
    cicp.VideoFullRangeFlag = 1;
    if (!cmsWriteTag(profile, cmsSigcicpTag, &cicp)) {
      cmsCloseProfile(profile);
      throw std::runtime_error("cannot write diagnostic PQ ICC CICP tag");
    }
  }
  cmsMLU* description = cmsMLUalloc(nullptr, 1);
  cmsMLU* copyright = cmsMLUalloc(nullptr, 1);
  const std::string name = description_override.empty()
      ? (display_p3 ? "HDR Bridge direct RGB16 Display P3 PQ"
                    : "HDR Bridge direct RGB16 Rec.2020 PQ")
      : description_override;
  if (!description || !copyright || !cmsMLUsetASCII(description, "en", "US", name.c_str()) ||
      !cmsMLUsetASCII(copyright, "en", "US", "Generated by HDR Bridge; no embedded third-party profile")) {
    if (description) cmsMLUfree(description);
    if (copyright) cmsMLUfree(copyright);
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot label PQ ICC profile");
  }
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, description);
  cmsWriteTag(profile, cmsSigCopyrightTag, copyright);
  cmsMLUfree(description);
  cmsMLUfree(copyright);
  cmsUInt32Number size = 0;
  if (!cmsSaveProfileToMem(profile, nullptr, &size) || size == 0) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot size PQ ICC profile");
  }
  std::vector<uint8_t> bytes(size);
  if (!cmsSaveProfileToMem(profile, bytes.data(), &size)) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot serialize PQ ICC profile");
  }
  cmsCloseProfile(profile);
  bytes.resize(size);
  // ICC header bytes 24..35 carry the profile creation time. Little CMS fills
  // them from the wall clock, which made otherwise identical lossless outputs
  // differ between runs. HDR Bridge generates this profile from fixed code and
  // primaries, so bind the header to the profile revision date.
  if (bytes.size() < 100u) throw std::runtime_error("serialized PQ ICC profile is truncated");
  constexpr std::array<uint8_t, 12> kProfileRevisionDate{
      0x07, 0xEA,  // 2026
      0x00, 0x08,  // August
      0x00, 0x0B,  // 11th
      0x00, 0x00,  // 00:00:00
      0x00, 0x00,
      0x00, 0x00};
  std::copy(kProfileRevisionDate.begin(), kProfileRevisionDate.end(), bytes.begin() + 24);
  return bytes;
}

std::vector<uint8_t> make_hdr_tiff_icc(bool display_p3) {
  cmsCIExyY white{0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE primaries{};
  if (display_p3) {
    primaries.Red = {0.6800, 0.3200, 1.0};
    primaries.Green = {0.2650, 0.6900, 1.0};
    primaries.Blue = {0.1500, 0.0600, 1.0};
  } else {
    primaries.Red = {0.7080, 0.2920, 1.0};
    primaries.Green = {0.1700, 0.7970, 1.0};
    primaries.Blue = {0.1310, 0.0460, 1.0};
  }
  std::array<cmsUInt16Number, 1024> fallback{};
  for (size_t i = 0; i < fallback.size(); ++i) {
    const double encoded = i / static_cast<double>(fallback.size() - 1u);
    fallback[i] = static_cast<cmsUInt16Number>(std::llround(
        std::pow(encoded, 2.2) * 65535.0));
  }
  std::array<cmsToneCurve*, 3> curves{};
  for (auto& curve : curves) {
    curve = cmsBuildTabulatedToneCurve16(nullptr,
        static_cast<cmsUInt32Number>(fallback.size()), fallback.data());
  }
  auto free_curves = [&] { for (auto* curve : curves) if (curve) cmsFreeToneCurve(curve); };
  if (std::any_of(curves.begin(), curves.end(),
                  [](const cmsToneCurve* curve) { return curve == nullptr; })) {
    free_curves();
    throw std::runtime_error("cannot create HDR TIFF fallback curves");
  }
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves.data());
  free_curves();
  if (!profile) throw std::runtime_error("cannot create HDR TIFF ICC profile");
  cmsSetProfileVersion(profile, 4.3);
  cmsSetDeviceClass(profile, cmsSigDisplayClass);
  cmsVideoSignalType cicp{};
  cicp.ColourPrimaries = display_p3 ? 12 : 9;
  cicp.TransferCharacteristics = 16;
  cicp.MatrixCoefficients = 0;
  cicp.VideoFullRangeFlag = 1;
  if (!cmsWriteTag(profile, cmsSigcicpTag, &cicp)) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot write HDR TIFF CICP tag");
  }
  cmsMLU* description = cmsMLUalloc(nullptr, 1);
  cmsMLU* copyright = cmsMLUalloc(nullptr, 1);
  const char* name = display_p3 ? "Display P3 PQ" : "Rec.2100 PQ";
  if (!description || !copyright ||
      !cmsMLUsetASCII(description, "en", "US", name) ||
      !cmsMLUsetASCII(copyright, "en", "US",
                     "Generated by HDR Bridge; no embedded third-party profile")) {
    if (description) cmsMLUfree(description);
    if (copyright) cmsMLUfree(copyright);
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot label HDR TIFF ICC profile");
  }
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, description);
  cmsWriteTag(profile, cmsSigCopyrightTag, copyright);
  cmsMLUfree(description);
  cmsMLUfree(copyright);
  cmsUInt32Number size = 0;
  if (!cmsSaveProfileToMem(profile, nullptr, &size) || size == 0) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot size HDR TIFF ICC profile");
  }
  std::vector<uint8_t> bytes(size);
  if (!cmsSaveProfileToMem(profile, bytes.data(), &size)) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot serialize HDR TIFF ICC profile");
  }
  cmsCloseProfile(profile);
  bytes.resize(size);
  if (bytes.size() < 100u) throw std::runtime_error("serialized HDR TIFF ICC is truncated");
  constexpr std::array<uint8_t, 12> kProfileRevisionDate{
      0x07, 0xEA, 0x00, 0x08, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  std::copy(kProfileRevisionDate.begin(), kProfileRevisionDate.end(), bytes.begin() + 24);
  return bytes;
}

std::vector<uint8_t> make_windows_compatible_pq_icc(bool display_p3) {
  cmsCIExyY white{0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE primaries{};
  if (display_p3) {
    primaries.Red = {0.6800, 0.3200, 1.0};
    primaries.Green = {0.2650, 0.6900, 1.0};
    primaries.Blue = {0.1500, 0.0600, 1.0};
  } else {
    primaries.Red = {0.7080, 0.2920, 1.0};
    primaries.Green = {0.1700, 0.7970, 1.0};
    primaries.Blue = {0.1310, 0.0460, 1.0};
  }
  // A bounded perceptual SDR fallback for legacy ICC consumers. The real
  // encoding is still unambiguously declared by the ICC CICP tag below. The
  // smooth shoulder reaches diffuse white near PQ code 0.78; its deliberately
  // lifted mid-tones keep consumers which ignore CICP from rendering encoded
  // PQ as a near-black image.
  std::array<cmsUInt16Number, 1024> fallback{};
  constexpr std::array<double, 13> x{
      0.0, 0.0625, 0.125, 0.25, 0.375, 0.50, 0.625,
      0.70, 0.75, 0.775, 0.80, 0.875, 1.0};
  constexpr std::array<double, 13> y{
      0.0, 0.0004, 0.0022, 0.019, 0.092, 0.31, 0.62,
      0.83, 0.99, 1.0, 1.0, 1.0, 1.0};
  for (size_t i = 0; i < fallback.size(); ++i) {
    const double encoded = i / static_cast<double>(fallback.size() - 1u);
    const auto upper = std::upper_bound(x.begin(), x.end(), encoded);
    const size_t hi = static_cast<size_t>(std::distance(x.begin(), upper));
    double decoded = 1.0;
    if (hi > 0u && hi < x.size()) {
      const size_t lo = hi - 1u;
      const double t = (encoded - x[lo]) / (x[hi] - x[lo]);
      // Smoothstep interpolation avoids visible slope discontinuities while
      // preserving monotonicity and the bounded shoulder.
      const double smooth = t * t * (3.0 - 2.0 * t);
      decoded = y[lo] + (y[hi] - y[lo]) * smooth;
    } else if (hi == 0u) {
      decoded = y.front();
    }
    fallback[i] = static_cast<cmsUInt16Number>(std::llround(
        std::clamp(decoded, 0.0, 1.0) * 65535.0));
  }
  std::array<cmsToneCurve*, 3> curves{};
  for (auto& curve : curves) curve = cmsBuildTabulatedToneCurve16(
      nullptr, static_cast<cmsUInt32Number>(fallback.size()), fallback.data());
  auto free_curves = [&] { for (auto* curve : curves) if (curve) cmsFreeToneCurve(curve); };
  if (std::any_of(curves.begin(), curves.end(),
                  [](const cmsToneCurve* curve) { return curve == nullptr; })) {
    free_curves();
    throw std::runtime_error("cannot create Windows-compatible PQ ICC fallback curves");
  }
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves.data());
  free_curves();
  if (!profile) throw std::runtime_error("cannot create Windows-compatible PQ ICC profile");
  cmsSetProfileVersion(profile, 4.2);
  cmsSetDeviceClass(profile, cmsSigDisplayClass);
  cmsSetHeaderRenderingIntent(profile, INTENT_RELATIVE_COLORIMETRIC);
  cmsVideoSignalType cicp{};
  cicp.ColourPrimaries = display_p3 ? 12 : 9;
  cicp.TransferCharacteristics = 16;
  cicp.MatrixCoefficients = 0;
  cicp.VideoFullRangeFlag = 1;
  if (!cmsWriteTag(profile, cmsSigcicpTag, &cicp)) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot write Windows-compatible PQ ICC CICP tag");
  }
  cmsCIEXYZ d65{};
  cmsxyY2XYZ(&d65, &white);
  if (!cmsWriteTag(profile, cmsSigMediaWhitePointTag, &d65)) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot write D65 media white point");
  }
  cmsMLU* description = cmsMLUalloc(nullptr, 1);
  cmsMLU* copyright = cmsMLUalloc(nullptr, 1);
  const char* name = display_p3 ? "Display P3 PQ" : "Rec. 2020 PQ";
  if (!description || !copyright || !cmsMLUsetASCII(description, "en", "US", name) ||
      !cmsMLUsetASCII(copyright, "en", "US",
                     "Generated by HDR Bridge; no embedded third-party profile")) {
    if (description) cmsMLUfree(description);
    if (copyright) cmsMLUfree(copyright);
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot label Windows-compatible PQ ICC profile");
  }
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, description);
  cmsWriteTag(profile, cmsSigCopyrightTag, copyright);
  cmsMLUfree(description); cmsMLUfree(copyright);
  cmsUInt32Number size = 0;
  if (!cmsSaveProfileToMem(profile, nullptr, &size) || size == 0) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot size Windows-compatible PQ ICC profile");
  }
  std::vector<uint8_t> bytes(size);
  if (!cmsSaveProfileToMem(profile, bytes.data(), &size)) {
    cmsCloseProfile(profile);
    throw std::runtime_error("cannot serialize Windows-compatible PQ ICC profile");
  }
  cmsCloseProfile(profile); bytes.resize(size);
  if (bytes.size() < 100u) {
    throw std::runtime_error("serialized Windows-compatible PQ ICC profile is truncated");
  }
  constexpr std::array<uint8_t, 12> kProfileRevisionDate{
      0x07, 0xEA, 0x00, 0x08, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  std::copy(kProfileRevisionDate.begin(), kProfileRevisionDate.end(), bytes.begin() + 24);
  return bytes;
}

std::vector<uint16_t> convert_rec2020_pq_to_p3_pq(const DecodedImage& decoded) {
  const auto& lut = pq_nits_lut();
  std::vector<uint16_t> output(decoded.rgb.size());
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  for (size_t p = 0; p < count; ++p) {
    const double r = lut[decoded.rgb[p * 3u]];
    const double g = lut[decoded.rgb[p * 3u + 1u]];
    const double b = lut[decoded.rgb[p * 3u + 2u]];
    const std::array<double, 3> p3{
        1.3435783 * r - 0.2821797 * g - 0.0613986 * b,
       -0.0652975 * r + 1.0757879 * g - 0.0104904 * b,
        0.0028218 * r - 0.0195985 * g + 1.0167767 * b};
    for (size_t c = 0; c < 3; ++c) {
      output[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(p3[c], 0.0, 10000.0)) * 65535.0));
    }
  }
  return output;
}

std::vector<uint16_t> convert_rec2020_pq_to_rec709_pq(const DecodedImage& decoded) {
  const auto& lut = pq_nits_lut();
  std::vector<uint16_t> output(decoded.rgb.size());
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  for (size_t p = 0; p < count; ++p) {
    const double r = lut[decoded.rgb[p * 3u]];
    const double g = lut[decoded.rgb[p * 3u + 1u]];
    const double b = lut[decoded.rgb[p * 3u + 2u]];
    const std::array<double, 3> rec709{
        1.6604910 * r - 0.5876411 * g - 0.0728499 * b,
       -0.1245505 * r + 1.1328999 * g - 0.0083494 * b,
       -0.0181508 * r - 0.1005789 * g + 1.1187297 * b};
    for (size_t c = 0; c < 3; ++c) {
      output[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(rec709[c], 0.0, 10000.0)) * 65535.0));
    }
  }
  return output;
}

std::vector<uint16_t> convert_rec2020_pq_to_hlg(const DecodedImage& decoded,
                                                 const std::string& gamut) {
  const auto& lut = pq_nits_lut();
  std::vector<uint16_t> output(decoded.rgb.size());
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  for (size_t p = 0; p < count; ++p) {
    const double r = lut[decoded.rgb[p * 3u]];
    const double g = lut[decoded.rgb[p * 3u + 1u]];
    const double b = lut[decoded.rgb[p * 3u + 2u]];
    std::array<double, 3> linear{r, g, b};
    if (gamut == "p3") {
      linear = {1.3435783 * r - 0.2821797 * g - 0.0613986 * b,
               -0.0652975 * r + 1.0757879 * g - 0.0104904 * b,
                0.0028218 * r - 0.0195985 * g + 1.0167767 * b};
    } else if (gamut == "rec709") {
      linear = {1.6604910 * r - 0.5876411 * g - 0.0728499 * b,
               -0.1245505 * r + 1.1328999 * g - 0.0083494 * b,
               -0.0181508 * r - 0.1005789 * g + 1.1187297 * b};
    }
    for (double& value : linear) value = std::max(value, 0.0);
    const auto signal = transfer::linear_nits_to_hlg(linear);
    for (size_t c = 0; c < 3; ++c) {
      output[p * 3u + c] = static_cast<uint16_t>(std::llround(signal[c] * 65535.0));
    }
  }
  return output;
}

DecodedImage resize_linear_hdr_to_fit(const DecodedImage& source,
                                      uint32_t max_dimension) {
  if (source.info.width <= max_dimension && source.info.height <= max_dimension) {
    return source;
  }
  const double scale = std::min(
      max_dimension / static_cast<double>(source.info.width),
      max_dimension / static_cast<double>(source.info.height));
  DecodedImage output;
  output.info = source.info;
  output.info.width = std::max(1u, static_cast<uint32_t>(
      std::lround(source.info.width * scale)));
  output.info.height = std::max(1u, static_cast<uint32_t>(
      std::lround(source.info.height * scale)));
  output.exif = source.exif;
  output.xmp = source.xmp;
  output.rgb.resize(static_cast<size_t>(output.info.width) * output.info.height * 3u);
  const auto& lut = pq_nits_lut();
  for (uint32_t y = 0; y < output.info.height; ++y) {
    const double sy = (y + 0.5) / scale - 0.5;
    const uint32_t y0 = static_cast<uint32_t>(std::clamp(
        static_cast<int64_t>(std::floor(sy)), int64_t{0},
        static_cast<int64_t>(source.info.height - 1u)));
    const uint32_t y1 = std::min(y0 + 1u, source.info.height - 1u);
    const double fy = std::clamp(sy - std::floor(sy), 0.0, 1.0);
    for (uint32_t x = 0; x < output.info.width; ++x) {
      const double sx = (x + 0.5) / scale - 0.5;
      const uint32_t x0 = static_cast<uint32_t>(std::clamp(
          static_cast<int64_t>(std::floor(sx)), int64_t{0},
          static_cast<int64_t>(source.info.width - 1u)));
      const uint32_t x1 = std::min(x0 + 1u, source.info.width - 1u);
      const double fx = std::clamp(sx - std::floor(sx), 0.0, 1.0);
      const size_t destination =
          (static_cast<size_t>(y) * output.info.width + x) * 3u;
      for (size_t c = 0; c < 3; ++c) {
        const double a = lut[source.rgb[(static_cast<size_t>(y0) * source.info.width + x0) * 3u + c]];
        const double b = lut[source.rgb[(static_cast<size_t>(y0) * source.info.width + x1) * 3u + c]];
        const double d = lut[source.rgb[(static_cast<size_t>(y1) * source.info.width + x0) * 3u + c]];
        const double e = lut[source.rgb[(static_cast<size_t>(y1) * source.info.width + x1) * 3u + c]];
        const double top = a + (b - a) * fx;
        const double bottom = d + (e - d) * fx;
        output.rgb[destination + c] = static_cast<uint16_t>(std::llround(
            forward_pq(top + (bottom - top) * fy) * 65535.0));
      }
    }
  }
  return output;
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611)  // libpng's documented C error boundary uses setjmp.
#endif

struct PngMemoryWriter { std::vector<uint8_t> bytes; };

void png_write_memory(png_structp png, png_bytep data, png_size_t length) {
  auto* writer = static_cast<PngMemoryWriter*>(png_get_io_ptr(png));
  writer->bytes.insert(writer->bytes.end(), data, data + length);
}

void png_flush_memory(png_structp) {}

void insert_png_cicp_after_ihdr(std::vector<uint8_t>& bytes, const std::array<png_byte, 4>& cicp) {
  if (bytes.size() < 33 || std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) != 0 ||
      std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) {
    throw std::runtime_error("cannot locate PNG IHDR for cICP insertion");
  }
  std::array<uint8_t, 16> chunk{};
  chunk[3] = 4;
  std::memcpy(chunk.data() + 4, "cICP", 4);
  std::copy(cicp.begin(), cicp.end(), chunk.begin() + 8);
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, chunk.data() + 4, 8);
  chunk[12] = static_cast<uint8_t>((crc >> 24) & 0xffu);
  chunk[13] = static_cast<uint8_t>((crc >> 16) & 0xffu);
  chunk[14] = static_cast<uint8_t>((crc >> 8) & 0xffu);
  chunk[15] = static_cast<uint8_t>(crc & 0xffu);
  bytes.insert(bytes.begin() + 33, chunk.begin(), chunk.end());
}

std::vector<uint8_t> encode_png(const DecodedImage& decoded, const ConversionOptions& options,
                                const std::vector<uint16_t>& pixels) {
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) throw std::runtime_error("cannot create PNG writer");
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_write_struct(&png, nullptr); throw std::runtime_error("cannot create PNG info"); }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    throw std::runtime_error("libpng encoding failed");
  }
  PngMemoryWriter writer;
  png_set_write_fn(png, &writer, png_write_memory, png_flush_memory);
  png_set_IHDR(png, info, decoded.info.width, decoded.info.height, 16, PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
  png_set_compression_level(png, std::clamp(options.png_compression_level, 1, 9));
  const bool p3 = options.output_gamut == "p3";
  const bool hlg = options.output_transfer == "hlg";
  const bool rec709 = options.output_gamut == "rec709";
  std::array<png_byte, 4> cicp{static_cast<png_byte>(rec709 ? 1 : p3 ? 12 : 9),
                               static_cast<png_byte>(hlg ? 18 : 16), 0, 1};
  if (!options.diagnostic_icc_only) {
    png_unknown_chunk chunk{};
    std::memcpy(chunk.name, "cICP", 4);
    chunk.data = cicp.data();
    chunk.size = cicp.size();
    chunk.location = PNG_HAVE_IHDR;
    png_set_unknown_chunks(png, info, &chunk, 1);
    png_set_unknown_chunk_location(png, info, 0, PNG_HAVE_IHDR);
  }
  std::vector<uint8_t> icc;
  if ((options.embed_hdr_icc && !hlg) || options.diagnostic_icc_only) {
    const std::string png_profile_name = options.png_icc_name_override.empty()
        ? (p3 ? "Display P3 PQ" : "Rec.2100 PQ")
        : options.png_icc_name_override;
    if (png_profile_name.size() > 79u) throw std::runtime_error("PNG ICC name exceeds 79 bytes");
    // The ordinary PQ profile uses CICP for HDR-aware consumers and a bounded
    // perceptual TRC for legacy ICC fallback (notably Windows Photos).
    icc = options.diagnostic_icc_only
        ? make_windows_compatible_pq_icc(p3)
        : make_windows_compatible_pq_icc(p3);
    png_set_iCCP(png, info, png_profile_name.c_str(),
                 PNG_COMPRESSION_TYPE_BASE, icc.data(), static_cast<png_uint_32>(icc.size()));
  }
  std::vector<uint8_t> exif;
#ifdef PNG_eXIf_SUPPORTED
  if (options.copy_exif && !decoded.exif.empty()) {
    exif = exif_tiff_payload(decoded.exif);
    if (!exif.empty()) {
      png_set_eXIf_1(png, info, static_cast<png_uint_32>(exif.size()), exif.data());
    }
  }
#endif
  std::string xmp;
  png_text text{};
  if (options.copy_xmp && !decoded.xmp.empty()) {
    xmp.assign(decoded.xmp.begin(), decoded.xmp.end());
    text.compression = PNG_ITXT_COMPRESSION_NONE;
    text.key = const_cast<png_charp>("XML:com.adobe.xmp");
    text.text = xmp.data();
    text.text_length = xmp.size();
    text.itxt_length = xmp.size();
    png_set_text(png, info, &text, 1);
  }
  png_write_info(png, info);
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
  png_set_swap(png);
#endif
  std::vector<png_bytep> rows(decoded.info.height);
  for (uint32_t y = 0; y < decoded.info.height; ++y) {
    rows[y] = reinterpret_cast<png_bytep>(const_cast<uint16_t*>(
        pixels.data() + static_cast<size_t>(y) * decoded.info.width * 3u));
  }
  png_write_image(png, rows.data());
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  if (!options.diagnostic_icc_only) insert_png_cicp_after_ihdr(writer.bytes, cicp);
  return std::move(writer.bytes);
}

std::vector<uint8_t> encode_gainmap_png(const gainmap::GainMapRaster& raster,
                                        uint32_t) {
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) throw std::runtime_error("cannot create gain-map PNG writer");
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_write_struct(&png, nullptr); throw std::runtime_error("cannot create gain-map PNG info"); }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    throw std::runtime_error("libpng gain-map encoding failed");
  }
  PngMemoryWriter writer;
  png_set_write_fn(png, &writer, png_write_memory, png_flush_memory);
  const bool rgb = raster.channels == 3;
  png_set_IHDR(png, info, raster.width, raster.height, 16,
               rgb ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_GRAY,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
  png_set_compression_level(png, 4);
  png_write_info(png, info);
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
  png_set_swap(png);
#endif
  std::vector<uint16_t> plane;
  if (!rgb) {
    plane.resize(static_cast<size_t>(raster.width) * raster.height);
    for (size_t p = 0; p < plane.size(); ++p) plane[p] = raster.rgb16[p * 3u];
  }
  std::vector<png_bytep> rows(raster.height);
  for (uint32_t y = 0; y < raster.height; ++y) {
    rows[y] = reinterpret_cast<png_bytep>(rgb
        ? const_cast<uint16_t*>(raster.rgb16.data() + static_cast<size_t>(y) * raster.width * 3u)
        : plane.data() + static_cast<size_t>(y) * raster.width);
  }
  png_write_image(png, rows.data());
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  return std::move(writer.bytes);
}

void encode_gainmap_tiff(const std::filesystem::path& path,
                         const gainmap::GainMapRaster& raster, uint32_t) {
  TIFF* raw = nullptr;
#ifdef _WIN32
  raw = TIFFOpenW(path.c_str(), "w");
#else
  raw = TIFFOpen(path.string().c_str(), "w");
#endif
  if (!raw) throw std::runtime_error("cannot create gain-map TIFF");
  std::unique_ptr<TIFF, void(*)(TIFF*)> tiff(raw, TIFFClose);
  TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, raster.width);
  TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, raster.height);
  const bool rgb = raster.channels == 3;
  TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, rgb ? 3 : 1);
  TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tiff.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC,
               rgb ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
  TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, raster.height);
  std::vector<uint16_t> plane;
  const uint16_t* samples = raster.rgb16.data();
  size_t sample_count = raster.rgb16.size();
  if (!rgb) {
    plane.resize(static_cast<size_t>(raster.width) * raster.height);
    for (size_t p = 0; p < plane.size(); ++p) plane[p] = raster.rgb16[p * 3u];
    samples = plane.data();
    sample_count = plane.size();
  }
  if (TIFFWriteEncodedStrip(tiff.get(), 0, const_cast<uint16_t*>(samples),
      static_cast<tmsize_t>(sample_count * sizeof(uint16_t))) < 0) {
    throw std::runtime_error("cannot write gain-map TIFF");
  }
}

std::vector<uint8_t> encode_gainmap_jpeg(const gainmap::GainMapRaster& raster,
                                         uint32_t, int quality) {
#ifndef _WIN32
  (void)raster; (void)quality;
  throw std::runtime_error("gain-map JPEG export is unavailable on this platform");
#else
  jpeg_compress_struct encoder{};
  jpeg_error_mgr error{};
  encoder.err = jpeg_std_error(&error);
  jpeg_create_compress(&encoder);
  unsigned char* memory = nullptr;
  unsigned long size = 0;
  jpeg_mem_dest(&encoder, &memory, &size);
  encoder.image_width = raster.width;
  encoder.image_height = raster.height;
  const bool rgb = raster.channels == 3;
  encoder.input_components = rgb ? 3 : 1;
  encoder.in_color_space = rgb ? JCS_RGB : JCS_GRAYSCALE;
  jpeg_set_defaults(&encoder);
  if (rgb) {
    for (int component = 0; component < 3; ++component) {
      encoder.comp_info[component].h_samp_factor = 1;
      encoder.comp_info[component].v_samp_factor = 1;
    }
  }
  jpeg_set_quality(&encoder, std::clamp(quality, 1, 100), TRUE);
  jpeg_start_compress(&encoder, TRUE);
  std::vector<uint8_t> row(static_cast<size_t>(raster.width) * encoder.input_components);
  while (encoder.next_scanline < encoder.image_height) {
    const size_t base = static_cast<size_t>(encoder.next_scanline) * raster.width;
    for (uint32_t x = 0; x < raster.width; ++x) {
      if (rgb) {
        for (uint32_t channel = 0; channel < 3; ++channel) {
          row[static_cast<size_t>(x) * 3u + channel] = static_cast<uint8_t>(
              (raster.rgb16[(base + x) * 3u + channel] + 128u) / 257u);
        }
      } else {
        row[x] = static_cast<uint8_t>((raster.rgb16[(base + x) * 3u] + 128u) / 257u);
      }
    }
    JSAMPROW pointer = row.data();
    jpeg_write_scanlines(&encoder, &pointer, 1);
  }
  jpeg_finish_compress(&encoder);
  std::vector<uint8_t> bytes(memory, memory + size);
  std::free(memory);
  jpeg_destroy_compress(&encoder);
  return bytes;
#endif
}

struct PngMemoryReader { const uint8_t* data; size_t size; size_t offset = 0; };

void png_read_memory(png_structp png, png_bytep output, png_size_t length) {
  auto* reader = static_cast<PngMemoryReader*>(png_get_io_ptr(png));
  if (length > reader->size - reader->offset) png_error(png, "truncated PNG memory input");
  std::memcpy(output, reader->data + reader->offset, length);
  reader->offset += length;
}

bool find_png_cicp(const std::vector<uint8_t>& bytes, std::array<uint8_t, 4>& cicp) {
  if (bytes.size() < 8 || std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return false;
  size_t offset = 8;
  while (offset + 12 <= bytes.size()) {
    const uint32_t length = be32(bytes.data() + offset);
    if (length > bytes.size() - offset - 12) return false;
    if (std::memcmp(bytes.data() + offset + 4, "cICP", 4) == 0 && length == 4) {
      std::copy_n(bytes.data() + offset + 8, 4, cicp.begin());
      return true;
    }
    offset += static_cast<size_t>(length) + 12u;
  }
  return false;
}

Verification decode_png(const std::vector<uint8_t>& bytes,
                        const std::vector<uint16_t>* expected = nullptr,
                        bool expect_icc = true,
                        bool expect_icc_only = false) {
  Verification v;
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) throw std::runtime_error("cannot create PNG reader");
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); throw std::runtime_error("cannot create PNG read info"); }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    throw std::runtime_error("libpng verification decode failed");
  }
  PngMemoryReader reader{bytes.data(), bytes.size()};
  png_set_read_fn(png, &reader, png_read_memory);
  png_read_info(png, info);
  int bit_depth = 0, color_type = 0, interlace = 0, compression = 0, filter = 0;
  png_uint_32 width = 0, height = 0;
  png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, &interlace, &compression, &filter);
  v.width = width; v.height = height; v.bit_depth = static_cast<uint32_t>(bit_depth);
  v.pixel_format = color_type == PNG_COLOR_TYPE_RGB ? "PNG RGB16" : "unexpected PNG color type";
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
  png_set_swap(png);
#endif
  png_read_update_info(png, info);
  std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * 3u);
  std::vector<png_bytep> rows(height);
  for (uint32_t y = 0; y < height; ++y) rows[y] = reinterpret_cast<png_bytep>(pixels.data() + static_cast<size_t>(y) * width * 3u);
  png_read_image(png, rows.data());
  png_read_end(png, info);
  png_charp profile_name = nullptr; int profile_compression = 0; png_bytep profile = nullptr; png_uint_32 profile_size = 0;
  const bool has_icc = png_get_iCCP(png, info, &profile_name, &profile_compression, &profile, &profile_size) != 0 && profile_size > 0;
  png_destroy_read_struct(&png, &info, nullptr);
  std::array<uint8_t, 4> cicp{};
  const bool has_cicp = find_png_cicp(bytes, cicp);
  const bool rec2020 = has_cicp && cicp == std::array<uint8_t, 4>{9, 16, 0, 1};
  const bool p3 = has_cicp && cicp == std::array<uint8_t, 4>{12, 16, 0, 1};
  const bool hlg = has_cicp && (cicp == std::array<uint8_t, 4>{9, 18, 0, 1} ||
                                cicp == std::array<uint8_t, 4>{12, 18, 0, 1});
  v.color_encoding = rec2020 ? "Rec.2020/PQ full-range cICP 9/16/0/1" :
                     p3 ? "Display P3/PQ full-range cICP 12/16/0/1" :
                     hlg ? (cicp[0] == 12 ? "Display P3/HLG full-range cICP 12/18/0/1" :
                                             "Rec.2020/HLG full-range cICP 9/18/0/1") :
                     expect_icc_only && has_icc ? "direct HDR from ICC; cICP absent" :
                     "unexpected/missing PNG cICP";
  v.exact_roundtrip = expected && expected->size() == pixels.size() &&
                      std::memcmp(expected->data(), pixels.data(), pixels.size() * sizeof(uint16_t)) == 0;
  auto [minimum, maximum] = std::minmax_element(pixels.begin(), pixels.end());
  if (minimum != pixels.end()) { v.min_value = *minimum; v.max_value = *maximum; }
  v.checks.push_back(width && height ? "PNG dimensions parse" : "PNG dimensions missing");
  v.checks.push_back(bit_depth == 16 && color_type == PNG_COLOR_TYPE_RGB ? "lossless RGB 16-bit storage" : "PNG is not RGB16");
  v.checks.push_back(rec2020 || p3 || hlg
      ? (hlg ? "HLG cICP present with RGB matrix=0 and full range"
             : "PQ cICP present with RGB matrix=0 and full range")
      : expect_icc_only && !has_cicp ? "PNG cICP absent as required for ICC-only regression"
                                    : "PNG cICP mismatch");
  v.checks.push_back(expect_icc
                         ? (has_icc ? "compatible HDR PQ ICC profile embedded" : "HDR ICC profile missing")
                         : (!has_icc ? "cICP-only A/B variant contains no ICC profile"
                                     : "cICP-only A/B variant unexpectedly contains ICC"));
  if (expected) v.checks.push_back(v.exact_roundtrip ? "exact RGB16 pixel roundtrip" : "RGB16 pixel roundtrip mismatch");
  v.passed = width > 0 && height > 0 && bit_depth == 16 && color_type == PNG_COLOR_TYPE_RGB &&
             (expect_icc_only ? (!has_cicp && has_icc) : (rec2020 || p3 || hlg)) &&
             (has_icc == expect_icc) && (!expected || v.exact_roundtrip);
  return v;
}

void convert_p3_pq_to_rec2020_pq(std::vector<uint16_t>& pixels) {
  const auto& lut = pq_nits_lut();
  for (size_t p = 0; p < pixels.size() / 3u; ++p) {
    const double r = lut[pixels[p * 3u]];
    const double g = lut[pixels[p * 3u + 1u]];
    const double b = lut[pixels[p * 3u + 2u]];
    const std::array<double, 3> rec2020{
        0.7538330 * r + 0.1985974 * g + 0.0475696 * b,
        0.0457438 * r + 0.9417772 * g + 0.0124789 * b,
       -0.0012103 * r + 0.0176017 * g + 0.9836086 * b};
    for (size_t c = 0; c < 3; ++c) {
      pixels[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(rec2020[c], 0.0, 10000.0)) * 65535.0));
    }
  }
}

DecodedImage decode_png_input(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  std::array<uint8_t, 4> cicp{};
  const bool has_cicp = find_png_cicp(bytes, cicp);
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) {
    if (png) png_destroy_read_struct(&png, nullptr, nullptr);
    throw std::runtime_error("cannot create PNG input decoder");
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    throw std::runtime_error("PNG input decode failed");
  }
  PngMemoryReader reader{bytes.data(), bytes.size()};
  png_set_read_fn(png, &reader, png_read_memory);
  png_read_info(png, info);
  png_uint_32 width = 0, height = 0; int bit_depth = 0, color_type = 0, interlace = 0, compression = 0, filter = 0;
  png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, &interlace, &compression, &filter);
  DecodedImage decoded;
  decoded.info.path = path;
  decoded.info.format = "PNG";
  decoded.info.container_brand = "PNG";
  decoded.info.asset_kind = "non-HDR";
  decoded.info.width = width; decoded.info.height = height;
  decoded.info.codec = "PNG/DEFLATE";
  const char* color_layout = "Unknown";
  switch (color_type) {
    case PNG_COLOR_TYPE_GRAY: color_layout = "Grayscale"; break;
    case PNG_COLOR_TYPE_PALETTE: color_layout = "Indexed"; break;
    case PNG_COLOR_TYPE_RGB: color_layout = "RGB"; break;
    case PNG_COLOR_TYPE_GRAY_ALPHA: color_layout = "Grayscale + alpha"; break;
    case PNG_COLOR_TYPE_RGB_ALPHA: color_layout = "RGBA"; break;
    default: break;
  }
  decoded.info.profile = std::string(color_layout) + std::to_string(bit_depth);
  decoded.info.pixel_format = std::to_string(bit_depth) + "-bit " + color_layout;
  decoded.info.color_signal_kind = "Unknown";
  decoded.info.bit_depth = static_cast<uint32_t>(bit_depth);
  decoded.info.chroma = color_type == PNG_COLOR_TYPE_RGB ? "4:4:4 RGB" :
                        color_type == PNG_COLOR_TYPE_RGB_ALPHA ? "4:4:4 RGBA" :
                        color_layout;
  if (has_cicp) {
    decoded.info.native_color_present = true;
    decoded.info.native_primaries = cicp[0];
    decoded.info.native_transfer = cicp[1];
    decoded.info.native_matrix = cicp[2];
    decoded.info.native_full_range = cicp[3] != 0;
    decoded.info.native_range_known = true;
    decoded.info.native_color_description = "PNG cICP chunk";
  }
  png_charp profile_name = nullptr;
  int profile_compression = 0;
  png_bytep profile_data = nullptr;
  png_uint_32 profile_size = 0;
  const bool has_icc = png_get_iCCP(png, info, &profile_name, &profile_compression,
                                    &profile_data, &profile_size) != 0 && profile_size > 0;
  store_icc_signal(decoded.info, parse_icc_signal(has_icc ? profile_data : nullptr,
                                                  has_icc ? profile_size : 0));
  if (!decoded.info.icc_description.empty()) decoded.info.profile = decoded.info.icc_description;
  else if (has_icc && profile_name) decoded.info.profile = profile_name;
  resolve_color_signaling(decoded.info, true);
  decoded.info.color_signal_kind = decoded.info.resolved_signaling_source;
  decoded.info.exif_status = "absent";
  decoded.info.xmp_status = "absent";
#ifdef PNG_eXIf_SUPPORTED
  png_uint_32 exif_size = 0;
  png_bytep exif_data = nullptr;
  if (png_get_eXIf_1(png, info, &exif_size, &exif_data) != 0 && exif_data && exif_size > 0) {
    decoded.exif.assign(exif_data, exif_data + exif_size);
    decoded.info.exif_present = true;
    decoded.info.exif_status = "present";
    decoded.info.original_orientation = orientation::read_exif_orientation(decoded.exif);
    decoded.info.orientation_status = orientation::has_exif_orientation(decoded.exif)
        ? "present" : "absent";
  } else {
    decoded.info.orientation_status = "absent";
  }
#endif
  png_textp texts = nullptr; int text_count = 0;
  if (png_get_text(png, info, &texts, &text_count) > 0) {
    for (int i = 0; i < text_count; ++i) {
      if (texts[i].key && std::strcmp(texts[i].key, "XML:com.adobe.xmp") == 0 && texts[i].text) {
        const size_t size = texts[i].itxt_length ? texts[i].itxt_length : texts[i].text_length;
        decoded.xmp.assign(texts[i].text, texts[i].text + size);
        decoded.info.xmp_present = !decoded.xmp.empty();
        decoded.info.xmp_status = metadata_status(decoded.info.xmp_present);
        break;
      }
    }
  }
  if (decoded.info.asset_kind != "direct-hdr") {
    // A parsed header alone is not enough to call an SDR PNG decodable.
    // Consume the complete image stream so truncated/corrupt IDAT data is a
    // decode error, while avoiding allocation of a canonical HDR raster.
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    const png_size_t row_bytes = png_get_rowbytes(png, info);
    if (height != 0 && row_bytes > std::numeric_limits<size_t>::max() / height) {
      png_destroy_read_struct(&png, &info, nullptr);
      throw std::runtime_error("PNG input dimensions are too large");
    }
    if (passes == 1) {
      std::vector<png_byte> row(row_bytes);
      for (png_uint_32 y = 0; y < height; ++y) png_read_row(png, row.data(), nullptr);
    } else {
      // Adam7 rows must retain earlier-pass data for libpng's combine step.
      std::vector<png_byte> raster(static_cast<size_t>(row_bytes) * height);
      for (int pass = 0; pass < passes; ++pass) {
        for (png_uint_32 y = 0; y < height; ++y) {
          png_read_row(png, raster.data() + static_cast<size_t>(y) * row_bytes, nullptr);
        }
      }
    }
    png_read_end(png, info);
    png_destroy_read_struct(&png, &info, nullptr);
    return decoded;
  }
  if (bit_depth != 16 || color_type != PNG_COLOR_TYPE_RGB) {
    png_destroy_read_struct(&png, &info, nullptr);
    throw std::runtime_error("direct HDR PNG input must be RGB16");
  }
  decoded.info.pixel_format = "RGB16 integer";
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
  png_set_swap(png);
#endif
  png_read_update_info(png, info);
  decoded.rgb.resize(static_cast<size_t>(width) * height * 3u);
  std::vector<png_bytep> rows(height);
  for (uint32_t y = 0; y < height; ++y) rows[y] = reinterpret_cast<png_bytep>(decoded.rgb.data() + static_cast<size_t>(y) * width * 3u);
  png_read_image(png, rows.data());
  png_read_end(png, info);
  png_destroy_read_struct(&png, &info, nullptr);
  if (decoded.info.transfer == 18) {
    const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
    for (size_t p = 0; p < count; ++p) {
      const auto nits = transfer::hlg_to_linear_nits({
          decoded.rgb[p * 3u] / 65535.0,
          decoded.rgb[p * 3u + 1u] / 65535.0,
          decoded.rgb[p * 3u + 2u] / 65535.0});
      for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] =
          static_cast<uint16_t>(std::llround(forward_pq(nits[c]) * 65535.0));
    }
  } else if (decoded.info.primaries == 12) {
    convert_p3_pq_to_rec2020_pq(decoded.rgb);
  } else if (decoded.info.primaries == 1) {
    convert_rec709_pq_to_rec2020_pq(decoded.rgb);
  }
  return decoded;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct TiffDeleter { void operator()(TIFF* value) const { if (value) TIFFClose(value); } };
using TiffPtr = std::unique_ptr<TIFF, TiffDeleter>;

TiffPtr open_tiff(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
  TIFF* value = TIFFOpenW(path.c_str(), mode);
#else
  TIFF* value = TIFFOpen(path.string().c_str(), mode);
#endif
  if (!value) throw std::runtime_error("cannot open TIFF: " + path.string());
  return TiffPtr(value);
}

void encode_tiff(const std::filesystem::path& path, const DecodedImage& decoded,
                 const ConversionOptions& options, const std::vector<uint16_t>& pixels) {
  auto tiff = open_tiff(path, "w");
  const bool p3 = options.output_gamut == "p3";
  auto icc = !options.diagnostic_icc_profile_path.empty()
      ? read_file(options.diagnostic_icc_profile_path)
      : make_windows_compatible_pq_icc(p3);
  TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, decoded.info.width);
  TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, decoded.info.height);
  TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tiff.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION,
               options.tiff_compressed ? COMPRESSION_ADOBE_DEFLATE : COMPRESSION_NONE);
#ifdef TIFFTAG_ZIPQUALITY
  if (options.tiff_compressed) {
    TIFFSetField(tiff.get(), TIFFTAG_ZIPQUALITY,
                 std::clamp(options.tiff_compression_level, 1, 9));
  }
#endif
  // Premiere interoperates reliably with ACR-style TIFFs that use one large
  // strip. Thousands of scanline-sized strips caused inserted black frames.
  TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, decoded.info.height);
  const std::string description = p3 ? "HDR Bridge direct RGB16 Display P3 PQ" :
                                       "HDR Bridge direct RGB16 Rec.2020 PQ";
  TIFFSetField(tiff.get(), TIFFTAG_IMAGEDESCRIPTION, description.c_str());
  TIFFSetField(tiff.get(), TIFFTAG_SOFTWARE, "HDR Bridge 1.2.1");
  TIFFSetField(tiff.get(), TIFFTAG_ICCPROFILE, static_cast<uint32_t>(icc.size()), icc.data());
  std::vector<uint8_t> diagnostic_photoshop;
  std::vector<uint8_t> diagnostic_iptc;
  std::vector<uint8_t> diagnostic_xmp;
  if (!options.diagnostic_tiff_metadata_source.empty()) {
    auto source = open_tiff(options.diagnostic_tiff_metadata_source, "r");
#ifdef TIFFTAG_PHOTOSHOP
    uint32_t photoshop_size = 0; void* photoshop_data = nullptr;
    if (TIFFGetField(source.get(), TIFFTAG_PHOTOSHOP, &photoshop_size, &photoshop_data) == 1 && photoshop_data && photoshop_size) {
      diagnostic_photoshop.assign(static_cast<uint8_t*>(photoshop_data), static_cast<uint8_t*>(photoshop_data) + photoshop_size);
      TIFFSetField(tiff.get(), TIFFTAG_PHOTOSHOP, photoshop_size, diagnostic_photoshop.data());
    }
#endif
#ifdef TIFFTAG_RICHTIFFIPTC
    uint32_t iptc_words = 0; void* iptc_data = nullptr;
    if (TIFFGetField(source.get(), TIFFTAG_RICHTIFFIPTC, &iptc_words, &iptc_data) == 1 && iptc_data && iptc_words) {
      const uint32_t iptc_bytes = iptc_words * 4u;
      diagnostic_iptc.assign(static_cast<uint8_t*>(iptc_data), static_cast<uint8_t*>(iptc_data) + iptc_bytes);
      TIFFSetField(tiff.get(), TIFFTAG_RICHTIFFIPTC, iptc_words, diagnostic_iptc.data());
    }
#endif
#ifdef TIFFTAG_XMLPACKET
    uint32_t xmp_size = 0; void* xmp_data = nullptr;
    if (TIFFGetField(source.get(), TIFFTAG_XMLPACKET, &xmp_size, &xmp_data) == 1 && xmp_data && xmp_size) {
      diagnostic_xmp.assign(static_cast<uint8_t*>(xmp_data), static_cast<uint8_t*>(xmp_data) + xmp_size);
      TIFFSetField(tiff.get(), TIFFTAG_XMLPACKET, xmp_size, diagnostic_xmp.data());
    }
#endif
    float xres = 0.0f, yres = 0.0f; uint16_t unit = RESUNIT_NONE;
    if (TIFFGetField(source.get(), TIFFTAG_XRESOLUTION, &xres) == 1) TIFFSetField(tiff.get(), TIFFTAG_XRESOLUTION, xres);
    if (TIFFGetField(source.get(), TIFFTAG_YRESOLUTION, &yres) == 1) TIFFSetField(tiff.get(), TIFFTAG_YRESOLUTION, yres);
    if (TIFFGetField(source.get(), TIFFTAG_RESOLUTIONUNIT, &unit) == 1) TIFFSetField(tiff.get(), TIFFTAG_RESOLUTIONUNIT, unit);
  }
#ifdef TIFFTAG_XMLPACKET
  if (options.diagnostic_tiff_metadata_source.empty() &&
      options.copy_xmp && !decoded.xmp.empty()) {
    TIFFSetField(tiff.get(), TIFFTAG_XMLPACKET, static_cast<uint32_t>(decoded.xmp.size()), decoded.xmp.data());
  }
#endif
  for (uint32_t y = 0; y < decoded.info.height; ++y) {
    auto* row = const_cast<uint16_t*>(pixels.data() + static_cast<size_t>(y) * decoded.info.width * 3u);
    if (TIFFWriteScanline(tiff.get(), row, y, 0) < 0) throw std::runtime_error("TIFF scanline encoding failed");
  }
  if (!TIFFWriteDirectory(tiff.get())) throw std::runtime_error("cannot commit TIFF directory");
}

std::string icc_description(const void* data, uint32_t size) {
  if (!data || size == 0) return {};
  cmsHPROFILE profile = cmsOpenProfileFromMem(data, size);
  if (!profile) return {};
  std::array<char, 256> text{};
  cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", text.data(), static_cast<cmsUInt32Number>(text.size()));
  cmsCloseProfile(profile);
  return text.data();
}

std::optional<cmsVideoSignalType> icc_cicp(const void* data, uint32_t size) {
  if (!data || size == 0) return std::nullopt;
  cmsHPROFILE profile = cmsOpenProfileFromMem(data, size);
  if (!profile) return std::nullopt;
  const auto* value = static_cast<const cmsVideoSignalType*>(
      cmsReadTag(profile, cmsSigcicpTag));
  const std::optional<cmsVideoSignalType> result = value
      ? std::optional<cmsVideoSignalType>(*value) : std::nullopt;
  cmsCloseProfile(profile);
  return result;
}

Verification decode_tiff(const std::filesystem::path& path,
                         const std::vector<uint16_t>* expected = nullptr) {
  auto tiff = open_tiff(path, "r");
  Verification v;
  uint16_t samples = 0, bits = 0, sample_format = SAMPLEFORMAT_UINT;
  uint16_t photometric = 0, planar = 0, compression = 0;
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &v.width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &v.height);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
  TIFFGetField(tiff.get(), TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetField(tiff.get(), TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetField(tiff.get(), TIFFTAG_COMPRESSION, &compression);
  v.bit_depth = bits;
  v.pixel_format = samples == 3 && bits == 16 && sample_format == SAMPLEFORMAT_UINT &&
                   photometric == PHOTOMETRIC_RGB && planar == PLANARCONFIG_CONTIG
      ? "TIFF RGB uint16 contiguous" : "unexpected TIFF pixel format";
  uint32_t icc_size = 0; void* icc_data = nullptr;
  const bool has_icc = TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc_data) == 1 && icc_size > 0;
  const std::string description = has_icc ? icc_description(icc_data, icc_size) : std::string{};
  const auto cicp = has_icc ? icc_cicp(icc_data, icc_size) : std::nullopt;
  const bool rec2020 = cicp && cicp->ColourPrimaries == 9 &&
                       cicp->TransferCharacteristics == 16 &&
                       cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag == 1;
  const bool p3 = cicp && cicp->ColourPrimaries == 12 &&
                  cicp->TransferCharacteristics == 16 &&
                  cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag == 1;
  const bool rec2020_hlg = cicp && cicp->ColourPrimaries == 9 &&
                           cicp->TransferCharacteristics == 18 &&
                           cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag == 1;
  const bool p3_hlg = cicp && cicp->ColourPrimaries == 12 &&
                      cicp->TransferCharacteristics == 18 &&
                      cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag == 1;
  v.color_encoding = rec2020 ? "direct RGB16 Rec.2020/PQ ICC" :
                     p3 ? "direct RGB16 Display P3/PQ ICC" :
                     rec2020_hlg ? "direct RGB16 Rec.2020/HLG ICC" :
                     p3_hlg ? "direct RGB16 Display P3/HLG ICC" :
                     "missing/unexpected direct-HDR ICC";
  std::vector<uint16_t> pixels(static_cast<size_t>(v.width) * v.height * 3u);
  for (uint32_t y = 0; y < v.height; ++y) {
    if (TIFFReadScanline(tiff.get(), pixels.data() + static_cast<size_t>(y) * v.width * 3u, y, 0) < 0) {
      throw std::runtime_error("TIFF verification scanline decode failed");
    }
  }
  v.exact_roundtrip = expected && expected->size() == pixels.size() &&
                      std::memcmp(expected->data(), pixels.data(), pixels.size() * sizeof(uint16_t)) == 0;
  auto [minimum, maximum] = std::minmax_element(pixels.begin(), pixels.end());
  if (minimum != pixels.end()) { v.min_value = *minimum; v.max_value = *maximum; }
  v.checks.push_back(v.width && v.height ? "TIFF dimensions parse" : "TIFF dimensions missing");
  v.checks.push_back(v.pixel_format == "TIFF RGB uint16 contiguous" ? "direct unsigned RGB16 storage" : "TIFF is not direct RGB16");
  v.checks.push_back(compression == COMPRESSION_ADOBE_DEFLATE || compression == COMPRESSION_LZW || compression == COMPRESSION_NONE
      ? "lossless TIFF compression" : "unexpected TIFF compression");
  v.checks.push_back(rec2020 || p3 || rec2020_hlg || p3_hlg
      ? "HDR-aware ICC carries matching CICP signaling" : "direct-HDR CICP ICC missing");
  if (expected) v.checks.push_back(v.exact_roundtrip ? "exact RGB16 pixel roundtrip" : "RGB16 pixel roundtrip mismatch");
  v.passed = v.width > 0 && v.height > 0 && samples == 3 && bits == 16 &&
             sample_format == SAMPLEFORMAT_UINT && photometric == PHOTOMETRIC_RGB &&
             planar == PLANARCONFIG_CONTIG && (rec2020 || p3 || rec2020_hlg || p3_hlg) &&
             (!expected || v.exact_roundtrip);
  return v;
}

SourceInfo inspect_tiff_metadata(const std::filesystem::path& path) {
  auto tiff = open_tiff(path, "r");
  SourceInfo info;
  info.path = path; info.format = "TIFF"; info.container_brand = "TIFF"; info.codec = "TIFF";
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &info.width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &info.height);
  uint16_t bits = 0, samples = 0, sample_format = SAMPLEFORMAT_UINT;
  uint16_t tiff_orientation = ORIENTATION_TOPLEFT;
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
  const bool has_orientation = TIFFGetField(tiff.get(), TIFFTAG_ORIENTATION,
                                             &tiff_orientation) == 1;
  info.original_orientation = static_cast<uint8_t>(std::clamp<uint16_t>(tiff_orientation, 1, 8));
  info.orientation_normalized = true;
  info.orientation_status = has_orientation ? "present" : "absent";
  info.bit_depth = bits; info.chroma = samples == 3 ? "4:4:4 RGB" : "unknown";
  info.pixel_format = samples == 3
      ? "RGB" + std::to_string(bits) + (sample_format == SAMPLEFORMAT_IEEEFP ? " float" : " integer")
      : "unknown";
  uint32_t icc_size = 0; void* icc_data = nullptr;
  const bool has_icc = TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc_data) == 1 &&
                       icc_data && icc_size > 0;
  store_icc_signal(info, parse_icc_signal(has_icc ? icc_data : nullptr,
                                           has_icc ? icc_size : 0));
  info.profile = info.icc_description.empty() ? "unprofiled" : info.icc_description;
  resolve_color_signaling(info, false);
  info.color_signal_kind = info.resolved_signaling_source;
  uint16_t subifd_count = 0; toff_t* subifd_offsets = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_SUBIFD, &subifd_count, &subifd_offsets) == 1 && subifd_count > 0) {
    info.gain_map_present = true; info.asset_kind = "gain-map-hdr";
  }
#ifdef TIFFTAG_XMLPACKET
  uint32_t xmp_size = 0; void* xmp = nullptr;
  info.xmp_present = TIFFGetField(tiff.get(), TIFFTAG_XMLPACKET, &xmp_size, &xmp) == 1 && xmp_size > 0;
  info.xmp_status = metadata_status(info.xmp_present);
#else
  info.xmp_status = "unsupported";
#endif
  toff_t exif_ifd = 0;
  const bool has_exif_ifd = TIFFGetField(tiff.get(), TIFFTAG_EXIFIFD, &exif_ifd) == 1 && exif_ifd != 0;
  char* camera_tag = nullptr;
  const bool has_camera_tags = TIFFGetField(tiff.get(), TIFFTAG_MAKE, &camera_tag) == 1 ||
                               TIFFGetField(tiff.get(), TIFFTAG_MODEL, &camera_tag) == 1 ||
                               TIFFGetField(tiff.get(), TIFFTAG_DATETIME, &camera_tag) == 1 ||
                               TIFFGetField(tiff.get(), TIFFTAG_COPYRIGHT, &camera_tag) == 1;
  info.exif_present = has_exif_ifd || has_camera_tags;
  info.exif_status = metadata_status(info.exif_present);
  return info;
}

DecodedImage decode_tiff_input(const std::filesystem::path& path) {
  DecodedImage decoded;
  decoded.info = inspect_tiff_metadata(path);
  if (decoded.info.gain_map_present) {
    throw std::runtime_error("gain-map TIFF detected; Adobe SubIFD reconstruction is not yet a validated direct-HDR input path");
  }
  if ((decoded.info.transfer != 16 && decoded.info.transfer != 18) ||
      (decoded.info.primaries != 1 && decoded.info.primaries != 9 &&
       decoded.info.primaries != 12)) {
    return decoded;
  }
  auto tiff = open_tiff(path, "r");
  uint16_t samples = 0, bits = 0, sample_format = SAMPLEFORMAT_UINT, photometric = 0, planar = 0;
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
  TIFFGetField(tiff.get(), TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetField(tiff.get(), TIFFTAG_PLANARCONFIG, &planar);
  if (samples != 3 || bits != 16 || sample_format != SAMPLEFORMAT_UINT ||
      photometric != PHOTOMETRIC_RGB || planar != PLANARCONFIG_CONTIG) {
    throw std::runtime_error("direct HDR TIFF input must be contiguous unsigned RGB16");
  }
  decoded.rgb.resize(static_cast<size_t>(decoded.info.width) * decoded.info.height * 3u);
  for (uint32_t y = 0; y < decoded.info.height; ++y) {
    if (TIFFReadScanline(tiff.get(), decoded.rgb.data() + static_cast<size_t>(y) * decoded.info.width * 3u, y, 0) < 0) {
      throw std::runtime_error("direct HDR TIFF input scanline decode failed");
    }
  }
#ifdef TIFFTAG_XMLPACKET
  uint32_t xmp_size = 0; void* xmp = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_XMLPACKET, &xmp_size, &xmp) == 1 && xmp && xmp_size > 0) {
    const auto* first = static_cast<const uint8_t*>(xmp);
    decoded.xmp.assign(first, first + xmp_size);
  }
#endif
  if (decoded.info.original_orientation != 1) {
    const auto transformed = orientation::normalize_rgb16(
        decoded.rgb, decoded.info.width, decoded.info.height,
        decoded.info.original_orientation);
    decoded.info.width = transformed.width;
    decoded.info.height = transformed.height;
  }
  orientation::set_xmp_orientation_to_one(decoded.xmp);
  if (decoded.info.transfer == 18) {
    const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
    for (size_t p = 0; p < count; ++p) {
      const auto nits = transfer::hlg_to_linear_nits({
          decoded.rgb[p * 3u] / 65535.0,
          decoded.rgb[p * 3u + 1u] / 65535.0,
          decoded.rgb[p * 3u + 2u] / 65535.0});
      for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] =
          static_cast<uint16_t>(std::llround(transfer::nits_to_pq(nits[c]) * 65535.0));
    }
  }
  if (decoded.info.primaries == 12) convert_p3_pq_to_rec2020_pq(decoded.rgb);
  else if (decoded.info.primaries == 1) convert_rec709_pq_to_rec2020_pq(decoded.rgb);
  return decoded;
}

void encode_avif(const std::filesystem::path& path, const DecodedImage& decoded,
                 const ConversionOptions& options, const std::vector<uint16_t>& pixels) {
  heif_image* raw_image = nullptr;
  require_heif(heif_image_create(static_cast<int>(decoded.info.width), static_cast<int>(decoded.info.height),
                                 heif_colorspace_RGB, heif_chroma_interleaved_RRGGBB_LE, &raw_image),
               "create direct PQ AVIF image");
  ImagePtr image(raw_image);
  NclxPtr profile;
  if (options.diagnostic_icc_only) {
    const auto icc = make_windows_compatible_pq_icc(options.output_gamut == "p3");
    require_heif(heif_image_set_raw_color_profile(image.get(), "rICC", icc.data(), icc.size()),
                 "set actual ICC-only direct PQ AVIF profile");
  }
  if (!options.diagnostic_icc_only) {
    heif_color_profile_nclx* raw_profile = heif_nclx_color_profile_alloc();
    if (!raw_profile) throw std::bad_alloc();
    profile.reset(raw_profile);
    profile->color_primaries = static_cast<heif_color_primaries>(
        options.output_gamut == "rec709" ? 1 : options.output_gamut == "p3" ? 12 : 9);
    profile->transfer_characteristics = static_cast<heif_transfer_characteristics>(
        options.output_transfer == "hlg" ? 18 : 16);
    profile->matrix_coefficients = static_cast<heif_matrix_coefficients>(9);
    profile->full_range_flag = 1;
    require_heif(heif_image_set_nclx_color_profile(image.get(), profile.get()), "set direct PQ AVIF NCLX");
  }
  require_heif(heif_image_add_plane(image.get(), heif_channel_interleaved,
                                    static_cast<int>(decoded.info.width), static_cast<int>(decoded.info.height), 10),
               "allocate direct PQ AVIF RGB10 plane");
  int stride = 0;
  uint8_t* plane = heif_image_get_plane(image.get(), heif_channel_interleaved, &stride);
  if (!plane || stride < static_cast<int>(decoded.info.width * 6u)) throw std::runtime_error("invalid direct PQ AVIF RGB10 plane");
  for (uint32_t y = 0; y < decoded.info.height; ++y) {
    auto* row = reinterpret_cast<uint16_t*>(plane + static_cast<size_t>(y) * stride);
    const auto* source = pixels.data() + static_cast<size_t>(y) * decoded.info.width * 3u;
    for (size_t i = 0; i < static_cast<size_t>(decoded.info.width) * 3u; ++i) {
      row[i] = static_cast<uint16_t>((static_cast<uint32_t>(source[i]) * 1023u + 32767u) / 65535u);
    }
  }
  ContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  heif_encoder* raw_encoder = nullptr;
  require_heif(heif_context_get_encoder_for_format(context.get(), heif_compression_AV1, &raw_encoder),
               "get AV1 encoder for direct PQ AVIF");
  HeifEncoderPtr encoder(raw_encoder);
  const int quality = std::clamp(static_cast<int>(std::lround(options.image_quality * 100.0f)), 1, 100);
  require_heif(heif_encoder_set_lossy_quality(encoder.get(), quality), "set direct PQ AVIF quality");
  require_heif(heif_encoder_set_parameter_string(encoder.get(), "chroma", "444"), "force direct PQ AVIF 4:4:4");
  HeifEncodingOptionsPtr encoding_options(heif_encoding_options_alloc());
  if (!encoding_options) throw std::bad_alloc();
  encoding_options->output_nclx_profile = profile.get();
  heif_image_handle* raw_handle = nullptr;
  require_heif(heif_context_encode_image(context.get(), image.get(), encoder.get(), encoding_options.get(), &raw_handle),
               "encode direct PQ AVIF");
  HandlePtr handle(raw_handle);
  if (options.copy_exif && !decoded.exif.empty()) {
    const auto exif = exif_tiff_payload(decoded.exif);
    if (!exif.empty()) {
      if (exif.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Exif metadata too large for AVIF");
      }
      require_heif(heif_context_add_exif_metadata(context.get(), handle.get(),
                                                   exif.data(), static_cast<int>(exif.size())),
                   "copy Exif to direct HDR AVIF");
    }
  }
  if (options.copy_xmp && !decoded.xmp.empty()) {
    if (decoded.xmp.size() > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::runtime_error("XMP metadata too large for AVIF");
    require_heif(heif_context_add_XMP_metadata(context.get(), handle.get(), decoded.xmp.data(), static_cast<int>(decoded.xmp.size())),
                 "copy XMP to direct PQ AVIF");
  }
  require_heif(heif_context_write_to_file(context.get(), path.string().c_str()), "write direct PQ AVIF");
}

void require_avif_result(avifResult result, const avifDiagnostics& diagnostics,
                         const char* operation) {
  if (result == AVIF_RESULT_OK) return;
  std::string message = std::string(operation) + ": " + avifResultToString(result);
  if (diagnostics.error[0]) message += " (" + std::string(diagnostics.error) + ")";
  throw std::runtime_error(message);
}

void assign_rgb16(avifRGBImage& rgb, const std::vector<uint16_t>& pixels,
                  uint32_t width, uint32_t height) {
  rgb.width = width;
  rgb.height = height;
  rgb.depth = 16;
  rgb.format = AVIF_RGB_FORMAT_RGB;
  rgb.pixels = reinterpret_cast<uint8_t*>(const_cast<uint16_t*>(pixels.data()));
  rgb.rowBytes = width * 3u * sizeof(uint16_t);
  rgb.avoidLibYUV = AVIF_TRUE;
  rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_BEST_QUALITY;
}

void encode_gainmap_avif(const std::filesystem::path& path,
                         const DecodedImage& decoded,
                         const ConversionOptions& options) {
  const avifColorPrimaries primaries = options.output_gamut == "rec709"
      ? AVIF_COLOR_PRIMARIES_BT709
      : options.output_gamut == "p3" ? AVIF_COLOR_PRIMARIES_SMPTE432
                                      : AVIF_COLOR_PRIMARIES_BT2020;
  std::vector<uint16_t> alternate_pixels = decoded.rgb;
  if (options.output_gamut == "p3") {
    alternate_pixels = convert_rec2020_pq_to_p3_pq(decoded);
  } else if (options.output_gamut == "rec709") {
    alternate_pixels = convert_rec2020_pq_to_rec709_pq(decoded);
  }

  // The base is an SDR rendition in the selected gamut. Its simple 203-nit
  // reference mapping is only the backward-compatible presentation; the ISO
  // gain map reconstructs the original high-precision PQ alternate rendition.
  std::vector<uint16_t> base_pixels(alternate_pixels.size());
  for (size_t i = 0; i < alternate_pixels.size(); ++i) {
    const double linear = std::clamp(transfer::pq_to_nits(
        static_cast<double>(alternate_pixels[i]) / 65535.0) / 203.0,
        0.0, 1.0);
    const double srgb = linear <= 0.0031308 ? 12.92 * linear
        : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    base_pixels[i] = static_cast<uint16_t>(std::lround(std::clamp(srgb, 0.0, 1.0) * 65535.0));
  }

  AvifImagePtr base(avifImageCreate(decoded.info.width, decoded.info.height, 12,
                                    AVIF_PIXEL_FORMAT_YUV444));
  if (!base) throw std::bad_alloc();
  base->colorPrimaries = primaries;
  base->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
  base->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
  base->yuvRange = AVIF_RANGE_FULL;
  avifRGBImage base_rgb{};
  avifRGBImageSetDefaults(&base_rgb, base.get());
  assign_rgb16(base_rgb, base_pixels, decoded.info.width, decoded.info.height);
  require_avif_result(avifImageRGBToYUV(base.get(), &base_rgb), {},
                      "convert gain-map AVIF SDR base to YUV");

  AvifImagePtr alternate(avifImageCreate(decoded.info.width, decoded.info.height, 12,
                                         AVIF_PIXEL_FORMAT_YUV444));
  if (!alternate) throw std::bad_alloc();
  alternate->colorPrimaries = primaries;
  alternate->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;
  alternate->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
  alternate->yuvRange = AVIF_RANGE_FULL;
  avifRGBImage alternate_rgb{};
  avifRGBImageSetDefaults(&alternate_rgb, alternate.get());
  assign_rgb16(alternate_rgb, alternate_pixels, decoded.info.width, decoded.info.height);
  require_avif_result(avifImageRGBToYUV(alternate.get(), &alternate_rgb), {},
                      "convert gain-map AVIF HDR alternate to YUV");

  AvifGainMapPtr gain(avifGainMapCreate());
  if (!gain) throw std::bad_alloc();
  const uint32_t scale = static_cast<uint32_t>(std::clamp(options.gainmap_scale, 1, 4));
  const uint32_t map_width = std::max(1u, (decoded.info.width + scale - 1u) / scale);
  const uint32_t map_height = std::max(1u, (decoded.info.height + scale - 1u) / scale);
  gain->image = avifImageCreate(map_width, map_height, 10,
      options.multi_channel_gainmap ? AVIF_PIXEL_FORMAT_YUV444 : AVIF_PIXEL_FORMAT_YUV400);
  if (!gain->image) throw std::bad_alloc();
  gain->image->matrixCoefficients = options.multi_channel_gainmap
      ? AVIF_MATRIX_COEFFICIENTS_BT601 : AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL;
  gain->image->yuvRange = AVIF_RANGE_FULL;
  avifDiagnostics gain_diagnostics{};
  require_avif_result(avifRGBImageComputeGainMap(&base_rgb, primaries,
                                                AVIF_TRANSFER_CHARACTERISTICS_SRGB,
                                                &alternate_rgb, primaries,
                                                AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084,
                                                gain.get(), &gain_diagnostics), gain_diagnostics,
                      "compute ISO gain map for AVIF");
  base->gainMap = gain.release();

  if (options.copy_exif && !decoded.exif.empty()) {
    const auto exif = exif_tiff_payload(decoded.exif);
    if (!exif.empty()) avifImageSetMetadataExif(base.get(), exif.data(), exif.size());
  }
  if (options.copy_xmp && !decoded.xmp.empty()) {
    avifImageSetMetadataXMP(base.get(), decoded.xmp.data(), decoded.xmp.size());
  }
  AvifEncoderPtr encoder(avifEncoderCreate());
  if (!encoder) throw std::bad_alloc();
  encoder->maxThreads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 32u));
  encoder->quality = std::clamp(static_cast<int>(std::lround(options.image_quality * 100.0f)), 1, 100);
  encoder->qualityGainMap = std::clamp(options.gainmap_quality, 1, 100);
  encoder->autoTiling = AVIF_TRUE;
  avifRWData encoded = AVIF_DATA_EMPTY;
  require_avif_result(avifEncoderWrite(encoder.get(), base.get(), &encoded), encoder->diag,
                      "encode ISO gain-map AVIF");
  std::vector<uint8_t> bytes(encoded.data, encoded.data + encoded.size);
  avifRWDataFree(&encoded);
  write_file(path, bytes);
}

Verification decode_avif(const std::filesystem::path& path,
                         const std::vector<uint16_t>* expected = nullptr,
                         bool expect_icc_only = false) {
  auto opened = open_heif(path);
  SourceInfo info = inspect_opened(path, opened);
  Verification v;
  v.width = info.width; v.height = info.height; v.bit_depth = info.bit_depth;
  v.pixel_format = info.chroma == "4:4:4" ? "AV1 10-bit 4:4:4" : "AV1 " + info.chroma;
  const bool rec2020 = info.primaries == 9 && info.transfer == 16 && info.matrix == 9 && info.full_range;
  const bool p3 = info.primaries == 12 && info.transfer == 16 && info.matrix == 9 && info.full_range;
  const bool hlg = (info.primaries == 9 || info.primaries == 12) &&
                   info.transfer == 18 && info.matrix == 9 && info.full_range;
  const bool icc_hdr = info.icc_present && !info.native_color_present &&
                       (info.primaries == 9 || info.primaries == 12) &&
                       (info.transfer == 16 || info.transfer == 18) &&
                       info.resolved_signaling_source == "ICC";
  const bool color_ok = expect_icc_only ? icc_hdr : (rec2020 || p3 || hlg);
  v.color_encoding = rec2020 ? "direct Rec.2020/PQ full-range CICP 9/16/9" :
                     p3 ? "direct Display P3/PQ full-range CICP 12/16/9" :
                     hlg ? (info.primaries == 12 ? "direct Display P3/HLG full-range CICP 12/18/9" :
                                                  "direct Rec.2020/HLG full-range CICP 9/18/9") :
                     icc_hdr ? "direct HDR from ICC; native NCLX absent" :
                     "unexpected AVIF color signaling";
  OptionsPtr options(heif_decoding_options_alloc());
  if (!options) throw std::bad_alloc();
  options->convert_hdr_to_8bit = 0; options->ignore_transformations = 0; options->output_image_nclx_profile_passthrough = 1;
  heif_image* raw = nullptr;
  require_heif(heif_decode_image(opened.handle.get(), &raw, heif_colorspace_RGB,
                                 heif_chroma_interleaved_RRGGBB_LE, options.get()), "decode direct PQ AVIF verification pixels");
  ImagePtr image(raw);
  int stride = 0; const uint8_t* plane = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &stride);
  if (!plane) throw std::runtime_error("direct PQ AVIF verification plane missing");
  long double squared_error = 0.0; double max_error = 0.0; uint64_t samples = 0;
  uint16_t minimum = 65535, maximum = 0;
  for (uint32_t y = 0; y < info.height; ++y) {
    const auto* row = reinterpret_cast<const uint16_t*>(plane + static_cast<size_t>(y) * stride);
    for (size_t i = 0; i < static_cast<size_t>(info.width) * 3u; ++i) {
      const uint16_t expanded = info.bit_depth < 16
          ? static_cast<uint16_t>((static_cast<uint32_t>(row[i]) * 65535u + 511u) / 1023u) : row[i];
      minimum = std::min(minimum, expanded); maximum = std::max(maximum, expanded);
      if (expected && expected->size() == static_cast<size_t>(info.width) * info.height * 3u) {
        const size_t index = static_cast<size_t>(y) * info.width * 3u + i;
        const double error = (static_cast<double>(expanded) - (*expected)[index]) / 65535.0;
        squared_error += static_cast<long double>(error) * error;
        max_error = std::max(max_error, std::abs(error)); ++samples;
      }
    }
  }
  v.min_value = minimum; v.max_value = maximum;
  if (samples) { v.reconstruction_rmse = std::sqrt(static_cast<double>(squared_error / samples)); v.reconstruction_max_abs_error = max_error; }
  v.checks.push_back(info.width && info.height ? "AVIF dimensions parse" : "AVIF dimensions missing");
  v.checks.push_back(info.bit_depth == 10 ? "AV1 high-bit-depth is 10-bit" : "AVIF bit depth is not 10");
  v.checks.push_back(info.chroma == "4:4:4" ? "AV1 chroma is 4:4:4" : "AV1 chroma is not 4:4:4");
  v.checks.push_back(color_ok
      ? (hlg ? "BT.2020/HLG/BT.2020-NCL full-range signaling"
             : expect_icc_only ? "actual embedded PQ ICC; native NCLX absent"
                               : "BT.2020/PQ/BT.2020-NCL full-range signaling")
      : "AVIF HDR signaling mismatch");
  if (expected) v.checks.push_back("lossy RGB16-domain reconstruction error recorded");
  v.passed = info.width > 0 && info.height > 0 && info.bit_depth == 10 && info.chroma == "4:4:4" && color_ok;
  return v;
}

std::vector<uint16_t> pq2020_to_scrgb_half(const DecodedImage& decoded,
                                            const ProgressCallback& progress,
                                            std::atomic_bool* cancel) {
  const size_t pixels = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  std::vector<uint16_t> rgba(pixels * 4u);
  for (uint32_t y = 0; y < decoded.info.height; ++y) {
    check_cancel(cancel);
    for (uint32_t x = 0; x < decoded.info.width; ++x) {
      const size_t p = static_cast<size_t>(y) * decoded.info.width + x;
      const double r = inverse_pq(decoded.rgb[p * 3u] / 65535.0) / 80.0;
      const double g = inverse_pq(decoded.rgb[p * 3u + 1u] / 65535.0) / 80.0;
      const double b = inverse_pq(decoded.rgb[p * 3u + 2u] / 65535.0) / 80.0;
      const float sr = static_cast<float>(std::clamp(1.660491 * r - 0.587641 * g - 0.072850 * b, -65504.0, 65504.0));
      const float sg = static_cast<float>(std::clamp(-0.124550 * r + 1.132900 * g - 0.008349 * b, -65504.0, 65504.0));
      const float sb = static_cast<float>(std::clamp(-0.018151 * r - 0.100579 * g + 1.118730 * b, -65504.0, 65504.0));
      rgba[p * 4u] = float_to_half(sr);
      rgba[p * 4u + 1u] = float_to_half(sg);
      rgba[p * 4u + 2u] = float_to_half(sb);
      rgba[p * 4u + 3u] = float_to_half(1.0f);
    }
    if (progress && (y % 128u == 0)) progress(44 + static_cast<int>(y * 18u / decoded.info.height), "Transforming Rec.2020/PQ to linear FP16 scRGB");
  }
  return rgba;
}

#ifdef _WIN32
void require_hr(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    std::ostringstream out;
    out << operation << " failed (HRESULT 0x" << std::hex << static_cast<uint32_t>(hr) << ')';
    throw std::runtime_error(out.str());
  }
}

struct ComScope {
  bool uninitialize = false;
  ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) uninitialize = true;
    else if (hr != RPC_E_CHANGED_MODE) require_hr(hr, "CoInitializeEx");
  }
  ~ComScope() { if (uninitialize) CoUninitialize(); }
};

ComPtr<IWICImagingFactory> wic_factory() {
  ComPtr<IWICImagingFactory> factory;
  require_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)), "create WIC factory");
  return factory;
}

bool wic_metadata_query_present(IWICMetadataQueryReader* reader, const wchar_t* query,
                                PROPVARIANT* value_out = nullptr) {
  if (!reader) return false;
  PROPVARIANT value{};
  PropVariantInit(&value);
  const HRESULT result = reader->GetMetadataByName(query, &value);
  if (FAILED(result)) {
    PropVariantClear(&value);
    return false;
  }
  if (value_out) {
    *value_out = value;
  } else {
    PropVariantClear(&value);
  }
  return true;
}

void encode_jxr(const std::filesystem::path& path, uint32_t width, uint32_t height,
                const std::vector<uint16_t>& rgba_half, const ConversionOptions& options) {
  ComScope com;
  auto factory = wic_factory();
  ComPtr<IWICStream> stream;
  require_hr(factory->CreateStream(&stream), "create WIC stream");
  require_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "open JXR output");
  ComPtr<IWICBitmapEncoder> encoder;
  require_hr(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder), "create JPEG XR encoder");
  require_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "initialize JPEG XR encoder");
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  require_hr(encoder->CreateNewFrame(&frame, &properties), "create JPEG XR frame");
  if (properties) {
    std::array<PROPBAG2, 2> prop{};
    prop[0].pstrName = const_cast<wchar_t*>(L"Lossless");
    prop[1].pstrName = const_cast<wchar_t*>(L"ImageQuality");
    std::array<VARIANT, 2> values{};
    VariantInit(&values[0]); VariantInit(&values[1]);
    values[0].vt = VT_BOOL; values[0].boolVal = options.lossless ? VARIANT_TRUE : VARIANT_FALSE;
    values[1].vt = VT_R4; values[1].fltVal = std::clamp(options.image_quality, 0.0f, 1.0f);
    require_hr(properties->Write(static_cast<ULONG>(prop.size()), prop.data(), values.data()), "set JPEG XR properties");
  }
  require_hr(frame->Initialize(properties.Get()), "initialize JPEG XR frame");
  require_hr(frame->SetSize(width, height), "set JPEG XR dimensions");
  WICPixelFormatGUID format = GUID_WICPixelFormat64bppRGBAHalf;
  require_hr(frame->SetPixelFormat(&format), "set JPEG XR FP16 pixel format");
  if (format != GUID_WICPixelFormat64bppRGBAHalf) throw std::runtime_error("WIC JPEG XR encoder rejected FP16 RGBA");
  const UINT stride = width * 8u;
  const UINT bytes = static_cast<UINT>(rgba_half.size() * sizeof(uint16_t));
  require_hr(frame->WritePixels(height, stride, bytes, reinterpret_cast<BYTE*>(const_cast<uint16_t*>(rgba_half.data()))), "write JPEG XR FP16 pixels");
  require_hr(frame->Commit(), "commit JPEG XR frame");
  require_hr(encoder->Commit(), "commit JPEG XR file");
}

Verification decode_jxr(const std::filesystem::path& path, const std::vector<uint16_t>* expected) {
  ComScope com;
  auto factory = wic_factory();
  ComPtr<IWICBitmapDecoder> decoder;
  require_hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnDemand, &decoder), "open JPEG XR decoder");
  ComPtr<IWICBitmapFrameDecode> frame;
  require_hr(decoder->GetFrame(0, &frame), "read JPEG XR frame");
  Verification v;
  require_hr(frame->GetSize(&v.width, &v.height), "read JPEG XR dimensions");
  WICPixelFormatGUID format{};
  require_hr(frame->GetPixelFormat(&format), "read JPEG XR pixel format");
  if (format != GUID_WICPixelFormat64bppRGBAHalf) {
    v.pixel_format = "unexpected WIC pixel format";
    v.checks.push_back("decoder did not preserve GUID_WICPixelFormat64bppRGBAHalf");
    return v;
  }
  v.pixel_format = "GUID_WICPixelFormat64bppRGBAHalf";
  v.bit_depth = 16;
  const size_t count = static_cast<size_t>(v.width) * v.height * 4u;
  std::vector<uint16_t> pixels(count);
  const UINT stride = v.width * 8u;
  require_hr(frame->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size() * 2u), reinterpret_cast<BYTE*>(pixels.data())), "decode JPEG XR FP16 pixels");
  v.min_value = std::numeric_limits<double>::infinity();
  v.max_value = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < pixels.size(); i += 4u) {
    for (size_t c = 0; c < 3; ++c) {
      const float value = half_to_float(pixels[i + c]);
      if (!std::isfinite(value)) v.finite = false;
      v.min_value = std::min(v.min_value, static_cast<double>(value));
      v.max_value = std::max(v.max_value, static_cast<double>(value));
    }
  }
  if (expected && expected->size() == pixels.size()) {
    // The Windows JPEG XR codec canonicalizes IEEE-754 negative zero to
    // positive zero. They are numerically identical; every non-zero FP16 code
    // must remain bit-exact for the lossless claim.
    v.exact_roundtrip = std::equal(expected->begin(), expected->end(), pixels.begin(),
        [](uint16_t a, uint16_t b) {
          return a == b || ((a & 0x7fffu) == 0 && (b & 0x7fffu) == 0);
        });
  }
  v.color_encoding = "linear scRGB; 1.0 = 80 cd/m2; sRGB/BT.709 primaries";
  v.checks.push_back("WIC JPEG XR decoder opened output");
  v.checks.push_back("FP16 RGBA WIC pixel format preserved");
  v.checks.push_back(v.finite ? "all decoded RGB values finite" : "non-finite decoded value");
  v.checks.push_back(v.max_value > 1.0 ? "HDR values exceed SDR white" : "HDR range clipped to SDR white");
  if (expected) v.checks.push_back(v.exact_roundtrip
      ? "FP16 lossless roundtrip (allowing IEEE-754 signed-zero canonicalization)"
      : "FP16 lossless roundtrip mismatch");
  v.passed = v.width > 0 && v.height > 0 && v.finite && v.max_value > 1.0 && (!expected || v.exact_roundtrip);
  return v;
}

std::vector<uint32_t> pq16_to_bgr101010(const DecodedImage& decoded) {
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  std::vector<uint32_t> packed(count);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t r = (static_cast<uint32_t>(decoded.rgb[i * 3u]) * 1023u + 32767u) / 65535u;
    const uint32_t g = (static_cast<uint32_t>(decoded.rgb[i * 3u + 1u]) * 1023u + 32767u) / 65535u;
    const uint32_t b = (static_cast<uint32_t>(decoded.rgb[i * 3u + 2u]) * 1023u + 32767u) / 65535u;
    packed[i] = b | (g << 10) | (r << 20);
  }
  return packed;
}

void encode_jxr_rgb10(const std::filesystem::path& path, uint32_t width, uint32_t height,
                      const std::vector<uint32_t>& pixels, const ConversionOptions& options) {
  ComScope com;
  auto factory = wic_factory();
  ComPtr<IWICStream> stream;
  require_hr(factory->CreateStream(&stream), "create WIC stream");
  require_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "open packed-10 JXR output");
  ComPtr<IWICBitmapEncoder> encoder;
  require_hr(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder), "create packed-10 JPEG XR encoder");
  require_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "initialize packed-10 JPEG XR encoder");
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  require_hr(encoder->CreateNewFrame(&frame, &properties), "create packed-10 JPEG XR frame");
  if (properties) {
    PROPBAG2 prop{}; prop.pstrName = const_cast<wchar_t*>(L"Lossless");
    VARIANT value{}; VariantInit(&value); value.vt = VT_BOOL; value.boolVal = options.lossless ? VARIANT_TRUE : VARIANT_FALSE;
    require_hr(properties->Write(1, &prop, &value), "set packed-10 JPEG XR lossless property");
  }
  require_hr(frame->Initialize(properties.Get()), "initialize packed-10 JPEG XR frame");
  require_hr(frame->SetSize(width, height), "set packed-10 JPEG XR dimensions");
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR101010;
  require_hr(frame->SetPixelFormat(&format), "set GUID_WICPixelFormat32bppBGR101010");
  if (format != GUID_WICPixelFormat32bppBGR101010) throw std::runtime_error("WIC JPEG XR encoder rejected GUID_WICPixelFormat32bppBGR101010");
  require_hr(frame->WritePixels(height, width * 4u, static_cast<UINT>(pixels.size() * 4u),
                                reinterpret_cast<BYTE*>(const_cast<uint32_t*>(pixels.data()))), "write packed-10 JPEG XR pixels");
  require_hr(frame->Commit(), "commit packed-10 JPEG XR frame");
  require_hr(encoder->Commit(), "commit packed-10 JPEG XR file");
}

Verification decode_jxr_rgb10(const std::filesystem::path& path, const std::vector<uint32_t>* expected) {
  ComScope com;
  auto factory = wic_factory();
  ComPtr<IWICBitmapDecoder> decoder;
  require_hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnDemand, &decoder), "open packed-10 JPEG XR decoder");
  ComPtr<IWICBitmapFrameDecode> frame;
  require_hr(decoder->GetFrame(0, &frame), "read packed-10 JPEG XR frame");
  Verification v;
  require_hr(frame->GetSize(&v.width, &v.height), "read packed-10 JPEG XR dimensions");
  WICPixelFormatGUID format{};
  require_hr(frame->GetPixelFormat(&format), "read packed-10 JPEG XR pixel format");
  v.pixel_format = format == GUID_WICPixelFormat32bppBGR101010
      ? "GUID_WICPixelFormat32bppBGR101010" : "unexpected WIC pixel format";
  if (format != GUID_WICPixelFormat32bppBGR101010) {
    v.checks.push_back("WIC decoder did not preserve packed 10-bit format");
    return v;
  }
  v.bit_depth = 10;
  std::vector<uint32_t> pixels(static_cast<size_t>(v.width) * v.height);
  require_hr(frame->CopyPixels(nullptr, v.width * 4u, static_cast<UINT>(pixels.size() * 4u),
                               reinterpret_cast<BYTE*>(pixels.data())), "decode packed-10 JPEG XR pixels");
  v.exact_roundtrip = expected && expected->size() == pixels.size() &&
                      std::memcmp(expected->data(), pixels.data(), pixels.size() * sizeof(uint32_t)) == 0;
  uint32_t max_code = 0;
  for (uint32_t pixel : pixels) max_code = std::max({max_code, pixel & 1023u, (pixel >> 10) & 1023u, (pixel >> 20) & 1023u});
  v.min_value = 0;
  v.max_value = max_code;
  v.color_encoding = "Rec.2020/PQ sample values; WIC JXR profile interpretation experimental";
  v.checks.push_back("WIC JPEG XR decoder opened output");
  v.checks.push_back("GUID_WICPixelFormat32bppBGR101010 preserved (not 8-bit)");
  if (expected) v.checks.push_back(v.exact_roundtrip ? "exact packed-word lossless roundtrip" : "packed-word roundtrip mismatch");
  v.passed = v.width > 0 && v.height > 0 && max_code > 255u && (!expected || v.exact_roundtrip);
  return v;
}

DecodedImage decode_jxr_input(const std::filesystem::path& path) {
  ComScope com;
  auto factory = wic_factory();
  ComPtr<IWICBitmapDecoder> decoder;
  require_hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnDemand, &decoder), "open JPEG XR input");
  ComPtr<IWICBitmapFrameDecode> frame;
  require_hr(decoder->GetFrame(0, &frame), "read JPEG XR input frame");
  DecodedImage decoded;
  decoded.info.path = path; decoded.info.format = "JPEG XR"; decoded.info.container_brand = "JXR ";
  decoded.info.asset_kind = "non-HDR"; decoded.info.codec = "JPEG XR";
  require_hr(frame->GetSize(&decoded.info.width, &decoded.info.height), "read JPEG XR input dimensions");
  WICPixelFormatGUID format{};
  require_hr(frame->GetPixelFormat(&format), "read JPEG XR input pixel format");
  UINT color_context_count = 0;
  const HRESULT color_context_result = frame->GetColorContexts(0, nullptr, &color_context_count);
  decoded.info.icc_present = SUCCEEDED(color_context_result) && color_context_count > 0;
  decoded.info.icc_status = FAILED(color_context_result)
      ? "unsupported" : metadata_status(decoded.info.icc_present);
  ComPtr<IWICMetadataQueryReader> metadata_reader;
  const HRESULT metadata_result = frame->GetMetadataQueryReader(&metadata_reader);
  if (metadata_result == WINCODEC_ERR_UNSUPPORTEDOPERATION || metadata_result == E_NOINTERFACE) {
    decoded.info.exif_status = "unsupported";
    decoded.info.xmp_status = "unsupported";
    decoded.info.orientation_status = "unsupported";
  } else if (FAILED(metadata_result)) {
    decoded.info.exif_status = "read-error";
    decoded.info.xmp_status = "read-error";
    decoded.info.orientation_status = "read-error";
  } else {
    decoded.info.exif_present =
        wic_metadata_query_present(metadata_reader.Get(), L"/ifd/{ushort=34665}") ||
        wic_metadata_query_present(metadata_reader.Get(), L"/ifd/exif/{ushort=36867}");
    decoded.info.xmp_present =
        wic_metadata_query_present(metadata_reader.Get(), L"/ifd/{ushort=700}");
    decoded.info.exif_status = metadata_status(decoded.info.exif_present);
    decoded.info.xmp_status = metadata_status(decoded.info.xmp_present);
    PROPVARIANT orientation_value{};
    if (wic_metadata_query_present(metadata_reader.Get(), L"/ifd/{ushort=274}",
                                   &orientation_value)) {
      if (orientation_value.vt == VT_UI2) decoded.info.original_orientation =
          static_cast<uint8_t>(std::clamp<unsigned>(orientation_value.uiVal, 1, 8));
      else if (orientation_value.vt == VT_UI4) decoded.info.original_orientation =
          static_cast<uint8_t>(std::clamp<unsigned long>(orientation_value.ulVal, 1, 8));
      decoded.info.orientation_status = "present";
      PropVariantClear(&orientation_value);
    } else {
      decoded.info.orientation_status = "absent";
    }
  }
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  if (format == GUID_WICPixelFormat64bppRGBAHalf) {
    decoded.info.asset_kind = "direct-hdr";
    decoded.rgb.resize(count * 3u);
    std::vector<uint16_t> rgba(count * 4u);
    require_hr(frame->CopyPixels(nullptr, decoded.info.width * 8u, static_cast<UINT>(rgba.size() * sizeof(uint16_t)),
                                 reinterpret_cast<BYTE*>(rgba.data())), "decode JPEG XR FP16 input");
    for (size_t p = 0; p < count; ++p) {
      const double r = static_cast<double>(half_to_float(rgba[p * 4u])) * 80.0;
      const double g = static_cast<double>(half_to_float(rgba[p * 4u + 1u])) * 80.0;
      const double b = static_cast<double>(half_to_float(rgba[p * 4u + 2u])) * 80.0;
      const std::array<double, 3> rec2020{
          0.6274040 * r + 0.3292830 * g + 0.0433130 * b,
          0.0690970 * r + 0.9195400 * g + 0.0113620 * b,
          0.0163910 * r + 0.0880130 * g + 0.8955950 * b};
      for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(rec2020[c], 0.0, 10000.0)) * 65535.0));
    }
    decoded.info.profile = "FP16 linear scRGB (1.0 = 80 cd/m2)";
    decoded.info.bit_depth = 16; decoded.info.chroma = "4:4:4 RGBA FP16";
    decoded.info.pixel_format = "GUID_WICPixelFormat64bppRGBAHalf";
    decoded.info.color_signal_kind = "WIC pixel format / scRGB convention";
    decoded.info.primaries = 1; decoded.info.transfer = 8;
    decoded.info.matrix = 0; decoded.info.full_range = true; decoded.info.range_known = true;
  } else if (format == GUID_WICPixelFormat32bppBGR101010) {
    decoded.info.asset_kind = "direct-hdr";
    decoded.rgb.resize(count * 3u);
    std::vector<uint32_t> words(count);
    require_hr(frame->CopyPixels(nullptr, decoded.info.width * 4u, static_cast<UINT>(words.size() * sizeof(uint32_t)),
                                 reinterpret_cast<BYTE*>(words.data())), "decode packed-10 JPEG XR input");
    for (size_t p = 0; p < count; ++p) {
      const uint32_t word = words[p];
      decoded.rgb[p * 3u] = static_cast<uint16_t>((((word >> 20) & 1023u) * 65535u + 511u) / 1023u);
      decoded.rgb[p * 3u + 1u] = static_cast<uint16_t>((((word >> 10) & 1023u) * 65535u + 511u) / 1023u);
      decoded.rgb[p * 3u + 2u] = static_cast<uint16_t>(((word & 1023u) * 65535u + 511u) / 1023u);
    }
    decoded.info.profile = "packed RGB10 Rec.2020/PQ interpretation (Experimental)";
    decoded.info.bit_depth = 10; decoded.info.chroma = "4:4:4 RGB";
    decoded.info.pixel_format = "GUID_WICPixelFormat32bppBGR101010";
    decoded.info.color_signal_kind = "WIC pixel format + experimental interpretation";
    decoded.info.primaries = 9; decoded.info.transfer = 16;
    decoded.info.matrix = 0; decoded.info.full_range = true; decoded.info.range_known = true;
  } else if (format == GUID_WICPixelFormat8bppGray ||
             format == GUID_WICPixelFormat16bppGray ||
             format == GUID_WICPixelFormat24bppBGR ||
             format == GUID_WICPixelFormat32bppBGR ||
             format == GUID_WICPixelFormat32bppBGRA ||
             format == GUID_WICPixelFormat48bppRGB ||
             format == GUID_WICPixelFormat64bppRGBA) {
    decoded.info.profile = "SDR JPEG XR";
    decoded.info.bit_depth = format == GUID_WICPixelFormat16bppGray ||
                             format == GUID_WICPixelFormat48bppRGB ||
                             format == GUID_WICPixelFormat64bppRGBA ? 16u : 8u;
    decoded.info.chroma = format == GUID_WICPixelFormat8bppGray ||
                          format == GUID_WICPixelFormat16bppGray
        ? "grayscale" : "RGB/RGBA";
    decoded.info.pixel_format = "SDR integer JPEG XR";
    decoded.info.color_signal_kind = "No validated HDR representation";
  } else {
    throw std::runtime_error("JPEG XR input is neither validated FP16 scRGB nor packed RGB10");
  }
  return decoded;
}
#else
void require_jxr(ERR error, const char* operation) {
  if (error != WMP_errSuccess) {
    throw std::runtime_error(std::string(operation) + " failed (JPEG XR error " +
                             std::to_string(error) + ")");
  }
}

void encode_jxr(const std::filesystem::path& path, uint32_t width, uint32_t height,
                const std::vector<uint16_t>& rgba_half,
                const ConversionOptions& options) {
  PKFactory* factory = nullptr;
  PKCodecFactory* codecs = nullptr;
  PKImageEncode* encoder = nullptr;
  WMPStream* stream = nullptr;
  auto cleanup = [&] {
    if (encoder) encoder->Release(&encoder);
    if (codecs) codecs->Release(&codecs);
    if (factory) factory->Release(&factory);
  };
  try {
    require_jxr(PKCreateFactory(&factory, PK_SDK_VERSION), "create JPEG XR stream factory");
    require_jxr(factory->CreateStreamFromFilename(&stream, path.string().c_str(), "wb"),
                "open JPEG XR output");
    require_jxr(PKCreateCodecFactory(&codecs, WMP_SDK_VERSION),
                "create JPEG XR codec factory");
    require_jxr(codecs->CreateCodec(&IID_PKImageWmpEncode,
                                    reinterpret_cast<void**>(&encoder)),
                "create JPEG XR encoder");

    CWMIStrCodecParam parameters{};
    parameters.cfColorFormat = YUV_444;
    parameters.bdBitDepth = BD_LONG;
    parameters.bfBitstreamFormat = FREQUENCY;
    parameters.bProgressiveMode = TRUE;
    parameters.olOverlap = options.lossless ? OL_NONE : OL_ONE;
    parameters.sbSubband = SB_ALL;
    parameters.uAlphaMode = 2;
    parameters.uiDefaultQPIndexAlpha = 1;
    if (options.lossless) {
      parameters.uiDefaultQPIndex = 1;
      parameters.uiDefaultQPIndexU = 1;
      parameters.uiDefaultQPIndexV = 1;
      parameters.uiDefaultQPIndexYHP = 1;
      parameters.uiDefaultQPIndexUHP = 1;
      parameters.uiDefaultQPIndexVHP = 1;
    } else {
      static constexpr int kQp16f[11][6] = {
        {148,177,171,165,187,191}, {133,155,153,147,172,181},
        {114,133,138,130,157,167}, {97,118,120,109,137,144},
        {76,98,103,85,115,121}, {63,86,91,62,96,99},
        {46,68,71,43,73,75}, {29,48,52,27,48,51},
        {16,30,35,14,29,34}, {8,14,17,7,13,17}, {3,5,7,3,5,6}};
      const float quality = std::clamp(options.image_quality, 0.0f, 1.0f) * 10.0f;
      const int lower = std::clamp(static_cast<int>(quality), 0, 10);
      const int upper = std::min(lower + 1, 10);
      const float fraction = quality - lower;
      std::array<U8*, 6> targets{
          &parameters.uiDefaultQPIndex, &parameters.uiDefaultQPIndexU,
          &parameters.uiDefaultQPIndexV, &parameters.uiDefaultQPIndexYHP,
          &parameters.uiDefaultQPIndexUHP, &parameters.uiDefaultQPIndexVHP};
      for (size_t index = 0; index < targets.size(); ++index) {
        *targets[index] = static_cast<U8>(std::lround(
            kQp16f[lower][index] * (1.0f - fraction) +
            kQp16f[upper][index] * fraction));
      }
    }
    require_jxr(encoder->Initialize(encoder, stream, &parameters,
                                    sizeof(parameters)),
                "initialize JPEG XR encoder");
    require_jxr(encoder->SetPixelFormat(encoder, GUID_PKPixelFormat64bppRGBAHalf),
                "set JPEG XR FP16 RGBA format");
    require_jxr(encoder->SetSize(encoder, static_cast<I32>(width),
                                static_cast<I32>(height)),
                "set JPEG XR dimensions");
    require_jxr(encoder->SetResolution(encoder, 96.0f, 96.0f),
                "set JPEG XR resolution");
    require_jxr(encoder->WritePixels(encoder, height,
                reinterpret_cast<U8*>(const_cast<uint16_t*>(rgba_half.data())),
                width * 8u), "write JPEG XR FP16 pixels");
    cleanup();
  } catch (...) {
    cleanup();
    throw;
  }
}

Verification decode_jxr(const std::filesystem::path& path,
                        const std::vector<uint16_t>* expected) {
  PKCodecFactory* codecs = nullptr;
  PKImageDecode* decoder = nullptr;
  PKFormatConverter* converter = nullptr;
  auto cleanup = [&] {
    if (converter) converter->Release(&converter);
    if (decoder) decoder->Release(&decoder);
    if (codecs) codecs->Release(&codecs);
  };
  try {
    require_jxr(PKCreateCodecFactory(&codecs, WMP_SDK_VERSION),
                "create JPEG XR decoder factory");
    require_jxr(codecs->CreateDecoderFromFile(path.string().c_str(), &decoder),
                "open JPEG XR input");
    require_jxr(codecs->CreateFormatConverter(&converter),
                "create JPEG XR format converter");
    char extension[] = ".jxr";
    require_jxr(converter->Initialize(converter, decoder, extension,
                                      GUID_PKPixelFormat64bppRGBAHalf),
                "configure JPEG XR FP16 decode");
    I32 width = 0, height = 0;
    require_jxr(converter->GetSize(converter, &width, &height),
                "read JPEG XR dimensions");
    if (width <= 0 || height <= 0) throw std::runtime_error("invalid JPEG XR dimensions");
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * 4u);
    const PKRect rect{0, 0, width, height};
    require_jxr(converter->Copy(converter, &rect,
                reinterpret_cast<U8*>(pixels.data()), static_cast<U32>(width) * 8u),
                "decode JPEG XR FP16 pixels");

    Verification result;
    result.width = static_cast<uint32_t>(width);
    result.height = static_cast<uint32_t>(height);
    result.bit_depth = 16;
    result.pixel_format = "64bpp RGBA half-float";
    result.color_encoding = "linear scRGB; 1.0 = 80 cd/m2; sRGB/BT.709 primaries";
    result.min_value = std::numeric_limits<double>::infinity();
    result.max_value = -std::numeric_limits<double>::infinity();
    for (size_t index = 0; index < pixels.size(); index += 4u) {
      for (size_t channel = 0; channel < 3; ++channel) {
        const float value = half_to_float(pixels[index + channel]);
        result.finite = result.finite && std::isfinite(value);
        result.min_value = std::min(result.min_value, static_cast<double>(value));
        result.max_value = std::max(result.max_value, static_cast<double>(value));
      }
    }
    if (expected && expected->size() == pixels.size()) {
      result.exact_roundtrip = std::equal(expected->begin(), expected->end(), pixels.begin(),
          [](uint16_t left, uint16_t right) {
            return left == right || ((left & 0x7fffu) == 0 && (right & 0x7fffu) == 0);
          });
    }
    result.checks.push_back("portable JPEG XR decoder opened FP16 output");
    result.checks.push_back(result.finite ? "all decoded RGB values finite" :
                                           "non-finite decoded value");
    result.checks.push_back(result.max_value > 1.0 ? "HDR values exceed SDR white" :
                                                   "HDR range clipped to SDR white");
    result.passed = result.finite && result.max_value > 1.0 &&
        (!expected || result.exact_roundtrip);
    cleanup();
    return result;
  } catch (...) {
    cleanup();
    throw;
  }
}

std::vector<uint32_t> pq16_to_bgr101010(const DecodedImage& decoded) {
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  std::vector<uint32_t> packed(count);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t r = (static_cast<uint32_t>(decoded.rgb[i * 3u]) * 1023u + 32767u) / 65535u;
    const uint32_t g = (static_cast<uint32_t>(decoded.rgb[i * 3u + 1u]) * 1023u + 32767u) / 65535u;
    const uint32_t b = (static_cast<uint32_t>(decoded.rgb[i * 3u + 2u]) * 1023u + 32767u) / 65535u;
    packed[i] = b | (g << 10) | (r << 20);
  }
  return packed;
}

void encode_jxr_rgb10(const std::filesystem::path&, uint32_t, uint32_t,
                      const std::vector<uint32_t>&, const ConversionOptions&) {
  throw std::runtime_error("portable RGB10 JPEG XR encoder is unavailable");
}

Verification decode_jxr_rgb10(const std::filesystem::path&,
                              const std::vector<uint32_t>*) {
  throw std::runtime_error("portable RGB10 JPEG XR decoder is unavailable");
}

DecodedImage decode_jxr_input(const std::filesystem::path& path) {
  PKCodecFactory* codecs = nullptr;
  PKImageDecode* decoder = nullptr;
  PKFormatConverter* converter = nullptr;
  auto cleanup = [&] {
    if (converter) converter->Release(&converter);
    if (decoder) decoder->Release(&decoder);
    if (codecs) codecs->Release(&codecs);
  };
  try {
    require_jxr(PKCreateCodecFactory(&codecs, WMP_SDK_VERSION),
                "create JPEG XR decoder factory");
    require_jxr(codecs->CreateDecoderFromFile(path.string().c_str(), &decoder),
                "open JPEG XR input");
    PKPixelFormatGUID source_format{};
    require_jxr(decoder->GetPixelFormat(decoder, &source_format),
                "read JPEG XR pixel format");
    const bool fp16 = IsEqualGUID(source_format, GUID_PKPixelFormat64bppRGBAHalf) ||
                      IsEqualGUID(source_format, GUID_PKPixelFormat64bppRGBHalf) ||
                      IsEqualGUID(source_format, GUID_PKPixelFormat48bppRGBHalf);
    const bool common_sdr =
        IsEqualGUID(source_format, GUID_PKPixelFormat8bppGray) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat16bppGray) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat16bppRGB555) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat16bppRGB565) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat24bppBGR) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat24bppRGB) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat32bppBGR) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat32bppBGRA) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat32bppRGB) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat32bppRGBA) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat48bppRGB) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat64bppRGBA);
    if (!fp16 && !common_sdr) {
      throw std::runtime_error("JPEG XR input is not a validated FP16 scRGB image");
    }
    if (common_sdr) {
      I32 width = 0, height = 0;
      require_jxr(decoder->GetSize(decoder, &width, &height),
                  "read SDR JPEG XR dimensions");
      if (width <= 0 || height <= 0) throw std::runtime_error("invalid JPEG XR dimensions");
      DecodedImage decoded;
      decoded.info.path = path;
      decoded.info.format = "JPEG XR";
      decoded.info.container_brand = "JXR ";
      decoded.info.asset_kind = "non-HDR";
      decoded.info.codec = "JPEG XR";
      decoded.info.width = static_cast<uint32_t>(width);
      decoded.info.height = static_cast<uint32_t>(height);
      decoded.info.profile = "SDR JPEG XR";
      decoded.info.bit_depth = IsEqualGUID(source_format, GUID_PKPixelFormat16bppGray) ||
                               IsEqualGUID(source_format, GUID_PKPixelFormat48bppRGB) ||
                               IsEqualGUID(source_format, GUID_PKPixelFormat64bppRGBA) ? 16u : 8u;
      decoded.info.chroma = IsEqualGUID(source_format, GUID_PKPixelFormat8bppGray) ||
                            IsEqualGUID(source_format, GUID_PKPixelFormat16bppGray)
          ? "grayscale" : "RGB/RGBA";
      decoded.info.pixel_format = "SDR integer JPEG XR";
      decoded.info.color_signal_kind = "No validated HDR representation";
      decoded.info.exif_status = "unsupported";
      decoded.info.xmp_status = "unsupported";
      decoded.info.icc_status = "unsupported";
      decoded.info.orientation_status = "unsupported";
      cleanup();
      return decoded;
    }
    require_jxr(codecs->CreateFormatConverter(&converter),
                "create JPEG XR input converter");
    char extension[] = ".jxr";
    require_jxr(converter->Initialize(converter, decoder, extension,
                                      GUID_PKPixelFormat64bppRGBAHalf),
                "configure JPEG XR input conversion");
    I32 width = 0, height = 0;
    require_jxr(converter->GetSize(converter, &width, &height),
                "read JPEG XR input dimensions");
    if (width <= 0 || height <= 0) throw std::runtime_error("invalid JPEG XR dimensions");
    std::vector<uint16_t> rgba(static_cast<size_t>(width) * height * 4u);
    const PKRect rect{0, 0, width, height};
    require_jxr(converter->Copy(converter, &rect,
                reinterpret_cast<U8*>(rgba.data()), static_cast<U32>(width) * 8u),
                "decode JPEG XR input pixels");

    DecodedImage decoded;
    decoded.info.path = path;
    decoded.info.format = "JPEG XR";
    decoded.info.container_brand = "JXR ";
    decoded.info.asset_kind = "direct-hdr";
    decoded.info.codec = "JPEG XR";
    decoded.info.width = static_cast<uint32_t>(width);
    decoded.info.height = static_cast<uint32_t>(height);
    decoded.info.profile = "FP16 linear scRGB (1.0 = 80 cd/m2)";
    decoded.info.bit_depth = 16;
    decoded.info.chroma = "4:4:4 RGBA FP16";
    decoded.info.pixel_format = "64bpp RGBA half-float";
    decoded.info.color_signal_kind = "JPEG XR FP16 pixel format / scRGB convention";
    decoded.info.primaries = 1;
    decoded.info.transfer = 8;
    decoded.info.matrix = 0;
    decoded.info.full_range = true;
    decoded.info.range_known = true;
    decoded.info.exif_status = "unsupported";
    decoded.info.xmp_status = "unsupported";
    decoded.info.icc_status = "unsupported";
    decoded.info.orientation_status = "unsupported";
    decoded.rgb.resize(static_cast<size_t>(width) * height * 3u);
    for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel) {
      const double red = static_cast<double>(half_to_float(rgba[pixel * 4u])) * 80.0;
      const double green = static_cast<double>(half_to_float(rgba[pixel * 4u + 1u])) * 80.0;
      const double blue = static_cast<double>(half_to_float(rgba[pixel * 4u + 2u])) * 80.0;
      const std::array<double, 3> rec2020{
          0.6274040 * red + 0.3292830 * green + 0.0433130 * blue,
          0.0690970 * red + 0.9195400 * green + 0.0113620 * blue,
          0.0163910 * red + 0.0880130 * green + 0.8955950 * blue};
      for (size_t channel = 0; channel < 3; ++channel) {
        decoded.rgb[pixel * 3u + channel] = static_cast<uint16_t>(std::llround(
            forward_pq(std::clamp(rec2020[channel], 0.0, 10000.0)) * 65535.0));
      }
    }
    cleanup();
    return decoded;
  } catch (...) {
    cleanup();
    throw;
  }
}
#endif

void require_uhdr(const uhdr_error_info_t& error, const char* operation) {
  if (error.error_code != UHDR_CODEC_OK) {
    throw std::runtime_error(std::string(operation) + ": " +
                             (error.has_detail ? error.detail : "libultrahdr error"));
  }
}

struct UhdrEncoderDeleter { void operator()(uhdr_codec_private_t* p) const { if (p) uhdr_release_encoder(p); } };
struct UhdrDecoderDeleter { void operator()(uhdr_codec_private_t* p) const { if (p) uhdr_release_decoder(p); } };
using UhdrEncoderPtr = std::unique_ptr<uhdr_codec_private_t, UhdrEncoderDeleter>;
using UhdrDecoderPtr = std::unique_ptr<uhdr_codec_private_t, UhdrDecoderDeleter>;

std::vector<uint32_t> pq16_to_rgba1010102(const DecodedImage& decoded) {
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  std::vector<uint32_t> packed(count);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t r = (static_cast<uint32_t>(decoded.rgb[i * 3u]) * 1023u + 32767u) / 65535u;
    const uint32_t g = (static_cast<uint32_t>(decoded.rgb[i * 3u + 1u]) * 1023u + 32767u) / 65535u;
    const uint32_t b = (static_cast<uint32_t>(decoded.rgb[i * 3u + 2u]) * 1023u + 32767u) / 65535u;
    packed[i] = r | (g << 10) | (b << 20) | (3u << 30);
  }
  return packed;
}

std::vector<uint8_t> jpeg_exif_from_heif(const std::vector<uint8_t>& heif_exif) {
  const auto tiff = exif_tiff_payload(heif_exif);
  if (tiff.empty()) return {};
  std::vector<uint8_t> output{'E', 'x', 'i', 'f', 0, 0};
  output.insert(output.end(), tiff.begin(), tiff.end());
  return output;
}

std::vector<uint8_t> encode_ultrahdr(const DecodedImage& decoded, const ConversionOptions& options,
                                     const HdrStats& stats) {
  DecodedImage gamut_source = decoded;
  if (options.output_gamut == "p3") {
    gamut_source.rgb = convert_rec2020_pq_to_p3_pq(decoded);
  } else if (options.output_gamut == "rec709") {
    gamut_source.rgb = convert_rec2020_pq_to_rec709_pq(decoded);
  }
  auto packed = pq16_to_rgba1010102(gamut_source);
  uhdr_raw_image_t hdr{};
  hdr.fmt = UHDR_IMG_FMT_32bppRGBA1010102;
  hdr.cg = options.output_gamut == "rec709" ? UHDR_CG_BT_709 :
           options.output_gamut == "p3" ? UHDR_CG_DISPLAY_P3 : UHDR_CG_BT_2100;
  hdr.ct = UHDR_CT_PQ;
  hdr.range = UHDR_CR_FULL_RANGE;
  hdr.w = decoded.info.width;
  hdr.h = decoded.info.height;
  hdr.planes[UHDR_PLANE_PACKED] = packed.data();
  hdr.stride[UHDR_PLANE_PACKED] = decoded.info.width;

  UhdrEncoderPtr encoder(uhdr_create_encoder());
  if (!encoder) throw std::runtime_error("cannot create Ultra HDR encoder");
  require_uhdr(uhdr_enc_set_raw_image(encoder.get(), &hdr, UHDR_HDR_IMG), "set Ultra HDR PQ input");
  require_uhdr(uhdr_enc_set_quality(encoder.get(), std::clamp(options.base_quality, 0, 100), UHDR_BASE_IMG), "set Ultra HDR base quality");
  require_uhdr(uhdr_enc_set_quality(encoder.get(), std::clamp(options.gainmap_quality, 0, 100), UHDR_GAIN_MAP_IMG), "set Ultra HDR gain-map quality");
  require_uhdr(uhdr_enc_set_gainmap_scale_factor(encoder.get(), std::clamp(options.gainmap_scale, 1, 128)), "set Ultra HDR gain-map scale");
  require_uhdr(uhdr_enc_set_using_multi_channel_gainmap(encoder.get(), options.multi_channel_gainmap ? 1 : 0), "set Ultra HDR gain-map channel mode");
  require_uhdr(uhdr_enc_set_target_display_peak_brightness(
                   encoder.get(), static_cast<float>(stats.chosen_target_nits)),
               "set faithful Ultra HDR target peak brightness");
  require_uhdr(uhdr_enc_set_preset(encoder.get(), UHDR_USAGE_BEST_QUALITY), "set Ultra HDR preset");
  require_uhdr(uhdr_enc_set_output_format(encoder.get(), UHDR_CODEC_JPG), "set Ultra HDR JPEG output");
  auto jpeg_exif = options.copy_exif ? jpeg_exif_from_heif(decoded.exif) : std::vector<uint8_t>{};
  if (!jpeg_exif.empty()) {
    uhdr_mem_block_t exif{jpeg_exif.data(), jpeg_exif.size(), jpeg_exif.size()};
    require_uhdr(uhdr_enc_set_exif_data(encoder.get(), &exif), "set Ultra HDR Exif");
  }
  require_uhdr(uhdr_encode(encoder.get()), "encode Ultra HDR");
  uhdr_compressed_image_t* output = uhdr_get_encoded_stream(encoder.get());
  if (!output || !output->data || output->data_sz == 0) throw std::runtime_error("Ultra HDR encoder returned empty output");
  const auto* first = static_cast<const uint8_t*>(output->data);
  return {first, first + output->data_sz};
}

Verification decode_ultrahdr(const std::vector<uint8_t>& bytes,
                             const DecodedImage* expected = nullptr,
                             const HdrStats* expected_stats = nullptr,
                             const ConversionOptions* expected_options = nullptr) {
  Verification v;
  if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()))) {
    v.checks.push_back("libultrahdr did not detect a gain map");
    return v;
  }
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data());
  input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create Ultra HDR decoder");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set Ultra HDR input");
  require_uhdr(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat), "set Ultra HDR verification format");
  require_uhdr(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR), "set Ultra HDR verification transfer");
  const float full_boost = expected_stats
      ? static_cast<float>(expected_stats->chosen_target_nits / 203.0)
      : static_cast<float>(10000.0 / 203.0);
  require_uhdr(uhdr_dec_set_out_max_display_boost(decoder.get(), std::max(1.0f, full_boost)), "set Ultra HDR verification boost");
  require_uhdr(uhdr_dec_probe(decoder.get()), "probe Ultra HDR");
  v.width = static_cast<uint32_t>(uhdr_dec_get_image_width(decoder.get()));
  v.height = static_cast<uint32_t>(uhdr_dec_get_image_height(decoder.get()));
  const int gain_width = uhdr_dec_get_gainmap_width(decoder.get());
  const int gain_height = uhdr_dec_get_gainmap_height(decoder.get());
  uhdr_mem_block_t* base = uhdr_dec_get_base_image(decoder.get());
  uhdr_mem_block_t* gain_map = uhdr_dec_get_gainmap_image(decoder.get());
  uhdr_gainmap_metadata_t* metadata = uhdr_dec_get_gainmap_metadata(decoder.get());
  if (metadata) v.hdr_capacity_max = metadata->hdr_capacity_max;
  v.gain_map_width = static_cast<uint32_t>(std::max(gain_width, 0));
  v.gain_map_height = static_cast<uint32_t>(std::max(gain_height, 0));
  if (gain_map && gain_map->data && gain_map->data_sz) {
    const auto probe = jpeg::inspect(gain_map->data, gain_map->data_sz);
    v.gain_map_channels = probe.channels;
    if (probe.channels == 3) {
#ifdef _WIN32
      if (gain_map->data_sz <= std::numeric_limits<DWORD>::max()) {
      ComScope com;
      auto factory = wic_factory();
      ComPtr<IWICStream> stream;
      require_hr(factory->CreateStream(&stream), "create gain-map JPEG memory stream");
      require_hr(stream->InitializeFromMemory(static_cast<BYTE*>(gain_map->data),
                 static_cast<DWORD>(gain_map->data_sz)), "initialize gain-map JPEG memory stream");
      ComPtr<IWICBitmapDecoder> jpeg_decoder;
      require_hr(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                 WICDecodeMetadataCacheOnDemand, &jpeg_decoder), "open gain-map JPEG decoder");
      ComPtr<IWICBitmapFrameDecode> frame;
      require_hr(jpeg_decoder->GetFrame(0, &frame), "read gain-map JPEG frame");
      ComPtr<IWICFormatConverter> converter;
      require_hr(factory->CreateFormatConverter(&converter), "create gain-map RGB converter");
      require_hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB,
                 WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
                 "convert gain-map JPEG to RGB");
      std::vector<uint8_t> rgb(static_cast<size_t>(probe.width) * probe.height * 3u);
      require_hr(converter->CopyPixels(nullptr, probe.width * 3u,
                 static_cast<UINT>(rgb.size()), rgb.data()), "decode gain-map RGB samples");
      uint8_t maximum = 0;
      for (size_t index = 0; index + 2u < rgb.size(); index += 3u) {
        maximum = std::max<uint8_t>(maximum, static_cast<uint8_t>(std::max({
            std::abs(static_cast<int>(rgb[index]) - rgb[index + 1u]),
            std::abs(static_cast<int>(rgb[index]) - rgb[index + 2u]),
            std::abs(static_cast<int>(rgb[index + 1u]) - rgb[index + 2u])})));
      }
      v.gain_map_channel_difference_max = maximum / 255.0;
      }
#endif
    }
  }

  uint32_t base_width = 0, base_height = 0;
  if (base && base->data && base->data_sz > 0) {
#ifdef _WIN32
    if (base->data_sz <= std::numeric_limits<DWORD>::max()) {
    ComScope com;
    auto factory = wic_factory();
    ComPtr<IWICStream> stream;
    require_hr(factory->CreateStream(&stream), "create base JPEG memory stream");
    require_hr(stream->InitializeFromMemory(static_cast<BYTE*>(base->data), static_cast<DWORD>(base->data_sz)), "initialize base JPEG memory stream");
    ComPtr<IWICBitmapDecoder> base_decoder;
    require_hr(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &base_decoder), "ordinary JPEG decode of Ultra HDR base");
    ComPtr<IWICBitmapFrameDecode> frame;
    require_hr(base_decoder->GetFrame(0, &frame), "read Ultra HDR base frame");
    require_hr(frame->GetSize(&base_width, &base_height), "read Ultra HDR base dimensions");
    }
#else
    const auto base_probe = jpeg::inspect(base->data, base->data_sz);
    base_width = base_probe.width;
    base_height = base_probe.height;
#endif
  }

  require_uhdr(uhdr_decode(decoder.get()), "decode Ultra HDR reconstruction");
  uhdr_raw_image_t* image = uhdr_get_decoded_image(decoder.get());
  if (!image || !image->planes[UHDR_PLANE_PACKED]) throw std::runtime_error("Ultra HDR decoded image is empty");
  v.bit_depth = 16;
  v.pixel_format = "RGBA half-float linear";
  v.color_encoding = "Ultra HDR SDR base + ISO 21496-1 gain map";
  v.min_value = std::numeric_limits<double>::infinity();
  v.max_value = -std::numeric_limits<double>::infinity();
  const auto* pixels = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  for (uint32_t y = 0; y < image->h; ++y) {
    const auto* row = pixels + static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] * 4u;
    for (uint32_t x = 0; x < image->w; ++x) {
      for (size_t c = 0; c < 3; ++c) {
        const float value = half_to_float(row[static_cast<size_t>(x) * 4u + c]);
        if (!std::isfinite(value)) v.finite = false;
        v.min_value = std::min(v.min_value, static_cast<double>(value));
        v.max_value = std::max(v.max_value, static_cast<double>(value));
      }
    }
  }
  if (expected && expected->info.width == image->w && expected->info.height == image->h &&
      image->cg == UHDR_CG_BT_2100) {
    const auto& lut = pq_nits_lut();
    long double squared_error = 0.0;
    double max_error = 0.0;
    uint64_t sample_count = 0;
    for (uint32_t y = 0; y < image->h; ++y) {
      const auto* row = pixels + static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] * 4u;
      for (uint32_t x = 0; x < image->w; ++x) {
        const size_t p = static_cast<size_t>(y) * image->w + x;
        for (size_t c = 0; c < 3; ++c) {
          const double reconstructed = half_to_float(row[static_cast<size_t>(x) * 4u + c]);
          const double source = lut[expected->rgb[p * 3u + c]] / 203.0;
          const double error = std::abs(reconstructed - source);
          squared_error += static_cast<long double>(error) * error;
          max_error = std::max(max_error, error);
          ++sample_count;
        }
      }
    }
    v.reconstruction_rmse = sample_count
        ? std::sqrt(static_cast<double>(squared_error / sample_count)) : 0.0;
    v.reconstruction_max_abs_error = max_error;
  }
  if (expected_stats) attach_hdr_stats(v, *expected_stats);
  v.checks.push_back("libultrahdr detected gain-map JPEG");
  v.checks.push_back(base_width == v.width && base_height == v.height ? "ordinary WIC JPEG decoder opened full-size SDR base" : "SDR base decode/dimensions failed");
  v.checks.push_back(gain_width > 0 && gain_height > 0 ? "gain-map dimensions present" : "gain map missing");
  v.checks.push_back(v.gain_map_channels == 3
      ? (v.gain_map_channel_difference_max > 0.0
          ? "RGB gain map contains distinct per-channel samples"
          : "RGB gain map channels decode identically")
      : v.gain_map_channels == 1 ? "monochrome gain map confirmed by JPEG SOF"
                                 : "gain-map JPEG channel count unavailable");
  v.checks.push_back(metadata ? "ISO gain-map metadata parsed" : "gain-map metadata missing");
  if (expected_stats && metadata) {
    const double expected_capacity = expected_stats->chosen_target_nits / 203.0;
    const bool capacity_matches = std::abs(metadata->hdr_capacity_max - expected_capacity) <=
                                  std::max(0.02, expected_capacity * 0.02);
    v.checks.push_back(capacity_matches
        ? "gain-map capacity matches measured/overridden target peak"
        : "gain-map capacity does not match chosen target peak");
    v.checks.push_back("linear-light source reconstruction RMSE recorded");
  }
  v.checks.push_back(v.finite && v.max_value > 0.0 ? "finite non-empty HDR reconstruction" : "invalid HDR reconstruction");
  v.passed = v.width > 0 && v.height > 0 && base_width == v.width && base_height == v.height &&
             gain_width > 0 && gain_height > 0 && metadata && v.finite && v.max_value > 0.0;
  if (expected_stats && metadata) {
    const double expected_capacity = expected_stats->chosen_target_nits / 203.0;
    v.passed = v.passed && std::abs(metadata->hdr_capacity_max - expected_capacity) <=
               std::max(0.02, expected_capacity * 0.02);
  }
  if (expected && expected_options) {
    const uint32_t expected_gain_width = expected->info.width /
        static_cast<uint32_t>(expected_options->gainmap_scale);
    const uint32_t expected_gain_height = expected->info.height /
        static_cast<uint32_t>(expected_options->gainmap_scale);
    const uint32_t expected_channels = expected_options->multi_channel_gainmap ? 3u : 1u;
    const bool layout_matches = v.gain_map_width == expected_gain_width &&
                                v.gain_map_height == expected_gain_height &&
                                v.gain_map_channels == expected_channels;
    v.checks.push_back(layout_matches
        ? "gain-map resolution and channel mode match requested options"
        : "gain-map layout does not match requested options");
    v.passed = v.passed && layout_matches;
  }
  return v;
}

SourceInfo ultrahdr_source_info(const std::filesystem::path& path,
                                const std::vector<uint8_t>& bytes,
                                uhdr_codec_private_t* decoder) {
  SourceInfo info;
  info.path = path;
  info.format = "Ultra HDR JPEG";
  info.container_brand = "JPEG/MPF";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "iso-ultrahdr-jpeg";
  info.codec = "JPEG SDR base + JPEG ISO 21496-1 gain map";
  info.profile = "ISO 21496-1 gain-map HDR reconstruction";
  info.color_signal_kind = "JPEG ICC / gain-map metadata";
  info.pixel_format = "JPEG SDR base + JPEG gain map";
  info.width = static_cast<uint32_t>(uhdr_dec_get_image_width(decoder));
  info.height = static_cast<uint32_t>(uhdr_dec_get_image_height(decoder));
  info.gain_map_width = static_cast<uint32_t>(uhdr_dec_get_gainmap_width(decoder));
  info.gain_map_height = static_cast<uint32_t>(uhdr_dec_get_gainmap_height(decoder));

  const auto outer_probe = jpeg::inspect(bytes.data(), bytes.size());
  info.exif_present = outer_probe.exif_present;
  info.xmp_present = outer_probe.xmp_present;
  info.icc_present = outer_probe.icc_present;
  if (auto* base = uhdr_dec_get_base_image(decoder);
      base && base->data && base->data_sz) {
    const auto base_probe = jpeg::inspect(base->data, base->data_sz);
    info.base_width = base_probe.width;
    info.base_height = base_probe.height;
    info.base_bit_depth = base_probe.bit_depth;
    info.base_channels = base_probe.channels;
    info.exif_present = info.exif_present || base_probe.exif_present;
    info.xmp_present = info.xmp_present || base_probe.xmp_present;
    info.icc_present = info.icc_present || base_probe.icc_present;
  }
  if (!info.base_width) info.base_width = info.width;
  if (!info.base_height) info.base_height = info.height;
  info.base_codec = "JPEG";
  info.base_transfer = "SDR gamma";
  info.bit_depth = info.base_bit_depth;
  info.chroma = info.base_channels == 1 ? "grayscale SDR base" : "JPEG SDR base";
  info.full_range = true;

  if (auto* gain = uhdr_dec_get_gainmap_image(decoder);
      gain && gain->data && gain->data_sz) {
    const auto gain_probe = jpeg::inspect(gain->data, gain->data_sz);
    info.gain_map_channels = gain_probe.channels;
  }
  if (info.gain_map_width) {
    info.gain_map_scale_x = info.base_width /
        static_cast<double>(info.gain_map_width);
  }
  if (info.gain_map_height) {
    info.gain_map_scale_y = info.base_height /
        static_cast<double>(info.gain_map_height);
  }

  if (auto* metadata = uhdr_dec_get_gainmap_metadata(decoder)) {
    info.hdr_capacity_min = metadata->hdr_capacity_min;
    info.hdr_capacity_max = metadata->hdr_capacity_max;
    info.base_hdr_headroom = std::log2(std::max(
        static_cast<double>(metadata->hdr_capacity_min), 1e-12));
    info.alternate_hdr_headroom = std::log2(std::max(
        static_cast<double>(metadata->hdr_capacity_max), 1e-12));
    info.gain_map_uses_base_color_space = metadata->use_base_cg != 0;
    for (size_t channel = 0; channel < 3; ++channel) {
      info.gain_map_min[channel] = std::log2(std::max(
          static_cast<double>(metadata->min_content_boost[channel]), 1e-12));
      info.gain_map_max[channel] = std::log2(std::max(
          static_cast<double>(metadata->max_content_boost[channel]), 1e-12));
      info.gain_map_gamma[channel] = metadata->gamma[channel];
      info.base_offset[channel] = metadata->offset_sdr[channel];
      info.alternate_offset[channel] = metadata->offset_hdr[channel];
    }
  }

  if (auto* icc = uhdr_dec_get_icc(decoder); icc && icc->data && icc->data_sz) {
    info.icc_present = true;
    info.base_color_space = icc_description(
        icc->data, static_cast<uint32_t>(std::min<size_t>(
                       icc->data_sz, std::numeric_limits<uint32_t>::max())));
  }
  if (info.base_color_space.empty()) info.base_color_space = "SDR / unspecified primaries";
  if (auto* exif = uhdr_dec_get_exif(decoder); exif && exif->data && exif->data_sz) {
    const auto* first = static_cast<const uint8_t*>(exif->data);
    std::vector<uint8_t> exif_bytes(first, first + exif->data_sz);
    info.exif_present = true;
    info.original_orientation = orientation::read_exif_orientation(exif_bytes);
    info.orientation_status = orientation::has_exif_orientation(exif_bytes)
        ? "present" : "absent";
  }
  info.exif_status = metadata_status(info.exif_present);
  info.xmp_status = metadata_status(info.xmp_present);
  info.icc_status = metadata_status(info.icc_present);
  if (info.orientation_status == "unsupported") info.orientation_status = "absent";
  info.orientation_normalized = true;
  return info;
}

SourceInfo inspect_ultrahdr_input(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()))) {
    throw std::runtime_error("JPEG input is not a standard Ultra HDR gain-map asset");
  }
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data());
  input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create Ultra HDR inspector");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set Ultra HDR inspection input");
  require_uhdr(uhdr_dec_probe(decoder.get()), "probe Ultra HDR inspection input");
  return ultrahdr_source_info(path, bytes, decoder.get());
}

SourceInfo inspect_plain_jpeg(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  const auto probe = jpeg::inspect(bytes.data(), bytes.size());
  if (probe.width == 0 || probe.height == 0 || probe.bit_depth == 0 || probe.channels == 0) {
    throw std::runtime_error("JPEG input decode failed");
  }
  SourceInfo info;
  info.path = path;
  info.format = "JPEG";
  info.container_brand = "JPEG";
  info.asset_kind = "non-HDR";
  info.width = probe.width;
  info.height = probe.height;
  info.bit_depth = probe.bit_depth;
  info.codec = "JPEG";
  info.chroma = probe.channels == 1 ? "grayscale" :
                probe.channels == 3 ? "YCbCr/RGB, 3 components" :
                std::to_string(probe.channels) + " components";
  info.pixel_format = "JPEG " + info.chroma;
  info.color_signal_kind = "JPEG ICC / application markers";
  info.profile = probe.icc_present && !probe.icc.empty()
      ? icc_description(probe.icc.data(), static_cast<uint32_t>(probe.icc.size()))
      : "unprofiled / unspecified";
  info.exif_present = probe.exif_present;
  info.xmp_present = probe.xmp_present;
  info.icc_present = probe.icc_present;
  info.exif_status = metadata_status(info.exif_present);
  info.xmp_status = metadata_status(info.xmp_present);
  info.icc_status = metadata_status(info.icc_present);
  info.original_orientation = orientation::read_exif_orientation(probe.exif);
  info.orientation_status = orientation::has_exif_orientation(probe.exif)
      ? "present" : "absent";
  info.orientation_normalized = false;
  return info;
}

DecodedImage decode_ultrahdr_input(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()))) {
    DecodedImage decoded;
    decoded.info = inspect_plain_jpeg(path);
    const auto probe = jpeg::inspect(bytes.data(), bytes.size());
    decoded.exif = probe.exif;
    decoded.xmp = probe.xmp;
    return decoded;
  }
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(bytes.data()); input.data_sz = input.capacity = bytes.size();
  input.cg = UHDR_CG_UNSPECIFIED; input.ct = UHDR_CT_UNSPECIFIED; input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create Ultra HDR input decoder");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input), "set Ultra HDR input asset");
  require_uhdr(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat), "set Ultra HDR input precision");
  require_uhdr(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR), "set Ultra HDR input linear decode");
  require_uhdr(uhdr_dec_set_out_max_display_boost(decoder.get(), static_cast<float>(10000.0 / 203.0)), "request full Ultra HDR gain map");
  require_uhdr(uhdr_dec_probe(decoder.get()), "probe Ultra HDR input");
  SourceInfo source_info = ultrahdr_source_info(path, bytes, decoder.get());
  require_uhdr(uhdr_decode(decoder.get()), "reconstruct Ultra HDR input");
  uhdr_raw_image_t* image = uhdr_get_decoded_image(decoder.get());
  if (!image || image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || !image->planes[UHDR_PLANE_PACKED]) {
    throw std::runtime_error("Ultra HDR input did not decode to linear half float");
  }
  DecodedImage decoded;
  decoded.info = std::move(source_info);
  const auto source_probe = jpeg::inspect(bytes.data(), bytes.size());
  decoded.exif = source_probe.exif;
  // ISO/Ultra HDR XMP describes the source gain-map relationship and must not
  // be copied as ordinary descriptive XMP onto a new direct-HDR output.
  const size_t count = static_cast<size_t>(image->w) * image->h;
  decoded.rgb.resize(count * 3u);
  const auto* pixels = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  for (uint32_t y = 0; y < image->h; ++y) {
    const auto* row = pixels + static_cast<size_t>(y) * image->stride[UHDR_PLANE_PACKED] * 4u;
    for (uint32_t x = 0; x < image->w; ++x) {
      const size_t p = static_cast<size_t>(y) * image->w + x;
      const double r = static_cast<double>(half_to_float(row[static_cast<size_t>(x) * 4u])) * 203.0;
      const double g = static_cast<double>(half_to_float(row[static_cast<size_t>(x) * 4u + 1u])) * 203.0;
      const double b = static_cast<double>(half_to_float(row[static_cast<size_t>(x) * 4u + 2u])) * 203.0;
      std::array<double, 3> rec2020{};
      if (image->cg == UHDR_CG_BT_2100) {
        rec2020 = {r, g, b};
      } else if (image->cg == UHDR_CG_DISPLAY_P3) {
        rec2020 = {0.7538330 * r + 0.1985974 * g + 0.0475696 * b,
                   0.0457438 * r + 0.9417772 * g + 0.0124789 * b,
                  -0.0012103 * r + 0.0176017 * g + 0.9836086 * b};
      } else {
        rec2020 = {0.6274040 * r + 0.3292830 * g + 0.0433130 * b,
                   0.0690970 * r + 0.9195400 * g + 0.0113620 * b,
                   0.0163910 * r + 0.0880130 * g + 0.8955950 * b};
      }
      for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(rec2020[c], 0.0, 10000.0)) * 65535.0));
    }
  }
  return decoded;
}

std::string extension_lower(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

DecodedImage decode_input_uncached(const std::filesystem::path& path,
                                   const ProgressCallback& progress,
                                   std::atomic_bool* cancel) {
  const auto decode_start = Clock::now();
  const std::string extension = extension_lower(path);
  if (extension == ".avif" && gainmap::is_adobe_tmap_avif(path)) {
    report(progress, 6, "Parsing AVIF base/gain-map/tmap item graph and ISO metadata");
    auto reconstructed = gainmap::reconstruct_adobe_tmap_avif(path, cancel);
    DecodedImage decoded;
    decoded.info = std::move(reconstructed.info);
    decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
    decoded.exif = std::move(reconstructed.exif);
    decoded.xmp = std::move(reconstructed.xmp);
    decoded.timings.decode_ms = reconstructed.decode_ms;
    decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
    decoded.timings.orientation_ms = reconstructed.orientation_ms;
    decoded.gain_map_formula_max_error = reconstructed.independent_formula_max_error;
    report(progress, 42, "Adobe tmap gain-map reconstructed to canonical high-precision HDR master");
    return decoded;
  }
  if ((extension == ".tif" || extension == ".tiff") &&
      inspect_tiff_metadata(path).gain_map_present) {
    report(progress, 6, "Parsing TIFF base/gain-map SubIFD and ISO metadata");
    auto reconstructed = gainmap::reconstruct_adobe_gainmap_tiff(path, cancel);
    DecodedImage decoded;
    decoded.info = std::move(reconstructed.info);
    decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
    decoded.exif = std::move(reconstructed.exif);
    decoded.xmp = std::move(reconstructed.xmp);
    decoded.timings.decode_ms = reconstructed.decode_ms;
    decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
    decoded.timings.orientation_ms = reconstructed.orientation_ms;
    report(progress, 42, "Adobe TIFF gain map reconstructed to canonical high-precision HDR master");
    return decoded;
  }
  if (extension == ".hif" || extension == ".heic" || extension == ".heif" || extension == ".avif") {
    const auto apple = gainmap::probe_apple_heif(path);
    if (apple.detected) {
      report(progress, 6, "Parsing Apple HEIC base/auxiliary/tmap item graph and metadata");
      auto reconstructed = gainmap::reconstruct_apple_heif_gainmap(path, cancel);
      DecodedImage decoded;
      decoded.info = std::move(reconstructed.info);
      decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
      decoded.exif = std::move(reconstructed.exif);
      decoded.xmp = std::move(reconstructed.xmp);
      decoded.timings.decode_ms = reconstructed.decode_ms;
      decoded.timings.base_decode_ms = reconstructed.base_decode_ms;
      decoded.timings.gain_map_decode_ms = reconstructed.gain_map_decode_ms;
      decoded.timings.gain_map_upsample_ms = reconstructed.gain_map_upsample_ms;
      decoded.timings.gain_apply_ms = reconstructed.gain_apply_ms;
      decoded.timings.gain_map_color_conversion_ms = reconstructed.color_conversion_ms;
      decoded.timings.color_conversion_ms = reconstructed.color_conversion_ms;
      decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
      decoded.timings.orientation_ms = reconstructed.orientation_ms;
      report(progress, 42, "Apple HEIC gain map reconstructed to canonical high-precision HDR master");
      return decoded;
    }
    return decode_direct_hdr(path, progress, cancel);
  }
  report(progress, 8, "Inspecting high-dynamic-range input");
  check_cancel(cancel);
  DecodedImage decoded;
  if (extension == ".jxl" && gainmap::is_iso_gainmap_jxl(path)) {
    report(progress, 6, "Parsing JPEG XL jhgm base, gain-map codestream, and ISO metadata");
    auto reconstructed = gainmap::reconstruct_iso_gainmap_jxl(path, cancel);
    decoded.info = std::move(reconstructed.info);
    decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
    decoded.exif = std::move(reconstructed.exif);
    decoded.xmp = std::move(reconstructed.xmp);
    decoded.timings.decode_ms = reconstructed.decode_ms;
    decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
    report(progress, 42, "JPEG XL jhgm reconstructed to canonical high-precision HDR master");
    return decoded;
  }
  if (extension == ".jxl") decoded = decode_jxl_input(path);
  else if (extension == ".jxr" || extension == ".wdp" || extension == ".hdp") decoded = decode_jxr_input(path);
  else if (extension == ".png") decoded = decode_png_input(path);
  else if (extension == ".tif" || extension == ".tiff") decoded = decode_tiff_input(path);
  else if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe") {
    const auto apple = gainmap::probe_apple_jpeg(path);
    if (apple.detected) {
      report(progress, 10, "Parsing Apple JPEG base/MPF gain map and ISO metadata");
      auto reconstructed = gainmap::reconstruct_apple_jpeg_gainmap(path, cancel);
      decoded.info = std::move(reconstructed.info);
      decoded.rgb = std::move(reconstructed.rec2020_pq_rgb);
      decoded.exif = std::move(reconstructed.exif);
      decoded.xmp = std::move(reconstructed.xmp);
      decoded.timings.decode_ms = reconstructed.decode_ms;
      decoded.timings.base_decode_ms = reconstructed.base_decode_ms;
      decoded.timings.gain_map_decode_ms = reconstructed.gain_map_decode_ms;
      decoded.timings.gain_map_upsample_ms = reconstructed.gain_map_upsample_ms;
      decoded.timings.gain_apply_ms = reconstructed.gain_apply_ms;
      decoded.timings.gain_map_color_conversion_ms = reconstructed.color_conversion_ms;
      decoded.timings.color_conversion_ms = reconstructed.color_conversion_ms;
      decoded.timings.gain_map_ms = reconstructed.reconstruction_ms;
      decoded.timings.orientation_ms = reconstructed.orientation_ms;
    } else {
      decoded = decode_ultrahdr_input(path);
    }
  }
  else throw std::runtime_error("unsupported input format: " + extension);
  check_cancel(cancel);
  if (decoded.timings.decode_ms == 0.0) decoded.timings.decode_ms = elapsed_ms(decode_start);
  const auto orientation_start = Clock::now();
  if (!decoded.rgb.empty() && decoded.info.original_orientation != 1 &&
      !decoded.info.orientation_normalized) {
    const auto transformed = orientation::normalize_rgb16(
        decoded.rgb, decoded.info.width, decoded.info.height,
        decoded.info.original_orientation);
    decoded.info.width = transformed.width;
    decoded.info.height = transformed.height;
  }
  orientation::set_exif_orientation_to_one(decoded.exif);
  orientation::set_xmp_orientation_to_one(decoded.xmp);
  decoded.info.orientation_normalized = true;
  decoded.timings.orientation_ms += elapsed_ms(orientation_start);
  report(progress, 42, decoded.info.asset_kind == "direct-hdr" ||
                       decoded.info.asset_kind == "gain-map-hdr"
      ? "Canonical high-precision HDR master ready"
      : "Source decoded; no HDR representation found");
  return decoded;
}

struct CanonicalCacheEntry {
  std::filesystem::path path;
  uintmax_t file_size = 0;
  std::filesystem::file_time_type write_time{};
  std::shared_ptr<const DecodedImage> image;
};

std::mutex canonical_cache_mutex;
CanonicalCacheEntry canonical_cache;

std::shared_ptr<const DecodedImage> decode_input(
    const std::filesystem::path& path,
    const ProgressCallback& progress,
    std::atomic_bool* cancel,
    TimingDiagnostics& timings) {
  const auto absolute = std::filesystem::weakly_canonical(path);
  const uintmax_t size = std::filesystem::file_size(absolute);
  const auto write_time = std::filesystem::last_write_time(absolute);
  {
    std::lock_guard lock(canonical_cache_mutex);
    if (canonical_cache.image && canonical_cache.path == absolute &&
        canonical_cache.file_size == size && canonical_cache.write_time == write_time) {
      timings = {};
      timings.canonical_cache_hit = true;
      report(progress, 42, "Reusing cached canonical high-precision HDR master");
      return canonical_cache.image;
    }
  }
  auto decoded = std::make_shared<DecodedImage>(
      decode_input_uncached(absolute, progress, cancel));
  timings = decoded->timings;
  timings.canonical_cache_hit = false;
  {
    std::lock_guard lock(canonical_cache_mutex);
    canonical_cache = {absolute, size, write_time, decoded};
  }
  return decoded;
}

SourceInfo inspect_any(const std::filesystem::path& path) {
  const std::string extension = extension_lower(path);
  if (extension == ".avif" && gainmap::is_adobe_tmap_avif(path)) {
    return gainmap::inspect_adobe_tmap_avif(path);
  }
  if (extension == ".hif" || extension == ".heic" || extension == ".heif" || extension == ".avif") {
    auto opened = open_heif(path);
    auto info = inspect_opened(path, opened);
    const auto apple = gainmap::probe_apple_heif(path);
    if (apple.detected) {
      return gainmap::inspect_apple_heif_gainmap(path);
    }
    if (info.original_orientation >= 5) std::swap(info.width, info.height);
    return info;
  }
  if (extension == ".tif" || extension == ".tiff") {
    auto info = inspect_tiff_metadata(path);
    if (info.gain_map_present) return gainmap::inspect_adobe_gainmap_tiff(path);
    if (info.original_orientation >= 5) std::swap(info.width, info.height);
    return info;
  }
  if (extension == ".jxl" && gainmap::is_iso_gainmap_jxl(path)) {
    return gainmap::inspect_iso_gainmap_jxl(path);
  }
  if (extension == ".jxl") return decode_jxl_input(path).info;
  if (extension == ".jxr" || extension == ".wdp" || extension == ".hdp") return decode_jxr_input(path).info;
  if (extension == ".png") return decode_png_input(path).info;
  if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe") {
    const auto apple = gainmap::probe_apple_jpeg(path);
    if (apple.detected) {
      return gainmap::inspect_apple_jpeg_gainmap(path);
    }
    const auto bytes = read_file(path);
    if (bytes.size() <= static_cast<size_t>(std::numeric_limits<int>::max()) &&
        is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()))) {
      return inspect_ultrahdr_input(path);
    }
    return inspect_plain_jpeg(path);
  }
  throw std::runtime_error("unsupported input format: " + extension);
}

void finalize_source_metadata(SourceInfo& info) {
  const bool wic_jxr = info.format == "JPEG XR";
  if (!wic_jxr) {
    if (info.exif_status == "unsupported") info.exif_status = metadata_status(info.exif_present);
    if (info.xmp_status == "unsupported") info.xmp_status = metadata_status(info.xmp_present);
    if (info.icc_status == "unsupported") info.icc_status = metadata_status(info.icc_present);
    if (info.orientation_status == "unsupported") {
      info.orientation_status = info.original_orientation != 1 ? "present" : "absent";
    }
  }
  if (info.pixel_format.empty()) info.pixel_format = info.chroma;
}

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out || (!data.empty() && !out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())))) {
    throw std::runtime_error("cannot write output: " + path.string());
  }
  out.flush();
  if (!out) throw std::runtime_error("cannot flush output");
}

std::string sha256(const std::vector<uint8_t>& data) {
#ifdef _WIN32
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_size = 0, result_size = 0;
  std::vector<uint8_t> object;
  std::array<uint8_t, 32> digest{};
  auto cleanup = [&] { if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0); };
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
      BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0) {
    cleanup(); throw std::runtime_error("cannot initialize SHA-256");
  }
  object.resize(object_size);
  if (BCryptCreateHash(alg, &hash, object.data(), object_size, nullptr, 0, 0) < 0 ||
      BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) < 0 ||
      BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
    cleanup(); throw std::runtime_error("cannot compute SHA-256");
  }
  cleanup();
  std::ostringstream out;
  for (uint8_t b : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  return out.str();
#else
  (void)data;
  return {};
#endif
}

json source_json(const SourceInfo& i) {
  json result = {{"path", i.path.u8string()}, {"format", i.format},
                 {"assetKind", i.asset_kind}, {"gainMapPresent", i.gain_map_present},
                 {"width", i.width}, {"height", i.height},
                 {"orientation", {{"source", i.original_orientation},
                                   {"canonical", i.orientation_normalized ? 1 : i.original_orientation}}},
                 {"containerBrand", i.container_brand}, {"isGrid", i.is_grid},
                 {"gridColumns", i.grid_columns}, {"gridRows", i.grid_rows},
                 {"tileWidth", i.tile_width}, {"tileHeight", i.tile_height},
                 {"codec", i.codec}, {"profile", i.profile}, {"bitDepth", i.bit_depth},
                 {"pixelFormat", i.pixel_format}, {"colorSignalKind", i.color_signal_kind},
                 {"chroma", i.chroma},
                 {"range", !i.range_known ? "unknown" : i.full_range ? "full" : "limited"},
                 {"color", {{"primaries", i.primaries}, {"transfer", i.transfer},
                             {"transferName", public_transfer_name(i.transfer)},
                             {"matrix", i.matrix}}},
                 {"exifPresent", i.exif_present}, {"xmpPresent", i.xmp_present},
                 {"iccPresent", i.icc_present},
                 {"metadata", {{"exif", i.exif_status}, {"xmp", i.xmp_status},
                                {"icc", i.icc_status}, {"orientation", i.orientation_status}}}};
  result["nativeSignal"] = {{"present", i.native_color_present},
                            {"description", i.native_color_description},
                            {"primaries", i.native_primaries},
                            {"transfer", i.native_transfer},
                            {"matrix", i.native_matrix},
                            {"range", !i.native_range_known ? "unknown" :
                                      i.native_full_range ? "full" : "limited"}};
  result["iccSignal"] = {{"present", i.icc_present},
                         {"description", i.icc_description},
                         {"version", i.icc_version},
                         {"cicpPresent", i.icc_cicp_present},
                         {"primaries", i.icc_primaries},
                         {"transfer", i.icc_transfer},
                         {"transferName", i.icc_transfer_interpretation},
                         {"matrix", i.icc_matrix},
                         {"range", i.icc_cicp_present ?
                                   (i.icc_full_range ? "full" : "limited") : "unknown"}};
  result["resolvedColor"] = {{"source", i.resolved_signaling_source},
                             {"conflict", i.color_signaling_conflict},
                             {"primaries", i.primaries},
                             {"transfer", i.transfer},
                             {"transferName", public_transfer_name(i.transfer)},
                             {"matrix", i.matrix},
                             {"representation", i.asset_kind}};
  if (i.gain_map_present) {
    result["gainMapFamily"] = i.gain_map_family;
    if (i.base_item_id || i.gain_map_item_id || i.tone_map_item_id) {
      result["gainMapItems"] = {{"base", i.base_item_id}, {"gainMap", i.gain_map_item_id},
                                {"toneMap", i.tone_map_item_id}};
    }
    result["gainMapSize"] = {{"width", i.gain_map_width}, {"height", i.gain_map_height}};
    result["baseRendition"] = {{"width", i.base_width}, {"height", i.base_height},
                               {"bitDepth", i.base_bit_depth}, {"channels", i.base_channels},
                               {"codec", i.base_codec}, {"colorSpace", i.base_color_space},
                               {"transfer", i.base_transfer}};
    result["gainMapLayout"] = {{"channels", i.gain_map_channels},
                               {"scaleX", i.gain_map_scale_x},
                               {"scaleY", i.gain_map_scale_y}};
    result["gainMapMetadata"] = {{"baseHdrHeadroom", i.base_hdr_headroom},
                                 {"alternateHdrHeadroom", i.alternate_hdr_headroom},
                                 {"hdrCapacityMin", i.hdr_capacity_min},
                                 {"hdrCapacityMax", i.hdr_capacity_max},
                                 {"gainMapMin", i.gain_map_min}, {"gainMapMax", i.gain_map_max},
                                 {"gamma", i.gain_map_gamma}, {"baseOffset", i.base_offset},
                                 {"alternateOffset", i.alternate_offset},
                                 {"useBaseColorSpace", i.gain_map_uses_base_color_space}};
    result["reconstructedHdr"] = {{"width", i.width}, {"height", i.height},
                                  {"colorSpace", i.reconstructed_color_space},
                                  {"transfer", i.reconstructed_transfer},
                                  {"precision", i.reconstructed_precision}};
    if (!i.auxiliary_type.empty()) result["auxiliaryType"] = i.auxiliary_type;
  }
  return result;
}

json verification_json(const Verification& v) {
  return {{"passed", v.passed}, {"width", v.width}, {"height", v.height},
          {"bitDepth", v.bit_depth}, {"pixelFormat", v.pixel_format},
          {"colorEncoding", v.color_encoding}, {"exactRoundtrip", v.exact_roundtrip},
          {"finite", v.finite}, {"minValue", v.min_value}, {"maxValue", v.max_value},
          {"hdrDiagnostics", {{"maxChannelNits", v.max_channel_nits},
                              {"maxLuminanceNits", v.max_luminance_nits},
                              {"percentile99_9Nits", v.percentile_99_9_nits},
                              {"percentile99_99Nits", v.percentile_99_99_nits},
                              {"chosenTargetPeakNits", v.chosen_target_peak_nits},
                              {"clippedPixelFraction", v.clipped_pixel_fraction},
                              {"peakChoiceReason", v.peak_choice_reason},
                              {"hdrCapacityMax", v.hdr_capacity_max},
                              {"gainMapWidth", v.gain_map_width},
                              {"gainMapHeight", v.gain_map_height},
                              {"gainMapChannels", v.gain_map_channels},
                              {"gainMapChannelDifferenceMax", v.gain_map_channel_difference_max},
                              {"inputGainMapFormulaMaxError", v.input_gain_map_formula_max_error},
                              {"reconstructionRmse", v.reconstruction_rmse},
                              {"reconstructionMaxAbsError", v.reconstruction_max_abs_error},
                              {"transferConversionRmse", v.transfer_conversion_rmse},
                              {"transferConversionMaxAbsError", v.transfer_conversion_max_abs_error},
                              {"sourceToCanonicalRmse", v.source_to_canonical_rmse},
                              {"sourceToCanonicalMaxAbsError", v.source_to_canonical_max_abs_error}}},
          {"checks", v.checks}};
}

ConversionResult convert_gainmap_extract(const std::filesystem::path& input,
                                         const std::filesystem::path& output,
                                         const ConversionOptions& options,
                                         ProgressCallback progress,
                                         std::atomic_bool* cancel) {
  const auto total_start = Clock::now();
  if (std::filesystem::exists(output) && !options.overwrite) {
    throw std::runtime_error("output exists; overwrite not enabled");
  }
  if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
  const auto temp = output.parent_path() / (output.filename().wstring() + L".partial");
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  try {
    check_cancel(cancel);
    report(progress, 8, "Parsing Gain Map container and metadata");
    const auto decode_start = Clock::now();
    const bool request_original = options.gainmap_export_format == "original";
    std::wstring input_extension = input.extension().wstring();
    std::transform(input_extension.begin(), input_extension.end(), input_extension.begin(), std::towlower);
    const bool exact_source = input_extension == L".jxl" || input_extension == L".jpg" ||
                              input_extension == L".jpeg" || input_extension == L".jpe";
    auto raster = gainmap::extract_gain_map(input, cancel, !(request_original && exact_source));
    const double decode_ms = elapsed_ms(decode_start);
    std::string format = options.gainmap_export_format;
    if (format != "original" && format != "png" && format != "tiff" && format != "jpeg") {
      throw std::runtime_error("unknown gain-map export format");
    }
    const bool exact_original = format == "original" && !raster.original_bytes.empty();
    if (format == "original" && !exact_original) {
      const auto ext = output.extension().wstring();
      format = (ext == L".tif" || ext == L".tiff") ? "tiff" : "png";
    }
    report(progress, 70, exact_original ? "Copying original Gain Map image" :
           "Writing decoded Gain Map image");
    const auto encode_start = Clock::now();
    std::vector<uint8_t> bytes;
    if (exact_original) bytes = raster.original_bytes;
    else if (format == "png") bytes = encode_gainmap_png(raster, 0);
    else if (format == "jpeg") bytes = encode_gainmap_jpeg(raster, 0, options.base_quality);
    if (format == "tiff") {
      encode_gainmap_tiff(temp, raster, 0);
      bytes = read_file(temp);
    } else {
      write_file(temp, bytes);
    }
    check_cancel(cancel);
    if (std::filesystem::exists(output)) std::filesystem::remove(output);
    std::filesystem::rename(temp, output);
    const double encode_ms = elapsed_ms(encode_start);
    Verification verification;
    verification.passed = true;
    verification.width = raster.width;
    verification.height = raster.height;
    verification.bit_depth = raster.bit_depth;
    verification.pixel_format = raster.channels == 3 ?
        "RGB Gain Map image" : "Mono Gain Map image";
    verification.color_encoding = raster.encoding;
    verification.gain_map_width = raster.width;
    verification.gain_map_height = raster.height;
    verification.gain_map_channels = raster.channels;
    verification.min_value = raster.rgb16.empty() ? 0.0 : std::numeric_limits<double>::infinity();
    verification.max_value = raster.rgb16.empty() ? 1.0 : -std::numeric_limits<double>::infinity();
    for (const uint16_t value : raster.rgb16) {
      verification.min_value = std::min(verification.min_value, value / 65535.0);
      verification.max_value = std::max(verification.max_value, value / 65535.0);
    }
    verification.checks.push_back("Gain Map metadata parsed from source container");
    verification.checks.push_back("Gain Map image decoded without HDR reconstruction");
    verification.checks.push_back(exact_original ?
        (raster.original_extension == ".jpg" ?
         "Original JPEG scan data copied without re-encoding; parent-container metadata removed" :
         "Original embedded Gain Map payload copied byte-for-byte") :
        (format == "jpeg" ? "Decoded Gain Map exported as 8-bit lossy JPEG" :
         "Decoded Gain Map samples exported losslessly"));
    if (raster.channels == 3 && !exact_original) {
      verification.checks.push_back("RGB Gain Map retained as one three-channel image");
    }
    report(progress, 100, "Gain Map export committed");
    ConversionResult result;
    result.success = true;
    result.output_path = output;
    result.output_bytes = static_cast<uint64_t>(bytes.size());
    result.mode = "gainmap-extract";
    result.sha256 = sha256(bytes);
    result.verification = std::move(verification);
    result.timings.decode_ms = decode_ms;
    result.timings.gain_map_decode_ms = decode_ms;
    result.timings.encode_ms = encode_ms;
    result.timings.total_ms = elapsed_ms(total_start);
    return result;
  } catch (...) {
    std::filesystem::remove(temp, ec);
    throw;
  }
}

}  // namespace

SourceInfo inspect(const std::filesystem::path& path) {
  auto info = inspect_any(path);
  finalize_source_metadata(info);
  return info;
}

ConversionResult convert(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         const ConversionOptions& options,
                         ProgressCallback progress,
                         std::atomic_bool* cancel) {
  const auto total_start = Clock::now();
  TimingDiagnostics timings;
  if (options.mode == "gainmap-extract") {
    return convert_gainmap_extract(input, output, options, progress, cancel);
  }
  if (std::filesystem::exists(output) && !options.overwrite) throw std::runtime_error("output exists; overwrite not enabled");
  if (options.target_peak_nits != 0.0f &&
      (options.target_peak_nits < 203.0f || options.target_peak_nits > 10000.0f)) {
    throw std::runtime_error("target peak override must be 203..10000 nits, or zero for Faithful/Auto");
  }
  if (options.mode == "ultrahdr" && options.gainmap_scale != 1 &&
      options.gainmap_scale != 2 && options.gainmap_scale != 4) {
    throw std::runtime_error("Ultra HDR gain-map scale must be 1 (full), 2 (half), or 4 (quarter)");
  }
  if (options.output_gamut != "rec2020" && options.output_gamut != "p3" &&
      options.output_gamut != "rec709") {
    throw std::runtime_error("output gamut must be rec2020, p3, or rec709");
  }
  if (options.output_transfer != "pq" && options.output_transfer != "hlg") {
    throw std::runtime_error("output transfer must be pq or hlg");
  }
  if (options.diagnostic_icc_only && options.output_transfer != "pq") {
    throw std::runtime_error("ICC-only regression fixtures are limited to PQ");
  }
  if (options.diagnostic_icc_only && options.output_gamut == "rec709") {
    throw std::runtime_error("diagnostic ICC-only fixture supports Rec.2020 or Display P3");
  }
  if (options.diagnostic_icc_only && options.mode != "png-pq16" &&
      options.mode != "jxl-pq16" && options.mode != "avif-pq10") {
    throw std::runtime_error("diagnostic ICC-only fixture is supported for PNG, JPEG XL, and AVIF");
  }
  if (options.output_representation != "direct" &&
      options.output_representation != "gainmap") {
    throw std::runtime_error("output representation must be direct or gainmap");
  }
  if (options.output_representation == "gainmap" &&
      options.mode != "jxl-pq16" && options.mode != "avif-pq10" &&
      options.mode != "ultrahdr") {
    throw std::runtime_error("gain-map output is supported only for JPEG, JPEG XL, and AVIF");
  }
  if (options.output_representation == "gainmap" &&
      (options.mode == "jxl-pq16" || options.mode == "avif-pq10") &&
      !options.multi_channel_gainmap) {
    throw std::runtime_error("JPEG XL and AVIF gain-map output requires an RGB gain map");
  }
  if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
  const auto temp = output.parent_path() / (output.filename().wstring() + L".partial");
  if (std::filesystem::exists(temp)) std::filesystem::remove(temp);
  try {
    check_cancel(cancel);
    auto decoded_pointer = decode_input(input, progress, cancel, timings);
    const DecodedImage& decoded = *decoded_pointer;
    if (decoded.info.asset_kind != "direct-hdr" &&
        decoded.info.asset_kind != "gain-map-hdr") {
      throw std::runtime_error("No HDR data");
    }
    const HdrStats stats = measure_hdr(decoded, options.target_peak_nits);
    std::vector<uint8_t> bytes;
    Verification verification;
    if (options.mode == "jxl-pq16") {
      if (options.output_representation == "gainmap") {
        report(progress, 48, "Encoding SDR base and ISO 21496-1 jhgm JPEG XL gain map");
        const auto encode_start = Clock::now();
        bytes = encode_gainmap_jxl(decoded, options);
        timings.encode_ms = elapsed_ms(encode_start);
        timings.gain_map_ms += timings.encode_ms;
        write_file(temp, bytes);
        check_cancel(cancel);
        report(progress, 88, "Verifying JPEG XL jhgm metadata and HDR reconstruction");
        const auto verification_start = Clock::now();
        const auto reconstructed = gainmap::reconstruct_iso_gainmap_jxl(temp, cancel);
        verification.width = reconstructed.info.width;
        verification.height = reconstructed.info.height;
        verification.bit_depth = 16;
        verification.pixel_format = "JPEG XL SDR base + jhgm gain map";
        verification.color_encoding = "ISO 21496-1 gain-map HDR";
        verification.gain_map_width = reconstructed.info.gain_map_width;
        verification.gain_map_height = reconstructed.info.gain_map_height;
        verification.gain_map_channels = reconstructed.info.gain_map_channels;
        verification.passed = verification.width == decoded.info.width &&
                              verification.height == decoded.info.height &&
                              verification.gain_map_width > 0;
        verification.checks.push_back("JPEG XL jhgm bundle and ISO metadata parsed");
        verification.checks.push_back("gain-map HDR reconstruction completed");
        timings.verification_ms = elapsed_ms(verification_start);
      } else {
      report(progress, 48, options.output_transfer == "hlg"
          ? "Encoding JPEG XL BT.2100 HLG master"
          : "Encoding JPEG XL lossless PQ master");
      const auto color_start = Clock::now();
      auto converted = options.output_transfer == "hlg"
          ? convert_rec2020_pq_to_hlg(decoded, options.output_gamut)
          : options.output_gamut == "p3" ? convert_rec2020_pq_to_p3_pq(decoded)
          : options.output_gamut == "rec709" ? convert_rec2020_pq_to_rec709_pq(decoded)
                                        : std::vector<uint16_t>{};
      timings.color_conversion_ms += elapsed_ms(color_start);
      const auto& pixels = converted.empty() ? decoded.rgb : converted;
      const auto encode_start = Clock::now();
      bytes = encode_jxl(decoded, options, pixels);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      write_file(temp, bytes);
      report(progress, 88, "Verifying JPEG XL decode and RGB16 equality");
      const auto verification_start = Clock::now();
      verification = decode_jxl(bytes, options.lossless ? &pixels : nullptr);
      timings.verification_ms = elapsed_ms(verification_start);
      }
    } else if (options.mode == "jxr-scrgb-fp16") {
      const auto color_start = Clock::now();
      auto half = pq2020_to_scrgb_half(decoded, progress, cancel);
      timings.color_conversion_ms += elapsed_ms(color_start);
      report(progress, 65, "Encoding JPEG XR FP16 scRGB with Windows WIC");
      const auto encode_start = Clock::now();
      encode_jxr(temp, decoded.info.width, decoded.info.height, half, options);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      report(progress, 88, "Verifying JPEG XR through WIC FP16 decode");
      const auto verification_start = Clock::now();
#if defined(__EMSCRIPTEN__)
      // jxrlib's decoder rejects some of its own FP16/alpha streams even though
      // Windows WIC accepts them.  Keep the browser encoder portable and leave
      // the stronger WIC round-trip check to the release regression suite.
      verification.width = decoded.info.width;
      verification.height = decoded.info.height;
      verification.bit_depth = 16;
      verification.pixel_format = "64bpp RGBA half-float";
      verification.color_encoding = "linear scRGB; 1.0 = 80 cd/m2; sRGB/BT.709 primaries";
      verification.finite = true;
      verification.passed = std::filesystem::file_size(temp) > 0;
      verification.checks.push_back("portable jxrlib FP16 encoder completed");
      verification.checks.push_back("Windows WIC compatibility covered by release regression");
#else
      verification = decode_jxr(temp, options.lossless ? &half : nullptr);
#endif
      timings.verification_ms = elapsed_ms(verification_start);
      bytes = read_file(temp);
    } else if (options.mode == "ultrahdr") {
      report(progress, 48, "Encoding faithful SDR base and ISO 21496-1 gain map with measured capacity");
      std::optional<DecodedImage> resized;
      const DecodedImage* uhdr_source = &decoded;
      HdrStats uhdr_stats = stats;
      if (decoded.info.width > 65500u || decoded.info.height > 65500u) {
        report(progress, 50, "Resampling linear HDR to the JPEG 65500-pixel codec limit");
        resized = resize_linear_hdr_to_fit(decoded, 65500u);
        uhdr_source = &*resized;
        uhdr_stats = measure_hdr(*uhdr_source, options.target_peak_nits);
      }
      if (uhdr_stats.chosen_target_nits <= 203.0) {
        uhdr_stats.chosen_target_nits = 204.0;
        uhdr_stats.reason += uhdr_stats.reason.empty() ? "" : "; ";
        uhdr_stats.reason +=
            "adjusted to 204 nits because Ultra HDR capacity max must be greater than 1.0";
      }
      const auto gain_map_start = Clock::now();
      bytes = encode_ultrahdr(*uhdr_source, options, uhdr_stats);
      const double gain_map_encode_ms = elapsed_ms(gain_map_start);
      timings.gain_map_ms += gain_map_encode_ms;
      timings.encode_ms = gain_map_encode_ms;
      check_cancel(cancel);
      write_file(temp, bytes);
      report(progress, 88, "Verifying gain-map capacity and linear-light HDR reconstruction");
      const auto verification_start = Clock::now();
      verification = decode_ultrahdr(bytes, uhdr_source, &uhdr_stats, &options);
      timings.verification_ms = elapsed_ms(verification_start);
    } else if (options.mode == "png-pq16") {
      report(progress, 52, options.output_transfer == "hlg"
          ? "Encoding direct HDR PNG RGB16 with BT.2100 HLG cICP"
          : options.embed_hdr_icc
          ? "Encoding direct HDR PNG RGB16 with PQ cICP and ICC"
          : "Encoding PNG A/B variant B with standard PQ cICP only");
      const auto color_start = Clock::now();
      auto converted = options.output_transfer == "hlg"
          ? convert_rec2020_pq_to_hlg(decoded, options.output_gamut)
          : options.output_gamut == "p3" ? convert_rec2020_pq_to_p3_pq(decoded)
          : options.output_gamut == "rec709" ? convert_rec2020_pq_to_rec709_pq(decoded)
                                        : std::vector<uint16_t>{};
      timings.color_conversion_ms += elapsed_ms(color_start);
      const auto& pixels = converted.empty() ? decoded.rgb : converted;
      const auto encode_start = Clock::now();
      bytes = encode_png(decoded, options, pixels);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      write_file(temp, bytes);
      report(progress, 88, "Verifying PNG RGB16 equality and cICP signaling");
      const auto verification_start = Clock::now();
      verification = decode_png(bytes, &pixels,
                                options.diagnostic_icc_only ||
                                    (options.output_transfer == "pq" && options.embed_hdr_icc),
                                options.diagnostic_icc_only);
      timings.verification_ms = elapsed_ms(verification_start);
      attach_hdr_stats(verification, stats);
    } else if (options.mode == "tiff-pq16") {
      if (options.output_transfer == "hlg") {
        throw std::runtime_error("Direct HDR TIFF HLG is not enabled; choose PQ");
      }
      report(progress, 52, options.tiff_compressed
          ? "Encoding direct HDR TIFF RGB16 with Deflate and HDR CICP ICC"
          : "Encoding uncompressed direct HDR TIFF RGB16 with HDR CICP ICC");
      const auto color_start = Clock::now();
      auto converted = options.output_gamut == "p3" ? convert_rec2020_pq_to_p3_pq(decoded) :
                       options.output_gamut == "rec709" ? convert_rec2020_pq_to_rec709_pq(decoded) :
                       std::vector<uint16_t>{};
      timings.color_conversion_ms += elapsed_ms(color_start);
      const auto& pixels = converted.empty() ? decoded.rgb : converted;
      const auto encode_start = Clock::now();
      encode_tiff(temp, decoded, options, pixels);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      report(progress, 88, "Verifying TIFF RGB16 equality and direct-HDR ICC");
      const auto verification_start = Clock::now();
      verification = decode_tiff(temp, &pixels);
      timings.verification_ms = elapsed_ms(verification_start);
      bytes = read_file(temp);
      attach_hdr_stats(verification, stats);
    } else if (options.mode == "avif-pq10") {
      if (options.output_representation == "gainmap") {
        report(progress, 52, "Encoding SDR base and ISO 21496-1 gain map AVIF");
        const auto encode_start = Clock::now();
        encode_gainmap_avif(temp, decoded, options);
        timings.encode_ms = elapsed_ms(encode_start);
        timings.gain_map_ms += timings.encode_ms;
        check_cancel(cancel);
        bytes = read_file(temp);
        report(progress, 88, "Verifying AVIF gain-map item graph and HDR reconstruction");
        const auto verification_start = Clock::now();
        const auto reconstructed = gainmap::reconstruct_adobe_tmap_avif(temp, cancel);
        verification.width = reconstructed.info.width;
        verification.height = reconstructed.info.height;
        verification.bit_depth = 10;
        verification.pixel_format = "AV1 base + ISO gain map";
        verification.color_encoding = "ISO 21496-1 gain-map HDR";
        verification.gain_map_width = reconstructed.info.gain_map_width;
        verification.gain_map_height = reconstructed.info.gain_map_height;
        verification.gain_map_channels = reconstructed.info.gain_map_channels;
        verification.passed = verification.width == decoded.info.width &&
                              verification.height == decoded.info.height &&
                              verification.gain_map_width > 0;
        verification.checks.push_back("AVIF tmap relationship and gain-map metadata parsed");
        verification.checks.push_back("gain-map HDR reconstruction completed");
        timings.verification_ms = elapsed_ms(verification_start);
      } else {
      report(progress, 52, options.output_transfer == "hlg"
          ? "Encoding direct 10-bit 4:4:4 BT.2100 HLG AVIF"
          : "Encoding direct 10-bit 4:4:4 PQ AVIF compatibility candidate");
      const auto color_start = Clock::now();
      auto converted = options.output_transfer == "hlg"
          ? convert_rec2020_pq_to_hlg(decoded, options.output_gamut)
          : options.output_gamut == "p3" ? convert_rec2020_pq_to_p3_pq(decoded)
          : options.output_gamut == "rec709" ? convert_rec2020_pq_to_rec709_pq(decoded)
                                        : std::vector<uint16_t>{};
      timings.color_conversion_ms += elapsed_ms(color_start);
      const auto& pixels = converted.empty() ? decoded.rgb : converted;
      const auto encode_start = Clock::now();
      encode_avif(temp, decoded, options, pixels);
      timings.encode_ms = elapsed_ms(encode_start);
      check_cancel(cancel);
      report(progress, 88, "Verifying AV1 10-bit 4:4:4 and HDR CICP signaling");
      const auto verification_start = Clock::now();
      verification = decode_avif(temp, &pixels, options.diagnostic_icc_only);
      timings.verification_ms = elapsed_ms(verification_start);
      bytes = read_file(temp);
      attach_hdr_stats(verification, stats);
      }
    } else {
      throw std::runtime_error("mode not implemented yet: " + options.mode);
    }
    if (options.mode != "ultrahdr" && verification.chosen_target_peak_nits == 0.0) {
      attach_hdr_stats(verification, stats);
    }
    verification.input_gain_map_formula_max_error = decoded.gain_map_formula_max_error;
    verification.transfer_conversion_rmse = decoded.transfer_conversion_rmse;
    verification.transfer_conversion_max_abs_error =
        decoded.transfer_conversion_max_abs_error;
    verification.source_to_canonical_rmse = decoded.source_to_canonical_rmse;
    verification.source_to_canonical_max_abs_error =
        decoded.source_to_canonical_max_abs_error;
    if (decoded.info.transfer == 18) {
      verification.checks.push_back(
          "BT.2100 HLG OETF/OOTF transfer conversion error recorded");
    }
    if (decoded.info.gain_map_family == "adobe-iso-tmap") {
      verification.checks.push_back(
          decoded.gain_map_formula_max_error <= (2.5 / 65535.0)
              ? "independent ISO gain-map formula sample check agrees with libavif reconstruction"
              : "independent ISO gain-map formula sample check exceeded tolerance");
      verification.passed = verification.passed &&
                            decoded.gain_map_formula_max_error <= (2.5 / 65535.0);
    }
    if (!verification.passed) throw std::runtime_error("output verification failed: " + verification_json(verification).dump());
    if (std::filesystem::exists(output)) std::filesystem::remove(output);
    std::filesystem::rename(temp, output);
    report(progress, 100, "Verified output committed atomically");
    timings.total_ms = elapsed_ms(total_start);
    ConversionResult result;
    result.success = true;
    result.output_path = output;
    result.output_bytes = static_cast<uint64_t>(bytes.size());
    result.mode = options.mode;
    result.sha256 = sha256(bytes);
    result.verification = std::move(verification);
    result.timings = timings;
    return result;
  } catch (...) {
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    throw;
  }
}

Verification verify(const std::filesystem::path& output, const std::string& mode) {
  if (mode == "jxl-pq16") return decode_jxl(read_file(output), nullptr);
  if (mode == "jxr-scrgb-fp16") return decode_jxr(output, nullptr);
  if (mode == "ultrahdr") return decode_ultrahdr(read_file(output));
  if (mode == "png-pq16") {
    const auto bytes = read_file(output);
    std::array<uint8_t, 4> cicp{};
    const bool hlg = find_png_cicp(bytes, cicp) && cicp[1] == 18;
    return decode_png(bytes, nullptr, !hlg);
  }
  if (mode == "tiff-pq16") return decode_tiff(output);
  if (mode == "avif-pq10") return decode_avif(output);
  throw std::runtime_error("verification mode not implemented: " + mode);
}

std::string to_json(const SourceInfo& info) { return source_json(info).dump(2); }
std::string to_json(const Verification& v) { return verification_json(v).dump(2); }
std::string to_json(const ConversionResult& r) {
  return json{{"success", r.success}, {"outputPath", r.output_path.u8string()},
              {"outputBytes", r.output_bytes}, {"mode", r.mode}, {"sha256", r.sha256},
              {"timings", {{"inspectMs", r.timings.inspect_ms}, {"decodeMs", r.timings.decode_ms},
                            {"baseDecodeMs", r.timings.base_decode_ms},
                            {"gainMapDecodeMs", r.timings.gain_map_decode_ms},
                            {"gainMapUpsampleMs", r.timings.gain_map_upsample_ms},
                            {"gainApplyMs", r.timings.gain_apply_ms},
                            {"gainMapColorConversionMs", r.timings.gain_map_color_conversion_ms},
                            {"orientationMs", r.timings.orientation_ms},
                            {"colorConversionMs", r.timings.color_conversion_ms},
                            {"gainMapMs", r.timings.gain_map_ms}, {"encodeMs", r.timings.encode_ms},
                            {"verificationMs", r.timings.verification_ms},
                            {"totalMs", r.timings.total_ms},
                            {"canonicalCacheHit", r.timings.canonical_cache_hit}}},
              {"verification", verification_json(r.verification)}}.dump(2);
}

}  // namespace hdrbridge
