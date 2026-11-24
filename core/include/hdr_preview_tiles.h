#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace hdrbridge::preview {

namespace detail {

inline float half_to_float_finite(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16u;
  uint32_t exponent = (half >> 10u) & 0x1fu;
  uint32_t mantissa = half & 0x3ffu;
  uint32_t bits = 0;
  if (exponent == 31u) {
    if (mantissa != 0u) return 0.0f;
    return (half & 0x8000u) ? -65504.0f : 65504.0f;
  }
  if (exponent == 0u) {
    if (mantissa == 0u) {
      bits = sign;
    } else {
      int shift = 0;
      while ((mantissa & 0x400u) == 0u) {
        mantissa <<= 1u;
        ++shift;
      }
      mantissa &= 0x3ffu;
      bits = sign | (static_cast<uint32_t>(127 - 14 - shift) << 23u) | (mantissa << 13u);
    }
  } else {
    bits = sign | ((exponent + 127u - 15u) << 23u) | (mantissa << 13u);
  }
  float result = 0.0f;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

inline const std::array<float, 65536>& half_to_float_lut() {
  static const std::array<float, 65536> lut = [] {
    std::array<float, 65536> table{};
    for (uint32_t i = 0; i < 65536u; ++i) {
      table[i] = half_to_float_finite(static_cast<uint16_t>(i));
    }
    return table;
  }();
  return lut;
}

inline uint16_t float_to_half(float value) {
  if (!std::isfinite(value)) return 0u;
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16u) & 0x8000u;
  int exponent = static_cast<int>((bits >> 23u) & 0xffu) - 127 + 15;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponent <= 0) {
    if (exponent < -10) return static_cast<uint16_t>(sign);
    mantissa = (mantissa | 0x800000u) >> static_cast<uint32_t>(1 - exponent);
    return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13u));
  }
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7bffu);
  mantissa += 0x1000u;
  if (mantissa & 0x800000u) {
    mantissa = 0;
    ++exponent;
  }
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7bffu);
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10u) | (mantissa >> 13u));
}

}  // namespace detail

struct TileSpan {
  uint32_t offset;
  uint32_t length;
  uint32_t sample_offset;
  uint32_t sample_length;
};

struct Tile {
  TileSpan x, y;
};

struct MipLevel {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint16_t> rgba_fp16;
};

struct LevelExtent {
  uint32_t width = 0;
  uint32_t height = 0;
};

struct TrilinearSelection {
  size_t level0 = 0;
  size_t level1 = 0;
  float blend = 0.0f;
};

struct AxisDrawSpan {
  double base_start = 0.0;
  double base_end = 0.0;
  size_t tile0 = 0;
  size_t tile1 = 0;
};

inline std::vector<TileSpan> split_texture_axis(uint32_t length, uint32_t limit,
                                                uint32_t margin = 1u) {
  if (!length || !limit) throw std::invalid_argument("Invalid preview texture dimensions");
  if (length <= limit) return {{0, length, 0, length}};
  if (limit <= 2u * margin) {
    throw std::invalid_argument("Preview texture limit is too small for tiling");
  }

  // Overlap neighboring texels along internal seams so bilinear filtering stays continuous.
  const uint32_t interior_limit = limit - 2u * margin;
  const uint32_t count = 1u + (length - 2u * margin - 1u) / interior_limit;
  std::vector<uint32_t> sizes(count, length / count);
  uint32_t assigned = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t before = i != 0u ? margin : 0u;
    const uint32_t after = (i + 1u != count) ? margin : 0u;
    sizes[i] = std::min(sizes[i], limit - before - after);
    assigned += sizes[i];
  }
  uint32_t remaining = length - assigned;
  while (remaining > 0u) {
    for (uint32_t i = 0; i < count && remaining > 0u; ++i) {
      const uint32_t before = i != 0u ? margin : 0u;
      const uint32_t after = (i + 1u != count) ? margin : 0u;
      const uint32_t cap = limit - before - after;
      if (sizes[i] < cap) {
        ++sizes[i];
        --remaining;
      }
    }
  }

  uint32_t offset = 0;
  std::vector<TileSpan> spans;
  spans.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t before = i != 0u ? margin : 0u;
    const uint32_t after = (i + 1u != count) ? margin : 0u;
    const uint32_t size = sizes[i];
    spans.push_back({offset, size, offset - before, size + before + after});
    offset += size;
  }
  return spans;
}

