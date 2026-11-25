#include "codecs/avif.h"
#include "codecs/jxl.h"
#include "codecs/jxr.h"
#include "codecs/png.h"
#include "codecs/tiff.h"
#include "codecs/ultrahdr.h"
#include "infrastructure/files.h"
#include <stdexcept>

namespace hdrbridge {

using namespace detail;

Verification verify(const std::filesystem::path& output, const std::string& mode) {
  if (mode == "jxl-pq16") return decode_jxl(read_file(output), nullptr);
  if (mode == "jxr-scrgb-fp16") return decode_jxr(output, nullptr);
  if (mode == "ultrahdr") return decode_ultrahdr(read_file(output));
  if (mode == "png-pq16") {
    const auto bytes = read_file(output);
    std::array<uint8_t, 4> cicp{};
    const bool hlg = find_png_cicp(bytes, cicp) && cicp[1] == 18;
    return decode_png(bytes, nullptr, !hlg);
  }
  if (mode == "tiff-pq16") return decode_tiff(output);
  if (mode == "avif-pq10") return decode_avif(output);
  throw std::runtime_error("verification mode not implemented: " + mode);
}

}  // namespace hdrbridge
