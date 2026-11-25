#include "codecs/gainmap/jpeg_export.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32) || defined(__ANDROID__)
#include <csetjmp>
#include <jpeglib.h>
#endif

namespace hdrbridge::detail {

std::vector<uint8_t> encode_gainmap_jpeg(const gainmap::GainMapRaster& raster,
                                         uint32_t, int quality) {
#if !defined(_WIN32) && !defined(__ANDROID__)
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


#if defined(_WIN32) || defined(__ANDROID__)
struct GainmapJpegError { jpeg_error_mgr base; jmp_buf jump; };
static void gainmap_jpeg_error(j_common_ptr decoder) {
  auto* error = reinterpret_cast<GainmapJpegError*>(decoder->err);
  longjmp(error->jump, 1);
}
#endif

void verify_gainmap_jpeg(const std::vector<uint8_t>& actual,
                        const gainmap::GainMapRaster& raster, size_t channels) {
#if defined(_WIN32) || defined(__ANDROID__)
  jpeg_decompress_struct decoder{};
  GainmapJpegError error{};
  decoder.err = jpeg_std_error(&error.base);
  error.base.error_exit = gainmap_jpeg_error;
  if (setjmp(error.jump)) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("gain-map JPEG readback failed");
  }
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, actual.data(), static_cast<unsigned long>(actual.size()));
  jpeg_read_header(&decoder, TRUE);
  if (decoder.image_width != raster.width || decoder.image_height != raster.height ||
      decoder.num_components != channels || decoder.data_precision != 8) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("gain-map JPEG metadata readback mismatch");
  }
  jpeg_destroy_decompress(&decoder);
#else
  throw std::runtime_error("gain-map JPEG readback unavailable");
#endif
}

}  // namespace hdrbridge::detail
