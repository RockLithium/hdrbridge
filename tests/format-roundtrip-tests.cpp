#include "hdrbridge_core.h"
#include "../core/src/gainmap_adapters.h"
#include <png.h>
#include <tiffio.h>
#include <jpeglib.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#define NOMINMAX
#include <windows.h>

namespace {
constexpr unsigned width = 64, height = 48;
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
void fixture(const std::filesystem::path& path, bool hdr, bool rotated = false) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") || !file) throw std::runtime_error("fixture open failed");
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) throw std::runtime_error("fixture writer failed");
  if (setjmp(png_jmpbuf(png))) throw std::runtime_error("fixture PNG failed");
  png_init_io(png, file);
  png_set_IHDR(png, info, width, height, hdr ? 16 : 8, PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  if (hdr) {
    const png_byte cicp[]{9, 16, 0, 1};
    png_write_chunk(png, reinterpret_cast<png_const_bytep>("cICP"), cicp, sizeof(cicp));
    if (rotated) {
      const png_byte exif[]{'I','I',42,0,8,0,0,0,1,0,0x12,1,3,0,1,0,0,0,6,0,0,0,0,0,0,0};
      png_write_chunk(png, reinterpret_cast<png_const_bytep>("eXIf"), exif, sizeof(exif));
    }
  }
  std::vector<png_byte> row(width * 3u * (hdr ? 2u : 1u));
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) for (unsigned c = 0; c < 3; ++c) {
      // 0.55..0.65 PQ: smooth and distinct channels, safely within P3.
      const unsigned value = 36000u + x * 50u + y * 30u + c * 1800u;
      if (hdr) { row[(x*3u+c)*2u] = static_cast<png_byte>(value >> 8); row[(x*3u+c)*2u+1] = static_cast<png_byte>(value); }
      else row[x*3u+c] = static_cast<png_byte>(value >> 8);
    }
    png_write_row(png, row.data());
  }
  png_write_end(png, info); png_destroy_write_struct(&png, &info); std::fclose(file);
}
double half(uint16_t bits) {
  const int sign = (bits & 0x8000) ? -1 : 1;
  const int exponent = (bits >> 10) & 31;
  const int fraction = bits & 1023;
  if (exponent == 31) return fraction ? NAN : sign * INFINITY;
  if (!exponent) return sign * std::ldexp(static_cast<double>(fraction), -24);
  return sign * std::ldexp(1.0 + fraction / 1024.0, exponent - 15);
}
void compare_export(const std::filesystem::path& path, const std::string& format,
                    const hdrbridge::gainmap::GainMapRaster& expected) {
  if (format == "original") {
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    require(bytes == expected.original_bytes, "original payload differs");
    return;
  }
  const size_t channels = expected.channels == 3 ? 3u : 1u;
  auto compare = [&](const uint16_t* row, uint32_t y) {
    for (uint32_t x = 0; x < expected.width; ++x)
      for (size_t c = 0; c < channels; ++c)
        require(row[x * channels + c] == expected.rgb16[(static_cast<size_t>(y) * expected.width + x) * 3u + c],
                format + " pixel mismatch");
  };
  if (format == "png") {
    FILE* file = nullptr; require(!_wfopen_s(&file, path.c_str(), L"rb") && file, "PNG open");
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) throw std::runtime_error("PNG read failed");
    png_init_io(png, file); png_read_info(png, info);
    require(png_get_image_width(png, info) == expected.width && png_get_image_height(png, info) == expected.height &&
            png_get_bit_depth(png, info) == 16 && png_get_channels(png, info) == channels, "PNG header mismatch");
    png_set_swap(png); png_read_update_info(png, info);
    std::vector<uint16_t> row(expected.width * channels);
    for (uint32_t y = 0; y < expected.height; ++y) {
      png_read_row(png, reinterpret_cast<png_bytep>(row.data()), nullptr); compare(row.data(), y);
    }
    png_read_end(png, info); png_destroy_read_struct(&png, &info, nullptr); std::fclose(file);
  } else if (format == "tiff") {
    TIFF* tif = TIFFOpenW(path.c_str(), "r"); require(tif != nullptr, "TIFF open");
    uint32_t width = 0, height = 0; uint16_t count = 0, bits = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width); TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &count); TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    require(width == expected.width && height == expected.height && count == channels && bits == 16, "TIFF header mismatch");
    std::vector<uint16_t> row(width * channels);
    for (uint32_t y = 0; y < height; ++y) { require(TIFFReadScanline(tif, row.data(), y, 0) >= 0, "TIFF row failed"); compare(row.data(), y); }
    TIFFClose(tif);
  } else {
    FILE* file = nullptr; require(!_wfopen_s(&file, path.c_str(), L"rb") && file, "JPEG open");
    jpeg_decompress_struct jpeg{}; jpeg_error_mgr error{}; jpeg.err = jpeg_std_error(&error); jpeg_create_decompress(&jpeg);
    jpeg_stdio_src(&jpeg, file); require(jpeg_read_header(&jpeg, TRUE) == JPEG_HEADER_OK, "JPEG header failed");
    require(jpeg.image_width == expected.width && jpeg.image_height == expected.height && jpeg.num_components == channels && jpeg.data_precision == 8, "JPEG header mismatch");
    jpeg.out_color_space = channels == 3 ? JCS_RGB : JCS_GRAYSCALE; jpeg_start_decompress(&jpeg);
    std::vector<uint8_t> row(expected.width * channels); double squared = 0; size_t samples = 0;
    while (jpeg.output_scanline < jpeg.output_height) {
      const size_t y = jpeg.output_scanline; JSAMPROW pointer = row.data(); jpeg_read_scanlines(&jpeg, &pointer, 1);
      for (uint32_t x = 0; x < expected.width; ++x) for (size_t c = 0; c < channels; ++c) {
        const double delta = row[x * channels + c] / 255.0 - expected.rgb16[(y * expected.width + x) * 3u + c] / 65535.0;
        squared += delta * delta; ++samples;
      }
    }
    jpeg_finish_decompress(&jpeg); jpeg_destroy_decompress(&jpeg); std::fclose(file);
    const double rmse = std::sqrt(squared / samples); std::cout << "extracted JPEG RMSE " << rmse << '\n';
    require(rmse < .03, "JPEG pixel mismatch");
  }
}
void compare_preview(const hdrbridge::PreviewImage& a, const hdrbridge::PreviewImage& b,
                     double limit, const std::string& label) {
  require(a.width == b.width && a.height == b.height && a.rgba_fp16.size() == b.rgba_fp16.size(), label + " dimensions");
  double sum = 0; size_t n = 0;
  for (size_t i = 0; i < a.rgba_fp16.size(); ++i) if (i % 4 != 3) {
    const double v = half(b.rgba_fp16[i]);
    require(std::isfinite(v), label + " nonfinite");
    const double d = half(a.rgba_fp16[i]) - v; sum += d*d; ++n;
  }
  const double rmse = std::sqrt(sum / n);
  std::cout << label << " preview RMSE " << rmse << '\n';
  if (rmse > limit) throw std::runtime_error(label + " preview RMSE " + std::to_string(rmse));
}
}
int main() {
  const auto dir = std::filesystem::temp_directory_path() / ("hdrbridge-roundtrip-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    std::filesystem::create_directories(dir);
    const auto src = dir / "source.png";
    fixture(src, true); fixture(dir / "sdr.png", false);
    require(hdrbridge::inspect(dir / "sdr.png").asset_kind == "non-HDR", "SDR classification");
    require(hdrbridge::inspect(src).asset_kind == "direct-hdr", "HDR classification");
    const auto reference = hdrbridge::decode_preview(src, width);
    struct Case { const char* mode; const char* name; const char* transfer; const char* gamut; bool gain; int scale; double rmse; };
    const Case cases[]{
      {"png-pq16", "direct.png", "pq", "rec2020", false, 2, 0},
      {"png-pq16", "hlg.png", "hlg", "rec2020", false, 2, .01},
      {"jxl-pq16", "direct.jxl", "pq", "rec2020", false, 2, 0},
      {"jxl-pq16", "hlg.jxl", "hlg", "rec2020", false, 2, .01},
      {"tiff-pq16", "direct.tiff", "pq", "rec2020", false, 2, 0},
      {"avif-pq10", "direct.avif", "pq", "rec2020", false, 2, .06},
      {"jxr-scrgb-fp16", "direct.jxr", "pq", "rec2020", false, 2, 0},
      {"ultrahdr", "gain.jpg", "pq", "rec2020", false, 2, .12},
      {"jxl-pq16", "gain-full.jxl", "pq", "rec2020", true, 1, .08},
      {"jxl-pq16", "gain-half.jxl", "pq", "p3", true, 2, .08},
      {"avif-pq10", "gain-full.avif", "pq", "rec2020", true, 1, .08},
      {"avif-pq10", "gain-half.avif", "pq", "p3", true, 2, .08},
    };
    for (const auto& c : cases) {
      hdrbridge::ConversionOptions options;
      options.mode = c.mode; options.output_transfer = c.transfer; options.output_gamut = c.gamut;
      options.output_representation = c.gain ? "gainmap" : "direct";
      options.multi_channel_gainmap = c.gain; options.gainmap_scale = c.scale; options.effort = 3;
      const auto path = dir / c.name;
      const auto result = hdrbridge::convert(src, path, options);
      require(result.success && result.verification.passed, std::string(c.name) + " verification");
      require(result.verification.width == width && result.verification.height == height, std::string(c.name) + " metadata dimensions");
      if (!c.gain && (std::string(c.mode) == "png-pq16" ||
                      std::string(c.mode) == "jxl-pq16" ||
                      std::string(c.mode) == "tiff-pq16" ||
                      std::string(c.mode) == "jxr-scrgb-fp16"))
        require(result.verification.exact_roundtrip, std::string(c.name) + " full pixel roundtrip");
      if (c.gain) {
        require(result.verification.gain_map_width == (width + c.scale - 1) / c.scale &&
                result.verification.gain_map_height == (height + c.scale - 1) / c.scale &&
                result.verification.gain_map_channels == 3, std::string(c.name) + " gain map metadata");
      }
      hdrbridge::clear_input_cache();
      compare_preview(reference, hdrbridge::decode_preview(path, width), c.rmse, c.name);
    }
    fixture(dir / "oriented.png", true, true);
    const auto oriented = hdrbridge::inspect(dir / "oriented.png");
    require(oriented.width == height && oriented.height == width, "EXIF orientation dimensions");
    hdrbridge::ConversionOptions oriented_options; oriented_options.mode = "png-pq16";
    const auto oriented_result = hdrbridge::convert(dir / "oriented.png", dir / "oriented-out.png", oriented_options);
    require(oriented_result.verification.passed && oriented_result.verification.exact_roundtrip &&
            oriented_result.verification.width == height && oriented_result.verification.height == width,
            "EXIF orientation roundtrip");
    hdrbridge::ConversionOptions icc_options; icc_options.mode = "png-pq16"; icc_options.diagnostic_icc_only = true;
    require(hdrbridge::convert(src, dir / "icc.png", icc_options).verification.passed, "ICC-only output");
    require(hdrbridge::inspect(dir / "icc.png").asset_kind == "direct-hdr", "ICC-only classification");
    const auto map = hdrbridge::gainmap::extract_gain_map(dir / "gain-half.jxl", nullptr, true);
    for (const char* format : {"original", "png", "tiff", "jpeg"}) {
      hdrbridge::ConversionOptions options; options.mode = "gainmap-extract"; options.gainmap_export_format = format;
      const auto output = dir / (std::string("extracted-") + format + (std::string(format) == "original" ? ".jxl" : "." + std::string(format)));
      require(hdrbridge::convert(dir / "gain-half.jxl", output, options).verification.passed, std::string("extract ") + format);
      compare_export(output, format, map);
    }
    const auto preservation = [&](bool extraction, bool locked) {
      const auto target = dir / (extraction ? "preserve-extract.png" : "preserve-convert.png");
      { std::ofstream out(target, std::ios::binary); out << "original sentinel"; }
      HANDLE lock = INVALID_HANDLE_VALUE;
      if (locked) {
        lock = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(lock != INVALID_HANDLE_VALUE, "lock target");
      }
      hdrbridge::ConversionOptions options;
      options.mode = extraction ? "gainmap-extract" : "png-pq16";
      options.gainmap_export_format = "png"; options.overwrite = true;
      std::atomic_bool cancel{false};
      bool failed = false;
      std::string error;
      try {
        hdrbridge::convert(extraction ? dir / "gain-half.jxl" : src, target, options,
            [&](int percent, const std::string&) {
              if (!locked && percent >= (extraction ? 70 : 88)) cancel.store(true);
            }, &cancel);
      } catch (const std::exception& e) { failed = true; error = e.what(); }
      if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
      require(failed, "expected failed conversion");
      require(locked ? error.find("commit") != std::string::npos || error.find("replace") != std::string::npos || error.find("output") != std::string::npos
                     : error.find("cancelled") != std::string::npos,
              "unexpected conversion failure: " + error);
      std::ifstream in(target, std::ios::binary);
      const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      require(content == "original sentinel", "failed conversion changed target");
      for (const auto& entry : std::filesystem::directory_iterator(dir))
        require(entry.path().filename().wstring().find(L".hdrbridge-") != 0,
                "failed conversion left staging file");
    };
    preservation(false, false); preservation(true, false);
    preservation(false, true); preservation(true, true);
    hdrbridge::ConversionOptions completed_options;
    completed_options.mode = "png-pq16";
    const auto completed = hdrbridge::convert(src, dir / "completed.png", completed_options,
        [](int percent, const std::string&) {
          if (percent == 100) throw std::runtime_error("completion notification failed");
        });
    require(completed.success && completed.verification.passed &&
            std::filesystem::exists(completed.output_path), "committed result reported failure");
    std::filesystem::remove_all(dir); return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n'; std::error_code ignored; std::filesystem::remove_all(dir, ignored); return 1;
  }
}
