#include "codecs/gainmap/jpeg.h"
#include "codecs/gainmap/decode_support.h"
#include "codecs/gainmap/extract_support.h"
#include "codecs/gainmap/icc_description.h"
#include "codecs/gainmap/reconstruction_math.h"
#include "codecs/jpeg_probe.h"
#include "color/signaling.h"
#include "image/orientation.h"
#include "infrastructure/progress.h"
#include "infrastructure/timing.h"
#include "metadata/source_metadata.h"
#include <ultrahdr_api.h>
#include <expat.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32) || defined(__ANDROID__)
#include <jpeglib.h>
#include <setjmp.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace hdrbridge::gainmap {

using namespace detail;

namespace {

struct UhdrDecoderDeleter {
  void operator()(uhdr_codec_private_t* value) const {
    if (value) uhdr_release_decoder(value);
  }
};
using UhdrDecoderPtr = std::unique_ptr<uhdr_codec_private_t, UhdrDecoderDeleter>;

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

std::vector<uint8_t> standalone_gainmap_jpeg(const uint8_t* data, size_t size) {
  if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) {
    throw std::runtime_error("embedded gain-map JPEG is invalid");
  }
  std::vector<uint8_t> output{0xff, 0xd8};
  size_t cursor = 2;
  while (cursor < size) {
    const size_t marker_start = cursor;
    if (data[cursor++] != 0xff) throw std::runtime_error("invalid gain-map JPEG marker stream");
    while (cursor < size && data[cursor] == 0xff) ++cursor;
    if (cursor >= size) throw std::runtime_error("truncated gain-map JPEG marker");
    const uint8_t marker = data[cursor++];
    if (marker == 0xda) {
      output.insert(output.end(), data + marker_start, data + size);
      return output;
    }
    if (marker == 0xd9) {
      output.push_back(0xff); output.push_back(0xd9);
      return output;
    }
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) {
      output.insert(output.end(), data + marker_start, data + cursor);
      continue;
    }
    if (cursor + 2 > size) throw std::runtime_error("truncated gain-map JPEG segment");
    const size_t segment_size = (static_cast<size_t>(data[cursor]) << 8) | data[cursor + 1];
    if (segment_size < 2 || segment_size > size - cursor) {
      throw std::runtime_error("invalid gain-map JPEG segment size");
    }
    const size_t segment_end = cursor + segment_size;
    // APP1/APP2 and the other private APP segments describe the parent
    // MPF/Ultra-HDR relationship. They are invalid or misleading on the
    // detached auxiliary JPEG. Keep only ordinary JFIF and Adobe markers.
    const bool private_metadata = (marker >= 0xe1 && marker <= 0xed) ||
                                  marker == 0xef || marker == 0xfe;
    if (!private_metadata) {
      output.insert(output.end(), data + marker_start, data + segment_end);
    }
    cursor = segment_end;
  }
  throw std::runtime_error("gain-map JPEG ended before image data");
}

#if defined(_WIN32) || defined(__ANDROID__)
std::vector<uint16_t> decode_jpeg_rgb16(const uint8_t* bytes, size_t size,
                                        uint32_t& width, uint32_t& height,
                                        std::atomic_bool* cancel) {
  if (!bytes || size == 0 || size > std::numeric_limits<unsigned long>::max()) {
    throw std::runtime_error("embedded gain-map JPEG is empty or too large");
  }
  struct ErrorManager {
    jpeg_error_mgr base{};
    jmp_buf jump{};
    char message[JMSG_LENGTH_MAX]{};
  } error;
  jpeg_decompress_struct decoder{};
  decoder.err = jpeg_std_error(&error.base);
  error.base.error_exit = [](j_common_ptr common) {
    auto* manager = reinterpret_cast<ErrorManager*>(common->err);
    (*common->err->format_message)(common, manager->message);
    longjmp(manager->jump, 1);
  };
  if (setjmp(error.jump)) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error(std::string("embedded gain-map JPEG decode failed: ") + error.message);
  }
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, const_cast<unsigned char*>(bytes), static_cast<unsigned long>(size));
  if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK || !jpeg_start_decompress(&decoder)) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("embedded gain-map JPEG header is invalid");
  }
  width = decoder.output_width;
  height = decoder.output_height;
  const int components = decoder.output_components;
  if (!width || !height || (components != 1 && components != 3 && components != 4)) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("embedded gain-map JPEG has an unsupported pixel layout");
  }
  std::vector<uint8_t> row(static_cast<size_t>(width) * components);
  std::vector<uint16_t> samples(static_cast<size_t>(width) * height * 3u);
  while (decoder.output_scanline < decoder.output_height) {
    check_gainmap_cancel(cancel);
    JSAMPROW row_ptr = row.data();
    jpeg_read_scanlines(&decoder, &row_ptr, 1);
    const uint32_t y = decoder.output_scanline - 1u;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t value_r = row[static_cast<size_t>(x) * components];
      const uint8_t value_g = components == 1 ? value_r : row[static_cast<size_t>(x) * components + 1u];
      const uint8_t value_b = components == 1 ? value_r : row[static_cast<size_t>(x) * components + 2u];
      const size_t p = (static_cast<size_t>(y) * width + x) * 3u;
      samples[p] = static_cast<uint16_t>(value_r) * 257u;
      samples[p + 1u] = static_cast<uint16_t>(value_g) * 257u;
      samples[p + 2u] = static_cast<uint16_t>(value_b) * 257u;
    }
  }
  jpeg_finish_decompress(&decoder);
  jpeg_destroy_decompress(&decoder);
  return samples;
}
#endif

bool jpeg_has_mpf_app2(const uint8_t* data, size_t size) {
  if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
  size_t offset = 2;
  while (offset + 1 < size) {
    while (offset < size && data[offset] != 0xff) ++offset;
    while (offset < size && data[offset] == 0xff) ++offset;
    if (offset >= size) break;
    const uint8_t marker = data[offset++];
    if (marker == 0xd9 || marker == 0xda) break;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (offset + 2 > size) break;
    const size_t segment_size = (static_cast<size_t>(data[offset]) << 8) |
                                data[offset + 1u];
    if (segment_size < 2 || segment_size > size - offset) break;
    const uint8_t* payload = data + offset + 2u;
    const size_t payload_size = segment_size - 2u;
    if (marker == 0xe2 && payload_size >= 4 && std::memcmp(payload, "MPF\0", 4) == 0) {
      return true;
    }
    offset += segment_size;
  }
  return false;
}

