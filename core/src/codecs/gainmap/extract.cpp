#include "codecs/gainmap/raster.h"
#include "codecs/gainmap/avif.h"
#include "codecs/gainmap/tiff.h"
#include "codecs/gainmap/jxl.h"
#include "codecs/gainmap/apple_heif.h"
#include "codecs/gainmap/jpeg.h"
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace hdrbridge::gainmap {

GainMapRaster extract_gain_map(const std::filesystem::path& path,
                               std::atomic_bool* cancel,
                               bool decode_pixels) {
  const auto extension = path.extension().wstring();
  std::wstring upper = extension;
  std::transform(upper.begin(), upper.end(), upper.begin(), towupper);
  if (upper == L".AVIF" && is_adobe_tmap_avif(path)) return extract_avif_gainmap(path, cancel);
  if ((upper == L".TIF" || upper == L".TIFF") && is_adobe_gainmap_tiff(path)) return extract_tiff_gainmap(path, cancel);
  if (upper == L".JXL" && is_iso_gainmap_jxl(path)) return extract_jxl_gainmap(path, cancel, decode_pixels);
  if (upper == L".HEIF" || upper == L".HEIC" || upper == L".HIF") {
    if (probe_apple_heif(path).detected) return extract_heif_gainmap(path, cancel);
  }
  if (upper == L".JPG" || upper == L".JPEG" || upper == L".JPE") {
    return extract_jpeg_gainmap(path, cancel, decode_pixels);
  }
  throw std::runtime_error("No supported Gain Map was found in this file");
}

}  // namespace hdrbridge::gainmap
