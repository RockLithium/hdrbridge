#include "metadata/exif_output.h"
#include <cstdlib>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      std::abort();                                                            \
  } while (false)
#include <cstdint>
#include <cstring>
#include <vector>
using B = std::vector<uint8_t>;
static void put16(B &b, size_t p, uint16_t v, bool le) {
  b[p + !le] = uint8_t(v);
  b[p + le] = uint8_t(v >> 8);
}
static void put32(B &b, size_t p, uint32_t v, bool le) {
  for (int i = 0; i < 4; ++i)
    b[p + i] = uint8_t(v >> (8 * (le ? i : 3 - i)));
}
static void entry(B &b, size_t p, uint16_t tag, uint16_t type, uint32_t n,
                  uint32_t value, bool le) {
  put16(b, p, tag, le);
  put16(b, p + 2, type, le);
  put32(b, p + 4, n, le);
  put32(b, p + 8, value, le);
}
static B fixture(bool le, bool prefix, int orientation, bool xiaomi) {
  const size_t x = prefix ? 6 : 0;
  B b(x + 220, 0);
  if (prefix)
    std::memcpy(b.data(), "Exif\0\0", 6);
  b[x] = b[x + 1] = le ? 'I' : 'M';
  put16(b, x + 2, 42, le);
  put32(b, x + 4, 8, le);
  put16(b, x + 8, 5, le);
  entry(b, x + 10, 0x112, 3, 1, 0, le);
  put16(b, x + 18, uint16_t(orientation), le);
  entry(b, x + 22, 0x10f, 2, 7, 170, le);
  entry(b, x + 34, 0x110, 2, 8, 177, le);
  entry(b, x + 46, 0x8825, 4, 1, 190, le);
  entry(b, x + 58, 0x8769, 4, 1, 80, le);
  put32(b, x + 70, 200, le);
  put16(b, x + 80, 2, le);
  entry(b, x + 82, 0x927c, 7, 12, 150, le);
  entry(b, x + 94, 0x829a, 5, 1, 162, le);
  put32(b, x + 106, 210, le);
  std::memcpy(b.data() + x + 150, xiaomi ? "Xiaomi\0\1ABCD" : "Other!\0\1ABCD",
              12);
  std::memcpy(b.data() + x + 170, "Xiaomi\0", 7);
  std::memcpy(b.data() + x + 177, "Model X\0", 8);
  return b;
}
int main() {
  for (bool le : {false, true})
    for (bool prefix : {false, true})
      for (int orientation : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        auto b = fixture(le, prefix, orientation, true);
        auto original = b;
        size_t x = prefix ? 6 : 0;
        CHECK(hdrbridge::metadata::prepare_output_exif(b));
        CHECK(b.size() == original.size());
        B expected = original;
        put16(expected, x + 18, 1, le);
        put16(expected, x + 80, 1, le);
        std::memmove(expected.data() + x + 82, expected.data() + x + 94, 16);
        std::memset(expected.data() + x + 98, 0, 12);
        CHECK(b == expected);
        auto other = fixture(le, prefix, 1, false);
        auto old = other;
        CHECK(hdrbridge::metadata::prepare_output_exif(other));
        CHECK(other != old);
        CHECK(other[x + 80 + (le ? 0 : 1)] == 1);
        auto heif_raw = fixture(le, false, 0, true);
        B heif(7, 0);
        heif[3] = 3;
        heif.insert(heif.end(), heif_raw.begin(), heif_raw.end());
        CHECK(hdrbridge::metadata::prepare_output_exif(heif));
        CHECK(heif[7 + 80 + (le ? 0 : 1)] == 1);
        CHECK(heif[3] == 3);
        auto short_file = fixture(le, prefix, 0, true);
        short_file.resize(x + 100);
        old = short_file;
        CHECK(!hdrbridge::metadata::prepare_output_exif(short_file));
        CHECK(short_file == old);
        auto wrong = fixture(le, prefix, 0, true);
        put16(wrong, x + 12, 2, le);
        old = wrong;
        CHECK(hdrbridge::metadata::prepare_output_exif(wrong));
        CHECK(wrong[x + 12] == old[x + 12] && wrong[x + 13] == old[x + 13]);
        CHECK(wrong[x + 80 + (le ? 0 : 1)] == 1);
        for (int value : {0, 1, 8}) {
          auto long_orientation = fixture(le, prefix, value, true);
          put16(long_orientation, x + 12, 4, le);
          put32(long_orientation, x + 18, value, le);
          CHECK(hdrbridge::metadata::prepare_output_exif(long_orientation));
          B check = long_orientation;
          put16(check, x + 12, 3, le);
          put16(check, x + 18, 1, le);
          check[x + 20] = check[x + 21] = 0;
          CHECK(check == long_orientation);
        }
        auto bad = fixture(le, prefix, 0, true);
        put32(bad, x + 90, 99999, le);
        old = bad;
        CHECK(!hdrbridge::metadata::prepare_output_exif(bad));
        CHECK(bad == old);
      }
}