std::string_view trim_xml_ascii(std::string_view text) {
  const auto is_space = [](unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
  };
  while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

struct AdobeHdrGmMetadata {
  bool has_version = false;
  std::string version;
  bool has_min = false;
  std::array<double, 3> gain_map_min{0.0, 0.0, 0.0};
  bool has_max = false;
  std::array<double, 3> gain_map_max{0.0, 0.0, 0.0};
  bool has_gamma = false;
  std::array<double, 3> gamma{1.0, 1.0, 1.0};
  bool has_offset_sdr = false;
  std::array<double, 3> offset_sdr{1.0 / 64.0, 1.0 / 64.0, 1.0 / 64.0};
  bool has_offset_hdr = false;
  std::array<double, 3> offset_hdr{1.0 / 64.0, 1.0 / 64.0, 1.0 / 64.0};
  bool has_capacity_min = false;
  double hdr_capacity_min = 0.0;
  bool has_capacity_max = false;
  double hdr_capacity_max = 0.0;
  bool has_base_is_hdr = false;
  bool base_rendition_is_hdr = false;
  bool parse_error = false;
};

struct GainMapXmpSignals {
  bool is_apple = false;
  bool is_adobe_hdrgm = false;
  AdobeHdrGmMetadata adobe;
  bool any() const { return is_apple || is_adobe_hdrgm; }
};

class GainMapXmpReader {
 public:
  GainMapXmpSignals parse(const uint8_t* data, size_t byte_size, bool stop_on_any = false) {
    if (!data || byte_size == 0 || byte_size > 8u * 1024u * 1024u) return {};
    size_t size = byte_size;
    const bool utf16 = size >= 2u &&
        (data[0] == 0 || data[1] == 0 ||
         (data[0] == 0xffu && data[1] == 0xfeu) ||
         (data[0] == 0xfeu && data[1] == 0xffu));
    if (!utf16) {
      while (size > 0 && data[size - 1u] == 0) --size;
    }
    if (size == 0) return {};

    stop_on_any_ = stop_on_any;
    const std::unique_ptr<XML_ParserStruct, decltype(&XML_ParserFree)> handle(
        XML_ParserCreateNS(nullptr, '|'), XML_ParserFree);
    if (!handle) return {};
    parser = handle.get();
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, start_element, end_element);
    XML_SetCharacterDataHandler(parser, characters);
    XML_SetStartDoctypeDeclHandler(parser, deny_doctype);
    const XML_Status status = XML_Parse(
        parser, reinterpret_cast<const char*>(data),
        static_cast<int>(size), XML_TRUE);
    if (status == XML_STATUS_OK ||
        (aborted_on_match_ && XML_GetErrorCode(parser) == XML_ERROR_ABORTED)) {
      return signals_;
    }
    return {};
  }

 private:
  enum class Capture { None, AuxiliaryImageType, HdrGainMapProperty };
  enum class AdobeProp {
    None, Version, GainMapMin, GainMapMax, Gamma,
    OffsetSdr, OffsetHdr, HdrCapacityMin, HdrCapacityMax, BaseRenditionIsHdr
  };

  static constexpr std::string_view kRdfNs =
      "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
  static constexpr std::string_view kRdfRoot =
      "http://www.w3.org/1999/02/22-rdf-syntax-ns#|RDF";
  static constexpr std::string_view kRdfDescription =
      "http://www.w3.org/1999/02/22-rdf-syntax-ns#|Description";
  static constexpr std::string_view kApdiNs =
      "http://ns.apple.com/pixeldatainfo/1.0/";
  static constexpr std::string_view kHdrGmNs =
      "http://ns.apple.com/HDRGainMap/1.0/";
  static constexpr std::string_view kAdobeHdrGmNs =
      "http://ns.adobe.com/hdr-gain-map/1.0/";

  XML_Parser parser = nullptr;
  int depth = 0;
  int rdf_depth = 0;
  int capture_depth = 0;
  Capture capture = Capture::None;
  std::string value;
  AdobeProp adobe_prop = AdobeProp::None;
  int adobe_prop_depth = 0;
  bool adobe_in_seq = false;
  int adobe_seq_depth = 0;
  bool adobe_in_li = false;
  int adobe_li_depth = 0;
  std::string adobe_text;
  std::vector<std::string> adobe_seq_items;
  bool stop_on_any_ = false;
  bool aborted_on_match_ = false;
  GainMapXmpSignals signals_{};

  static std::pair<std::string_view, std::string_view> split_ns(std::string_view name) {
    const size_t sep = name.find('|');
    if (sep == std::string_view::npos) return {{}, name};
    return {name.substr(0, sep), name.substr(sep + 1u)};
  }

  static Capture classify_apple_property(std::string_view uri, std::string_view local) {
    if (uri == kApdiNs && local == "AuxiliaryImageType") {
      return Capture::AuxiliaryImageType;
    }
    if (uri == kHdrGmNs &&
        (local == "HDRGainMapVersion" || local == "HDRGainMapHeadroom")) {
      return Capture::HdrGainMapProperty;
    }
    return Capture::None;
  }

  static AdobeProp classify_adobe_property(std::string_view uri, std::string_view local) {
    if (uri != kAdobeHdrGmNs) return AdobeProp::None;
    if (local == "Version") return AdobeProp::Version;
    if (local == "GainMapMin") return AdobeProp::GainMapMin;
    if (local == "GainMapMax") return AdobeProp::GainMapMax;
    if (local == "Gamma") return AdobeProp::Gamma;
    if (local == "OffsetSDR") return AdobeProp::OffsetSdr;
    if (local == "OffsetHDR") return AdobeProp::OffsetHdr;
    if (local == "HDRCapacityMin") return AdobeProp::HdrCapacityMin;
    if (local == "HDRCapacityMax") return AdobeProp::HdrCapacityMax;
    if (local == "BaseRenditionIsHDR") return AdobeProp::BaseRenditionIsHdr;
    return AdobeProp::None;
  }

  static bool parse_finite_double(std::string_view raw, double& out) {
    const std::string_view trimmed = trim_xml_ascii(raw);
    if (trimmed.empty()) return false;
    std::string copy(trimmed);
    char* end = nullptr;
    const double v = std::strtod(copy.c_str(), &end);
    if (!end || end != copy.c_str() + copy.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
  }

  void apply_adobe_values(AdobeProp prop, const std::vector<std::string_view>& values) {
    if (prop == AdobeProp::None || values.empty()) return;
    auto& meta = signals_.adobe;
    auto set_vec = [&](std::array<double, 3>& dst, bool& flag) {
      if (values.size() == 1u) {
        double v = 0.0;
        if (!parse_finite_double(values[0], v)) { meta.parse_error = true; return; }
        dst = {v, v, v};
        flag = true;
      } else if (values.size() == 3u) {
        std::array<double, 3> parsed{};
        for (size_t i = 0; i < 3u; ++i) {
          if (!parse_finite_double(values[i], parsed[i])) { meta.parse_error = true; return; }
        }
        dst = parsed;
        flag = true;
      } else {
        meta.parse_error = true;
      }
    };
    auto set_scalar = [&](double& dst, bool& flag) {
      if (values.size() != 1u || !parse_finite_double(values[0], dst)) {
        meta.parse_error = true;
        return;
      }
      flag = true;
    };
    switch (prop) {
      case AdobeProp::Version: {
        if (values.size() != 1u) { meta.parse_error = true; break; }
        const std::string_view trimmed = trim_xml_ascii(values[0]);
        if (trimmed.empty()) { meta.parse_error = true; break; }
        meta.version.assign(trimmed);
        meta.has_version = true;
        break;
      }
      case AdobeProp::GainMapMin: set_vec(meta.gain_map_min, meta.has_min); break;
      case AdobeProp::GainMapMax: set_vec(meta.gain_map_max, meta.has_max); break;
      case AdobeProp::Gamma: set_vec(meta.gamma, meta.has_gamma); break;
      case AdobeProp::OffsetSdr: set_vec(meta.offset_sdr, meta.has_offset_sdr); break;
      case AdobeProp::OffsetHdr: set_vec(meta.offset_hdr, meta.has_offset_hdr); break;
      case AdobeProp::HdrCapacityMin: set_scalar(meta.hdr_capacity_min, meta.has_capacity_min); break;
      case AdobeProp::HdrCapacityMax: set_scalar(meta.hdr_capacity_max, meta.has_capacity_max); break;
      case AdobeProp::BaseRenditionIsHdr: {
        if (values.size() != 1u) { meta.parse_error = true; break; }
        const std::string_view trimmed = trim_xml_ascii(values[0]);
        if (trimmed == "True" || trimmed == "true") {
          meta.base_rendition_is_hdr = true;
          meta.has_base_is_hdr = true;
        } else if (trimmed == "False" || trimmed == "false") {
          meta.base_rendition_is_hdr = false;
          meta.has_base_is_hdr = true;
        } else {
          meta.parse_error = true;
        }
        break;
      }
      case AdobeProp::None: break;
    }
  }

  static bool matches_value(Capture kind, std::string_view raw) {
    const std::string_view trimmed = trim_xml_ascii(raw);
    if (kind == Capture::AuxiliaryImageType) return trimmed == kAppleHdrGainMapUrn;
    if (kind == Capture::HdrGainMapProperty) return !trimmed.empty();
    return false;
  }

  bool done() const {
    return stop_on_any_ ? signals_.any() : false;
  }

  void abort_matched() {
    aborted_on_match_ = true;
    XML_StopParser(parser, XML_FALSE);
  }

  void start(std::string_view name, const XML_Char** attributes) {
    ++depth;
    if (depth > 64) {
      XML_StopParser(parser, XML_FALSE);
      return;
    }
    if (!rdf_depth && (name == kRdfRoot || name == kRdfDescription)) {
      rdf_depth = depth;
    }
    if (!rdf_depth) return;

    const auto [elem_uri, elem_local] = split_ns(name);
    if (elem_uri == kAdobeHdrGmNs) {
      signals_.is_adobe_hdrgm = true;
      if (stop_on_any_) { abort_matched(); return; }
      const AdobeProp prop = classify_adobe_property(elem_uri, elem_local);
      if (prop != AdobeProp::None && adobe_prop == AdobeProp::None) {
        adobe_prop = prop;
        adobe_prop_depth = depth;
        adobe_in_seq = false;
        adobe_seq_depth = 0;
        adobe_in_li = false;
        adobe_li_depth = 0;
        adobe_text.clear();
        adobe_seq_items.clear();
      }
    } else if (adobe_prop != AdobeProp::None && elem_uri == kRdfNs) {
      if (!adobe_in_seq && (elem_local == "Seq" || elem_local == "Bag") &&
          depth == adobe_prop_depth + 1) {
        adobe_in_seq = true;
        adobe_seq_depth = depth;
      } else if (adobe_in_seq && !adobe_in_li && elem_local == "li" &&
                 depth == adobe_seq_depth + 1) {
        adobe_in_li = true;
        adobe_li_depth = depth;
        adobe_text.clear();
      }
    }

    const Capture elem_prop = classify_apple_property(elem_uri, elem_local);
    for (const XML_Char** attr = attributes; attr && *attr; attr += 2) {
      const auto [attr_uri, attr_local] = split_ns(attr[0]);
      if (attr_uri == kAdobeHdrGmNs) {
        signals_.is_adobe_hdrgm = true;
        if (stop_on_any_) { abort_matched(); return; }
        apply_adobe_values(classify_adobe_property(attr_uri, attr_local), {attr[1]});
      }
      if (elem_prop == Capture::AuxiliaryImageType &&
          attr_uri == kRdfNs && attr_local == "resource" &&
          matches_value(elem_prop, attr[1])) {
        signals_.is_apple = true;
        if (stop_on_any_) { abort_matched(); return; }
      }
      const Capture attr_prop = classify_apple_property(attr_uri, attr_local);
      if (attr_prop != Capture::None && matches_value(attr_prop, attr[1])) {
        signals_.is_apple = true;
        if (stop_on_any_) { abort_matched(); return; }
      }
    }
    if (elem_prop != Capture::None) {
      capture = elem_prop;
      capture_depth = depth;
      value.clear();
    }
  }

  void end() {
    if (adobe_in_li && depth == adobe_li_depth) {
      adobe_seq_items.push_back(std::move(adobe_text));
      adobe_text.clear();
      adobe_in_li = false;
      adobe_li_depth = 0;
    } else if (adobe_in_seq && depth == adobe_seq_depth) {
      adobe_in_seq = false;
      adobe_seq_depth = 0;
    } else if (adobe_prop != AdobeProp::None && depth == adobe_prop_depth) {
      if (!adobe_seq_items.empty()) {
        std::vector<std::string_view> views;
        views.reserve(adobe_seq_items.size());
        for (const auto& s : adobe_seq_items) views.push_back(s);
        apply_adobe_values(adobe_prop, views);
      } else if (!trim_xml_ascii(adobe_text).empty()) {
        apply_adobe_values(adobe_prop, {adobe_text});
      }
      adobe_prop = AdobeProp::None;
      adobe_prop_depth = 0;
      adobe_text.clear();
      adobe_seq_items.clear();
    }

    if (depth == capture_depth && capture != Capture::None) {
      if (matches_value(capture, value)) {
        signals_.is_apple = true;
        if (done()) { abort_matched(); return; }
      }
      capture = Capture::None;
      capture_depth = 0;
      value.clear();
    }
    if (depth == rdf_depth) rdf_depth = 0;
    --depth;
  }

  static void XMLCALL start_element(void* context, const XML_Char* name,
                                    const XML_Char** attributes) {
    auto& reader = *static_cast<GainMapXmpReader*>(context);
    try {
      reader.start(name, attributes);
    } catch (...) {
      XML_StopParser(reader.parser, XML_FALSE);
    }
  }

  static void XMLCALL end_element(void* context, const XML_Char*) {
    auto& reader = *static_cast<GainMapXmpReader*>(context);
    try {
      reader.end();
    } catch (...) {
      XML_StopParser(reader.parser, XML_FALSE);
    }
  }

  static void XMLCALL characters(void* context, const XML_Char* data, int length) {
    auto& reader = *static_cast<GainMapXmpReader*>(context);
    if (length <= 0) return;
    try {
      if ((reader.adobe_in_li && reader.depth == reader.adobe_li_depth) ||
          (reader.adobe_prop != AdobeProp::None && !reader.adobe_in_seq &&
           reader.depth == reader.adobe_prop_depth)) {
        if (reader.adobe_text.size() < 512u) {
          reader.adobe_text.append(
              data, std::min<size_t>(static_cast<size_t>(length), 512u - reader.adobe_text.size()));
        }
      }
      if (reader.depth != reader.capture_depth || reader.capture == Capture::None ||
          reader.value.size() >= 512u) {
        return;
      }
      reader.value.append(
          data, std::min<size_t>(static_cast<size_t>(length), 512u - reader.value.size()));
    } catch (...) {
      XML_StopParser(reader.parser, XML_FALSE);
    }
  }

  static void XMLCALL deny_doctype(void* context, const XML_Char*,
                                   const XML_Char*, const XML_Char*, int) {
    XML_StopParser(static_cast<GainMapXmpReader*>(context)->parser, XML_FALSE);
  }
};

bool has_apple_gainmap_xmp(const std::vector<uint8_t>& xmp) {
  return GainMapXmpReader{}.parse(xmp.data(), xmp.size(), /*stop_on_any=*/false).is_apple;
}

void collect_mpf_offsets(const uint8_t* payload, size_t payload_size,
                         size_t tiff_file_offset, size_t total_size,
                         std::vector<size_t>& candidate_offsets) {
  if (!payload || payload_size < 12u || std::memcmp(payload, "MPF\0", 4u) != 0) return;
  const uint8_t* tiff = payload + 4u;
  const size_t tiff_size = payload_size - 4u;
  const bool big_endian = tiff[0] == 'M' && tiff[1] == 'M';
  const bool little_endian = tiff[0] == 'I' && tiff[1] == 'I';
  if (!big_endian && !little_endian) return;
  const auto read16 = [&](size_t off) -> uint16_t {
    if (off > tiff_size || tiff_size - off < 2u) return 0;
    return big_endian
        ? static_cast<uint16_t>((tiff[off] << 8u) | tiff[off + 1u])
        : static_cast<uint16_t>(tiff[off] | (tiff[off + 1u] << 8u));
  };
  const auto read32 = [&](size_t off) -> uint32_t {
    if (off > tiff_size || tiff_size - off < 4u) return 0;
    return big_endian
        ? ((uint32_t(tiff[off]) << 24u) | (uint32_t(tiff[off + 1u]) << 16u) |
           (uint32_t(tiff[off + 2u]) << 8u) | uint32_t(tiff[off + 3u]))
        : (uint32_t(tiff[off]) | (uint32_t(tiff[off + 1u]) << 8u) |
           (uint32_t(tiff[off + 2u]) << 16u) | (uint32_t(tiff[off + 3u]) << 24u));
  };
  if (read16(2u) != 42u) return;
  const size_t ifd_off = read32(4u);
  if (ifd_off > tiff_size || tiff_size - ifd_off < 2u) return;
  const uint16_t tag_count = read16(ifd_off);
  size_t entry = ifd_off + 2u;
  for (uint16_t idx = 0; idx < tag_count; ++idx) {
    if (tiff_size - entry < 12u) break;
    if (read16(entry) != 0xB002u) {
      entry += 12u;
      continue;  // MP Entry
    }
    const uint32_t byte_count = read32(entry + 4u);
    const uint32_t val_off = read32(entry + 8u);
    if (val_off > tiff_size || byte_count > tiff_size - val_off) break;
    const size_t end_off = static_cast<size_t>(val_off) + byte_count;
    for (size_t pos = val_off; end_off - pos >= 16u; pos += 16u) {
      const uint32_t img_off = read32(pos + 8u);
      if (tiff_file_offset <= total_size && img_off > 0 &&
          img_off <= total_size - tiff_file_offset) {
        candidate_offsets.push_back(tiff_file_offset + img_off);
      }
    }
    break;
  }
}

struct JpegStreamSignals {
  bool has_iso_app2 = false;
  bool has_full_iso_app2 = false;
  GainMapXmpSignals xmp;
  size_t stream_end = 0;
};

JpegStreamSignals scan_jpeg_stream_signals(const uint8_t* data, size_t size, size_t start,
                                           std::vector<size_t>& next_offsets,
                                           bool stop_on_any) {
  JpegStreamSignals result;
  if (!data || start > size || size - start < 4u ||
      data[start] != 0xffu || data[start + 1u] != 0xd8u) {
    return result;
  }
  constexpr char kIsoUrn[] = "urn:iso:std:iso:ts:21496:-1";
  constexpr size_t kIsoPrefixLen = sizeof(kIsoUrn);  // includes '\0'
  constexpr char kXmpPrefix[] = "http://ns.adobe.com/xap/1.0/";
  constexpr size_t kXmpPrefixLen = sizeof(kXmpPrefix);  // includes '\0'

  size_t cursor = start + 2u;
  while (cursor < size) {
    while (cursor < size && data[cursor] != 0xffu) ++cursor;
    while (cursor < size && data[cursor] == 0xffu) ++cursor;
    if (cursor >= size) break;
    const uint8_t marker = data[cursor++];
    if (marker == 0xd9u) {
      result.stream_end = cursor;
      if (start == 0) {
        while (cursor < size && data[cursor] == 0x00u) ++cursor;
        if (size - cursor >= 2u && data[cursor] == 0xffu && data[cursor + 1u] == 0xd8u) {
          next_offsets.push_back(cursor);
        }
      }
      break;
    }
    if (marker == 0x01u || (marker >= 0xd0u && marker <= 0xd8u)) continue;
    if (size - cursor < 2u) break;
    const size_t segment_size =
        (static_cast<size_t>(data[cursor]) << 8u) | data[cursor + 1u];
    if (segment_size < 2u || segment_size > size - cursor) break;
    const uint8_t* payload = data + cursor + 2u;
    const size_t payload_size = segment_size - 2u;
    if (marker == 0xe2u) {
      if (payload_size >= kIsoPrefixLen &&
          std::memcmp(payload, kIsoUrn, kIsoPrefixLen) == 0) {
        result.has_iso_app2 = true;
        if (payload_size > kIsoPrefixLen + 4u) {
          result.has_full_iso_app2 = true;
        }
        if (stop_on_any) return result;
      }
      if (start == 0) {
        collect_mpf_offsets(payload, payload_size, cursor + 2u + 4u, size, next_offsets);
      }
    } else if (marker == 0xe1u && payload_size > kXmpPrefixLen &&
               std::memcmp(payload, kXmpPrefix, kXmpPrefixLen) == 0) {
      const auto parsed = GainMapXmpReader{}.parse(
          payload + kXmpPrefixLen, payload_size - kXmpPrefixLen, stop_on_any);
      if (parsed.is_apple) result.xmp.is_apple = true;
      if (parsed.is_adobe_hdrgm) {
        result.xmp.is_adobe_hdrgm = true;
        if (!stop_on_any) result.xmp.adobe = parsed.adobe;
      }
      if (stop_on_any && result.xmp.any()) return result;
    }
    cursor += segment_size;
    if (marker == 0xdau) {
      // Skip entropy-coded scan to EOI so concatenated non-MPF secondary JPEGs are found.
      while (size - cursor >= 2u) {
        if (data[cursor] != 0xffu) { ++cursor; continue; }
        size_t mpos = cursor + 1u;
        while (mpos < size && data[mpos] == 0xffu) ++mpos;
        if (mpos >= size) { cursor = size; break; }
        const uint8_t m = data[mpos];
        if (m == 0x00u || (m >= 0xd0u && m <= 0xd7u)) {
          cursor = mpos + 1u;
          continue;
        }
        if (m == 0xd9u) {
          cursor = mpos + 1u;
          result.stream_end = cursor;
          if (start == 0) {
            while (cursor < size && data[cursor] == 0x00u) ++cursor;
            if (size - cursor >= 2u && data[cursor] == 0xffu && data[cursor + 1u] == 0xd8u) {
              next_offsets.push_back(cursor);
            }
          }
          break;
        }
        cursor = mpos + 1u;
      }
      break;
    }
  }
  return result;
}

bool jpeg_declares_gainmap(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 4u || bytes[0] != 0xffu || bytes[1] != 0xd8u) return false;
  std::vector<size_t> queue{0u};
  std::vector<size_t> visited;
  for (size_t idx = 0; idx < queue.size() && idx < 16u; ++idx) {
    const size_t offset = queue[idx];
    if (std::find(visited.begin(), visited.end(), offset) != visited.end()) continue;
    visited.push_back(offset);
    const auto signals = scan_jpeg_stream_signals(
        bytes.data(), bytes.size(), offset, queue, /*stop_on_any=*/idx > 0);
    if (idx > 0 && (signals.has_iso_app2 || signals.xmp.any())) {
      return true;
    }
  }
  return false;
}

