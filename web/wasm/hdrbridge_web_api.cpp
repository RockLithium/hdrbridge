#include "hdrbridge_core.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> output_bytes;
std::string info_json = "{}";
std::string last_error;

std::string clean_extension(const char* value) {
  std::string extension = value ? value : "";
  if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
  if (extension.empty() || extension.size() > 8 ||
      !std::all_of(extension.begin(), extension.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0;
      })) {
    throw std::runtime_error("invalid input extension");
  }
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return extension;
}

void write_input(const std::filesystem::path& path,
                 const uint8_t* bytes,
                 size_t byte_count) {
  if (!bytes || byte_count == 0) throw std::runtime_error("input is empty");
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream || !stream.write(reinterpret_cast<const char*>(bytes),
                               static_cast<std::streamsize>(byte_count))) {
    throw std::runtime_error("cannot stage the input in browser memory");
  }
}

std::vector<uint8_t> read_output(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::runtime_error("the converter did not create an output");
  const auto size = stream.tellg();
  if (size <= 0) throw std::runtime_error("the converter created an empty output");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  stream.seekg(0);
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("cannot read the converted output");
  }
  return bytes;
}

struct StagedFiles {
  std::filesystem::path input;
  std::filesystem::path output;
  ~StagedFiles() {
    std::error_code ignored;
    std::filesystem::remove(input, ignored);
    if (!output.empty()) std::filesystem::remove(output, ignored);
  }
};

StagedFiles stage_input(const uint8_t* bytes, size_t byte_count,
                        const char* extension) {
  StagedFiles files;
  files.input = std::filesystem::path("/tmp") /
      ("hdrbridge-input." + clean_extension(extension));
  write_input(files.input, bytes, byte_count);
  return files;
}

const char* mode_for_format(int format) {
  switch (format) {
    case 0: return "ultrahdr";
    case 1: return "png-pq16";
    case 2: return "jxl-pq16";
    case 3: return "jxr-scrgb-fp16";
    case 4: return "avif-pq10";
    case 5: return "tiff-pq16";
    default: throw std::runtime_error("invalid output format");
  }
}

const char* extension_for_format(int format) {
  switch (format) {
    case 0: return "jpg";
    case 1: return "png";
    case 2: return "jxl";
    case 3: return "jxr";
    case 4: return "avif";
    case 5: return "tiff";
    default: throw std::runtime_error("invalid output format");
  }
}

int fail(const std::exception& error) {
  output_bytes.clear();
  info_json = "{}";
  last_error = error.what();
  return 1;
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int hb_inspect_asset(const uint8_t* bytes,
                                         size_t byte_count,
                                         const char* extension) {
  output_bytes.clear();
  info_json = "{}";
  last_error.clear();
  try {
    auto files = stage_input(bytes, byte_count, extension);
    info_json = hdrbridge::to_json(hdrbridge::inspect(files.input));
    return 0;
  } catch (const std::exception& error) {
    return fail(error);
  }
}

EMSCRIPTEN_KEEPALIVE int hb_convert_asset(
    const uint8_t* bytes, size_t byte_count, const char* extension,
    int format, int primaries, int transfer, int output_representation,
    int encoding_value,
    int lossless, int copy_exif, int copy_xmp, int gain_scale,
    int gain_channels, float target_peak_nits) {
  output_bytes.clear();
  info_json = "{}";
  last_error.clear();
  try {
    auto files = stage_input(bytes, byte_count, extension);
    files.output = std::filesystem::path("/tmp") /
        (std::string("hdrbridge-output.") + extension_for_format(format));

    hdrbridge::ConversionOptions options;
    options.mode = mode_for_format(format);
    options.output_gamut = primaries == 1 ? "rec709" : primaries == 12 ? "p3" : "rec2020";
    options.output_transfer = transfer == 18 ? "hlg" : "pq";
    options.output_representation = output_representation ? "gainmap" : "direct";
    options.lossless = lossless != 0;
    options.image_quality = std::clamp(encoding_value / 100.0f, 0.01f, 1.0f);
    options.base_quality = std::clamp(encoding_value, 1, 100);
    options.gainmap_quality = options.base_quality;
    options.gainmap_scale = gain_scale;
    options.multi_channel_gainmap = gain_channels != 0;
    options.target_peak_nits = target_peak_nits;
    options.png_compression_level = std::clamp(encoding_value, 1, 9);
    options.tiff_compression_level = std::clamp(encoding_value, 1, 9);
    options.copy_exif = copy_exif != 0;
    options.copy_xmp = copy_xmp != 0;
    options.overwrite = true;

    const auto result = hdrbridge::convert(files.input, files.output, options);
    if (!result.success) throw std::runtime_error("conversion verification failed");
    output_bytes = read_output(files.output);
    info_json = hdrbridge::to_json(result);
    return 0;
  } catch (const std::exception& error) {
    return fail(error);
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
  info_json = "{}";
  last_error.clear();
}

}  // extern "C"
