#include "app_update.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <chrono>
#include <vector>

namespace hdrbridge::app_update {
namespace {
using json = nlohmann::json;
constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kRepoPath[] = L"/repos/RockLithium/hdrbridge/releases/latest";
constexpr std::size_t kMaxManifestBytes = 1024 * 1024;
constexpr std::uint64_t kMaxInstallerBytes = 256ull * 1024 * 1024;
constexpr DWORD kTimeoutMs = 15000;
constexpr auto kTotalRequestTimeout = std::chrono::minutes(3);

void set_error(std::string* out, std::string message) {
  if (out) *out = std::move(message);
}

std::wstring widen(std::string_view value) {
  if (value.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0) return {};
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), count);
  return result;
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool is_hex_sha256(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

struct WinHandle {
  HINTERNET value = nullptr;
  ~WinHandle() { if (value) WinHttpCloseHandle(value); }
  WinHandle(const WinHandle&) = delete;
  WinHandle& operator=(const WinHandle&) = delete;
  WinHandle() = default;
  explicit WinHandle(HINTERNET h) : value(h) {}
};

std::optional<std::string> get_https(std::wstring host, std::wstring path,
                                     std::uint64_t limit, std::string* error,
                                     bool allow_cdn_redirect = false, unsigned redirect_depth = 0,
                                     ProgressCallback progress = {}) {
  WinHandle session(WinHttpOpen(L"HDRBridge-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.value) { set_error(error, "Could not initialize HTTPS client."); return std::nullopt; }
  WinHttpSetTimeouts(session.value, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
  WinHandle connection(WinHttpConnect(session.value, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection.value) { set_error(error, "Could not connect to the release host."); return std::nullopt; }
  WinHandle request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE));
  if (!request.value) { set_error(error, "Could not create HTTPS request."); return std::nullopt; }
  DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));
  std::wstring headers = L"Accept: application/vnd.github+json\r\nUser-Agent: HDRBridge\r\n";
  if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) {
    set_error(error, "HTTPS request failed."); return std::nullopt;
  }
  DWORD status = 0, status_size = sizeof(status);
  WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
  if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
    if (!allow_cdn_redirect || redirect_depth >= 5) {
      set_error(error, "Unexpected redirect from the release host."); return std::nullopt;
    }
    DWORD bytes = 0;
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    std::vector<wchar_t> location((bytes / sizeof(wchar_t)) + 1, L'\0');
    if (!bytes || !WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION,
          WINHTTP_HEADER_NAME_BY_INDEX, location.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) {
      set_error(error, "Download redirect did not include a valid location."); return std::nullopt;
    }
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(location.data(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
      set_error(error, "Download redirect was not HTTPS."); return std::nullopt;
    }
    std::wstring next_host(parts.lpszHostName, parts.dwHostNameLength);
    if (next_host != L"github.com" && next_host != L"release-assets.githubusercontent.com" &&
        next_host != L"objects.githubusercontent.com") {
      set_error(error, "Download redirected to an untrusted host."); return std::nullopt;
    }
    std::wstring next_path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) next_path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    return get_https(std::move(next_host), std::move(next_path), limit, error, true, redirect_depth + 1, std::move(progress));
  }
  if (status != 200) {
    DWORD remain = 0;
    WinHttpQueryDataAvailable(request.value, &remain);
    std::string detail;
    if (remain && remain <= 2048) {
      detail.resize(remain);
      DWORD read = 0;
      if (WinHttpReadData(request.value, detail.data(), remain, &read)) detail.resize(read);
      else detail.clear();
    }
    std::string rate;
    DWORD header_bytes = 0;
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Remaining",
                        nullptr, &header_bytes, WINHTTP_NO_HEADER_INDEX);
    if (header_bytes) {
      std::vector<wchar_t> header(header_bytes / sizeof(wchar_t) + 1, L'\0');
      if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Remaining",
                              header.data(), &header_bytes, WINHTTP_NO_HEADER_INDEX)) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, header.data(), -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 1) {
          rate.resize(bytes);
          WideCharToMultiByte(CP_UTF8, 0, header.data(), -1, rate.data(), bytes, nullptr, nullptr);
          rate.pop_back();
        }
      }
    }
    std::string message = "Release host returned HTTP " + std::to_string(status);
    if (!rate.empty()) message += " (X-RateLimit-Remaining=" + rate + ")";
    if (!detail.empty()) message += ": " + detail;
    set_error(error, std::move(message)); return std::nullopt;
  }
  std::string body;
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t total = 0;
  DWORD content_length = 0, content_length_size = sizeof(content_length);
  if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &content_length_size,
                          WINHTTP_NO_HEADER_INDEX) && content_length > limit) {
    set_error(error, "Download exceeds the configured size limit."); return std::nullopt;
  }
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.value, &available)) { set_error(error, "Could not read HTTPS response."); return std::nullopt; }
    if (!available) break;
    if (available > limit || body.size() > limit - available) { set_error(error, "Download exceeds the configured size limit."); return std::nullopt; }
    const auto old_size = body.size();
    body.resize(old_size + available);
    DWORD read = 0;
    if (!WinHttpReadData(request.value, body.data() + old_size, available, &read)) { set_error(error, "Could not read HTTPS response."); return std::nullopt; }
    body.resize(old_size + read);
    total += read;
    if (std::chrono::steady_clock::now() - started > kTotalRequestTimeout) {
      set_error(error, "HTTPS response exceeded the total time limit."); return std::nullopt;
    }
    if (progress) progress(total, content_length);
  }
  return body;
}

