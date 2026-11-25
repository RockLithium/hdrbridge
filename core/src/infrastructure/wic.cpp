#ifdef _WIN32
#include "infrastructure/wic.h"
#include <sstream>
#include <stdexcept>

namespace hdrbridge::detail {

void require_hr(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    std::ostringstream out;
    out << operation << " failed (HRESULT 0x" << std::hex << static_cast<uint32_t>(hr) << ')';
    throw std::runtime_error(out.str());
  }
}

ComPtr<IWICImagingFactory> wic_factory() {
  ComPtr<IWICImagingFactory> factory;
  require_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)), "create WIC factory");
  return factory;
}

}  // namespace hdrbridge::detail
#endif
