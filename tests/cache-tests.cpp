#include "hdrbridge_core.h"

#include <png.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {

using Pixels = std::array<png_uint_16, 12>;  // 2x2 RGB16

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void write_png(const fs::path& path, const Pixels& pixels, bool hdr) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
    throw std::runtime_error("cannot create test PNG");
  }
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) {
    if (png) png_destroy_write_struct(&png, nullptr);
    std::fclose(file);
    throw std::runtime_error("cannot create PNG writer");
  }
#pragma warning(push)
#pragma warning(disable : 4611)
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
    throw std::runtime_error("cannot write test PNG");
  }
#pragma warning(pop)
  png_init_io(png, file);
  png_set_IHDR(png, info, 2, 2, 16, PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  if (hdr) png_set_cICP(png, info, 9, 16, 0, 1);  // Rec.2020, PQ, identity, full range.
  png_set_compression_level(png, 0);  // Keep same-geometry fixture sizes stable.
  png_write_info(png, info);
  png_byte rows_storage[2][12]{};
  for (size_t i = 0; i < pixels.size(); ++i) {
    rows_storage[i / 6][(i % 6) * 2] = static_cast<png_byte>(pixels[i] >> 8);
    rows_storage[i / 6][(i % 6) * 2 + 1] = static_cast<png_byte>(pixels[i] & 0xff);
  }
  png_bytep rows[]{rows_storage[0], rows_storage[1]};
  png_write_image(png, rows);
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  std::fclose(file);
}

Pixels read_png(const fs::path& path) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) throw std::runtime_error("cannot read output PNG");
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) {
    if (png) png_destroy_read_struct(&png, nullptr, nullptr);
    std::fclose(file);
    throw std::runtime_error("cannot create PNG reader");
  }
#pragma warning(push)
#pragma warning(disable : 4611)
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(file);
    throw std::runtime_error("cannot decode output PNG");
  }
#pragma warning(pop)
  png_init_io(png, file);
  png_read_info(png, info);
  require(png_get_bit_depth(png, info) == 16 && png_get_color_type(png, info) == PNG_COLOR_TYPE_RGB,
          "output is not RGB16 PNG");
  png_byte rows_storage[2][12]{};
  png_bytep rows[]{rows_storage[0], rows_storage[1]};
  png_read_image(png, rows);
  Pixels pixels{};
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<png_uint_16>((rows_storage[i / 6][(i % 6) * 2] << 8) |
                                         rows_storage[i / 6][(i % 6) * 2 + 1]);
  }
  png_destroy_read_struct(&png, &info, nullptr);
  std::fclose(file);
  return pixels;
}

hdrbridge::ConversionResult convert(const fs::path& input, const fs::path& output) {
  hdrbridge::ConversionOptions options;
  options.mode = "png-pq16";
  options.overwrite = true;
  return hdrbridge::convert(input, output, options);
}

bool conversion_fails(const fs::path& input, const fs::path& output) {
  try {
    const auto result = convert(input, output);
    return !result.success;
  } catch (...) {
    return true;
  }
}

}  // namespace