std::vector<uint8_t> build_iso_app2_segment(const AdobeHdrGmMetadata& meta, bool multichannel) {
  constexpr char kIsoUrn[] = "urn:iso:std:iso:ts:21496:-1";
  constexpr uint32_t kDenom = 1000000u;
  auto push_be16 = [](std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>(v & 0xffu));
  };
  auto push_be32 = [](std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24u) & 0xffu));
    out.push_back(static_cast<uint8_t>((v >> 16u) & 0xffu));
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>(v & 0xffu));
  };
  auto to_u32 = [&](double val, uint32_t& out) -> bool {
    const double scaled = val * static_cast<double>(kDenom);
    if (!std::isfinite(scaled) || scaled < 0.0 ||
        scaled > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
      return false;
    }
    out = static_cast<uint32_t>(std::llround(scaled));
    return true;
  };
  auto to_i32 = [&](double val, int32_t& out) -> bool {
    const double scaled = val * static_cast<double>(kDenom);
    if (!std::isfinite(scaled) ||
        scaled < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        scaled > static_cast<double>(std::numeric_limits<int32_t>::max())) {
      return false;
    }
    out = static_cast<int32_t>(std::llround(scaled));
    return true;
  };

  std::vector<uint8_t> payload;
  payload.insert(payload.end(), kIsoUrn, kIsoUrn + sizeof(kIsoUrn));
  push_be16(payload, 0);
  push_be16(payload, 0);
  const uint8_t flags = static_cast<uint8_t>(0x40u | 0x08u | (multichannel ? 0x80u : 0x00u));
  payload.push_back(flags);
  push_be32(payload, kDenom);
  uint32_t base_hr = 0, alt_hr = 0;
  if (!to_u32(meta.hdr_capacity_min, base_hr) || !to_u32(meta.hdr_capacity_max, alt_hr)) {
    return {};
  }
  push_be32(payload, base_hr);
  push_be32(payload, alt_hr);
  const size_t channels = multichannel ? 3u : 1u;
  for (size_t c = 0; c < channels; ++c) {
    int32_t g_min = 0, g_max = 0, off_sdr = 0, off_hdr = 0;
    uint32_t gamma = 0;
    if (!to_i32(meta.gain_map_min[c], g_min) ||
        !to_i32(meta.gain_map_max[c], g_max) ||
        !to_u32(meta.gamma[c], gamma) || gamma == 0u ||
        !to_i32(meta.offset_sdr[c], off_sdr) ||
        !to_i32(meta.offset_hdr[c], off_hdr)) {
      return {};
    }
    push_be32(payload, static_cast<uint32_t>(g_min));
    push_be32(payload, static_cast<uint32_t>(g_max));
    push_be32(payload, gamma);
    push_be32(payload, static_cast<uint32_t>(off_sdr));
    push_be32(payload, static_cast<uint32_t>(off_hdr));
  }
  const size_t seg_len = 2u + payload.size();
  if (seg_len > 0xffffu) return {};
  std::vector<uint8_t> segment;
  segment.reserve(2u + seg_len);
  segment.push_back(0xffu);
  segment.push_back(0xe2u);
  push_be16(segment, static_cast<uint16_t>(seg_len));
  segment.insert(segment.end(), payload.begin(), payload.end());
  return segment;
}

