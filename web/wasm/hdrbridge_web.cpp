#include "hdr_transfer.h"

#include <emscripten/emscripten.h>
#include <libheif/heif.h>
#include <png.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct ContextDeleter {
  void operator()(heif_context* value) const { heif_context_free(value); }
};
struct HandleDeleter {
  void operator()(heif_image_handle* value) const {
    heif_image_handle_release(value);
  }
};
struct ImageDeleter {
  void operator()(heif_image* value) const { heif_image_release(value); }
};
struct OptionsDeleter {
  void operator()(heif_decoding_options* value) const {
    heif_decoding_options_free(value);
  }
};
struct NclxDeleter {
  void operator()(heif_color_profile_nclx* value) const {
    heif_nclx_color_profile_free(value);
  }
};

using ContextPtr = std::unique_ptr<heif_context, ContextDeleter>;
using HandlePtr = std::unique_ptr<heif_image_handle, HandleDeleter>;
using ImagePtr = std::unique_ptr<heif_image, ImageDeleter>;
using OptionsPtr = std::unique_ptr<heif_decoding_options, OptionsDeleter>;
using NclxPtr = std::unique_ptr<heif_color_profile_nclx, NclxDeleter>;

std::vector<uint8_t> output_bytes;
std::string info_json = "{}";
std::string last_error;

void require_heif(const heif_error& error, const char* operation) {
  if (error.code == heif_error_Ok) return;
  throw std::runtime_error(std::string(operation) + ": " +
      (error.message ? error.message : "libheif error"));
}

std::string json_escape(const std::string& text) {
  std::ostringstream out;
  for (const char ch : text) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out << "?";
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

struct PngWriter {
  std::vector<uint8_t> bytes;
};

void png_write_memory(png_structp png, png_bytep data, png_size_t size) {
  auto* writer = static_cast<PngWriter*>(png_get_io_ptr(png));
  writer->bytes.insert(writer->bytes.end(), data, data + size);
}

void png_flush_memory(png_structp) {}

struct PngReader {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

void png_read_memory(png_structp png, png_bytep output, png_size_t size) {
  auto* reader = static_cast<PngReader*>(png_get_io_ptr(png));
  if (!reader || size > reader->size - reader->offset) {
    png_error(png, "unexpected end of PNG stream");
  }
  std::memcpy(output, reader->bytes + reader->offset, size);
  reader->offset += size;
}

uint32_t crc32_chunk(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320u &
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u)));
    }
  }
  return crc ^ 0xffffffffu;
}

void append_be32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 24));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value));
}

void insert_cicp_after_ihdr(std::vector<uint8_t>& png,
                            const std::array<uint8_t, 4>& cicp) {
  // Signature (8) + IHDR length/type/data/CRC (25).
  if (png.size() < 33 || std::memcmp(png.data() + 12, "IHDR", 4) != 0) {
    throw std::runtime_error("PNG writer produced an invalid stream");
  }
  std::vector<uint8_t> chunk;
  append_be32(chunk, 4);
  chunk.insert(chunk.end(), {'c', 'I', 'C', 'P'});
  chunk.insert(chunk.end(), cicp.begin(), cicp.end());
  append_be32(chunk, crc32_chunk(chunk.data() + 4, 8));
  png.insert(png.begin() + 33, chunk.begin(), chunk.end());
}

uint32_t png_pixel_crc32(const uint8_t* bytes, size_t size) {
  if (!bytes || size < 33) throw std::runtime_error("invalid PNG input");
  png_structp png = png_create_read_struct(
      PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) throw std::runtime_error("cannot create PNG reader");
  png_infop png_info = png_create_info_struct(png);
  if (!png_info) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    throw std::runtime_error("cannot create PNG read info");
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &png_info, nullptr);
    throw std::runtime_error("libpng decoding failed");
  }
  PngReader reader{bytes, size, 0};
  png_set_read_fn(png, &reader, png_read_memory);
  png_read_info(png, png_info);
  const uint32_t width = png_get_image_width(png, png_info);
  const uint32_t height = png_get_image_height(png, png_info);
  if (png_get_bit_depth(png, png_info) != 16 ||
      png_get_color_type(png, png_info) != PNG_COLOR_TYPE_RGB) {
    png_destroy_read_struct(&png, &png_info, nullptr);
    throw std::runtime_error("expected a 16-bit RGB PNG");
  }
  png_set_swap(png);
  png_read_update_info(png, png_info);
  std::vector<uint16_t> pixels(
      static_cast<size_t>(width) * height * 3u);
  std::vector<png_bytep> rows(height);
  for (uint32_t y = 0; y < height; ++y) {
    rows[y] = reinterpret_cast<png_bytep>(
        pixels.data() + static_cast<size_t>(y) * width * 3u);
  }
  png_read_image(png, rows.data());
  png_read_end(png, nullptr);
  png_destroy_read_struct(&png, &png_info, nullptr);
  return crc32_chunk(reinterpret_cast<const uint8_t*>(pixels.data()),
                     pixels.size() * sizeof(uint16_t));
}

