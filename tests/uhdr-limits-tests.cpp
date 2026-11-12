#include "uhdr_limits.h"
#include "ultrahdr_api.h"
#include "ultrahdr/jpegdecoderhelper.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

static void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

static std::vector<uint8_t> jpeg_header(uint16_t width, uint16_t height) {
  // Header-only grayscale JPEG, sufficient for jpeg_read_header without pixels.
  return {0xff, 0xd8, 0xff, 0xc0, 0x00, 0x0b, 0x08,
          uint8_t(height >> 8), uint8_t(height), uint8_t(width >> 8), uint8_t(width),
          0x01, 0x01, 0x11, 0x00,
          0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00, 0xff, 0xd9};
}

int main() {
  try {
    require(hdrbridge_uhdr_mul(16384, 12288) == 201326592, "valid multiplication failed");
    bool overflow = false;
    try { (void)hdrbridge_uhdr_mul(std::numeric_limits<size_t>::max(), 2); }
    catch (const std::overflow_error&) { overflow = true; }
    require(overflow, "multiplication overflow accepted");
    overflow = false;
    try { (void)hdrbridge_uhdr_add(std::numeric_limits<size_t>::max(), 1); }
    catch (const std::overflow_error&) { overflow = true; }
    require(overflow, "addition overflow accepted");

    auto* encoder = uhdr_create_encoder();
    require(encoder != nullptr, "create encoder failed");
    uhdr_raw_image_t raw{};
    raw.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
    raw.cg = UHDR_CG_BT_2100;
    raw.ct = UHDR_CT_LINEAR;
    raw.range = UHDR_CR_FULL_RANGE;
    raw.w = 16384;
    raw.h = 12288;
    auto status = uhdr_enc_set_raw_image(encoder, &raw, UHDR_HDR_IMG);
    require(status.error_code == UHDR_CODEC_INVALID_PARAM &&
            std::strstr(status.detail, "nullptr"), "raw null plane validation failed");
    uhdr_release_encoder(encoder);

    for (auto dimensions : {std::pair<uint16_t, uint16_t>{16384, 12288}, {65500, 65500}}) {
      const auto bytes = jpeg_header(dimensions.first, dimensions.second);
      ultrahdr::JpegDecoderHelper decoder;
      const auto result = decoder.parseImage(bytes.data(), bytes.size());
      require(result.error_code == UHDR_CODEC_OK, "JPEG header rejected");
    }
    const auto bytes = jpeg_header(8192, 16383);
    ultrahdr::JpegDecoderHelper decoder;
    const auto result = decoder.parseImage(bytes.data(), bytes.size());
    require(result.error_code == UHDR_CODEC_OK, "valid JPEG header rejected");
    std::cout << "Ultra HDR dimension and buffer size checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