bool is_supported_adobe_hdrgm_version(std::string_view version) {
  return version == "1.0";
}

std::string check_unsupported_adobe_hdrgm_version(const JpegStreamSignals& stream) {
  if (stream.xmp.is_adobe_hdrgm && stream.xmp.adobe.has_version &&
      !is_supported_adobe_hdrgm_version(stream.xmp.adobe.version)) {
    return "unsupported Adobe hdrgm:Version '" + stream.xmp.adobe.version + "'";
  }
  return {};
}

std::vector<uint8_t> synthesize_iso_app2_for_xmp_jpeg(
    const std::vector<uint8_t>& outer_bytes,
    bool& out_adapted_xmp,
    std::string& out_error) {
  out_adapted_xmp = false;
  out_error.clear();
  if (outer_bytes.size() < 4u || outer_bytes[0] != 0xffu || outer_bytes[1] != 0xd8u) {
    return {};
  }
  std::vector<size_t> queue{0u};
  const auto primary = scan_jpeg_stream_signals(
      outer_bytes.data(), outer_bytes.size(), 0u, queue, /*stop_on_any=*/false);
  if (primary.has_full_iso_app2) return {};

  const size_t primary_end = primary.stream_end > 0
      ? primary.stream_end
      : (queue.size() > 1u ? queue[1] : outer_bytes.size());

  std::vector<size_t> visited{0u};
  size_t candidate_rank = 0;
  for (size_t idx = 1; idx < queue.size() && idx < 16u; ++idx) {
    const size_t offset = queue[idx];
    if (std::find(visited.begin(), visited.end(), offset) != visited.end()) continue;
    visited.push_back(offset);
    if (offset > outer_bytes.size() || outer_bytes.size() - offset < 4u ||
        outer_bytes[offset] != 0xffu || outer_bytes[offset + 1u] != 0xd8u) {
      continue;
    }
    ++candidate_rank;
    const auto secondary = scan_jpeg_stream_signals(
        outer_bytes.data(), outer_bytes.size(), offset, queue, /*stop_on_any=*/false);
    const size_t sec_end = secondary.stream_end > offset
        ? secondary.stream_end : outer_bytes.size();

    if (const auto err = check_unsupported_adobe_hdrgm_version(secondary); !err.empty()) {
      out_error = err;
      return {};
    }
    if (secondary.has_iso_app2) {
      if (candidate_rank == 1u) {
        // First secondary image already carries ISO APP2; let libultrahdr open outer_bytes natively.
        return {};
      }
      // Native ISO gain map appeared after an unrelated MPF sub-image; assemble [primary, secondary]
      // so libultrahdr's second-stream selection decodes the actual gain map.
      std::vector<uint8_t> paired;
      paired.reserve(primary_end + (sec_end - offset));
      paired.insert(paired.end(), outer_bytes.begin(),
                    outer_bytes.begin() + static_cast<std::ptrdiff_t>(primary_end));
      paired.insert(paired.end(),
                    outer_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    outer_bytes.begin() + static_cast<std::ptrdiff_t>(sec_end));
      return paired;
    }
    // Only synthesize ISO metadata onto a sub-image that itself carries Adobe hdrgm XMP;
    // ignore unrelated MPF/concatenated sub-images even when the primary carries hdrgm XMP.
    if (!secondary.xmp.is_adobe_hdrgm) continue;
    if (const auto err = check_unsupported_adobe_hdrgm_version(primary); !err.empty()) {
      out_error = err;
      return {};
    }
    AdobeHdrGmMetadata meta = secondary.xmp.adobe;
    const auto& fallback = primary.xmp.adobe;
    if (meta.parse_error || fallback.parse_error) {
      out_error = "invalid Adobe hdrgm XMP gain-map metadata";
      return {};
    }
    if (!meta.has_version && fallback.has_version) {
      meta.version = fallback.version;
      meta.has_version = true;
    }
    if (!meta.has_min && fallback.has_min) {
      meta.gain_map_min = fallback.gain_map_min;
      meta.has_min = true;
    }
    if (!meta.has_max && fallback.has_max) {
      meta.gain_map_max = fallback.gain_map_max;
      meta.has_max = true;
    }
    if (!meta.has_gamma && fallback.has_gamma) {
      meta.gamma = fallback.gamma;
      meta.has_gamma = true;
    }
    if (!meta.has_offset_sdr && fallback.has_offset_sdr) {
      meta.offset_sdr = fallback.offset_sdr;
      meta.has_offset_sdr = true;
    }
    if (!meta.has_offset_hdr && fallback.has_offset_hdr) {
      meta.offset_hdr = fallback.offset_hdr;
      meta.has_offset_hdr = true;
    }
    if (!meta.has_capacity_min && fallback.has_capacity_min) {
      meta.hdr_capacity_min = fallback.hdr_capacity_min;
      meta.has_capacity_min = true;
    }
    if (!meta.has_capacity_max && fallback.has_capacity_max) {
      meta.hdr_capacity_max = fallback.hdr_capacity_max;
      meta.has_capacity_max = true;
    }
    if (!meta.has_base_is_hdr && fallback.has_base_is_hdr) {
      meta.base_rendition_is_hdr = fallback.base_rendition_is_hdr;
      meta.has_base_is_hdr = true;
    }
    if (!meta.has_version || !is_supported_adobe_hdrgm_version(meta.version) ||
        !meta.has_max || !meta.has_capacity_max ||
        meta.base_rendition_is_hdr ||
        meta.hdr_capacity_min < 0.0 || meta.hdr_capacity_max < meta.hdr_capacity_min) {
      continue;
    }
    bool channels_valid = true;
    for (size_t c = 0; c < 3u; ++c) {
      if (meta.gain_map_max[c] < meta.gain_map_min[c] || meta.gamma[c] <= 0.0) {
        channels_valid = false;
        break;
      }
    }
    if (!channels_valid) continue;

    bool multichannel =
        meta.gain_map_min[0] != meta.gain_map_min[1] ||
        meta.gain_map_min[0] != meta.gain_map_min[2] ||
        meta.gain_map_max[0] != meta.gain_map_max[1] ||
        meta.gain_map_max[0] != meta.gain_map_max[2] ||
        meta.gamma[0] != meta.gamma[1] || meta.gamma[0] != meta.gamma[2] ||
        meta.offset_sdr[0] != meta.offset_sdr[1] || meta.offset_sdr[0] != meta.offset_sdr[2] ||
        meta.offset_hdr[0] != meta.offset_hdr[1] || meta.offset_hdr[0] != meta.offset_hdr[2];
    if (!multichannel) {
      try {
        const auto sec_probe = jpeg::inspect(outer_bytes.data() + offset, sec_end - offset);
        if (sec_probe.channels == 3u) multichannel = true;
      } catch (...) {
      }
    }
    const auto iso_seg = build_iso_app2_segment(meta, multichannel);
    if (iso_seg.empty()) continue;

    out_adapted_xmp = true;
    std::vector<uint8_t> patched;
    patched.reserve(primary_end + iso_seg.size() + (sec_end - offset));
    patched.insert(patched.end(), outer_bytes.begin(),
                   outer_bytes.begin() + static_cast<std::ptrdiff_t>(primary_end));
    patched.push_back(0xffu);
    patched.push_back(0xd8u);
    patched.insert(patched.end(), iso_seg.begin(), iso_seg.end());
    patched.insert(patched.end(),
                   outer_bytes.begin() + static_cast<std::ptrdiff_t>(offset + 2u),
                   outer_bytes.begin() + static_cast<std::ptrdiff_t>(sec_end));
    return patched;
  }
  return {};
}