std::vector<uint8_t> encode_png(const std::vector<uint16_t>& pixels,
                                uint32_t width,
                                uint32_t height,
                                uint8_t primaries,
                                uint8_t transfer,
                                int compression_level) {
  png_structp png = png_create_write_struct(
      PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) throw std::runtime_error("cannot create PNG writer");
  png_infop png_info = png_create_info_struct(png);
  if (!png_info) {
    png_destroy_write_struct(&png, nullptr);
    throw std::runtime_error("cannot create PNG info");
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &png_info);
    throw std::runtime_error("libpng encoding failed");
  }

  PngWriter writer;
  png_set_write_fn(png, &writer, png_write_memory, png_flush_memory);
  png_set_IHDR(png, png_info, width, height, 16, PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE,
               PNG_FILTER_TYPE_BASE);
  png_set_compression_level(png, std::clamp(compression_level, 1, 9));
  png_text software{};
  software.compression = PNG_TEXT_COMPRESSION_NONE;
  software.key = const_cast<png_charp>("Software");
  software.text = const_cast<png_charp>("HDR Bridge Web");
  png_set_text(png, png_info, &software, 1);
  png_write_info(png, png_info);

  // PNG stores 16-bit samples in network byte order; WebAssembly is LE.
  png_set_swap(png);
  std::vector<png_bytep> rows(height);
  for (uint32_t y = 0; y < height; ++y) {
    rows[y] = reinterpret_cast<png_bytep>(const_cast<uint16_t*>(
        pixels.data() + static_cast<size_t>(y) * width * 3u));
  }
  png_write_image(png, rows.data());
  png_write_end(png, png_info);
  png_destroy_write_struct(&png, &png_info);

  insert_cicp_after_ihdr(writer.bytes, {primaries, transfer, 0, 1});
  return std::move(writer.bytes);
}

std::array<double, 3> p3_to_rec2020(const std::array<double, 3>& p3) {
  return {
      0.7538330 * p3[0] + 0.1985974 * p3[1] + 0.0475696 * p3[2],
      0.0457438 * p3[0] + 0.9417772 * p3[1] + 0.0124789 * p3[2],
     -0.0012103 * p3[0] + 0.0176017 * p3[1] + 0.9836086 * p3[2]};
}

std::array<double, 3> rec2020_to_p3(const std::array<double, 3>& rec2020) {
  return {
      1.3435783 * rec2020[0] - 0.2821797 * rec2020[1] - 0.0613986 * rec2020[2],
     -0.0652975 * rec2020[0] + 1.0757879 * rec2020[1] - 0.0104904 * rec2020[2],
      0.0028218 * rec2020[0] - 0.0195985 * rec2020[1] + 1.0167767 * rec2020[2]};
}

uint16_t quantize(double signal) {
  return static_cast<uint16_t>(std::llround(
      std::clamp(signal, 0.0, 1.0) * 65535.0));
}

struct ConversionStats {
  double peak_nits = 0.0;
  double min_linear = std::numeric_limits<double>::infinity();
  double max_linear = -std::numeric_limits<double>::infinity();
};

