#include "canonical/input.h"
#include "metadata/source_metadata.h"

namespace hdrbridge {

using namespace detail;

SourceInfo inspect(const std::filesystem::path& path) {
  auto info = inspect_any(path);
  finalize_source_metadata(info);
  return info;
}

}  // namespace hdrbridge