inline std::vector<Tile> texture_tiles(uint32_t width, uint32_t height, uint32_t limit,
                                       uint32_t margin = 1u) {
  const auto columns = split_texture_axis(width, limit, margin);
  const auto rows = split_texture_axis(height, limit, margin);
  std::vector<Tile> tiles;
  for (const auto& y : rows) {
    for (const auto& x : columns) tiles.push_back({x, y});
  }
  return tiles;
}

// Box-filter a linear scRGB RGBA16F image by 2x per axis (25% texel count per level).
inline MipLevel downsample_2x_rgba_fp16(uint32_t width, uint32_t height,
                                        const uint16_t* rgba_fp16,
                                        const std::atomic_bool* cancel = nullptr) {
  if (!width || !height || !rgba_fp16) {
    throw std::invalid_argument("Invalid mipmap source dimensions");
  }
  const auto cancelled = [cancel] {
    return cancel && cancel->load(std::memory_order_relaxed);
  };
  if (cancelled()) return {};
  const auto& lut = detail::half_to_float_lut();
  const uint32_t dst_width = std::max<uint32_t>(1u, (width + 1u) / 2u);
  const uint32_t dst_height = std::max<uint32_t>(1u, (height + 1u) / 2u);
  MipLevel level;
  level.width = dst_width;
  level.height = dst_height;
  level.rgba_fp16.resize(static_cast<size_t>(dst_width) * dst_height * 4u);

  const size_t src_stride = static_cast<size_t>(width) * 4u;

  if ((width % 2u) == 0u && (height % 2u) == 0u) {
    for (uint32_t oy = 0; oy < dst_height; ++oy) {
      if ((oy & 63u) == 0u && cancelled()) return {};
      const uint32_t y0 = oy * 2u;
      uint16_t* dst_row = level.rgba_fp16.data() + static_cast<size_t>(oy) * dst_width * 4u;
      const uint16_t* r0 = rgba_fp16 + static_cast<size_t>(y0) * src_stride;
      const uint16_t* r1 = r0 + src_stride;
      for (uint32_t ox = 0; ox < dst_width; ++ox) {
        const size_t base = static_cast<size_t>(ox) * 8u;
        const uint16_t* p0 = r0 + base;
        const uint16_t* p1 = r1 + base;
        constexpr float inv4 = 0.25f;
        uint16_t* out = dst_row + static_cast<size_t>(ox) * 4u;
        out[0] = detail::float_to_half((lut[p0[0]] + lut[p0[4]] + lut[p1[0]] + lut[p1[4]]) * inv4);
        out[1] = detail::float_to_half((lut[p0[1]] + lut[p0[5]] + lut[p1[1]] + lut[p1[5]]) * inv4);
        out[2] = detail::float_to_half((lut[p0[2]] + lut[p0[6]] + lut[p1[2]] + lut[p1[6]]) * inv4);
        out[3] = detail::float_to_half((lut[p0[3]] + lut[p0[7]] + lut[p1[3]] + lut[p1[7]]) * inv4);
      }
    }
    return level;
  }

  // Exact area-weighted resampling keeps every texel uniformly spaced on odd sizes.
  struct AxisSpan {
    uint32_t start = 0;
    uint32_t count = 0;
    float weight_sum = 0.0f;
    std::array<float, 3> weights{};
  };
  auto build_axis_spans = [](uint32_t src_len, uint32_t dst_len) {
    std::vector<AxisSpan> spans(dst_len);
    const double step = static_cast<double>(src_len) / static_cast<double>(dst_len);
    for (uint32_t o = 0; o < dst_len; ++o) {
      const double s0 = static_cast<double>(o) * step;
      const double s1 = (o + 1u == dst_len)
          ? static_cast<double>(src_len)
          : static_cast<double>(o + 1u) * step;
      const uint32_t start = std::min(src_len - 1u, static_cast<uint32_t>(std::floor(s0)));
      const uint32_t end = std::min(
          src_len, std::max(start + 1u, static_cast<uint32_t>(std::ceil(s1))));
      AxisSpan span{};
      span.start = start;
      span.count = std::min<uint32_t>(3u, end - start);
      for (uint32_t k = 0; k < span.count; ++k) {
        const double p0 = static_cast<double>(start + k);
        const double overlap = std::min(p0 + 1.0, s1) - std::max(p0, s0);
        const float w = static_cast<float>(std::max(0.0, overlap));
        span.weights[k] = w;
        span.weight_sum += w;
      }
      spans[o] = span;
    }
    return spans;
  };

  const auto x_spans = build_axis_spans(width, dst_width);
  const auto y_spans = build_axis_spans(height, dst_height);

  for (uint32_t oy = 0; oy < dst_height; ++oy) {
    if ((oy & 63u) == 0u && cancelled()) return {};
    const auto& ys = y_spans[oy];
    uint16_t* dst_row = level.rgba_fp16.data() + static_cast<size_t>(oy) * dst_width * 4u;
    for (uint32_t ox = 0; ox < dst_width; ++ox) {
      const auto& xs = x_spans[ox];
      const float area = ys.weight_sum * xs.weight_sum;
      const float inv = area > 0.0f ? (1.0f / area) : 0.0f;
      float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
      for (uint32_t ry = 0; ry < ys.count; ++ry) {
        const float wy = ys.weights[ry];
        const uint16_t* row = rgba_fp16 + static_cast<size_t>(ys.start + ry) * src_stride;
        float row0 = 0.0f, row1 = 0.0f, row2 = 0.0f, row3 = 0.0f;
        for (uint32_t rx = 0; rx < xs.count; ++rx) {
          const float wx = xs.weights[rx];
          const uint16_t* p = row + static_cast<size_t>(xs.start + rx) * 4u;
          row0 += lut[p[0]] * wx;
          row1 += lut[p[1]] * wx;
          row2 += lut[p[2]] * wx;
          row3 += lut[p[3]] * wx;
        }
        sum0 += row0 * wy;
        sum1 += row1 * wy;
        sum2 += row2 * wy;
        sum3 += row3 * wy;
      }
      uint16_t* out = dst_row + static_cast<size_t>(ox) * 4u;
      out[0] = detail::float_to_half(sum0 * inv);
      out[1] = detail::float_to_half(sum1 * inv);
      out[2] = detail::float_to_half(sum2 * inv);
      out[3] = detail::float_to_half(sum3 * inv);
    }
  }
  return level;
}

