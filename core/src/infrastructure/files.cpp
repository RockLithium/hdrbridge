#include "infrastructure/files.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace hdrbridge::detail {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("cannot open file: " + path.string());
  const auto size = in.tellg();
  if (size < 0) throw std::runtime_error("cannot determine file size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  in.seekg(0);
  if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("cannot read file");
  }
  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out || (!data.empty() && !out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())))) {
    throw std::runtime_error("cannot write output: " + path.string());
  }
  out.flush();
  if (!out) throw std::runtime_error("cannot flush output");
}

std::string extension_lower(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

}  // namespace hdrbridge::detail