std::optional<std::string> file_sha256(const std::filesystem::path& path, std::uint64_t* size_out,
                                      std::string* error) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_size = 0, result_size = 0;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
      BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0) {
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    set_error(error, "Could not initialize SHA-256 verification."); return std::nullopt;
  }
  std::vector<UCHAR> object(object_size);
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0); set_error(error, "Could not initialize SHA-256 verification."); return std::nullopt;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); set_error(error, "Could not open installer for verification."); return std::nullopt; }
  std::array<char, 64 * 1024> buffer{};
  std::uint64_t size = 0;
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = file.gcount();
    if (count > 0) {
      if (size > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(count) ||
          BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) {
        BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); set_error(error, "Could not hash installer."); return std::nullopt;
      }
      size += static_cast<std::uint64_t>(count);
    }
  }
  if (!file.eof()) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); set_error(error, "Could not read installer for verification."); return std::nullopt; }
  std::array<UCHAR, 32> digest{};
  const bool ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
  BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok) { set_error(error, "Could not finish installer SHA-256."); return std::nullopt; }
  static constexpr char hex[] = "0123456789abcdef";
  std::string actual; actual.reserve(64);
  for (auto byte : digest) { actual.push_back(hex[byte >> 4]); actual.push_back(hex[byte & 15]); }
  if (size_out) *size_out = size;
  return actual;
}

std::optional<std::string> expected_installer_name(std::string_view version) {
  if (!compare_versions(version, version)) return std::nullopt;
  return "HDRBridge-v" + std::string(version) + "-Windows-x64-Setup.exe";
}
}  // namespace

std::optional<int> compare_versions(std::string_view lhs, std::string_view rhs) {
  auto parse = [](std::string_view value) -> std::optional<std::array<unsigned, 3>> {
    if (value.starts_with('v') || value.starts_with('V')) value.remove_prefix(1);
    std::array<unsigned, 3> parts{};
    for (std::size_t i = 0; i < 3; ++i) {
      const auto end = value.find('.');
      const auto part = value.substr(0, end);
      if (part.empty()) return std::nullopt;
      if (part.size() > 1 && part.front() == '0') return std::nullopt;
      unsigned n = 0;
      const auto result = std::from_chars(part.data(), part.data() + part.size(), n);
      if (result.ec != std::errc{} || result.ptr != part.data() + part.size()) return std::nullopt;
      parts[i] = n;
      if (i < 2) {
        if (end == std::string_view::npos) return std::nullopt;
        value.remove_prefix(end + 1);
      } else if (end != std::string_view::npos) return std::nullopt;
    }
    return parts;
  };
  const auto a = parse(lhs), b = parse(rhs);
  if (!a || !b) return std::nullopt;
  if (*a < *b) return -1;
  if (*a > *b) return 1;
  return 0;
}

std::optional<std::string> parse_sha256_sums(std::string_view contents, std::string_view asset_name) {
  std::istringstream input{std::string(contents)};
  std::string line;
  std::optional<std::string> found;
  while (std::getline(input, line)) {
    std::istringstream row(line);
    std::string hash, name;
    if (!(row >> hash >> name)) continue;
    while (!name.empty() && name.front() == '*') name.erase(name.begin());
    if (name == asset_name) {
      if (!is_hex_sha256(hash) || found) return std::nullopt;
      found = lowercase(hash);
    }
  }
  return found;
}

