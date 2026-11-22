#include "../core/src/frame_xmp.h"

#include <iostream>
#include <stdexcept>
#include <string>

using hdrbridge::frame_exif::camera_logo_key;
using hdrbridge::frame_exif::caption_fields;
using hdrbridge::frame_exif::captions;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<uint8_t> packet(std::string_view xml) {
    return {xml.begin(), xml.end()};
}

void xmp_caption_tests() {
    const auto xmp = packet(R"xml(
<r:RDF xmlns:r="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
       xmlns:t="http://ns.adobe.com/tiff/1.0/" xmlns:e="http://ns.adobe.com/exif/1.0/"
       xmlns:c="http://cipa.jp/exif/1.0/" xmlns:a="urn:other">
  <r:Description t:Make="Canon" e:FNumber="4/1" e:ExposureTime="2/320" a:Model="Wrong">
    <t:Model>Canon EOS R6 Mark III</t:Model>
    <e:FocalLength>105/1</e:FocalLength>
    <e:ISOSpeedRatings><r:Seq><r:li>8000</r:li><r:li>1600</r:li></r:Seq></e:ISOSpeedRatings>
    <c:LensModel>RF24-105mm F4 L IS USM &amp; filter</c:LensModel>
    <a:CameraProfile><r:Description t:Model="Wrong" e:FNumber="3/1"/></a:CameraProfile>
  </r:Description>
</r:RDF>)xml");
    auto fields = caption_fields({}, xmp);
    check(camera_logo_key(fields.value("Make", "")) == "canon", "XMP camera make");
    check(fields.value("LensModel", "") == "RF24-105mm F4 L IS USM & filter", "XMP XML entities and lens");
    const auto lines = captions(fields);
    check(lines.first == "EOS R6 Mark III", "XMP namespace and element model");
    check(lines.second == "105mm f/4 1/160s ISO8000", "XMP rational and ISO sequence");

    const std::vector<uint8_t> exif = {
      'I','I',42,0,8,0,0,0,1,0,0x0f,0x01,2,0,4,0,0,0,'D','J','I',0,0,0,0,0
    };
    fields = caption_fields(exif, xmp);
    check(fields.value("Make", "") == "DJI" && fields.value("ISO", "") == "8000",
          "EXIF takes precedence with field-wise XMP fallback");
    const auto doctype = packet(R"xml(<!DOCTYPE r:RDF [<!ENTITY x SYSTEM "file:///local">]>
<r:RDF xmlns:r="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns:t="http://ns.adobe.com/tiff/1.0/">
  <r:Description t:Make="Canon"><t:Model>&x;</t:Model></r:Description>
</r:RDF>)xml");
    check(!caption_fields({}, doctype).contains("Make"), "Reject XMP with a document type declaration");
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
        xmp_caption_tests();
    } catch (const std::exception& e) {
        std::cerr << "frame caption test failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
