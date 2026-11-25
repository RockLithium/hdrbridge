#include "photo_frame/frame_exif.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using hdrbridge::frame_exif::camera_logo_key;
using hdrbridge::frame_exif::caption_fields;
using hdrbridge::frame_exif::captions;

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void camera_caption_tests() {
    struct Case {
        const char* make;
        const char* model;
        const char* logo;
        const char* title;
    };
    const Case cases[] = {
        {" NIKON CORPORATION ", "NIKON Z 8", "nikon", "Z 8"},
        {"Canon", "Canon Canon EOS R5", "canon", "EOS R5"},
        {"Canon", "CanonEOSR5", "canon", "CanonEOSR5"},
        {"FUJI PHOTO FILM CO., LTD.", "FUJI PHOTO FILM FinePix S5Pro", "fujifilm", "FinePix S5Pro"},
        {"Leica Camera AG", "Leica Camera M11", "leica", "M11"},
        {"OLYMPUS_IMAGING_CORP.", "X450,D535Z,C370Z", "olympus", "X450,D535Z,C370Z"},
        {"OM Digital Solutions", "OM-1", "omsystem", "OM-1"},
        {"RICOH IMAGING COMPANY, LTD.", "PENTAX 645Z", "pentax", "645Z"},
        {"PENTAX RICOH IMAGING", "PENTAX WG-3", "pentax", "WG-3"},
        {"PENTAX RICOH IMAGING", "GR", "ricoh", "GR"},
        {"Asahi Optical Co.,Ltd.", "PENTAX Optio 430", "pentax", "Optio 430"},
        {"Samsung Techwin", "SAMSUNG-SM-C105A", "samsung", "SM-C105A"},
        {"SAMSUNG ELEC.", "SAMSUNG/VP-D590", "samsung", "VP-D590"},
        {"KONICA MINOLTA CAMERA, Inc.", "DiMAGE A2", "minolta", "DiMAGE A2"},
        {"Konica Minolta Photo Imaging, Inc.", "DiMAGE G600", "minolta", "DiMAGE G600"},
        {"JK Imaging, Ltd.", "KODAK PIXPRO AZ401", "kodak", "PIXPRO AZ401"},
        {"JK Imaging, Ltd.", "OtherCam 100", "", "JK Imaging, Ltd. OtherCam 100"},
        {"Victor Hasselblad AB", "", "hasselblad", ""},
        {"PHASE ONE A/S", "Phase One IQ4 150MP", "phaseone", "IQ4 150MP"},
        {"Panasonic", "Panasonic DC-S5M2", "lumix", "DC-S5M2"},
        {"Panasonic", "DMC-GH4", "lumix", "DMC-GH4"},
        {"Panasonic", "LUMIX GH6", "lumix", "GH6"},
        {"Panasonic", "HC-X2000", "panasonic", "HC-X2000"},
        {"vivo", "iQOO 13", "iqoo", "13"},
        {"Xiaomi", "Xiaomi 15 Ultra", "xiaomi", "15 Ultra"},
        {"Apple", "iPhone 16 Pro Max", "apple", "iPhone 16 Pro Max"},
        {"Sony", "Alpha Sony Edition", "sony", "Alpha Sony Edition"},
        {"RED", "V-RAPTOR 8K VV", "", "RED V-RAPTOR 8K VV"},
        {"RED", "", "", "RED"},
    };
    for (const auto& c : cases) {
        const std::string label = std::string(c.make) + " / " + c.model;
        check(camera_logo_key(c.make, c.model) == c.logo, "logo: " + label);
        check(captions({{"Make", c.make}, {"Model", c.model}}).first == c.title,
              "title: " + label);
    }
    check(captions(nlohmann::json::object()) == std::pair<std::string, std::string>{},
          "metadata-free input needs no caption");
    check(captions({{"ISO", "100"}}).second == "ISO100", "partial exposure has no stray units");
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
    <e:FocalLength>70/1</e:FocalLength>
    <e:FocalLengthIn35mmFilm>105</e:FocalLengthIn35mmFilm>
    <e:ISOSpeedRatings><r:Seq><r:li>8000</r:li><r:li>1600</r:li></r:Seq></e:ISOSpeedRatings>
    <c:LensModel>RF24-105mm F4 L IS USM &amp; filter</c:LensModel>
    <a:CameraProfile><r:Description t:Model="Wrong" e:FNumber="3/1"/></a:CameraProfile>
  </r:Description>
</r:RDF>)xml");
    auto fields = caption_fields({}, xmp);
    check(camera_logo_key(fields.value("Make", "")) == "canon", "XMP camera make");
    check(fields.value("LensModel", "") == "RF24-105mm F4 L IS USM & filter", "XMP XML entities and lens");
    check(fields.value("FocalLength", "") == "70", "XMP rational focal length");
    const auto lines = captions(fields);
    check(lines.first == "EOS R6 Mark III", "XMP namespace and element model");
    check(lines.second == "105mm f/4 1/160s ISO8000", "XMP equivalent focal length, exposure and ISO");

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
        camera_caption_tests();
        xmp_caption_tests();
    } catch (const std::exception& e) {
        std::cerr << "frame caption test failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
