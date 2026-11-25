#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace hdrbridge::frame_exif {

nlohmann::json parse(const std::vector<uint8_t>& bytes);
nlohmann::json caption_fields(const std::vector<uint8_t>& exif, const std::vector<uint8_t>& xmp);
std::pair<std::string, std::string> captions(const nlohmann::json& fields);
std::string camera_logo_key(const std::string& make, const std::string& model = {});

}  // namespace hdrbridge::frame_exif