std::optional<Release> parse_release_checksums(std::string_view contents, std::string* error) {
  std::istringstream input{std::string(contents)};
  std::string line;
  std::optional<Release> selected;
  unsigned matches = 0;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream row(line);
    std::string digest, name, trailing;
    if (!(row >> digest >> name)) continue;
    if (!name.empty() && name.front() == '*') name.erase(name.begin());
    if (!name.starts_with("HDRBridge-v") || !name.ends_with("-Windows-x64-Setup.exe")) continue;
    const auto version = name.substr(std::string("HDRBridge-v").size(),
        name.size() - std::string("HDRBridge-v").size() - std::string("-Windows-x64-Setup.exe").size());
    const auto expected = expected_installer_name(version);
    if (!expected || *expected != name || !is_hex_sha256(digest) || (row >> trailing)) {
      set_error(error, "SHA256SUMS contains an invalid installer entry."); return std::nullopt;
    }
    if (++matches != 1) { set_error(error, "SHA256SUMS contains ambiguous installer entries."); return std::nullopt; }
    const std::string tag = "v" + version;
    Asset asset{name, "https://github.com/RockLithium/hdrbridge/releases/download/" + tag + "/" + name,
                lowercase(digest), 0};
    selected = Release{tag, version, "https://github.com/RockLithium/hdrbridge/releases/tag/" + tag,
                       std::move(asset)};
  }
  if (!selected) set_error(error, "SHA256SUMS does not contain exactly one stable Windows setup asset.");
  return selected;
}

std::optional<Release> parse_latest_release(std::string_view text, std::string* error) {
  try {
    const auto data = json::parse(text);
    if (!data.is_object() || data.value("draft", true) || data.value("prerelease", true)) {
      set_error(error, "Latest release is a draft or prerelease."); return std::nullopt;
    }
    const auto tag = data.at("tag_name").get<std::string>();
    auto valid = compare_versions(tag, tag);
    if (!valid) { set_error(error, "Release tag is not a stable numeric version."); return std::nullopt; }
    std::string version = tag;
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) version.erase(version.begin());
    const auto html = data.at("html_url").get<std::string>();
    if (html != "https://github.com/RockLithium/hdrbridge/releases/tag/" + tag) {
      set_error(error, "Release page URL does not match the trusted repository."); return std::nullopt;
    }
    const auto expected = expected_installer_name(version);
    if (!expected) { set_error(error, "Invalid release version."); return std::nullopt; }
    const auto& assets = data.at("assets");
    if (!assets.is_array()) { set_error(error, "Release assets are malformed."); return std::nullopt; }
    std::optional<Asset> installer;
    unsigned installer_count = 0;
    for (const auto& item : assets) {
      if (!item.is_object() || item.value("name", std::string{}) != *expected) continue;
      Asset candidate;
      candidate.name = item.at("name").get<std::string>();
      candidate.url = item.at("browser_download_url").get<std::string>();
      candidate.size = item.value("size", std::uint64_t{0});
      const std::string prefix = "https://github.com/RockLithium/hdrbridge/releases/download/" + tag + "/";
      if (!candidate.url.starts_with(prefix) || candidate.url.substr(prefix.size()) != candidate.name ||
          candidate.size == 0 || candidate.size > kMaxInstallerBytes) {
        set_error(error, "Installer asset URL or size is invalid."); return std::nullopt;
      }
      if (item.contains("digest") && item["digest"].is_string()) {
        const std::string digest = item["digest"].get<std::string>();
        if (digest.starts_with("sha256:") && is_hex_sha256(std::string_view(digest).substr(7)))
          candidate.sha256 = lowercase(digest.substr(7));
      }
      if (++installer_count > 1) { set_error(error, "Release contains duplicate installer assets."); return std::nullopt; }
      installer = std::move(candidate);
    }
    if (!installer || installer->sha256.empty()) { set_error(error, "Release is missing the installer or its SHA-256 digest."); return std::nullopt; }
    return Release{tag, version, html, std::move(*installer)};
  } catch (...) {
    set_error(error, "Could not parse GitHub release metadata."); return std::nullopt;
  }
}

std::optional<Release> check_latest(std::string* error) {
  std::string api_error;
  const auto response = get_https(kApiHost, kRepoPath, kMaxManifestBytes, &api_error);
  if (response) {
    if (auto release = parse_latest_release(*response, &api_error)) return release;
  }
  std::string sums_error;
  const auto sums = get_https(L"github.com", L"/RockLithium/hdrbridge/releases/latest/download/SHA256SUMS.txt",
                             kMaxManifestBytes, &sums_error, true);
  if (sums) {
    if (auto release = parse_release_checksums(*sums, &sums_error)) return release;
  }
  set_error(error, "GitHub API check failed (" + api_error + "); checksum fallback failed (" + sums_error + ").");
  return std::nullopt;
}

