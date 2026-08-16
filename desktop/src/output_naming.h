#pragma once

#include <filesystem>
#include <string>

namespace hdrbridge::desktop {

enum class CollisionPolicy { auto_number, overwrite };

struct NamingRequest {
  std::filesystem::path source;
  std::filesystem::path selected_folder;
  bool use_source_folder = true;
  std::wstring base_name;
  std::wstring suffix;
  std::wstring mode;
  std::wstring gainmap_export_format;
  CollisionPolicy collision = CollisionPolicy::auto_number;
};

std::wstring extension_for_mode(const std::wstring& mode,
                                const std::wstring& gainmap_export_format = L"",
                                const std::filesystem::path& source = {});
std::filesystem::path resolve_output_path(const NamingRequest& request);

}  // namespace hdrbridge::desktop
