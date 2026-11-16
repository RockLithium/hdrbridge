#include "../core/src/frame_exif.h"

#include <iostream>
#include <stdexcept>
#include <string>

using hdrbridge::frame_exif::camera_logo_key;
using hdrbridge::frame_exif::captions;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        check(camera_logo_key(" NIKON CORPORATION ") == "nikon", "Nikon logo key normalization");
        check(camera_logo_key("Fujifilm") == "fujifilm", "Fujifilm logo key");
        check(camera_logo_key("Xiaomi").empty(), "Xiaomi has no camera logo key");

        check(captions({{"Make", "Canon"}, {"Model", "Canon Canon EOSR5"}}).first == "EOSR5", "strip repeated Canon prefixes");
        check(captions({{"Make", "NIKON CORPORATION"}, {"Model", "NIKON Z8"}}).first == "Z8", "strip Nikon prefix");
        check(captions({{"Make", "Nikon"}, {"Model", "NIKON Z 8"}}).first == "Z 8", "strip Nikon prefix and preserve model spaces");
        check(captions({{"Make", "Xiaomi"}, {"Model", "Xiaomi15Ultra"}}).first == "Xiaomi15Ultra", "preserve compact Xiaomi model");
        check(captions({{"Make", "Xiaomi"}, {"Model", "Xiaomi 15 Ultra"}}).first == "Xiaomi 15 Ultra", "preserve spaced Xiaomi model");
        check(captions({{"Make", "vivo"}, {"Model", "vivoX200Ultra"}}).first == "vivoX200Ultra", "preserve compact vivo model");
        check(captions({{"Make", "vivo"}, {"Model", "vivo X200 Ultra"}}).first == "vivo X200 Ultra", "preserve spaced vivo model");
        check(captions({{"Make", "Apple"}, {"Model", "iPhone16ProMax"}}).first == "iPhone16ProMax", "preserve iPhone model");
        check(captions({{"Make", "Sony"}, {"Model", "Alpha SONYCorporation Edition"}}).first == "Alpha SONYCorporation Edition", "do not strip an interior Sony string");
        check(captions({{"Make", "Canon"}, {"Model", "CanonEOSR5"}}).first == "CanonEOSR5", "do not strip a partial-word Canon prefix");
        check(captions({{"Make", "  NIKON  "}}).first.empty(), "logo make without model has empty caption");
        check(captions({{"Make", "  Xiaomi  "}}).first == "Xiaomi", "non-logo make without model is fallback caption");

        const auto lines=captions({{"Make", "Canon"}, {"Model", "Canon EOS R5"},
                                   {"FocalLengthIn35mmFormat", "50"}, {"FNumber", "2.8"},
                                   {"ExposureTime", "1/100"}, {"ISO", "400"}});
        check(lines.first == "EOS R5", "camera caption model");
        check(lines.second == "50mm f/2.8 1/100s ISO400", "metadata caption format");
    } catch (const std::exception& e) {
        std::cerr << "frame caption test failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