int main() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path() /
                    ("hdrbridge-cache-regression-" + std::to_string(nonce));
  const auto source = root / "source.png";
  const auto other = root / "other.png";
  const auto sdr = root / "sdr.png";
  const auto corrupt = root / "corrupt.png";
  try {
    fs::create_directories(root);
    const Pixels a{32768, 40000, 50000, 25000, 35000, 45000,
                   30000, 42000, 52000, 20000, 32000, 48000};
    const Pixels b{31000, 39000, 49000, 23000, 33000, 43000,
                   28000, 40000, 50000, 18000, 30000, 46000};
    const Pixels c{29000, 37000, 47000, 21000, 31000, 41000,
                   26000, 38000, 48000, 16000, 28000, 44000};
    write_png(source, a, true);
    write_png(other, b, true);
    require(fs::file_size(source) == fs::file_size(other), "equal-sized fixture precondition failed");
    hdrbridge::clear_input_cache();

    const auto info = hdrbridge::inspect(source);
    require(info.transfer_kind == hdrbridge::HdrTransfer::pq_st2084, "PQ fixture inspection failed");
    auto first = convert(source, root / "first.png");
    require(first.success && first.verification.passed, "first conversion failed");
    require(first.timings.canonical_cache_hit, "inspect did not warm cache");
    require(read_png(first.output_path) == a, "first output pixels differ from source");
    auto second = convert(source, root / "second.png");
    require(second.success && second.timings.canonical_cache_hit, "repeat conversion did not hit cache");
    require(read_png(second.output_path) == a, "repeat conversion returned different pixels");

    auto switched = convert(other, root / "other-output.png");
    require(switched.success && !switched.timings.canonical_cache_hit, "different path reused cache");
    require(read_png(switched.output_path) == b, "different path returned stale pixels");

    // Replace bytes in-place while retaining the path and byte size.
    auto source_miss = convert(source, root / "source-after-switch.png");
    require(source_miss.success && !source_miss.timings.canonical_cache_hit,
            "switching back to source did not replace cached path");
    require(read_png(source_miss.output_path) == a, "switch-back output returned stale pixels");
    auto warmed_before_replace = convert(source, root / "warm-before-replace.png");
    require(warmed_before_replace.success && warmed_before_replace.timings.canonical_cache_hit,
            "source was not cached immediately before replacement");
    require(read_png(warmed_before_replace.output_path) == a,
            "warmup before replacement returned incorrect pixels");
    const auto old_time = fs::last_write_time(source);
    write_png(source, c, true);
    require(fs::file_size(source) == fs::file_size(other), "replacement changed fixture size");
    fs::last_write_time(source, old_time + std::chrono::seconds(5));
    auto replaced = convert(source, root / "replacement.png");
    require(replaced.success && !replaced.timings.canonical_cache_hit, "same-name/size replacement hit stale cache");
    require(read_png(replaced.output_path) == c, "replacement returned stale pixels");

    hdrbridge::clear_input_cache();
    auto cleared = convert(source, root / "cleared.png");
    require(cleared.success && !cleared.timings.canonical_cache_hit, "clear_input_cache did not invalidate entry");

    // Last-write time can be restored by staged copies. The Windows file
    // change time must still invalidate a same-path, same-size replacement.
    const auto restored_time = fs::last_write_time(source);
    write_png(source, b, true);
    require(fs::file_size(source) == fs::file_size(other), "restored-time replacement changed fixture size");
    fs::last_write_time(source, restored_time);
    auto restored_time_replacement = convert(source, root / "restored-time-replacement.png");
    require(restored_time_replacement.success && !restored_time_replacement.timings.canonical_cache_hit,
            "same-size replacement with restored mtime hit stale cache");
    require(read_png(restored_time_replacement.output_path) == b,
            "restored-mtime replacement returned stale pixels");
    auto restored_time_repeat = convert(source, root / "restored-time-repeat.png");
    require(restored_time_repeat.success && restored_time_repeat.timings.canonical_cache_hit,
            "replacement entry was not cacheable on repeat");
    require(read_png(restored_time_repeat.output_path) == b,
            "repeated restored-mtime replacement returned incorrect pixels");

    write_png(sdr, a, false);
    const auto sdr_info = hdrbridge::inspect(sdr);
    require(sdr_info.asset_kind == "non-HDR", "SDR fixture was misclassified");
    require(conversion_fails(sdr, root / "sdr-output.png"),
            "non-HDR input unexpectedly converted to HDR PNG");
    auto after_sdr_failure = convert(source, root / "after-sdr-failure.png");
    require(after_sdr_failure.success && !after_sdr_failure.timings.canonical_cache_hit,
            "cache miss after failed SDR decode was not reported");
    require(read_png(after_sdr_failure.output_path) == b, "cached HDR pixels changed after failed SDR conversion");

    { std::ofstream stream(corrupt, std::ios::binary); stream << "not a PNG"; }
    require(conversion_fails(corrupt, root / "corrupt-output.png"),
            "corrupt input unexpectedly converted");
    auto after_corrupt_failure = convert(source, root / "after-corrupt-failure.png");
    require(after_corrupt_failure.success && !after_corrupt_failure.timings.canonical_cache_hit,
            "cache miss after corrupt decode was not reported");
    require(read_png(after_corrupt_failure.output_path) == b,
            "corrupt decode exposed stale pixels instead of current HDR input");
    bool missing_failed = false;
    try { (void)hdrbridge::inspect(root / "missing.png"); }
    catch (...) { missing_failed = true; }
    require(missing_failed, "missing input inspection returned successfully");

    hdrbridge::clear_input_cache();
    fs::remove_all(root);
    std::cout << "PASS: canonical input cache regression\n";
    return 0;
  } catch (const std::exception& error) {
    std::error_code ignored;
    fs::remove_all(root, ignored);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