std::optional<VerifiedInstaller> download_installer(const Release& release,
    ProgressCallback progress, std::string* error) {
  const auto expected = expected_installer_name(release.version);
  if (!expected || release.installer.name != *expected || !is_hex_sha256(release.installer.sha256)) {
    set_error(error, "Installer metadata failed validation."); return std::nullopt;
  }
  constexpr std::wstring_view prefix = L"https://github.com/RockLithium/hdrbridge/releases/download/";
  const auto url = widen(release.installer.url);
  if (url.empty() || !url.starts_with(prefix)) { set_error(error, "Installer URL is not trusted."); return std::nullopt; }
  std::wstring suffix = url.substr(prefix.size());
  const auto slash = suffix.find(L'/');
  if (slash == std::wstring::npos || suffix.substr(0, slash) != widen(release.tag_name) || suffix.substr(slash + 1) != widen(release.installer.name)) {
    set_error(error, "Installer URL path does not match its release metadata."); return std::nullopt;
  }
  const auto bytes = get_https(L"github.com", L"/RockLithium/hdrbridge/releases/download/" + suffix,
                               kMaxInstallerBytes, error, true, 0, std::move(progress));
  if (!bytes) return std::nullopt;
  if (release.installer.size && bytes->size() != release.installer.size) { set_error(error, "Downloaded installer size does not match release metadata."); return std::nullopt; }
  if (bytes->size() > kMaxInstallerBytes) { set_error(error, "Download exceeds the configured size limit."); return std::nullopt; }
  const auto actual = [&]() -> std::optional<std::string> {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, result_size = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0) {
      if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); return std::nullopt;
    }
    std::vector<UCHAR> object(object_size); std::array<UCHAR, 32> digest{};
    const bool ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0 &&
      BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes->data())), static_cast<ULONG>(bytes->size()), 0) >= 0 &&
      BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return std::nullopt;
    static constexpr char hex[] = "0123456789abcdef"; std::string out; out.reserve(64);
    for (auto byte : digest) { out.push_back(hex[byte >> 4]); out.push_back(hex[byte & 15]); }
    return out;
  }();
  if (!actual) { set_error(error, "Could not calculate installer SHA-256."); return std::nullopt; }
  if (*actual != lowercase(release.installer.sha256)) { set_error(error, "Installer SHA-256 verification failed."); return std::nullopt; }
  wchar_t temp[MAX_PATH + 1]{};
  if (!GetTempPathW(MAX_PATH, temp)) { set_error(error, "Could not locate the temporary directory."); return std::nullopt; }
  std::error_code ec;
  wchar_t unique[MAX_PATH + 1]{};
  if (!GetTempFileNameW(temp, L"HBU", 0, unique) || !DeleteFileW(unique) || !CreateDirectoryW(unique, nullptr)) {
    set_error(error, "Could not create a unique update directory."); return std::nullopt;
  }
  const std::filesystem::path directory(unique);
  const auto final_path = directory / widen(release.installer.name);
  const auto part_path = final_path.wstring() + L".part";
  { std::ofstream file(part_path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(bytes->data(), static_cast<std::streamsize>(bytes->size()))) {
      std::filesystem::remove(part_path, ec); set_error(error, "Could not save verified installer."); return std::nullopt;
    }
    file.flush();
    if (!file) { file.close(); std::filesystem::remove(part_path, ec); set_error(error, "Could not flush saved installer."); return std::nullopt; }
    file.close();
    if (!file) { std::filesystem::remove(part_path, ec); set_error(error, "Could not close saved installer."); return std::nullopt; }
  }
  std::uint64_t stored_size = 0;
  const auto stored_hash = file_sha256(part_path, &stored_size, error);
  if (!stored_hash || (release.installer.size && stored_size != release.installer.size) ||
      stored_size > kMaxInstallerBytes || *stored_hash != lowercase(release.installer.sha256)) {
    std::filesystem::remove(part_path, ec); std::filesystem::remove(directory, ec);
    set_error(error, "Saved installer failed SHA-256 verification."); return std::nullopt;
  }
  std::filesystem::rename(part_path, final_path, ec);
  if (ec) { std::filesystem::remove(part_path, ec); std::filesystem::remove(directory, ec); set_error(error, "Could not finalize verified installer."); return std::nullopt; }
  return VerifiedInstaller{final_path, *stored_hash, stored_size};
}

bool verify_installer(const std::filesystem::path& path, std::string_view expected_sha256,
                      std::uint64_t expected_size, std::string* error) {
  if (!is_hex_sha256(expected_sha256)) { set_error(error, "Expected SHA-256 is invalid."); return false; }
  std::uint64_t size = 0;
  const auto hash = file_sha256(path, &size, error);
  if (!hash) return false;
  if ((expected_size && size != expected_size) || *hash != lowercase(std::string(expected_sha256))) {
    set_error(error, "Installer SHA-256 verification failed."); return false;
  }
  return true;
}

bool launch_installer(const VerifiedInstaller& installer, std::string* error) {
  if (!verify_installer(installer.path, installer.sha256, installer.size, error)) return false;
  SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas"; info.lpFile = installer.path.c_str(); info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) { set_error(error, "Could not launch the installer."); return false; }
  if (info.hProcess) CloseHandle(info.hProcess);
  return true;
}

}  // namespace hdrbridge::app_update
