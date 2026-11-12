#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace hdrbridge::app_update {

struct Asset {
  std::string name;
  std::string url;
  std::string sha256;
  std::uint64_t size = 0;
};

struct Release {
  std::string tag_name;
  std::string version;
  std::string html_url;
  Asset installer;
};

struct VerifiedInstaller {
  std::filesystem::path path;
  std::string sha256;
  std::uint64_t size = 0;
};

using ProgressCallback = std::function<void(std::uint64_t received, std::uint64_t total)>;

// These synchronous operations should be called from a worker thread. They never install or launch.
std::optional<Release> parse_latest_release(std::string_view json, std::string* error = nullptr);
std::optional<Release> parse_release_checksums(std::string_view checksums, std::string* error = nullptr);
std::optional<Release> check_latest(std::string* error = nullptr);
std::optional<VerifiedInstaller> download_installer(
    const Release& release, ProgressCallback progress = {}, std::string* error = nullptr);
bool verify_installer(const std::filesystem::path& path, std::string_view expected_sha256,
                      std::uint64_t expected_size = 0, std::string* error = nullptr);
// Call only after the user confirms the update in the UI. Uses the normal installer/UAC flow.
bool launch_installer(const VerifiedInstaller& installer, std::string* error = nullptr);

// Stable numeric versions only. Returns -1, 0, or 1; invalid input returns nullopt.
std::optional<int> compare_versions(std::string_view lhs, std::string_view rhs);
std::optional<std::string> parse_sha256_sums(std::string_view contents, std::string_view asset_name);

}  // namespace hdrbridge::app_update
