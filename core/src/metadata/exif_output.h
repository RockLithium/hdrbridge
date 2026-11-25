#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hdrbridge::metadata {
// Returns false on malformed metadata and leaves the input unchanged.
inline bool prepare_output_exif(std::vector<uint8_t> &bytes) {
  size_t base =
      bytes.size() >= 6 && std::memcmp(bytes.data(), "Exif\0\0", 6) == 0 ? 6
                                                                         : 0;
  if (!base && bytes.size() >= 4 &&
      !((bytes[0] == 'I' && bytes[1] == 'I') ||
        (bytes[0] == 'M' && bytes[1] == 'M'))) {
    const uint32_t off = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
                         uint32_t(bytes[2]) << 8 | bytes[3];
    if (size_t(off) > bytes.size() - 4)
      return false;
    base = 4 + size_t(off);
  }
  auto fits = [&](size_t p, size_t n) {
    return p <= bytes.size() && n <= bytes.size() - p;
  };
  if (!fits(base, 8))
    return false;
  const bool le = bytes[base] == 'I' && bytes[base + 1] == 'I';
  if (!le && !(bytes[base] == 'M' && bytes[base + 1] == 'M'))
    return false;
  auto r16 = [&](size_t p) -> uint16_t {
    return le ? uint16_t(bytes[p] | uint16_t(bytes[p + 1]) << 8)
              : uint16_t(uint16_t(bytes[p]) << 8 | bytes[p + 1]);
  };
  auto r32 = [&](size_t p) -> uint32_t {
    return le ? uint32_t(bytes[p]) | uint32_t(bytes[p + 1]) << 8 |
                    uint32_t(bytes[p + 2]) << 16 | uint32_t(bytes[p + 3]) << 24
              : uint32_t(bytes[p]) << 24 | uint32_t(bytes[p + 1]) << 16 |
                    uint32_t(bytes[p + 2]) << 8 | uint32_t(bytes[p + 3]);
  };
  auto w16 = [&](size_t p, uint16_t v) {
    bytes[p] = uint8_t(le ? v : v >> 8);
    bytes[p + 1] = uint8_t(le ? v >> 8 : v);
  };
  if (r16(base + 2) != 42)
    return false;
  auto table = [&](uint32_t offset, size_t &p, uint16_t &n) {
    if (offset < 8 || size_t(offset) > bytes.size() - base)
      return false;
    p = base + size_t(offset);
    if (!fits(p, 2))
      return false;
    n = r16(p);
    return fits(p + 2, size_t(n) * 12 + 4);
  };
  size_t ifd0 = 0;
  uint16_t count0 = 0;
  if (!table(r32(base + 4), ifd0, count0))
    return false;
  size_t orientation = 0, exif = 0, maker = 0;
  uint16_t exifcount = 0;
  for (size_t i = 0; i < count0; ++i) {
    const size_t p = ifd0 + 2 + i * 12;
    const auto tag = r16(p);
    if (tag == 0x0112 && (r16(p + 2) == 3 || r16(p + 2) == 4) &&
        r32(p + 4) == 1) {
      if (orientation)
        return false;
      orientation = p;
    }
    if (tag == 0x8769) {
      if (exif || r16(p + 2) != 4 || r32(p + 4) != 1 ||
          !table(r32(p + 8), exif, exifcount))
        return false;
    }
  }
  bool remove = false;
  if (exif) {
    // Overlapping directories could make mutation corrupt a separate table.
    const size_t aend = ifd0 + 2 + size_t(count0) * 12 + 4,
                 bend = exif + 2 + size_t(exifcount) * 12 + 4;
    if (ifd0 < bend && exif < aend)
      return false;
    for (size_t i = 0; i < exifcount; ++i) {
      const size_t p = exif + 2 + i * 12;
      if (r16(p) != 0x927c)
        continue;
      if (maker)
        return false;
      maker = p;
      const uint16_t type = r16(p + 2);
      static constexpr uint8_t widths[] = {0, 1, 1, 2, 4, 8, 1,
                                           1, 2, 4, 8, 4, 8};
      if (type == 0 || type >= sizeof(widths))
        return false;
      const uint32_t count = r32(p + 4);
      const uint64_t n = uint64_t(count) * widths[type];
      if (n > 4) {
        const uint32_t off = r32(p + 8);
        if (size_t(off) > bytes.size() - base ||
            n > bytes.size() - base - size_t(off))
          return false;
        const size_t payload = base + size_t(off);
        if ((payload < aend && ifd0 < payload + n) ||
            (payload < bend && exif < payload + n))
          return false;
      }
      remove = true;
    }
  }
  if (orientation) {
    w16(orientation + 2, 3);
    w16(orientation + 8, 1);
    bytes[orientation + 10] = bytes[orientation + 11] = 0;
  }
  if (remove) {
    // Do not propagate vendor MakerNotes into a newly rendered image.
    // Keep standard Exif and every out-of-line offset unchanged.
    const size_t end = exif + 2 + size_t(exifcount) * 12 + 4;
    std::memmove(bytes.data() + maker, bytes.data() + maker + 12,
                 end - (maker + 12));
    std::fill(bytes.begin() + end - 12, bytes.begin() + end, uint8_t(0));
    w16(exif, uint16_t(exifcount - 1));
  }
  return true;
}
} // namespace hdrbridge::metadata