void populate_jpeg_gainmap_source_info(
    SourceInfo& info,
    const std::filesystem::path& path,
    const std::vector<uint8_t>& outer_bytes,
    uhdr_codec_private_t* decoder,
    const uhdr_mem_block_t* base,
    const uhdr_mem_block_t* gain,
    const uhdr_gainmap_metadata_t* metadata,
    bool is_apple,
    bool source_has_iso_app2 = true,
    bool adapted_xmp_via_iso = false,
    std::vector<uint8_t>* out_exif = nullptr) {
  info.path = path;
  info.format = is_apple ? "Apple HDR JPEG" : "Ultra HDR JPEG";
  info.container_brand = "JPEG/MPF";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = is_apple ? "apple-mpf-iso" :
      (source_has_iso_app2 ? "iso-ultrahdr-jpeg" : "adobe-hdrgm-xmp");
  if (is_apple) {
    info.auxiliary_type = kAppleHdrGainMapUrn;
  }
  info.width = static_cast<uint32_t>(uhdr_dec_get_image_width(decoder));
  info.height = static_cast<uint32_t>(uhdr_dec_get_image_height(decoder));
  info.gain_map_width = static_cast<uint32_t>(uhdr_dec_get_gainmap_width(decoder));
  info.gain_map_height = static_cast<uint32_t>(uhdr_dec_get_gainmap_height(decoder));
  const auto outer_probe = jpeg::inspect(outer_bytes.data(), outer_bytes.size());
  const auto base_probe = (base && base->data && base->data_sz)
      ? jpeg::inspect(base->data, base->data_sz)
      : jpeg::Probe{};
  const auto gain_probe = (gain && gain->data && gain->data_sz)
      ? jpeg::inspect(gain->data, gain->data_sz)
      : jpeg::Probe{};
  info.base_width = base_probe.width ? base_probe.width : info.width;
  info.base_height = base_probe.height ? base_probe.height : info.height;
  info.base_bit_depth = base_probe.bit_depth;
  info.base_channels = base_probe.channels;
  info.base_codec = "JPEG";
  info.gain_map_channels = gain_probe.channels;
  if (info.gain_map_width) {
    info.gain_map_scale_x = info.base_width / static_cast<double>(info.gain_map_width);
  }
  if (info.gain_map_height) {
    info.gain_map_scale_y = info.base_height / static_cast<double>(info.gain_map_height);
  }
  info.exif_present = outer_probe.exif_present || base_probe.exif_present;
  info.xmp_present = outer_probe.xmp_present || base_probe.xmp_present;
  info.icc_present = outer_probe.icc_present || base_probe.icc_present;
  info.codec = is_apple
      ? "JPEG SDR base + MPF JPEG gain map + ISO 21496-1 metadata"
      : (source_has_iso_app2
             ? "JPEG SDR base + JPEG ISO 21496-1 gain map"
             : "JPEG SDR base + JPEG Adobe HDR Gain Map XMP gain map");
  info.color_signal_kind = is_apple
      ? "JPEG ICC / MPF + ISO gain-map metadata"
      : (source_has_iso_app2
             ? "JPEG ICC / gain-map metadata"
             : "JPEG ICC / Adobe HDR Gain Map XMP");
  info.pixel_format = "JPEG SDR base + JPEG gain map";
  info.bit_depth = is_apple ? 8u : info.base_bit_depth;
  info.chroma = is_apple
      ? "JPEG RGB base + gain map"
      : (info.base_channels == 1 ? "grayscale SDR base" : "JPEG SDR base");
  if (metadata) {
    info.base_hdr_headroom = std::log2(std::max(
        static_cast<double>(metadata->hdr_capacity_min), 1e-12));
    info.alternate_hdr_headroom = std::log2(std::max(
        static_cast<double>(metadata->hdr_capacity_max), 1e-12));
    info.hdr_capacity_min = metadata->hdr_capacity_min;
    info.hdr_capacity_max = metadata->hdr_capacity_max;
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

  auto base_icc = hdrbridge::detail::parse_icc_signal(
      outer_probe.icc.data(), outer_probe.icc.size());
  if (!base_icc.valid) {
    base_icc = hdrbridge::detail::parse_icc_signal(
        base_probe.icc.data(), base_probe.icc.size());
  }
  if (!base_icc.valid) {
    if (auto* icc = uhdr_dec_get_icc(decoder); icc && icc->data && icc->data_sz) {
      base_icc = hdrbridge::detail::parse_icc_signal(icc->data, icc->data_sz);
    }
  }
  base_icc.present = base_icc.present || info.icc_present;
  hdrbridge::detail::store_icc_signal(info, base_icc);

  info.primaries = base_icc.valid ? base_icc.primaries : 0;
  const uint16_t icc_transfer = base_icc.valid
      ? (base_icc.transfer ? base_icc.transfer : base_icc.sdr_transfer)
      : 0;
  info.transfer = icc_transfer;
  info.matrix = base_icc.valid ? base_icc.matrix : 0;
  const bool valid_icc_signal = base_icc.valid && (info.primaries != 0 || info.transfer != 0);
  if (is_apple) {
    info.full_range = valid_icc_signal;
    info.range_known = valid_icc_signal;
    if (valid_icc_signal) {
      info.resolved_signaling_source = "ICC";
    }
    info.base_transfer = info.transfer == 14 ? "BT.2020 SDR" :
                         info.transfer == 1 ? "BT.709" :
                         info.transfer == 13 ? "sRGB" :
                         info.transfer == 8 ? "Linear" : "Unspecified";
    info.base_color_space = !info.icc_description.empty()
        ? info.icc_description
        : (info.primaries == 1 ? "BT.709 / sRGB primaries" :
           info.primaries == 9 ? "BT.2020" :
           info.primaries == 12 ? "Display P3" : "Unspecified");
    info.profile = (info.base_color_space == "Unspecified"
        ? std::string("Apple HDR JPEG (unspecified base color space)")
        : info.base_color_space + " SDR base") + " + ISO gain-map HDR reconstruction";
  } else {
    info.full_range = true;
    if (valid_icc_signal || base_icc.cicp_present) {
      info.resolved_signaling_source = "ICC";
    }
    info.base_transfer = info.transfer == 14 ? "BT.2020 SDR" :
                         info.transfer == 1 ? "BT.709" :
                         info.transfer == 13 ? "sRGB" : "SDR gamma";
    info.base_color_space = !info.icc_description.empty()
        ? info.icc_description
        : (info.primaries == 1 ? "BT.709 / sRGB primaries" :
           info.primaries == 9 ? "BT.2020" :
           info.primaries == 12 ? "Display P3" : "SDR / unspecified primaries");
    info.profile = source_has_iso_app2
        ? "ISO 21496-1 gain-map HDR reconstruction"
        : (adapted_xmp_via_iso
               ? "Adobe HDR Gain Map XMP (adapted via internal ISO 21496-1)"
               : "Adobe HDR Gain Map XMP HDR reconstruction");
  }

  std::vector<uint8_t> exif_bytes = outer_probe.exif;
  if (exif_bytes.empty()) exif_bytes = base_probe.exif;
  if (exif_bytes.empty()) {
    if (auto* exif = uhdr_dec_get_exif(decoder); exif && exif->data && exif->data_sz) {
      const auto* first = static_cast<const uint8_t*>(exif->data);
      exif_bytes.assign(first, first + exif->data_sz);
    }
  }
  if (!exif_bytes.empty()) {
    info.exif_present = true;
    info.original_orientation = orientation::read_exif_orientation(exif_bytes);
    info.orientation_status = orientation::has_exif_orientation(exif_bytes) ? "present" : "absent";
    if (out_exif) *out_exif = std::move(exif_bytes);
  }
  info.exif_status = hdrbridge::detail::metadata_status(info.exif_present);
  info.xmp_status = hdrbridge::detail::metadata_status(info.xmp_present);
  if (info.orientation_status == "unsupported") info.orientation_status = "absent";
  if (info.original_orientation >= 5) {
    std::swap(info.width, info.height);
    std::swap(info.base_width, info.base_height);
    std::swap(info.gain_map_width, info.gain_map_height);
    if (info.gain_map_width) {
      info.gain_map_scale_x = info.base_width / static_cast<double>(info.gain_map_width);
    }
    if (info.gain_map_height) {
      info.gain_map_scale_y = info.base_height / static_cast<double>(info.gain_map_height);
    }
  }
  info.orientation_normalized = true;
}

SourceInfo inspect_plain_jpeg_bytes(const std::filesystem::path& path,
                                    const std::vector<uint8_t>& bytes,
                                    jpeg::Probe* out_probe = nullptr) {
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
  info.exif_present = probe.exif_present;
  info.xmp_present = probe.xmp_present;
  info.icc_present = probe.icc_present;
  auto icc = hdrbridge::detail::parse_icc_signal(
      probe.icc_present && !probe.icc.empty() ? probe.icc.data() : nullptr,
      probe.icc_present && !probe.icc.empty() ? probe.icc.size() : 0);
  icc.present = icc.present || info.icc_present;
  hdrbridge::detail::store_icc_signal(info, icc);
  if (icc.valid) {
    info.primaries = icc.primaries;
    info.transfer = icc.transfer ? icc.transfer : icc.sdr_transfer;
    info.matrix = icc.matrix;
    if (info.primaries != 0 || info.transfer != 0 || icc.cicp_present) {
      info.resolved_signaling_source = "ICC";
    }
  }
  info.profile = !info.icc_description.empty()
      ? info.icc_description
      : "unprofiled / unspecified";
  info.exif_status = hdrbridge::detail::metadata_status(info.exif_present);
  info.xmp_status = hdrbridge::detail::metadata_status(info.xmp_present);
  info.original_orientation = orientation::read_exif_orientation(probe.exif);
  info.orientation_status = orientation::has_exif_orientation(probe.exif)
      ? "present" : "absent";
  info.orientation_normalized = false;
  if (out_probe) *out_probe = probe;
  return info;
}

struct GainMapJpegSession {
  std::vector<uint8_t> synthesized_bytes;
  UhdrDecoderPtr decoder;
  uhdr_mem_block_t* base = nullptr;
  uhdr_mem_block_t* gain = nullptr;
  uhdr_gainmap_metadata_t* metadata = nullptr;
  bool is_apple = false;
  bool source_has_iso_app2 = false;
  bool adapted_xmp_via_iso = false;
};

UhdrDecoderPtr make_uhdr_decoder(const std::vector<uint8_t>& buffer,
                                 bool configure_linear_fp16) {
  uhdr_compressed_image_t input{};
  input.data = const_cast<uint8_t*>(buffer.data());
  input.data_sz = input.capacity = buffer.size();
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  UhdrDecoderPtr decoder(uhdr_create_decoder());
  if (!decoder) throw std::runtime_error("cannot create gain-map JPEG decoder");
  require_uhdr(uhdr_dec_set_image(decoder.get(), &input),
               "set gain-map JPEG input");
  if (configure_linear_fp16) {
    require_uhdr(uhdr_dec_set_out_img_format(decoder.get(),
                                             UHDR_IMG_FMT_64bppRGBAHalfFloat),
                 "set gain-map JPEG high-precision output");
    require_uhdr(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR),
                 "set gain-map JPEG linear output");
    require_uhdr(uhdr_dec_set_out_max_display_boost(
                     decoder.get(), static_cast<float>(10000.0 / 203.0)),
                 "request complete gain-map JPEG headroom");
  }
  return decoder;
}

