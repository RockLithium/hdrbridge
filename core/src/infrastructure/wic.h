#pragma once

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace hdrbridge::detail {

using Microsoft::WRL::ComPtr;

void require_hr(HRESULT hr, const char* operation);

struct ComScope {
  bool uninitialize = false;
  ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) uninitialize = true;
    else if (hr != RPC_E_CHANGED_MODE) require_hr(hr, "CoInitializeEx");
  }
  ~ComScope() { if (uninitialize) CoUninitialize(); }
};

ComPtr<IWICImagingFactory> wic_factory();

}  // namespace hdrbridge::detail
