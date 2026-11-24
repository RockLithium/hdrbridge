#include "hdr_preview_tiles.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

bool approx(float a, float b, float eps = 1e-2f) {
  return std::abs(a - b) <= eps;
}

}  // namespace

int main() {
  try {
    using namespace hdrbridge::preview;

    // 1. Trilinear axis subdivision across non-coincident level boundaries
    {
      const auto spans0 = split_texture_axis(20u, 8u);
      const auto spans1 = split_texture_axis(10u, 8u);
      const auto merged = trilinear_axis_spans(20u, 20u, spans0, 10u, spans1, 0.5f);
      require(merged.size() == spans0.size() + spans1.size() - 1u,
              "Expected split at every distinct tile boundary across both levels");
      double cursor = 0.0;
      for (const auto& s : merged) {
        require(std::abs(s.base_start - cursor) <= 1e-9 && s.base_end > s.base_start,
                "Sub-spans must form a strictly continuous partition");
        require(s.tile0 < spans0.size() && s.tile1 < spans1.size(),
                "Sub-span tile index out of range");
        cursor = s.base_end;
      }
      require(std::abs(cursor - 20.0) <= 1e-9, "Sub-spans must cover full axis");

      const auto single = trilinear_axis_spans(20u, 20u, spans0, 10u, spans1, 0.0f);
      require(single.size() == spans0.size(), "blend == 0 must not subdivide level0 tiles");
    }

    // 2. FP16 LUT subnormals/Inf and fast 2x2 downsample
    const auto& lut = detail::half_to_float_lut();
    require(approx(lut[0x0001u], std::ldexp(1.0f, -24), 1e-12f) &&
            approx(lut[0x0002u], std::ldexp(1.0f, -23), 1e-12f) &&
            approx(lut[0x0200u], std::ldexp(1.0f, -15), 1e-12f) &&
            approx(lut[0x7c00u], 65504.0f, 1e-2f) &&
            approx(lut[0xfc00u], -65504.0f, 1e-2f), "FP16 LUT edge-value mismatch");

    std::vector<uint16_t> fast_2x2(2u * 2u * 4u, 0u);
    for (size_t i = 0; i < 4u; ++i) fast_2x2[i * 4u] = 0x0002u;
    fast_2x2[1] = 0x7c00u;  // +Inf
    fast_2x2[2] = 0xfc00u;  // -Inf
    const auto fast_down = downsample_2x_rgba_fp16(2u, 2u, fast_2x2.data());
    require(fast_down.rgba_fp16[0] == 0x0002u &&
            approx(lut[fast_down.rgba_fp16[1]], 65504.0f / 4.0f, 32.0f) &&
            approx(lut[fast_down.rgba_fp16[2]], -65504.0f / 4.0f, 32.0f),
            "2x2 subnormal/Inf fast-path mismatch");

    // 3. Odd-sized uniform area-box 2x downsampling with negative scRGB and NaN resilience
    std::vector<uint16_t> odd_src(5u * 5u * 4u, 0u);
    for (uint32_t y = 0; y < 5u; ++y) {
      for (uint32_t x = 0; x < 5u; ++x) {
        const size_t idx = (static_cast<size_t>(y) * 5u + x) * 4u;
        const bool tail = (x >= 3u || y >= 3u);
        odd_src[idx + 0] = detail::float_to_half(tail ? 8.0f : 2.0f);
        odd_src[idx + 1] = detail::float_to_half(tail ? 0.5f : -0.5f);
        odd_src[idx + 2] = detail::float_to_half(1.0f);
        odd_src[idx + 3] = detail::float_to_half(1.0f);
      }
    }
    odd_src[0] = 0x7e00u;  // NaN in [0..5/3)x[0..5/3)
    const auto odd_down = downsample_2x_rgba_fp16(5u, 5u, odd_src.data());
    require(odd_down.width == 3u && odd_down.height == 3u, "Expected 5x5 -> 3x3 downsample");
    require(approx(lut[odd_down.rgba_fp16[0]], 1.28f, 2e-2f) &&
            approx(lut[odd_down.rgba_fp16[1]], -0.5f) &&
            approx(lut[odd_down.rgba_fp16[32]], 8.0f, 2e-2f) &&
            approx(lut[odd_down.rgba_fp16[33]], 0.5f, 2e-2f),
            "Odd area-box 2x downsample mismatch");

    // 4. Multi-level 2x mip chain, cancellation, and standard trilinear LOD selection
    std::vector<uint16_t> base(1024u * 512u * 4u, detail::float_to_half(1.0f));
    const auto mips = build_2x_mip_levels(1024u, 512u, base, 64u);
    require(mips.size() == 4u && mips[0].width == 512u && mips[3].width == 64u &&
            approx(lut[mips[3].rgba_fp16[0]], 1.0f), "2x mip chain mismatch");

    std::atomic_bool cancelled{true};
    require(build_2x_mip_levels(1024u, 512u, base, 64u, &cancelled).empty(),
            "Cancelled mipmap generation must abort cleanly");

    const std::vector<LevelExtent> extents = {
        {16320, 12240}, {8160, 6120}, {4080, 3060}, {2040, 1530}, {1020, 765},
    };
    struct TrilinearCase { float scale; size_t l0; size_t l1; float blend; };
    for (const TrilinearCase tc : {
             TrilinearCase{1.5f, 0u, 0u, 0.0f},
             TrilinearCase{1.0f, 0u, 0u, 0.0f},
             TrilinearCase{0.70710678f, 0u, 1u, 0.5f},
             TrilinearCase{0.50f, 1u, 1u, 0.0f},
             TrilinearCase{0.35355339f, 1u, 2u, 0.5f},
             TrilinearCase{0.25f, 2u, 2u, 0.0f},
             TrilinearCase{0.01f, 4u, 4u, 0.0f},
         }) {
      const auto sel = select_trilinear_levels(16320u, 12240u, tc.scale, extents);
      require(sel.level0 == tc.l0 && sel.level1 == tc.l1 &&
              approx(sel.blend, tc.blend, 1e-4f), "Trilinear LOD selection mismatch");
    }

    // Fallback clamping when fewer mip levels exist (e.g. OOM fallback)
    const auto fb = select_trilinear_levels(
        16320u, 12240u, 0.10f, {{16320u, 12240u}, {8160u, 6120u}});
    require(fb.level0 == 1u && fb.level1 == 1u && fb.blend == 0.0f,
            "Trilinear selection must clamp cleanly to deepest uploaded level");

    const auto non2 = select_trilinear_levels(
        12485u, 16648u, 0.70710678f, {{12485u, 16648u}, {6243u, 8324u}, {3122u, 4162u}});
    require(non2.level0 == 0u && non2.level1 == 1u && approx(non2.blend, 0.5f, 1e-3f),
            "Non-power-of-two trilinear blend mismatch");

    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
