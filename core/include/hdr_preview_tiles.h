#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace hdrbridge::preview {

struct TileSpan {
  uint32_t offset;
  uint32_t length;
  uint32_t sample_offset;
  uint32_t sample_length;
};

struct Tile {
  TileSpan x, y;
};

inline std::vector<TileSpan> split_texture_axis(uint32_t length, uint32_t limit) {
  if (!length || !limit) throw std::invalid_argument("Invalid preview texture dimensions");
  if (length <= limit) return {{0, length, 0, length}};
  if (limit <= 2) throw std::invalid_argument("Preview texture limit is too small for tiling");

  // One neighboring texel per internal edge keeps linear filtering continuous.
  const uint32_t count = 1 + (length - 3) / (limit - 2);
  const uint32_t base = length / count;
  uint32_t remaining = length % count;
  uint32_t offset = 0;
  std::vector<TileSpan> spans;
  spans.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t before = i != 0;
    const uint32_t after = i + 1 != count;
    uint32_t size = base;
    if (remaining && size < limit - before - after) {
      ++size;
      --remaining;
    }
    spans.push_back({offset, size, offset - before, size + before + after});
    offset += size;
  }
  return spans;
}

inline std::vector<Tile> texture_tiles(uint32_t width, uint32_t height, uint32_t limit) {
  const auto columns = split_texture_axis(width, limit);
  const auto rows = split_texture_axis(height, limit);
  std::vector<Tile> tiles;
  for (const auto& y : rows) {
    for (const auto& x : columns) tiles.push_back({x, y});
  }
  return tiles;
}

}  // namespace hdrbridge::preview
