#pragma once

#include <jxl/decode.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace hdrbridge {

// libjxl delivers metadata in chunks, including transparent brob decompression.
class JxlMetadataBoxes {
 public:
  JxlMetadataBoxes(JxlDecoder* decoder, std::vector<uint8_t>& exif,
                   std::vector<uint8_t>& xmp)
      : decoder_(decoder), exif_(exif), xmp_(xmp) {}

  void start_box() {
    finish_box();
    JxlBoxType type{};
    if (JxlDecoderGetBoxType(decoder_, type, JXL_TRUE) != JXL_DEC_SUCCESS) {
      throw std::runtime_error("cannot read JPEG XL metadata box type");
    }
    if (std::memcmp(type, "Exif", 4) == 0) {
      target_ = &exif_;
      exif_present = true;
    } else if (std::memcmp(type, "xml ", 4) == 0) {
      target_ = &xmp_;
      xmp_present = true;
    }
    if (target_) append_chunk();
  }

  void continue_box() {
    if (!target_) throw std::runtime_error("unexpected JPEG XL metadata buffer request");
    offset_ += kChunkSize - JxlDecoderReleaseBoxBuffer(decoder_);
    append_chunk();
  }

  void finish_box() {
    if (!target_) return;
    target_->resize(offset_ + kChunkSize - JxlDecoderReleaseBoxBuffer(decoder_));
    target_ = nullptr;
    offset_ = 0;
  }

  bool exif_present = false;
  bool xmp_present = false;

 private:
  void append_chunk() {
    target_->resize(offset_ + kChunkSize);
    if (JxlDecoderSetBoxBuffer(decoder_, target_->data() + offset_, kChunkSize) !=
        JXL_DEC_SUCCESS) {
      throw std::runtime_error("cannot read JPEG XL metadata box");
    }
  }

  static constexpr size_t kChunkSize = 65536;
  JxlDecoder* decoder_;
  std::vector<uint8_t>& exif_;
  std::vector<uint8_t>& xmp_;
  std::vector<uint8_t>* target_ = nullptr;
  size_t offset_ = 0;
};

}  // namespace hdrbridge
