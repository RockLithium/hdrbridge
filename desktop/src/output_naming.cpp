#include "output_naming.h"

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace hdrbridge::desktop {

namespace {

std::wstring validate_component(std::wstring value, const char* label,
                                bool allow_empty) {
  while (!value.empty() && (value.back() == L' ' || value.back() == L'.')) value.pop_back();
  if (!allow_empty && value.empty()) throw std::runtime_error(std::string(label) + " cannot be empty");
  constexpr wchar_t forbidden[] = L"<>:\"/\\|?*";
  if (value.find_first_of(forbidden) != std::wstring::npos ||
      std::any_of(value.begin(), value.end(), [](wchar_t c) { return c < 32; })) {
    throw std::runtime_error(std::string(label) + " contains a Windows-invalid character");
  }
  return value;
}

}  // namespace

std::wstring extension_for_mode(const std::wstring& mode) {
  if (mode == L"ultrahdr") return L".jpg";
  if (mode == L"png-pq16") return L".png";
  if (mode == L"jxl-pq16") return L".jxl";
  if (mode == L"jxr-scrgb-fp16" || mode == L"jxr-rgb10-experimental") return L".jxr";
  if (mode == L"avif-pq10") return L".avif";
  if (mode == L"tiff-pq16") return L".tif";
  throw std::runtime_error("unknown output mode");
}

std::filesystem::path resolve_output_path(const NamingRequest& request) {
  if (request.source.empty()) throw std::runtime_error("source path is missing");
  const auto folder = request.use_source_folder ? request.source.parent_path()
                                                 : request.selected_folder;
  if (folder.empty()) throw std::runtime_error("output folder is missing");
  const std::wstring base = validate_component(
      request.base_name.empty() ? request.source.stem().wstring() : request.base_name,
      "base name", false);
  const std::wstring suffix = validate_component(request.suffix, "suffix", true);
  const std::wstring extension = extension_for_mode(request.mode);
  auto candidate = folder / (base + suffix + extension);
  if (request.collision == CollisionPolicy::overwrite) return candidate;
  for (unsigned number = 2; std::filesystem::exists(candidate); ++number) {
    candidate = folder / (base + suffix + L" (" + std::to_wstring(number) + L")" + extension);
  }
  return candidate;
}

}  // namespace hdrbridge::desktop