ConversionStats transform_pixels(std::vector<uint16_t>& pixels,
                                 uint16_t source_primaries,
                                 uint16_t source_transfer,
                                 uint16_t output_primaries,
                                 uint16_t output_transfer) {
  if (output_transfer == 18 && output_primaries != 9) {
    throw std::runtime_error("HLG output is only defined for BT.2020");
  }
  ConversionStats stats;
  for (size_t p = 0; p < pixels.size() / 3u; ++p) {
    const std::array<double, 3> source{
        pixels[p * 3u] / 65535.0,
        pixels[p * 3u + 1u] / 65535.0,
        pixels[p * 3u + 2u] / 65535.0};
    std::array<uint16_t, 3> canonical{};
    if (source_transfer == 16 && source_primaries == 9) {
      canonical = {pixels[p * 3u], pixels[p * 3u + 1u], pixels[p * 3u + 2u]};
    } else {
      std::array<double, 3> source_linear = source_transfer == 16
          ? std::array<double, 3>{
              hdrbridge::transfer::pq_to_nits(source[0]),
              hdrbridge::transfer::pq_to_nits(source[1]),
              hdrbridge::transfer::pq_to_nits(source[2])}
          : hdrbridge::transfer::hlg_to_linear_nits(source);
      if (source_primaries == 12) source_linear = p3_to_rec2020(source_linear);
      canonical = {
          quantize(hdrbridge::transfer::nits_to_pq(source_linear[0])),
          quantize(hdrbridge::transfer::nits_to_pq(source_linear[1])),
          quantize(hdrbridge::transfer::nits_to_pq(source_linear[2]))};
    }

    std::array<double, 3> linear{
        hdrbridge::transfer::pq_to_nits(canonical[0] / 65535.0),
        hdrbridge::transfer::pq_to_nits(canonical[1] / 65535.0),
        hdrbridge::transfer::pq_to_nits(canonical[2] / 65535.0)};
    for (const double value : linear) {
      stats.min_linear = std::min(stats.min_linear, value);
      stats.max_linear = std::max(stats.max_linear, value);
      stats.peak_nits = std::max(stats.peak_nits, value);
    }
    if (output_transfer == 16 && output_primaries == 9) {
      pixels[p * 3u] = canonical[0];
      pixels[p * 3u + 1u] = canonical[1];
      pixels[p * 3u + 2u] = canonical[2];
    } else {
      if (output_primaries == 12) linear = rec2020_to_p3(linear);
      const std::array<double, 3> encoded = output_transfer == 16
          ? std::array<double, 3>{
              hdrbridge::transfer::nits_to_pq(linear[0]),
              hdrbridge::transfer::nits_to_pq(linear[1]),
              hdrbridge::transfer::nits_to_pq(linear[2])}
          : hdrbridge::transfer::linear_nits_to_hlg(linear);
      pixels[p * 3u] = quantize(encoded[0]);
      pixels[p * 3u + 1u] = quantize(encoded[1]);
      pixels[p * 3u + 2u] = quantize(encoded[2]);
    }
  }
  return stats;
}