// Build 2x-stepped downsampled mip levels (1/2, 1/4, 1/8, ...) without duplicating level 0.
inline std::vector<MipLevel> build_2x_mip_levels(uint32_t width, uint32_t height,
                                                 const std::vector<uint16_t>& rgba_fp16,
                                                 uint32_t min_stop_dimension = 64u,
                                                 const std::atomic_bool* cancel = nullptr) {
  if (!width || !height ||
      rgba_fp16.size() < static_cast<size_t>(width) * height * 4u) {
    throw std::invalid_argument("Invalid preview mipmap input");
  }
  std::vector<MipLevel> levels;
  uint32_t cur_w = width;
  uint32_t cur_h = height;
  const uint16_t* cur_ptr = rgba_fp16.data();
  while (std::max(cur_w, cur_h) > min_stop_dimension && (cur_w > 1u || cur_h > 1u)) {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      levels.clear();
      return levels;
    }
    auto next = downsample_2x_rgba_fp16(cur_w, cur_h, cur_ptr, cancel);
    if (!next.width || !next.height ||
        (cancel && cancel->load(std::memory_order_relaxed))) {
      levels.clear();
      return levels;
    }
    cur_w = next.width;
    cur_h = next.height;
    levels.push_back(std::move(next));
    cur_ptr = levels.back().rgba_fp16.data();
  }
  return levels;
}

