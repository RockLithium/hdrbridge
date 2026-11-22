#pragma once

#include "frame_exif.h"

#include <expat.h>

#include <charconv>
#include <locale>
#include <memory>
#include <numeric>
#include <string_view>

namespace hdrbridge::frame_exif {
namespace detail {

inline constexpr char namespace_separator = '|';
inline constexpr std::string_view rdf_description = "http://www.w3.org/1999/02/22-rdf-syntax-ns#|Description";
inline constexpr std::string_view rdf_root = "http://www.w3.org/1999/02/22-rdf-syntax-ns#|RDF";
inline constexpr std::string_view rdf_sequence = "http://www.w3.org/1999/02/22-rdf-syntax-ns#|Seq";
inline constexpr std::string_view rdf_item = "http://www.w3.org/1999/02/22-rdf-syntax-ns#|li";

struct CaptionProperty {
    const char* key = nullptr;
    bool numeric = false;
    bool array = false;
};

inline CaptionProperty caption_property(std::string_view name) {
    const auto separator = name.find(namespace_separator);
    if (separator == name.npos) return {};
    const auto uri = name.substr(0, separator);
    const auto local = name.substr(separator + 1);
    if (uri == "http://ns.adobe.com/tiff/1.0/") {
        if (local == "Make") return {"Make"};
        if (local == "Model") return {"Model"};
    }
    if (uri == "http://ns.adobe.com/exif/1.0/") {
        if (local == "FNumber") return {"FNumber", true};
        if (local == "ExposureTime") return {"ExposureTime", true};
        if (local == "FocalLength") return {"FocalLength", true};
        if (local == "FocalLengthIn35mmFilm") return {"FocalLengthIn35mmFormat", true};
        if (local == "ISOSpeedRatings") return {"ISO", true, true};
        if (local == "DateTimeOriginal") return {"DateTimeOriginal"};
    }
    if (uri == "http://cipa.jp/exif/1.0/") {
        if (local == "PhotographicSensitivity" || local == "ISOSpeed") return {"ISO", true};
        if (local == "LensModel") return {"LensModel"};
    }
    if (uri == "http://ns.adobe.com/exif/1.0/aux/" && local == "Lens") return {"LensModel"};
    return {};
}

inline std::string_view trim_caption_value(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == value.npos ? std::string_view{} :
        value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string format_xmp_caption_value(std::string_view value, CaptionProperty property) {
    value = trim_caption_value(value);
    if (value.empty() || value.size() > 512) return {};
    if (!property.numeric) return std::string(value);

    double number = 0;
    const auto slash = value.find('/');
    if (slash == value.npos) {
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return {};
    } else {
        const auto parse_uint = [](std::string_view part, uint32_t& result) {
            part = trim_caption_value(part);
            if (part.empty()) return false;
            const auto end = part.data() + part.size();
            const auto parsed = std::from_chars(part.data(), end, result);
            return parsed.ec == std::errc{} && parsed.ptr == end && result > 0;
        };
        uint32_t numerator = 0, denominator = 0;
        if (!parse_uint(value.substr(0, slash), numerator) ||
            !parse_uint(value.substr(slash + 1), denominator)) return {};
        if (std::string_view(property.key) == "ExposureTime" && numerator < denominator) {
            const auto divisor = std::gcd(numerator, denominator);
            return std::to_string(numerator / divisor) + "/" + std::to_string(denominator / divisor);
        }
        number = static_cast<double>(numerator) / denominator;
    }
    if (!std::isfinite(number) || number <= 0) return {};
    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::setprecision(8) << number;
    return formatted.str();
}

class XmpCaptionReader {
public:
    nlohmann::json parse(const std::vector<uint8_t>& bytes) {
        if (bytes.empty() || bytes.size() > 8u * 1024u * 1024u) return fields;
        size_t size = bytes.size();
        const bool utf16 = size >= 2 && (bytes[0] == 0 || bytes[1] == 0 ||
            (bytes[0] == 0xff && bytes[1] == 0xfe) || (bytes[0] == 0xfe && bytes[1] == 0xff));
        if (!utf16) while (size && bytes[size - 1] == 0) --size;
        const std::unique_ptr<XML_ParserStruct, decltype(&XML_ParserFree)> handle(
            XML_ParserCreateNS(nullptr, namespace_separator), XML_ParserFree);
        if (!handle) return fields;
        parser = handle.get();
        XML_SetUserData(parser, this);
        XML_SetElementHandler(parser, start_element, end_element);
        XML_SetCharacterDataHandler(parser, characters);
        XML_SetStartDoctypeDeclHandler(parser, deny_doctype);
        if (XML_Parse(parser, reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(size), XML_TRUE) != XML_STATUS_OK) {
            return nlohmann::json::object();
        }
        return std::move(fields);
    }

private:
    XML_Parser parser = nullptr;
    int depth = 0, rdf_depth = 0, description_depth = 0;
    int property_depth = 0, sequence_depth = 0, item_depth = 0;
    CaptionProperty property;
    std::string value;
    nlohmann::json fields = nlohmann::json::object();

    void store(CaptionProperty field, std::string_view raw) {
        if (!field.key || fields.contains(field.key)) return;
        auto formatted = format_xmp_caption_value(raw, field);
        if (!formatted.empty()) fields[field.key] = std::move(formatted);
    }

    void start(std::string_view name, const XML_Char** attributes) {
        ++depth;
        if (depth > 64) {
            XML_StopParser(parser, XML_FALSE);
        } else if (!rdf_depth && name == rdf_root) {
            rdf_depth = depth;
        } else if (rdf_depth && depth == rdf_depth + 1 && name == rdf_description) {
            description_depth = depth;
            for (; *attributes; attributes += 2) store(caption_property(attributes[0]), attributes[1]);
        } else if (description_depth && depth == description_depth + 1) {
            property = caption_property(name);
            if (property.key) {
                property_depth = depth;
                value.clear();
            }
        } else if (property_depth && property.array && depth == property_depth + 1 && name == rdf_sequence) {
            sequence_depth = depth;
        } else if (sequence_depth && depth == sequence_depth + 1 && name == rdf_item) {
            item_depth = depth;
            value.clear();
        }
    }

    void end() {
        if (depth == item_depth) {
            store(property, value);
            item_depth = 0;
            value.clear();
        }
        if (depth == sequence_depth) sequence_depth = 0;
        if (depth == property_depth) {
            store(property, value);
            property_depth = 0;
            property = {};
        }
        if (depth == description_depth) description_depth = 0;
        if (depth == rdf_depth) rdf_depth = 0;
        --depth;
    }

    static void XMLCALL start_element(void* context, const XML_Char* name, const XML_Char** attributes) {
        auto& reader = *static_cast<XmpCaptionReader*>(context);
        try { reader.start(name, attributes); }
        catch (...) { XML_StopParser(reader.parser, XML_FALSE); }
    }

    static void XMLCALL end_element(void* context, const XML_Char*) {
        auto& reader = *static_cast<XmpCaptionReader*>(context);
        try { reader.end(); }
        catch (...) { XML_StopParser(reader.parser, XML_FALSE); }
    }

    static void XMLCALL characters(void* context, const XML_Char* data, int length) {
        auto& reader = *static_cast<XmpCaptionReader*>(context);
        if (reader.depth != reader.property_depth && reader.depth != reader.item_depth) return;
        if (reader.depth == 0 || reader.value.size() >= 513) return;
        try { reader.value.append(data, std::min<size_t>(length, 513 - reader.value.size())); }
        catch (...) { XML_StopParser(reader.parser, XML_FALSE); }
    }

    static void XMLCALL deny_doctype(void* context, const XML_Char*, const XML_Char*, const XML_Char*, int) {
        XML_StopParser(static_cast<XmpCaptionReader*>(context)->parser, XML_FALSE);
    }
};

}  // namespace detail

inline nlohmann::json caption_fields(const std::vector<uint8_t>& exif, const std::vector<uint8_t>& xmp) {
    auto fields = parse(exif);
    if (xmp.empty()) return fields;
    auto fallback = detail::XmpCaptionReader{}.parse(xmp);
    for (auto& [key, value] : fallback.items()) {
        const auto existing = fields.find(key);
        if (existing == fields.end() || !existing->is_string() ||
            detail::trim_caption_value(existing->get_ref<const std::string&>()).empty() || *existing == "0") {
            fields[key] = std::move(value);
        }
    }
    return fields;
}

}  // namespace hdrbridge::frame_exif
