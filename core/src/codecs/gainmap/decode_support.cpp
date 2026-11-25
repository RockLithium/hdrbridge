#include "codecs/gainmap/decode_support.h"
#include <fstream>
#include <stdexcept>

namespace hdrbridge::gainmap::detail {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open gain-map input: " + path.string());
  const std::streamoff size = input.tellg();
  if (size < 0) throw std::runtime_error("cannot determine gain-map input size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  input.seekg(0);
  if (!bytes.empty() &&
      !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("cannot read gain-map input");
  }
  return bytes;
}

}  // namespace hdrbridge::gainmap::detail
