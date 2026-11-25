#pragma once

#include <cstdint>
#include <filesystem>
#include <libheif/heif.h>
#include <memory>

namespace hdrbridge::gainmap::detail {

struct HeifContextDeleter {
  void operator()(heif_context* value) const { heif_context_free(value); }
};
struct HeifHandleDeleter {
  void operator()(heif_image_handle* value) const {
    heif_image_handle_release(value);
  }
};
struct HeifImageDeleter {
  void operator()(heif_image* value) const { heif_image_release(value); }
};
struct HeifOptionsDeleter {
  void operator()(heif_decoding_options* value) const {
    heif_decoding_options_free(value);
  }
};

using HeifContextPtr = std::unique_ptr<heif_context, HeifContextDeleter>;
using HeifHandlePtr = std::unique_ptr<heif_image_handle, HeifHandleDeleter>;
using HeifImagePtr = std::unique_ptr<heif_image, HeifImageDeleter>;
using HeifOptionsPtr = std::unique_ptr<heif_decoding_options, HeifOptionsDeleter>;

struct ItemGraph {
  uint32_t base = 0;
  uint32_t gain = 0;
  uint32_t tone_map = 0;
};

void require_heif(heif_error result, const char* operation);
ItemGraph read_item_graph(const heif_context* context);

ItemGraph read_item_graph(const std::filesystem::path& path);

}  // namespace hdrbridge::gainmap::detail
