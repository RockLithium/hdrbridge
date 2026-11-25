#include "diagnostics/digest.h"
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace hdrbridge::detail {

std::string sha256(const std::vector<uint8_t>& data) {
#ifdef _WIN32
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_size = 0, result_size = 0;
  std::vector<uint8_t> object;
  std::array<uint8_t, 32> digest{};
  auto cleanup = [&] { if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0); };
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
      BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0) {
    cleanup(); throw std::runtime_error("cannot initialize SHA-256");
  }
  object.resize(object_size);
  if (BCryptCreateHash(alg, &hash, object.data(), object_size, nullptr, 0, 0) < 0 ||
      BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) < 0 ||
      BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
    cleanup(); throw std::runtime_error("cannot compute SHA-256");
  }
  cleanup();
  std::ostringstream out;
  for (uint8_t b : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  return out.str();
#else
  (void)data;
  return {};
#endif
}

}  // namespace hdrbridge::detail
