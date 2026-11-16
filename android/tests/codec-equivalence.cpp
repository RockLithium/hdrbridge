#include <jni.h>
#include <ultrahdr/gainmapmath.h>
#include <ultrahdr/ultrahdrcommon.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <vector>

// Test-only JNI hook; included only in codec-test builds.
extern "C" JNIEXPORT jstring JNICALL
Java_org_hdrbridge_DeviceRegressionRunner_codecSelfTest(JNIEnv* env, jclass) {
  try {
    constexpr unsigned width = 256, height = 256;
    std::vector<uint8_t> pixels(width * height * 4);
    for (unsigned i = 0; i < width * height; ++i) {
      pixels[4*i] = static_cast<uint8_t>(((i >> 11) & 31) * 255 / 31);
      pixels[4*i+1] = static_cast<uint8_t>(((i >> 6) & 31) * 255 / 31);
      pixels[4*i+2] = static_cast<uint8_t>((i & 63) * 255 / 63);
      pixels[4*i+3] = 255;
    }
    for (unsigned i = 0; i < 256; ++i) {
      pixels[4*i] = pixels[4*i+1] = pixels[4*i+2] = static_cast<uint8_t>(i);
    }
    const uint8_t patches[][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},
                                  {0,255,255},{255,0,255},{0,0,0},{255,255,255}};
    for (unsigned i = 0; i < 8; ++i)
      for (unsigned c = 0; c < 3; ++c) pixels[4*(256+i)+c] = patches[i][c];
    uhdr_raw_image_t source{};
    source.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    source.ct = UHDR_CT_SRGB;
    source.range = UHDR_CR_FULL_RANGE;
    source.w = width; source.h = height;
    source.planes[UHDR_PLANE_PACKED] = pixels.data();
    source.stride[UHDR_PLANE_PACKED] = width;
    const uhdr_color_gamut_t gamuts[] = {UHDR_CG_BT_709, UHDR_CG_DISPLAY_P3, UHDR_CG_BT_2100};
    const char* names[] = {"BT709", "DisplayP3", "BT2100"};
    bool allPassed = true;
    std::ostringstream out;
    out << "{\"gamuts\":{";
    for (int g = 0; g < 3; ++g) {
      source.cg = gamuts[g];
      auto scalar = ultrahdr::convert_raw_input_to_ycbcr(&source, false);
      auto neon = ultrahdr::convert_raw_input_to_ycbcr_neon(&source);
      if (!scalar || !neon) throw std::runtime_error("conversion returned null");
      if (g) out << ',';
      out << '"' << names[g] << "\":{";
      const char* planes[] = {"Y", "U", "V"};
      for (int p = 0; p < 3; ++p) {
        auto a = static_cast<const uint8_t*>(scalar->planes[p]);
        auto b = static_cast<const uint8_t*>(neon->planes[p]);
        int maximum = 0;
        double sum = 0;
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
          int d = int(a[y * scalar->stride[p] + x]) - int(b[y * neon->stride[p] + x]);
          maximum = std::max(maximum, std::abs(d));
          sum += d * d;
        }
        allPassed &= maximum <= 1;
        if (p) out << ',';
        out << '"' << planes[p] << "\":{\"maxAbsCode\":" << maximum
            << ",\"rmse\":" << std::sqrt(sum / (width * height)) << '}';
      }
      out << '}';
    }
    out << "},\"maxAllowed\":1,\"passed\":" << (allPassed ? "true" : "false") << '}';
    return env->NewStringUTF(out.str().c_str());
  } catch (const std::exception& e) {
    env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    return nullptr;
  }
}