std::optional<GainMapJpegSession> try_open_gainmap_jpeg_bytes(
    const std::vector<uint8_t>& bytes,
    bool configure_linear_fp16) {
  if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  GainMapJpegSession session;
  // Normalize XMP-only Adobe hdrgm JPEGs (including hybrid scalar/list XMP and MPF files with
  // unrelated sub-images before the gain map) before probing so libultrahdr receives the true
  // gain-map stream with complete multi-channel metadata.
  bool adapted_xmp = false;
  std::string xmp_error;
  session.synthesized_bytes = synthesize_iso_app2_for_xmp_jpeg(bytes, adapted_xmp, xmp_error);
  if (!xmp_error.empty()) {
    throw std::runtime_error(xmp_error);
  }
  const bool using_synthesized = !session.synthesized_bytes.empty() &&
      session.synthesized_bytes.size() <=
          static_cast<size_t>(std::numeric_limits<int>::max());
  if (using_synthesized) {
    session.adapted_xmp_via_iso = adapted_xmp;
    session.decoder = make_uhdr_decoder(session.synthesized_bytes, configure_linear_fp16);
  } else {
    session.decoder = make_uhdr_decoder(bytes, configure_linear_fp16);
  }
  // Probe once on the same decoder instance used for inspection and decode.
  uhdr_error_info_t probe_status = uhdr_dec_probe(session.decoder.get());
  if (probe_status.error_code != UHDR_CODEC_OK) {
    if (jpeg_declares_gainmap(bytes)) {
      require_uhdr(probe_status, "probe gain-map JPEG");
    }
    return std::nullopt;
  }
  session.base = uhdr_dec_get_base_image(session.decoder.get());
  session.gain = uhdr_dec_get_gainmap_image(session.decoder.get());
  session.metadata = uhdr_dec_get_gainmap_metadata(session.decoder.get());
  if (!session.adapted_xmp_via_iso && session.gain && session.gain->data && session.gain->data_sz) {
    std::vector<size_t> gain_queue;
    const auto gain_sig = scan_jpeg_stream_signals(
        static_cast<const uint8_t*>(session.gain->data), session.gain->data_sz,
        0u, gain_queue, /*stop_on_any=*/false);
    session.source_has_iso_app2 = gain_sig.has_iso_app2;
    if (!session.source_has_iso_app2) {
      if (const auto err = check_unsupported_adobe_hdrgm_version(gain_sig); !err.empty()) {
        throw std::runtime_error(err);
      }
      std::vector<size_t> primary_queue;
      const auto primary_sig = scan_jpeg_stream_signals(
          bytes.data(), bytes.size(), 0u, primary_queue, /*stop_on_any=*/false);
      if (const auto err = check_unsupported_adobe_hdrgm_version(primary_sig); !err.empty()) {
        throw std::runtime_error(err);
      }
    }
  }
  if (session.gain && session.gain->data && session.gain->data_sz &&
      jpeg_has_mpf_app2(bytes.data(), bytes.size())) {
    try {
      const auto gain_probe = jpeg::inspect(session.gain->data, session.gain->data_sz);
      session.is_apple = gain_probe.xmp_present && has_apple_gainmap_xmp(gain_probe.xmp);
    } catch (...) {
      session.is_apple = false;
    }
  }
  return session;
}

}  // namespace

