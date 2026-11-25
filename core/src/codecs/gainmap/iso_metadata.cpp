#include "codecs/gainmap/iso_metadata.h"
#include <stdexcept>

namespace hdrbridge::gainmap::detail {

namespace {

uint16_t read_be16(const uint8_t*& cursor, const uint8_t* end) {
  if (end - cursor < 2) throw std::runtime_error("truncated TIFF gain-map metadata");
  const uint16_t value = static_cast<uint16_t>((cursor[0] << 8) | cursor[1]);
  cursor += 2;
  return value;
}

uint32_t read_be32(const uint8_t*& cursor, const uint8_t* end) {
  if (end - cursor < 4) throw std::runtime_error("truncated TIFF gain-map metadata");
  const uint32_t value = (static_cast<uint32_t>(cursor[0]) << 24) |
                         (static_cast<uint32_t>(cursor[1]) << 16) |
                         (static_cast<uint32_t>(cursor[2]) << 8) |
                         static_cast<uint32_t>(cursor[3]);
  cursor += 4;
  return value;
}

double unsigned_fraction(const uint8_t*& cursor, const uint8_t* end) {
  const uint32_t numerator = read_be32(cursor, end);
  const uint32_t denominator = read_be32(cursor, end);
  if (denominator == 0) throw std::runtime_error("zero denominator in TIFF gain-map metadata");
  return static_cast<double>(numerator) / denominator;
}

double signed_fraction(const uint8_t*& cursor, const uint8_t* end) {
  const int32_t numerator = static_cast<int32_t>(read_be32(cursor, end));
  const uint32_t denominator = read_be32(cursor, end);
  if (denominator == 0) throw std::runtime_error("zero denominator in TIFF gain-map metadata");
  return static_cast<double>(numerator) / denominator;
}

}  // namespace

IsoGainMapMetadata parse_iso_gain_map_metadata(const void* data, size_t size) {
  if (!data || size < 21) throw std::runtime_error("ISO gain-map metadata is missing");
  const auto* cursor = static_cast<const uint8_t*>(data);
  const auto* end = cursor + size;
  const uint16_t minimum_version = read_be16(cursor, end);
  const uint16_t writer_version = read_be16(cursor, end);
  if (minimum_version != 0 || writer_version < minimum_version) {
    throw std::runtime_error("unsupported ISO gain-map metadata version");
  }
  if (cursor == end) throw std::runtime_error("truncated ISO gain-map flags");
  const uint8_t flags = *cursor++;
  if ((flags & 0x33) != 0) throw std::runtime_error("invalid reserved ISO gain-map metadata bits");
  if ((flags & 0x04) != 0) {
    throw std::runtime_error("HDR-base inverse-direction Apple gain map is not supported");
  }
  const bool common_denominator = (flags & 0x08) != 0;
  uint32_t denominator = 0;
  if (common_denominator) {
    denominator = read_be32(cursor, end);
    if (denominator == 0) throw std::runtime_error("zero common ISO gain-map denominator");
  }
  const auto read_unsigned = [&]() {
    const uint32_t numerator = read_be32(cursor, end);
    const uint32_t divisor = common_denominator ? denominator : read_be32(cursor, end);
    if (divisor == 0) throw std::runtime_error("zero ISO gain-map denominator");
    return static_cast<double>(numerator) / divisor;
  };
  const auto read_signed = [&]() {
    const int32_t numerator = static_cast<int32_t>(read_be32(cursor, end));
    const uint32_t divisor = common_denominator ? denominator : read_be32(cursor, end);
    if (divisor == 0) throw std::runtime_error("zero ISO gain-map denominator");
    return static_cast<double>(numerator) / divisor;
  };
  IsoGainMapMetadata metadata;
  metadata.multichannel = (flags & 0x80) != 0;
  metadata.use_base_color_space = (flags & 0x40) != 0;
  metadata.base_hdr_headroom = read_unsigned();
  metadata.alternate_hdr_headroom = read_unsigned();
  const size_t channels = metadata.multichannel ? 3u : 1u;
  for (size_t channel = 0; channel < channels; ++channel) {
    metadata.minimum[channel] = read_signed();
    metadata.maximum[channel] = read_signed();
    metadata.gamma[channel] = read_unsigned();
    metadata.base_offset[channel] = read_signed();
    metadata.alternate_offset[channel] = read_signed();
    if (metadata.maximum[channel] < metadata.minimum[channel] ||
        metadata.gamma[channel] <= 0.0) {
      throw std::runtime_error("invalid ISO gain-map channel metadata");
    }
  }
  for (size_t channel = channels; channel < 3; ++channel) {
    metadata.minimum[channel] = metadata.minimum[0];
    metadata.maximum[channel] = metadata.maximum[0];
    metadata.gamma[channel] = metadata.gamma[0];
    metadata.base_offset[channel] = metadata.base_offset[0];
    metadata.alternate_offset[channel] = metadata.alternate_offset[0];
  }
  if (cursor != end) throw std::runtime_error("unexpected trailing ISO gain-map metadata");
  return metadata;
}

IsoGainMapMetadata parse_tiff_gain_map_metadata(const void* data, uint32_t size) {
  // Adobe TIFF places a four-byte container version before the ISO 21496-1
  // GainMapMetadata syntax used by the AVIF tmap box.
  if (!data || size < 25) throw std::runtime_error("TIFF gain-map metadata is missing");
  const auto* cursor = static_cast<const uint8_t*>(data);
  const auto* end = cursor + size;
  const uint32_t container_version = read_be32(cursor, end);
  const uint16_t minimum_version = read_be16(cursor, end);
  const uint16_t writer_version = read_be16(cursor, end);
  if (container_version != 0 || minimum_version != 0 || writer_version < minimum_version) {
    throw std::runtime_error("unsupported TIFF/ISO gain-map metadata version");
  }
  const uint8_t flags = *cursor++;
  if ((flags & 0x3f) != 0) throw std::runtime_error("invalid reserved TIFF gain-map metadata bits");
  IsoGainMapMetadata metadata;
  metadata.multichannel = (flags & 0x80) != 0;
  metadata.use_base_color_space = (flags & 0x40) != 0;
  metadata.base_hdr_headroom = unsigned_fraction(cursor, end);
  metadata.alternate_hdr_headroom = unsigned_fraction(cursor, end);
  const size_t channel_count = metadata.multichannel ? 3u : 1u;
  for (size_t channel = 0; channel < channel_count; ++channel) {
    metadata.minimum[channel] = signed_fraction(cursor, end);
    metadata.maximum[channel] = signed_fraction(cursor, end);
    metadata.gamma[channel] = unsigned_fraction(cursor, end);
    metadata.base_offset[channel] = signed_fraction(cursor, end);
    metadata.alternate_offset[channel] = signed_fraction(cursor, end);
    if (metadata.maximum[channel] < metadata.minimum[channel] || metadata.gamma[channel] <= 0.0) {
      throw std::runtime_error("invalid TIFF gain-map channel metadata");
    }
  }
  for (size_t channel = channel_count; channel < 3; ++channel) {
    metadata.minimum[channel] = metadata.minimum[0];
    metadata.maximum[channel] = metadata.maximum[0];
    metadata.gamma[channel] = metadata.gamma[0];
    metadata.base_offset[channel] = metadata.base_offset[0];
    metadata.alternate_offset[channel] = metadata.alternate_offset[0];
  }
  if (cursor != end) throw std::runtime_error("unexpected trailing TIFF gain-map metadata");
  return metadata;
}

}  // namespace hdrbridge::gainmap::detail
