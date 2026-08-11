#include "output_naming.h"

#include <iostream>

int main() {
  using hdrbridge::desktop::CollisionPolicy;
  using hdrbridge::desktop::NamingRequest;
  NamingRequest request;
  request.source = LR"(C:\images\portrait.heic)";
  request.use_source_folder = true;
  request.base_name = L"portrait-edit";
  request.suffix = L"_master";
  request.mode = L"jxl-pq16";
  request.collision = CollisionPolicy::overwrite;
  const auto output = hdrbridge::desktop::resolve_output_path(request);
  if (output != std::filesystem::path(LR"(C:\images\portrait-edit_master.jxl)")) {
    std::cerr << "resolved output mismatch\n";
    return 1;
  }
  if (hdrbridge::desktop::extension_for_mode(L"ultrahdr") != L".jpg" ||
      hdrbridge::desktop::extension_for_mode(L"png-pq16") != L".png" ||
      hdrbridge::desktop::extension_for_mode(L"jxr-scrgb-fp16") != L".jxr" ||
      hdrbridge::desktop::extension_for_mode(L"avif-pq10") != L".avif" ||
      hdrbridge::desktop::extension_for_mode(L"tiff-pq16") != L".tif") {
    std::cerr << "mode extension mismatch\n";
    return 1;
  }

  // Output settings are session state. Queue completion, Clear, Remove and an
  // empty queue do not recreate them, so a new batch must still resolve to the
  // explicitly selected custom folder until the user switches policy.
  request.base_name.clear();
  request.suffix.clear();
  request.mode = L"png-pq16";
  request.selected_folder = LR"(D:\CustomOutput)";
  request.use_source_folder = false;
  request.source = LR"(C:\BatchA\first.heic)";
  if (hdrbridge::desktop::resolve_output_path(request) !=
      std::filesystem::path(LR"(D:\CustomOutput\first.png)")) {
    std::cerr << "Batch A custom output policy mismatch\n";
    return 1;
  }
  // Simulate Convert -> Clear -> Add Batch B: only the source changes.
  request.source = LR"(E:\BatchB\second.hif)";
  if (hdrbridge::desktop::resolve_output_path(request) !=
      std::filesystem::path(LR"(D:\CustomOutput\second.png)")) {
    std::cerr << "custom output policy did not survive a new batch\n";
    return 1;
  }
  request.use_source_folder = true;
  if (hdrbridge::desktop::resolve_output_path(request) !=
      std::filesystem::path(LR"(E:\BatchB\second.png)")) {
    std::cerr << "source-folder policy mismatch\n";
    return 1;
  }
  request.use_source_folder = false;
  if (hdrbridge::desktop::resolve_output_path(request) !=
      std::filesystem::path(LR"(D:\CustomOutput\second.png)")) {
    std::cerr << "custom folder was not retained across ON/OFF toggle\n";
    return 1;
  }
  try {
    request.base_name = L"invalid:name";
    (void)hdrbridge::desktop::resolve_output_path(request);
    std::cerr << "invalid Windows name accepted\n";
    return 1;
  } catch (const std::exception&) {
    return 0;
  }
}