void convert_heif(const uint8_t* bytes,
                  size_t byte_count,
                  int output_primaries,
                  int output_transfer,
                  int compression_level) {
  if (!bytes || byte_count < 12) throw std::runtime_error("input is empty");
  if ((output_primaries != 9 && output_primaries != 12) ||
      (output_transfer != 16 && output_transfer != 18)) {
    throw std::runtime_error("invalid output color signaling");
  }

  ContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  heif_context_set_max_decoding_threads(context.get(), 1);
  require_heif(heif_context_read_from_memory_without_copy(
      context.get(), bytes, byte_count, nullptr), "read HEIF");

  heif_image_handle* raw_handle = nullptr;
  require_heif(heif_context_get_primary_image_handle(
      context.get(), &raw_handle), "get primary image");
  HandlePtr handle(raw_handle);

  heif_color_profile_nclx* raw_nclx = nullptr;
  require_heif(heif_image_handle_get_nclx_color_profile(
      handle.get(), &raw_nclx), "read NCLX profile");
  NclxPtr nclx(raw_nclx);
  const uint16_t source_primaries =
      static_cast<uint16_t>(nclx->color_primaries);
  const uint16_t source_transfer =
      static_cast<uint16_t>(nclx->transfer_characteristics);
  const uint16_t source_matrix =
      static_cast<uint16_t>(nclx->matrix_coefficients);
  const bool source_full_range = nclx->full_range_flag != 0;
  const bool direct_pq = source_transfer == 16 &&
      (source_primaries == 9 || source_primaries == 12);
  const bool direct_hlg = source_transfer == 18 && source_primaries == 9;
  if (!direct_pq && !direct_hlg) {
    throw std::runtime_error(
        "input primary image is not direct PQ or BT.2100 HLG HDR");
  }

  const int source_bits = heif_image_handle_get_luma_bits_per_pixel(handle.get());
  if (source_bits <= 8 || source_bits > 16) {
    throw std::runtime_error("input does not expose a supported HDR bit depth");
  }

  OptionsPtr options(heif_decoding_options_alloc());
  if (!options) throw std::bad_alloc();
  options->convert_hdr_to_8bit = 0;
  options->output_image_nclx_profile_passthrough = 1;
  // Keep transformations enabled. libheif returns a canonical upright raster
  // and presentation dimensions, avoiding an extra full-frame orientation pass.
  options->ignore_transformations = 0;

  heif_image* raw_image = nullptr;
  require_heif(heif_decode_image(handle.get(), &raw_image,
      heif_colorspace_RGB, heif_chroma_interleaved_RRGGBB_LE,
      options.get()), "decode HEIF RGB16");
  ImagePtr image(raw_image);

  const int width = heif_image_get_primary_width(image.get());
  const int height = heif_image_get_primary_height(image.get());
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("decoded image has invalid dimensions");
  }
  const uint64_t sample_count = static_cast<uint64_t>(width) *
      static_cast<uint64_t>(height) * 3u;
  if (sample_count > std::numeric_limits<size_t>::max() / sizeof(uint16_t)) {
    throw std::runtime_error("decoded image is too large for this browser");
  }

  int stride = 0;
  const uint8_t* plane = heif_image_get_plane_readonly(
      image.get(), heif_channel_interleaved, &stride);
  if (!plane || stride < width * 6) {
    throw std::runtime_error("invalid RGB16 decode plane");
  }
  std::vector<uint16_t> pixels(static_cast<size_t>(sample_count));
  const uint32_t source_max = (1u << source_bits) - 1u;
  for (int y = 0; y < height; ++y) {
    const auto* source = reinterpret_cast<const uint16_t*>(
        plane + static_cast<size_t>(y) * static_cast<size_t>(stride));
    auto* target = pixels.data() + static_cast<size_t>(y) *
        static_cast<size_t>(width) * 3u;
    for (size_t i = 0; i < static_cast<size_t>(width) * 3u; ++i) {
      target[i] = source_bits < 16
          ? static_cast<uint16_t>((static_cast<uint32_t>(source[i]) * 65535u +
              source_max / 2u) / source_max)
          : source[i];
    }
  }

  const auto stats = transform_pixels(pixels, source_primaries,
      source_transfer, static_cast<uint16_t>(output_primaries),
      static_cast<uint16_t>(output_transfer));
  output_bytes = encode_png(pixels, static_cast<uint32_t>(width),
      static_cast<uint32_t>(height), static_cast<uint8_t>(output_primaries),
      static_cast<uint8_t>(output_transfer), compression_level);

  std::ostringstream json;
  json << "{\"input\":{\"container\":\"HEIF/HIF\",\"width\":"
       << width << ",\"height\":" << height
       << ",\"bitDepth\":" << source_bits
       << ",\"primaries\":" << source_primaries
       << ",\"transfer\":" << source_transfer
       << ",\"matrix\":" << source_matrix
       << ",\"fullRange\":" << (source_full_range ? "true" : "false")
       << "},\"output\":{\"format\":\"PNG\",\"bitDepth\":16,"
       << "\"primaries\":" << output_primaries
       << ",\"transfer\":" << output_transfer
       << ",\"matrix\":0,\"fullRange\":true},"
       << "\"reconstructed\":{\"peakNits\":" << stats.peak_nits
       << ",\"minLinear\":" << stats.min_linear
       << ",\"maxLinear\":" << stats.max_linear << "}}";
  info_json = json.str();
}