GainMapAuxiliaryProbe probe_apple_jpeg(const std::filesystem::path& path) {
  GainMapAuxiliaryProbe probe;
  probe.container = "JPEG";
  const auto bytes = read_file(path);
  if (!jpeg_has_mpf_app2(bytes.data(), bytes.size())) {
    return probe;
  }
  try {
    if (const auto session = try_open_gainmap_jpeg_bytes(bytes, /*configure_linear_fp16=*/false);
        session && session->is_apple) {
      probe.detected = true;
      probe.is_apple = true;
      probe.auxiliary_type = kAppleHdrGainMapUrn;
      probe.extraction_supported = false;
    }
  } catch (...) {
    return probe;
  }
  return probe;
}

SourceInfo inspect_jpeg_asset(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  const auto session = try_open_gainmap_jpeg_bytes(bytes, /*configure_linear_fp16=*/false);
  if (!session) {
    return inspect_plain_jpeg_bytes(path, bytes);
  }
  SourceInfo info;
  populate_jpeg_gainmap_source_info(
      info, path, bytes, session->decoder.get(),
      session->base, session->gain, session->metadata, session->is_apple,
      session->source_has_iso_app2, session->adapted_xmp_via_iso);
  return info;
}

hdrbridge::detail::DecodedImage decode_jpeg_asset(
    const std::filesystem::path& path,
    const hdrbridge::ProgressCallback& progress,
    std::atomic_bool* cancel) {
  using hdrbridge::detail::Clock;
  using hdrbridge::detail::elapsed_ms;
  using hdrbridge::detail::check_cancel;
  using hdrbridge::detail::report;

  check_cancel(cancel);
  const auto decode_start = Clock::now();
  const auto bytes = read_file(path);
  auto session = try_open_gainmap_jpeg_bytes(bytes, /*configure_linear_fp16=*/true);
  if (!session) {
    jpeg::Probe probe;
    hdrbridge::detail::DecodedImage decoded;
    decoded.info = inspect_plain_jpeg_bytes(path, bytes, &probe);
    decoded.exif = std::move(probe.exif);
    decoded.xmp = std::move(probe.xmp);
    decoded.timings.decode_ms = elapsed_ms(decode_start);
    return decoded;
  }

  if (session->is_apple) {
    report(progress, 10, "Parsing Apple JPEG base/MPF gain map and ISO metadata");
  }
  hdrbridge::detail::DecodedImage decoded;
  auto& info = decoded.info;
  // ISO/Apple Gain-Map XMP describes the source gain-map relationship and must
  // not be copied as ordinary descriptive XMP onto a new direct-HDR output.
  populate_jpeg_gainmap_source_info(
      info, path, bytes, session->decoder.get(),
      session->base, session->gain, session->metadata, session->is_apple,
      session->source_has_iso_app2, session->adapted_xmp_via_iso,
      &decoded.exif);

  double reference_white_nits = 203.0;
  // Only Apple Adaptive HDR JPEGs carry custom HAGC reference white; keep
  // ordinary Ultra HDR JPEGs pinned to the standard 203 nit reference white.
  if (session->is_apple &&
      session->base && session->base->data && session->base->data_sz &&
      session->gain && session->gain->data && session->gain->data_sz) {
    reference_white_nits =
        jpeg_adaptive_hdr_icc_reference_white_nits(
            static_cast<const uint8_t*>(session->base->data), session->base->data_sz,
            static_cast<const uint8_t*>(session->gain->data), session->gain->data_sz,
            /*allow_lumi=*/false)
            .value_or(203.0);
  }
  require_uhdr(uhdr_decode(session->decoder.get()), "reconstruct gain-map JPEG");
  uhdr_raw_image_t* image = uhdr_get_decoded_image(session->decoder.get());
  if (!image || image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat ||
      !image->planes[UHDR_PLANE_PACKED]) {
    throw std::runtime_error("gain-map JPEG reconstruction did not produce linear FP16 RGB");
  }
  decoded.timings.decode_ms = elapsed_ms(decode_start);

  const auto reconstruction_start = Clock::now();
  decoded.rgb.resize(static_cast<size_t>(image->w) * image->h * 3u);
  const auto* pixels = static_cast<const uint16_t*>(image->planes[UHDR_PLANE_PACKED]);
  const uint32_t worker_count = std::min<uint32_t>(codec_worker_count(), image->h);
  std::atomic<uint32_t> next_row{0};
  const auto convert_rows = [&] {
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
              half_to_float_local(row[static_cast<size_t>(x) * 4u]) * reference_white_nits,
              half_to_float_local(row[static_cast<size_t>(x) * 4u + 1u]) * reference_white_nits,
              half_to_float_local(row[static_cast<size_t>(x) * 4u + 2u]) * reference_white_nits};
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
          for (size_t channel = 0; channel < 3; ++channel) {
            decoded.rgb[pixel * 3u + channel] =
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
    workers.emplace_back(convert_rows);
  }
  convert_rows();
  for (auto& worker : workers) worker.join();
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  decoded.timings.gain_map_ms = elapsed_ms(reconstruction_start);
  decoded.timings.color_conversion_ms = decoded.timings.gain_map_ms;

  const auto orientation_start = Clock::now();
  const auto transformed = orientation::normalize_rgb16(
      decoded.rgb, image->w, image->h, info.original_orientation);
  info.width = transformed.width;
  info.height = transformed.height;
  info.orientation_normalized = true;
  if (session->is_apple) {
    info.primaries = 9;
    info.transfer = 16;
    info.matrix = 0;
    info.full_range = true;
    info.range_known = true;
  }
  orientation::set_exif_orientation_to_one(decoded.exif);
  decoded.timings.orientation_ms = elapsed_ms(orientation_start);
  return decoded;
}

