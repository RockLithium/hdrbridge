#include "hdrbridge_core.h"

#include <png.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void write_sdr_png(const std::filesystem::path& path) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
    throw std::runtime_error("cannot create temporary SDR PNG");
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
    throw std::runtime_error("cannot write temporary SDR PNG");
  }
#pragma warning(pop)
  png_init_io(png, file);
  png_set_IHDR(png, info, 2, 1, 8, PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  png_byte pixels[]{255, 0, 0, 0, 255, 0};
  png_bytep rows[]{pixels};
  png_write_image(png, rows);
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  std::fclose(file);
}

}  // namespace

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    "hdrbridge-sdr-rgb8-regression.png";
  try {
    write_sdr_png(path);
    const auto info = hdrbridge::inspect(path);
    std::filesystem::remove(path);
    if (info.asset_kind != "non-HDR" || info.bit_depth != 8 ||
        info.pixel_format != "8-bit RGB" || info.width != 2 || info.height != 1) {
      std::cerr << "SDR PNG was not classified as non-HDR RGB8\n";
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
