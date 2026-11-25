#include "codecs/png.h"
#include "color/icc_profile.h"
#include "color/signaling.h"
#include "color/gamut.h"
#include "color/pq.h"
#include "image/orientation.h"
#include "infrastructure/files.h"
#include "infrastructure/big_endian.h"
#include "infrastructure/pixel_workers.h"
#include "metadata/exif_output.h"
#include "metadata/exif_payload.h"
#include "metadata/source_metadata.h"
#include "hdrbridge_version.h"
#include <png.h>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace hdrbridge::detail {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611)  // libpng's documented C error boundary uses setjmp.
#endif

struct PngMemoryWriter { std::vector<uint8_t> bytes; };

static void png_write_memory(png_structp png, png_bytep data, png_size_t length) {
  auto* writer = static_cast<PngMemoryWriter*>(png_get_io_ptr(png));
  writer->bytes.insert(writer->bytes.end(), data, data + length);
}

static void png_flush_memory(png_structp) {}

static void insert_png_cicp_after_ihdr(std::vector<uint8_t>& bytes, const std::array<png_byte, 4>& cicp) {
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
  const bool rgba = decoded.alpha.size() ==
      static_cast<size_t>(decoded.info.width) * decoded.info.height;
  png_set_IHDR(png, info, decoded.info.width, decoded.info.height, 16,
               rgba ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
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
  if constexpr (std::endian::native == std::endian::little) png_set_swap(png);
  if (rgba) {
    std::vector<uint16_t> row(static_cast<size_t>(decoded.info.width) * 4u);
    for (uint32_t y = 0; y < decoded.info.height; ++y) {
      const size_t pixel_offset = static_cast<size_t>(y) * decoded.info.width;
      for (uint32_t x = 0; x < decoded.info.width; ++x) {
        const size_t source = (pixel_offset + x) * 3u;
        const size_t target = static_cast<size_t>(x) * 4u;
        row[target] = pixels[source];
        row[target + 1u] = pixels[source + 1u];
        row[target + 2u] = pixels[source + 2u];
        row[target + 3u] = decoded.alpha[pixel_offset + x];
      }
      png_write_row(png, reinterpret_cast<png_bytep>(row.data()));
    }
  } else {
    std::vector<png_bytep> rows(decoded.info.height);
    for (uint32_t y = 0; y < decoded.info.height; ++y) {
      rows[y] = reinterpret_cast<png_bytep>(const_cast<uint16_t*>(
          pixels.data() + static_cast<size_t>(y) * decoded.info.width * 3u));
    }
    png_write_image(png, rows.data());
  }
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
  if constexpr (std::endian::native == std::endian::little) png_set_swap(png);
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

struct PngMemoryReader { const uint8_t* data; size_t size; size_t offset = 0; };

static void png_read_memory(png_structp png, png_bytep output, png_size_t length) {
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
                        const std::vector<uint16_t>* expected,
                        bool expect_icc,
                        bool expect_icc_only,
                        const std::vector<uint16_t>* expected_alpha) {
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
  const bool rgba = color_type == PNG_COLOR_TYPE_RGB_ALPHA;
  v.pixel_format = rgba ? "PNG RGBA16" :
                   color_type == PNG_COLOR_TYPE_RGB ? "PNG RGB16" :
                                                      "unexpected PNG color type";
  if constexpr (std::endian::native == std::endian::little) png_set_swap(png);
  png_read_update_info(png, info);
  const size_t pixel_count = static_cast<size_t>(width) * height;
  std::vector<uint16_t> pixels(pixel_count * 3u);
  std::vector<uint16_t> alpha;
  if (rgba) {
    alpha.resize(pixel_count);
    std::vector<uint16_t> row(static_cast<size_t>(width) * 4u);
    for (uint32_t y = 0; y < height; ++y) {
      png_read_row(png, reinterpret_cast<png_bytep>(row.data()), nullptr);
      const size_t offset = static_cast<size_t>(y) * width;
      for (uint32_t x = 0; x < width; ++x) {
        const size_t source = static_cast<size_t>(x) * 4u;
        const size_t target = (offset + x) * 3u;
        pixels[target] = row[source];
        pixels[target + 1u] = row[source + 1u];
        pixels[target + 2u] = row[source + 2u];
        alpha[offset + x] = row[source + 3u];
      }
    }
  } else {
    std::vector<png_bytep> rows(height);
    for (uint32_t y = 0; y < height; ++y) rows[y] = reinterpret_cast<png_bytep>(pixels.data() + static_cast<size_t>(y) * width * 3u);
    png_read_image(png, rows.data());
  }
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
                      std::memcmp(expected->data(), pixels.data(), pixels.size() * sizeof(uint16_t)) == 0 &&
                      (!expected_alpha || (expected_alpha->size() == alpha.size() &&
                       std::memcmp(expected_alpha->data(), alpha.data(),
                                   alpha.size() * sizeof(uint16_t)) == 0));
  auto [minimum, maximum] = std::minmax_element(pixels.begin(), pixels.end());
  if (minimum != pixels.end()) { v.min_value = *minimum; v.max_value = *maximum; }
  v.checks.push_back(width && height ? "PNG dimensions parse" : "PNG dimensions missing");
  v.checks.push_back(bit_depth == 16 &&
      (color_type == PNG_COLOR_TYPE_RGB || rgba)
      ? (rgba ? "lossless RGBA 16-bit storage" : "lossless RGB 16-bit storage")
      : "PNG is not RGB16/RGBA16");
  v.checks.push_back(rec2020 || p3 || hlg
      ? (hlg ? "HLG cICP present with RGB matrix=0 and full range"
             : "PQ cICP present with RGB matrix=0 and full range")
      : expect_icc_only && !has_cicp ? "PNG cICP absent as required for ICC-only regression"
                                    : "PNG cICP mismatch");
  v.checks.push_back(expect_icc
                         ? (has_icc ? "compatible HDR PQ ICC profile embedded" : "HDR ICC profile missing")
                         : (!has_icc ? "cICP-only A/B variant contains no ICC profile"
                                     : "cICP-only A/B variant unexpectedly contains ICC"));
  if (expected) v.checks.push_back(v.exact_roundtrip
      ? (rgba ? "exact RGBA16 pixel and alpha roundtrip" : "exact RGB16 pixel roundtrip")
      : (rgba ? "RGBA16 pixel or alpha roundtrip mismatch" : "RGB16 pixel roundtrip mismatch"));
  v.passed = width > 0 && height > 0 && bit_depth == 16 &&
             (color_type == PNG_COLOR_TYPE_RGB || rgba) &&
             (expect_icc_only ? (!has_cicp && has_icc) : (rec2020 || p3 || hlg)) &&
             (has_icc == expect_icc) && (!expected || v.exact_roundtrip);
  return v;
}

struct PngInputWarnings {
  bool icc_illuminant = false;
};

static void png_input_warning(png_structp png, png_const_charp message) {
  if (std::string_view(message).ends_with(": PCS illuminant is not D50")) {
    static_cast<PngInputWarnings*>(png_get_error_ptr(png))->icc_illuminant = true;
  } else {
    std::fprintf(stderr, "libpng warning: %s\n", message);
  }
}

DecodedImage decode_png_input(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  std::array<uint8_t, 4> cicp{};
  const bool has_cicp = find_png_cicp(bytes, cicp);
  PngInputWarnings warnings;
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &warnings, nullptr, png_input_warning);
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
  if (warnings.icc_illuminant) {
    // Some exported profiles round the D50 Z coordinate down by one s15Fixed16
    // unit. Keep the original profile and report larger deviations.
    const bool rounded_d50 = has_icc && profile_size >= 128 &&
        be32(profile_data + 68) == 63190 && be32(profile_data + 72) == 65536 &&
        be32(profile_data + 76) == 54060 && decoded.info.icc_status == "present";
    if (!rounded_d50) std::fprintf(stderr, "libpng warning: iCCP: PCS illuminant is not D50\n");
  }
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
  const auto read_xmp = [&] {
    if (!decoded.xmp.empty()) return;
    png_textp texts = nullptr; int text_count = 0;
    if (png_get_text(png, info, &texts, &text_count) <= 0) return;
    for (int i = 0; i < text_count; ++i) {
      if (texts[i].key && std::strcmp(texts[i].key, "XML:com.adobe.xmp") == 0 && texts[i].text) {
        const size_t size = texts[i].itxt_length ? texts[i].itxt_length : texts[i].text_length;
        decoded.xmp.assign(texts[i].text, texts[i].text + size);
        decoded.info.xmp_present = !decoded.xmp.empty();
        decoded.info.xmp_status = metadata_status(decoded.info.xmp_present);
        break;
      }
    }
  };
  read_xmp();
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
    read_xmp();
    png_destroy_read_struct(&png, &info, nullptr);
    return decoded;
  }
  if (bit_depth != 16 || (color_type != PNG_COLOR_TYPE_RGB &&
                          color_type != PNG_COLOR_TYPE_RGB_ALPHA)) {
    png_destroy_read_struct(&png, &info, nullptr);
    throw std::runtime_error("direct HDR PNG input must be RGB16 or RGBA16");
  }
  const bool rgba = color_type == PNG_COLOR_TYPE_RGB_ALPHA;
  decoded.info.pixel_format = rgba ? "RGBA16 integer (straight alpha)" : "RGB16 integer";
  if constexpr (std::endian::native == std::endian::little) png_set_swap(png);
  png_read_update_info(png, info);
  const size_t pixel_count = static_cast<size_t>(width) * height;
  decoded.rgb.resize(pixel_count * 3u);
  if (rgba) {
    decoded.alpha.resize(pixel_count);
    std::vector<uint16_t> row(static_cast<size_t>(width) * 4u);
    for (uint32_t y = 0; y < height; ++y) {
      png_read_row(png, reinterpret_cast<png_bytep>(row.data()), nullptr);
      const size_t offset = static_cast<size_t>(y) * width;
      for (uint32_t x = 0; x < width; ++x) {
        const size_t source = static_cast<size_t>(x) * 4u;
        const size_t target = (offset + x) * 3u;
        decoded.rgb[target] = row[source];
        decoded.rgb[target + 1u] = row[source + 1u];
        decoded.rgb[target + 2u] = row[source + 2u];
        decoded.alpha[offset + x] = row[source + 3u];
      }
    }
  } else {
    std::vector<png_bytep> rows(height);
    for (uint32_t y = 0; y < height; ++y) rows[y] = reinterpret_cast<png_bytep>(
        decoded.rgb.data() + static_cast<size_t>(y) * width * 3u);
    png_read_image(png, rows.data());
  }
  png_read_end(png, info);
  read_xmp();
  png_destroy_read_struct(&png, &info, nullptr);
  if (decoded.info.transfer == 18) {
    const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
    parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
      for (size_t p = begin; p < end; ++p) {
        const auto nits = transfer::hlg_to_linear_nits({
            decoded.rgb[p * 3u] / 65535.0,
            decoded.rgb[p * 3u + 1u] / 65535.0,
            decoded.rgb[p * 3u + 2u] / 65535.0});
        for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] =
            static_cast<uint16_t>(std::llround(forward_pq(nits[c]) * 65535.0));
      }
    });
  } else if (decoded.info.primaries == 12) {
    convert_p3_pq_to_rec2020_pq(decoded.rgb);
  } else if (decoded.info.primaries == 1) {
    convert_rec709_pq_to_rec2020_pq(decoded.rgb);
  }
  return decoded;
}


void verify_gainmap_png(const std::vector<uint8_t>& actual,
                        const gainmap::GainMapRaster& raster, size_t channels) {
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) throw std::runtime_error("cannot create gain-map PNG reader");
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); throw std::runtime_error("cannot create PNG info"); }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    throw std::runtime_error("gain-map PNG readback failed");
  }
  PngMemoryReader reader{actual.data(), actual.size()};
  png_set_read_fn(png, &reader, png_read_memory);
  png_read_info(png, info);
  if (png_get_image_width(png, info) != raster.width ||
      png_get_image_height(png, info) != raster.height ||
      png_get_bit_depth(png, info) != 16 ||
      png_get_color_type(png, info) != (channels == 3 ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_GRAY)) {
    png_destroy_read_struct(&png, &info, nullptr);
    throw std::runtime_error("gain-map PNG metadata readback mismatch");
  }
  png_destroy_read_struct(&png, &info, nullptr);
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace hdrbridge::detail