GainMapRaster extract_jpeg_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel,
                                   bool decode_pixels) {
#if !defined(_WIN32) && !defined(__ANDROID__)
  (void)path; (void)cancel; (void)decode_pixels;
  throw std::runtime_error("embedded JPEG gain-map extraction is unavailable on this platform");
#else
  const auto begin = Clock::now();
  const auto bytes = read_file(path);
  auto session = try_open_gainmap_jpeg_bytes(bytes, /*configure_linear_fp16=*/false);
  if (!session) throw std::runtime_error("parse gain-map JPEG: input does not contain a gain-map JPEG");
  uhdr_mem_block_t* gain = session->gain;
  if (!gain || !gain->data || !gain->data_sz) throw std::runtime_error("gain-map JPEG image is missing");
  const auto probe = jpeg::inspect(gain->data, gain->data_sz);
  if (!decode_pixels) {
    GainMapRaster result;
    result.info.path = path;
    result.info.format = "JPEG";
    result.info.asset_kind = "gain-map-hdr";
    result.width = probe.width;
    result.height = probe.height;
    result.channels = probe.channels == 1 ? 1u : 3u;
    result.bit_depth = 8;
    result.encoding = "embedded JPEG gain-map image";
    result.original_bytes = standalone_gainmap_jpeg(
        static_cast<const uint8_t*>(gain->data), gain->data_sz);
    result.original_extension = ".jpg";
    result.decode_ms = elapsed_ms(begin);
    return result;
  }
  uint32_t width = 0, height = 0;
  auto samples = decode_jpeg_rgb16(static_cast<const uint8_t*>(gain->data), gain->data_sz,
                                   width, height, cancel);
  SourceInfo info;
  populate_jpeg_gainmap_source_info(
      info, path, bytes, session->decoder.get(), session->base, gain,
      session->metadata, session->is_apple,
      session->source_has_iso_app2, session->adapted_xmp_via_iso);
  if (probe.channels == 1) {
    std::vector<uint16_t> mono(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = samples[i * 3u];
    samples = std::move(mono);
  }
  auto result = make_raster(std::move(info), width, height, probe.channels,
                            std::move(samples), "embedded JPEG gain-map image", cancel);
  result.original_bytes = standalone_gainmap_jpeg(
      static_cast<const uint8_t*>(gain->data), gain->data_sz);
  result.original_extension = ".jpg";
  normalize_gainmap_orientation(result, result.info.original_orientation);
  result.decode_ms = elapsed_ms(begin);
  return result;
#endif
}

}  // namespace hdrbridge::gainmap
