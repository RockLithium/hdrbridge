#pragma once

#include "frame_exif.h"
#include "hdrbridge_core.h"

#include <tiffio.h>

#include <fstream>

namespace hdrbridge {

inline std::vector<uint8_t> read_tiff_input_exif(TIFF* tiff, SourceInfo& info) {
  toff_t exif_offset = 0, gps_offset = 0;
  char* text = nullptr;
  const bool has_fields =
      (TIFFGetField(tiff, TIFFTAG_EXIFIFD, &exif_offset) == 1 && exif_offset > 0) ||
      (TIFFGetField(tiff, TIFFTAG_GPSIFD, &gps_offset) == 1 && gps_offset > 0) ||
      TIFFGetField(tiff, TIFFTAG_MAKE, &text) == 1 ||
      TIFFGetField(tiff, TIFFTAG_MODEL, &text) == 1 ||
      TIFFGetField(tiff, TIFFTAG_DATETIME, &text) == 1 ||
      TIFFGetField(tiff, TIFFTAG_ARTIST, &text) == 1 ||
      TIFFGetField(tiff, TIFFTAG_COPYRIGHT, &text) == 1;
  info.exif_status = "absent";
  if (!has_fields) return {};
  if (TIFFIsBigTIFF(tiff)) {
    info.exif_status = "unsupported";
    return {};
  }
  try {
    // Read only the TIFF header, camera IFDs and retained metadata ranges.
    std::ifstream stream(info.path, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (size < 0) throw std::runtime_error("Cannot open TIFF metadata");
    const bool transpose = info.original_orientation >= 5;
    const uint32_t width = transpose ? info.height : info.width;
    const uint32_t height = transpose ? info.width : info.height;
    uint64_t remaining_metadata = 16u * 1024u * 1024u;
    auto exif = frame_exif::detail::repack([&](size_t offset, size_t count) {
      if (offset > static_cast<uint64_t>(size) ||
          count > static_cast<uint64_t>(size) - offset || count > remaining_metadata) {
        throw std::runtime_error("TIFF metadata size or offset is invalid");
      }
      std::vector<uint8_t> bytes(count);
      if (!stream.seekg(static_cast<std::streamoff>(offset)) ||
          !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count))) {
        throw std::runtime_error("Cannot read TIFF metadata");
      }
      remaining_metadata -= count;
      return bytes;
    }, width, height, true);
    info.exif_present = true;
    info.exif_status = "present";
    return exif;
  } catch (const std::runtime_error&) {
    info.exif_status = "read-error";
    return {};
  }
}

}  // namespace hdrbridge
