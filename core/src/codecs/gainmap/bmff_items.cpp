#include "codecs/gainmap/bmff_items.h"
#include "codecs/gainmap/decode_support.h"
#include <libheif/heif_items.h>
#include <stdexcept>
#include <vector>

namespace hdrbridge::gainmap::detail {

void require_heif(heif_error result, const char* operation) {
  if (result.code == heif_error_Ok) return;
  throw std::runtime_error(std::string(operation) + ": " +
                           (result.message ? result.message : "libheif error"));
}

ItemGraph read_item_graph(const heif_context* context) {
  const int count = heif_context_get_number_of_items(context);
  std::vector<heif_item_id> ids(static_cast<size_t>(std::max(count, 0)));
  const int got = ids.empty() ? 0 : heif_context_get_list_of_item_IDs(
      context, ids.data(), static_cast<int>(ids.size()));
  ItemGraph graph;
  for (int i = 0; i < got; ++i) {
    const heif_item_id id = ids[static_cast<size_t>(i)];
    if (heif_item_get_item_type(context, id) != heif_fourcc('t', 'm', 'a', 'p')) continue;
    graph.tone_map = id;
    for (int reference_index = 0;; ++reference_index) {
      uint32_t type = 0;
      heif_item_id* targets = nullptr;
      const size_t target_count = heif_context_get_item_references(
          context, id, reference_index, &type, &targets);
      if (target_count == 0) break;
      if (type == heif_fourcc('d', 'i', 'm', 'g') && target_count >= 2) {
        graph.base = targets[0];
        graph.gain = targets[1];
      }
      heif_release_item_references(context, &targets);
    }
  }
  return graph;
}

ItemGraph read_item_graph(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  HeifContextPtr context(heif_context_alloc());
  if (!context) throw std::bad_alloc();
  require_heif(heif_context_read_from_memory_without_copy(
                   context.get(), bytes.data(), bytes.size(), nullptr),
               "read AVIF item graph");
  return read_item_graph(context.get());
}

}  // namespace hdrbridge::gainmap::detail
