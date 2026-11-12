#include "../desktop/src/app_update.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <windows.h>

using namespace hdrbridge::app_update;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
 try {
  if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--live-download") {
    const auto stage_root = std::filesystem::absolute(std::filesystem::path(argv[2]));
    std::filesystem::create_directories(stage_root);
    if (!SetEnvironmentVariableW(L"TEMP", stage_root.c_str()) ||
        !SetEnvironmentVariableW(L"TMP", stage_root.c_str()))
      throw std::runtime_error("could not direct test temp files into the verification directory");
    std::string error;
    const auto release = check_latest(&error);
    require(release.has_value(), error.c_str());
    if (argc == 4) require(compare_versions(release->version, argv[3]) == 0,
                            "GitHub latest version differs from the expected current release");
    std::uint64_t last_received = 0;
    const auto installer = download_installer(*release, [&](std::uint64_t received, std::uint64_t) {
      require(received >= last_received, "download progress moved backwards");
      last_received = received;
    }, &error);
    require(installer.has_value(), error.c_str());
    require(installer->path.parent_path().parent_path() == stage_root, "download escaped the project verification directory");
    require(verify_installer(installer->path, release->installer.sha256, release->installer.size, &error),
            "downloaded setup failed independent integrity verification");
    std::cout << "live latest check and installer SHA-256 passed: version=" << release->version
              << " bytes=" << installer->size << " digest=" << installer->sha256 << '\n';
    std::error_code ec;
    std::filesystem::remove(installer->path, ec);
    if (ec) throw std::runtime_error("could not remove staged test installer: " + ec.message());
    const auto unique_dir = installer->path.parent_path();
    if (unique_dir.parent_path() != stage_root) throw std::runtime_error("refusing unexpected test cleanup path");
    std::filesystem::remove(unique_dir, ec);
    if (ec) throw std::runtime_error("could not remove staged test directory: " + ec.message());
    return 0;
  }
  require(compare_versions("1.3.2", "v1.3.2") == 0, "equal versions");
  require(compare_versions("v1.3.3", "1.3.2") == 1, "newer version");
  require(compare_versions("1.2.99", "1.3.0") == -1, "numeric component order");
  require(!compare_versions("1.3.2-beta.1", "1.3.2"), "reject prerelease version");
  require(!compare_versions("1.3", "1.3.0"), "reject missing component");
  require(!compare_versions("1.3.2.1", "1.3.2"), "reject extra component");
  require(!compare_versions("01.3.2", "1.3.2"), "reject leading zero");

  constexpr char hash[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const std::string filename = "HDRBridge-v1.3.3-Windows-x64-Setup.exe";
  const std::string asset_url = "https://github.com/RockLithium/hdrbridge/releases/download/v1.3.3/" + filename;
  const std::string prefix = "{\"draft\":false,\"prerelease\":false,\"tag_name\":\"v1.3.3\",\"html_url\":\"https://github.com/RockLithium/hdrbridge/releases/tag/v1.3.3\",\"assets\":[";
  const std::string asset = "{\"name\":\"" + filename + "\",\"browser_download_url\":\"" + asset_url +
      "\",\"size\":1234,\"digest\":\"sha256:" + hash + "\"}";
  const std::string manifest = prefix + asset + "]}";
  std::string error;
  const auto release = parse_latest_release(manifest, &error);
  require(release.has_value(), "parse valid release metadata");
  require(release->version == "1.3.3" && release->installer.sha256 == hash, "release parsed values");
  require(release->installer.size == 1234, "release size parsed");
  require(!parse_latest_release("{bad", &error), "reject malformed json");
  require(!parse_latest_release("{\"draft\":true}", &error), "reject draft");
  require(!parse_latest_release(prefix.substr(0, prefix.size()-1) + "true}", &error), "reject incomplete metadata");
  require(!parse_latest_release(prefix + asset + "," + asset + "]}", &error), "reject duplicate installer asset");

  auto wrong_host = manifest;
  auto at = wrong_host.find("https://github.com/RockLithium");
  wrong_host.replace(at, std::string("https://github.com/RockLithium").size(), "https://evil.example/RockLithium");
  require(!parse_latest_release(wrong_host, &error), "reject untrusted asset URL");
  auto wrong_digest = manifest;
  at = wrong_digest.find(hash);
  wrong_digest.replace(at, 64, "not-a-sha256");
  require(!parse_latest_release(wrong_digest, &error), "reject malformed digest");
  auto wrong_path = manifest;
  at = wrong_path.find("/releases/download/v1.3.3/");
  wrong_path.replace(at, std::string("/releases/download/v1.3.3/").size(), "/releases/download/v9.9.9/");
  require(!parse_latest_release(wrong_path, &error), "reject mismatched asset path");

  const std::string sums = std::string(hash) + "  " + filename + "\n";
  require(parse_sha256_sums(sums, filename) == hash, "parse matching SHA256SUMS entry");
  require(!parse_sha256_sums(sums, "other.exe"), "reject missing SHA256SUMS entry");
  require(!parse_sha256_sums(std::string(64, 'g') + "  " + filename, filename), "reject malformed SHA256SUMS");
  require(!parse_sha256_sums(sums + sums, filename), "reject duplicate SHA256SUMS entry");
  const auto checksum_release = parse_release_checksums(
      "README.txt  unrelated\n" + sums, &error);
  require(checksum_release && checksum_release->version == "1.3.3" &&
          checksum_release->installer.size == 0 && checksum_release->installer.sha256 == hash,
          "parse signed-by-digest fallback manifest");
  require(!parse_release_checksums(sums + sums, &error), "reject ambiguous fallback entries");
  require(!parse_release_checksums(std::string(64, 'z') + "  " + filename + "\n", &error),
          "reject malformed fallback digest");
  require(!parse_release_checksums(std::string(hash) + "  HDRBridge-v1.3.3-beta-Windows-x64-Setup.exe\n", &error),
          "reject prerelease fallback asset");

  const auto temp = std::filesystem::temp_directory_path() / L"hdrbridge-update-test-installer.bin";
  { std::ofstream f(temp, std::ios::binary | std::ios::trunc); f << "hello"; }
  constexpr std::string_view hello_sha = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
  require(verify_installer(temp, hello_sha, 5, &error), "verify correct staged-file SHA256 and size");
  require(!verify_installer(temp, hello_sha, 6, &error), "reject incorrect staged-file size");
  require(!verify_installer(temp, std::string(64, '0'), 5, &error), "reject corrupted staged-file digest");
  { std::ofstream f(temp, std::ios::binary | std::ios::trunc); f << "hella"; }
  require(!verify_installer(temp, hello_sha, 5, &error), "detect staged-file corruption");
  std::error_code ec; std::filesystem::remove(temp, ec);

  std::cout << "updater metadata and integrity tests passed\n";
 } catch (const std::exception& e) {
   std::cerr << "updater tests failed: " << e.what() << '\n';
   return 1;
 }
}