void inspect_heif(const uint8_t* bytes, size_t byte_count) {
  if (!bytes || byte_count < 12) throw std::runtime_error("input is empty");
  ContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  require_heif(heif_context_read_from_memory_without_copy(
      context.get(), bytes, byte_count, nullptr), "read HEIF");
  heif_image_handle* raw_handle = nullptr;
  require_heif(heif_context_get_primary_image_handle(
      context.get(), &raw_handle), "get primary image");
  HandlePtr handle(raw_handle);

  heif_color_profile_nclx* raw_nclx = nullptr;
  require_heif(heif_image_handle_get_nclx_color_profile(
      handle.get(), &raw_nclx), "read NCLX profile");
  NclxPtr nclx(raw_nclx);
  const uint16_t primaries = static_cast<uint16_t>(nclx->color_primaries);
  const uint16_t transfer =
      static_cast<uint16_t>(nclx->transfer_characteristics);
  const uint16_t matrix = static_cast<uint16_t>(nclx->matrix_coefficients);
  const bool full_range = nclx->full_range_flag != 0;
  const bool direct_pq = transfer == 16 && (primaries == 9 || primaries == 12);
  const bool direct_hlg = transfer == 18 && primaries == 9;

  bool exif = false;
  bool xmp = false;
  const int metadata_count =
      heif_image_handle_get_number_of_metadata_blocks(handle.get(), nullptr);
  std::vector<heif_item_id> metadata_ids(
      static_cast<size_t>(std::max(metadata_count, 0)));
  const int metadata_read = metadata_ids.empty() ? 0 :
      heif_image_handle_get_list_of_metadata_block_IDs(
          handle.get(), nullptr, metadata_ids.data(), metadata_count);
  for (int index = 0; index < metadata_read; ++index) {
    const char* type = heif_image_handle_get_metadata_type(
        handle.get(), metadata_ids[static_cast<size_t>(index)]);
    const char* content_type = heif_image_handle_get_metadata_content_type(
        handle.get(), metadata_ids[static_cast<size_t>(index)]);
    exif = exif || (type && std::strcmp(type, "Exif") == 0);
    xmp = xmp || (type && std::strcmp(type, "mime") == 0 && content_type &&
        (std::strstr(content_type, "xml") || std::strstr(content_type, "rdf")));
  }
  const bool icc = heif_image_handle_get_raw_color_profile_size(handle.get()) > 0;
  std::string brand = "HEIF";
  if (byte_count >= 12 && std::memcmp(bytes + 4, "ftyp", 4) == 0) {
    brand.assign(reinterpret_cast<const char*>(bytes + 8), 4);
  }
  const std::string asset = direct_pq ? "Direct HDR / PQ" :
      direct_hlg ? "Direct HDR / HLG" : "No supported direct HDR signal";
  const std::string primaries_name = primaries == 9 ? "BT.2020" :
      primaries == 12 ? "Display P3" : "CICP " + std::to_string(primaries);
  const std::string transfer_name = transfer == 16 ? "PQ / ST2084" :
      transfer == 18 ? "HLG / BT.2100" : "CICP " + std::to_string(transfer);

  std::ostringstream json;
  json << "{\"input\":{\"container\":\"HEIF/HIF\",\"brand\":\""
       << json_escape(brand) << "\",\"asset\":\"" << asset
       << "\",\"width\":" << heif_image_handle_get_width(handle.get())
       << ",\"height\":" << heif_image_handle_get_height(handle.get())
       << ",\"bitDepth\":"
       << heif_image_handle_get_luma_bits_per_pixel(handle.get())
       << ",\"primaries\":" << primaries << ",\"primariesName\":\""
       << primaries_name << "\",\"transfer\":" << transfer
       << ",\"transferName\":\"" << transfer_name
       << "\",\"matrix\":" << matrix
       << ",\"fullRange\":" << (full_range ? "true" : "false")
       << ",\"metadata\":{\"exif\":" << (exif ? "true" : "false")
       << ",\"xmp\":" << (xmp ? "true" : "false")
       << ",\"icc\":" << (icc ? "true" : "false") << "}},"
       << "\"supported\":" << (direct_pq || direct_hlg ? "true" : "false")
       << "}";
  info_json = json.str();
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int hb_convert_heif_to_png(
    const uint8_t* bytes, size_t byte_count,
    int output_primaries, int output_transfer, int compression_level) {
  output_bytes.clear();
  info_json = "{}";
  last_error.clear();
  try {
    convert_heif(bytes, byte_count, output_primaries, output_transfer,
                 compression_level);
    return 0;
  } catch (const std::exception& error) {
    last_error = error.what();
    return 1;
  } catch (...) {
    last_error = "unknown conversion error";
    return 1;
  }
}

EMSCRIPTEN_KEEPALIVE int hb_inspect_heif(
    const uint8_t* bytes, size_t byte_count) {
  info_json = "{}";
  last_error.clear();
  try {
    inspect_heif(bytes, byte_count);
    return 0;
  } catch (const std::exception& error) {
    last_error = error.what();
    return 1;
  }
}

EMSCRIPTEN_KEEPALIVE uint32_t hb_png_pixel_crc32(
    const uint8_t* bytes, size_t byte_count) {
  last_error.clear();
  try {
    return png_pixel_crc32(bytes, byte_count);
  } catch (const std::exception& error) {
    last_error = error.what();
    return 0;
  }
}

EMSCRIPTEN_KEEPALIVE const uint8_t* hb_output_data() {
  return output_bytes.empty() ? nullptr : output_bytes.data();
}

EMSCRIPTEN_KEEPALIVE size_t hb_output_size() { return output_bytes.size(); }

EMSCRIPTEN_KEEPALIVE const char* hb_info_json() { return info_json.c_str(); }

EMSCRIPTEN_KEEPALIVE const char* hb_last_error() { return last_error.c_str(); }

EMSCRIPTEN_KEEPALIVE void hb_clear_result() {
  output_bytes.clear();
  output_bytes.shrink_to_fit();
  info_json = "{}";
  last_error.clear();
}

}
