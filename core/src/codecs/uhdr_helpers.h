#pragma once

#include "canonical/decoded_image.h"
#include <ultrahdr_api.h>
#include <memory>

namespace hdrbridge::detail {

struct UhdrEncoderDeleter { void operator()(uhdr_codec_private_t* p) const { if (p) uhdr_release_encoder(p); } };
struct UhdrDecoderDeleter { void operator()(uhdr_codec_private_t* p) const { if (p) uhdr_release_decoder(p); } };
using UhdrEncoderPtr = std::unique_ptr<uhdr_codec_private_t, UhdrEncoderDeleter>;
using UhdrDecoderPtr = std::unique_ptr<uhdr_codec_private_t, UhdrDecoderDeleter>;

void require_uhdr(const uhdr_error_info_t& error, const char* operation);

std::vector<uint32_t> pq16_to_rgba1010102(const DecodedImage& decoded);

}  // namespace hdrbridge::detail