// Select adjacent 2x mip levels and linear-light LOD blend factor for standard trilinear filtering.
inline TrilinearSelection select_trilinear_levels(uint32_t base_width, uint32_t base_height,
                                                  float base_scale,
                                                  const std::vector<LevelExtent>& levels) {
  if (!base_width || !base_height || levels.size() <= 1u || !(base_scale > 0.0f) ||
      base_scale >= 1.0f) {
    return {};
  }
  const double target_lod = std::max(0.0, -std::log2(static_cast<double>(base_scale)));
  auto level_lod = [&](size_t idx) {
    const double rx = static_cast<double>(base_width) / static_cast<double>(levels[idx].width);
    const double ry = static_cast<double>(base_height) / static_cast<double>(levels[idx].height);
    return std::log2(std::max(1.0, std::max(rx, ry)));
  };

  size_t k0 = 0;
  double lod0 = 0.0;
  for (size_t i = 1; i < levels.size(); ++i) {
    if (!levels[i].width || !levels[i].height) break;
    const double lod_i = level_lod(i);
    if (lod_i <= target_lod + 1e-6) {
      k0 = i;
      lod0 = lod_i;
    } else {
      const double span = lod_i - lod0;
      float blend = span > 1e-6
          ? static_cast<float>(std::clamp((target_lod - lod0) / span, 0.0, 1.0))
          : 0.0f;
      if (blend <= 1e-5f) return {k0, k0, 0.0f};
      if (blend >= 1.0f - 1e-5f) return {i, i, 0.0f};
      return {k0, i, blend};
    }
  }
  return {k0, k0, 0.0f};
}

// Merge tile boundaries along one axis from level0 and level1 in base-image coordinates.
inline std::vector<AxisDrawSpan> trilinear_axis_spans(
    uint32_t base_length, uint32_t length0, const std::vector<TileSpan>& spans0,
    uint32_t length1, const std::vector<TileSpan>& spans1, float blend) {
  std::vector<AxisDrawSpan> out;
  if (!base_length || !length0 || spans0.empty()) return out;
  const double scale0 = static_cast<double>(base_length) / static_cast<double>(length0);
  if (!(blend > 0.0f) || !length1 || spans1.empty()) {
    out.reserve(spans0.size());
    for (size_t i = 0; i < spans0.size(); ++i) {
      const double start = static_cast<double>(spans0[i].offset) * scale0;
      const double end = (i + 1u == spans0.size())
          ? static_cast<double>(base_length)
          : static_cast<double>(spans0[i].offset + spans0[i].length) * scale0;
      out.push_back({start, end, i, i});
    }
    return out;
  }

  const double scale1 = static_cast<double>(base_length) / static_cast<double>(length1);
  out.reserve(spans0.size() + spans1.size());
  size_t i0 = 0;
  size_t i1 = 0;
  double cur = 0.0;
  while (i0 < spans0.size() && i1 < spans1.size()) {
    const double end0 = (i0 + 1u == spans0.size())
        ? static_cast<double>(base_length)
        : static_cast<double>(spans0[i0].offset + spans0[i0].length) * scale0;
    const double end1 = (i1 + 1u == spans1.size())
        ? static_cast<double>(base_length)
        : static_cast<double>(spans1[i1].offset + spans1[i1].length) * scale1;
    double next = 0.0;
    if (std::abs(end0 - end1) <= 1e-9) {
      next = std::max(end0, end1);
      if (next > cur) out.push_back({cur, next, i0, i1});
      cur = next;
      ++i0;
      ++i1;
    } else if (end0 < end1) {
      next = end0;
      if (next > cur) out.push_back({cur, next, i0, i1});
      cur = next;
      ++i0;
    } else {
      next = end1;
      if (next > cur) out.push_back({cur, next, i0, i1});
      cur = next;
      ++i1;
    }
  }
  return out;
}

}  // namespace hdrbridge::preview
