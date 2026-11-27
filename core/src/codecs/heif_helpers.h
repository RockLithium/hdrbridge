#pragma once

#include <libheif/heif.h>
#include "hdrbridge_core.h"
#include <memory>

namespace hdrbridge::detail {

struct HeifContextDeleter { void operator()(heif_context* p) const { heif_context_free(p); } };
struct HeifHandleDeleter { void operator()(heif_image_handle* p) const { heif_image_handle_release(p); } };
struct HeifImageDeleter { void operator()(heif_image* p) const { heif_image_release(p); } };
struct HeifOptionsDeleter { void operator()(heif_decoding_options* p) const { heif_decoding_options_free(p); } };
struct NclxDeleter { void operator()(heif_color_profile_nclx* p) const { heif_nclx_color_profile_free(p); } };
struct HeifEncoderDeleter { void operator()(heif_encoder* p) const { heif_encoder_release(p); } };
struct HeifEncodingOptionsDeleter { void operator()(heif_encoding_options* p) const { heif_encoding_options_free(p); } };

using ContextPtr = std::unique_ptr<heif_context, HeifContextDeleter>;
using HandlePtr = std::unique_ptr<heif_image_handle, HeifHandleDeleter>;
using ImagePtr = std::unique_ptr<heif_image, HeifImageDeleter>;
using OptionsPtr = std::unique_ptr<heif_decoding_options, HeifOptionsDeleter>;
using NclxPtr = std::unique_ptr<heif_color_profile_nclx, NclxDeleter>;
using HeifEncoderPtr = std::unique_ptr<heif_encoder, HeifEncoderDeleter>;
using HeifEncodingOptionsPtr = std::unique_ptr<heif_encoding_options, HeifEncodingOptionsDeleter>;

struct OpenedHeif {
  std::vector<uint8_t> bytes;
  ContextPtr context;
  HandlePtr handle;
};

void require_heif(const heif_error& e, const char* operation);

OpenedHeif open_heif(const std::filesystem::path& path);

OpenedHeif open_heif_bytes(std::vector<uint8_t> bytes);

SourceInfo inspect_opened(const std::filesystem::path& path, const OpenedHeif& opened);

}  // namespace hdrbridge::detail
