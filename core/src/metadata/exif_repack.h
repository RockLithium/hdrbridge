#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hdrbridge::metadata {

inline bool tiff_header(const uint8_t* data) {
    return (data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
           (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42);
}

// Repack metadata rather than embedding a source TIFF raster or thumbnail.
// Offsets passed to read are relative to the TIFF header, for both EXIF
// packets and standalone TIFF files.
template <class Read>
std::vector<uint8_t> repack(Read read, uint32_t width, uint32_t height, bool tiff_raster) {
    const auto header = read(0, 8);
    if (!tiff_header(header.data())) throw std::runtime_error("Invalid source EXIF TIFF header");
    const bool le = header[0] == 'I';
    const auto u16 = [&](const uint8_t* p) -> uint16_t {
        return le ? uint16_t(p[0] | uint16_t(p[1]) << 8)
                  : uint16_t(uint16_t(p[0]) << 8 | p[1]);
    };
    const auto u32 = [&](const uint8_t* p) -> uint32_t {
        return le ? uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24
                  : uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    const auto type_size = [](uint16_t type) -> size_t {
        switch (type) {
            case 1: case 2: case 6: case 7: return 1;
            case 3: case 8: return 2;
            case 4: case 9: case 11: return 4;
            case 5: case 10: case 12: return 8;
            default: return 0;
        }
    };
    struct Entry { uint16_t type; uint32_t count; std::vector<uint8_t> data; };
    using Directory = std::map<uint16_t, Entry>;
    std::array<Directory, 3> dirs;
    const auto read_directory = [&](uint32_t offset, Directory& dir, bool root) {
        if (!offset) return;
        const auto count_bytes = read(offset, 2);
        const uint16_t count = u16(count_bytes.data());
        if (count > 4096) throw std::runtime_error("Invalid EXIF directory");
        const auto entries = read(offset, 2 + size_t(count) * 12 + 4);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* entry = entries.data() + 2 + i * 12;
            const uint16_t tag = u16(entry);
            if (tag == 0x927c || tag == 0xa005 || tag == 0x8773 ||
                tag == 0x02bc || tag == 0x8649 || tag == 0x014a) continue;
            if (root && tiff_raster) {
                // TIFF's image IFD also describes the raster and may contain
                // application payloads. Only carry fields common to EXIF IFD0.
                switch (tag) {
                    case 0x010e: case 0x010f: case 0x0110: case 0x0112:
                    case 0x0131: case 0x0132: case 0x013b: case 0x8298:
                    case 0x8769: case 0x8825: break;
                    default: continue;
                }
            } else if (root) {
                switch (tag) {
                    case 0x0102: case 0x0103: case 0x0106: case 0x0111: case 0x0115:
                    case 0x0116: case 0x0117: case 0x011c: case 0x013d: case 0x0140:
                    case 0x0142: case 0x0143: case 0x0144: case 0x0145: case 0x0201:
                    case 0x0202: case 0x013e: case 0x013f: continue;
                    default: break;
                }
            }
            const uint16_t type = u16(entry + 2);
            const uint32_t values = u32(entry + 4);
            const size_t size = type_size(type);
            if (!size || values > std::numeric_limits<size_t>::max() / size) continue;
            const size_t bytes = size * values;
            auto data = bytes <= 4 ? std::vector<uint8_t>(entry + 8, entry + 8 + bytes)
                                  : read(u32(entry + 8), bytes);
            dir[tag] = {type, values, std::move(data)};
        }
    };
    read_directory(u32(header.data() + 4), dirs[0], true);
    for (const auto& [tag, index] : {std::pair<uint16_t, size_t>{0x8769, 1}, {0x8825, 2}}) {
        const auto found = dirs[0].find(tag);
        if (found == dirs[0].end()) continue;
        if (found->second.type == 4 && found->second.count == 1) {
            read_directory(u32(found->second.data.data()), dirs[index], false);
        }
        dirs[0].erase(found);
    }

    const auto number = [&](uint32_t n, uint16_t type) -> Entry {
        std::vector<uint8_t> bytes(type == 3 ? 2 : 4);
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = uint8_t(n >> (8 * (le ? i : bytes.size() - 1 - i)));
        }
        return {type, 1, std::move(bytes)};
    };
    dirs[0][0x0100] = number(width, 4);
    dirs[0][0x0101] = number(height, 4);
    dirs[0][0x0112] = number(1, 3);
    dirs[1][0xa002] = number(width, 4);
    dirs[1][0xa003] = number(height, 4);
    dirs[1][0xa001] = number(65535, 3);
    dirs[0][0x8769] = number(0, 4);
    if (!dirs[2].empty()) dirs[0][0x8825] = number(0, 4);
    std::vector<uint8_t> out(header.begin(), header.end());
    const auto put16 = [&](size_t pos, uint16_t value) {
        out[pos] = uint8_t(value >> (le ? 0 : 8));
        out[pos + 1] = uint8_t(value >> (le ? 8 : 0));
    };
    const auto put32 = [&](size_t pos, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) out[pos + i] = uint8_t(value >> (8 * (le ? i : 3 - i)));
    };
    std::array<uint32_t, 3> locations{};
    for (size_t d = 0; d < dirs.size(); ++d) {
        if (dirs[d].empty()) continue;
        locations[d] = uint32_t(out.size());
        out.resize(out.size() + 2 + dirs[d].size() * 12 + 4);
        put16(locations[d], uint16_t(dirs[d].size()));
    }
    put32(4, locations[0]);
    for (size_t d = 0; d < dirs.size(); ++d) {
        size_t i = 0;
        for (const auto& [tag, entry] : dirs[d]) {
            const size_t pos = locations[d] + 2 + i++ * 12;
            put16(pos, tag);
            put16(pos + 2, entry.type);
            put32(pos + 4, entry.count);
            if (d == 0 && (tag == 0x8769 || tag == 0x8825)) {
                put32(pos + 8, locations[tag == 0x8769 ? 1 : 2]);
            } else if (entry.data.size() <= 4) {
                std::copy(entry.data.begin(), entry.data.end(), out.begin() + pos + 8);
            } else {
                if (out.size() & 1) out.push_back(0);
                const size_t offset = out.size();
                if (offset > UINT32_MAX - entry.data.size()) throw std::runtime_error("EXIF too large");
                out.insert(out.end(), entry.data.begin(), entry.data.end());
                put32(pos + 8, uint32_t(offset));
            }
        }
    }
    return out;
}

inline std::vector<uint8_t> repack_exif(const std::vector<uint8_t>& raw, uint32_t width, uint32_t height) {
    if (raw.empty()) return {};
    const auto tiff = [&](size_t pos) {
        return pos <= raw.size() && raw.size() - pos >= 8 && tiff_header(raw.data() + pos);
    };
    size_t base = std::string::npos;
    if (tiff(0)) base = 0;
    else if (raw.size() >= 6 && std::equal(raw.begin(), raw.begin() + 6, "Exif\0\0") && tiff(6)) base = 6;
    else if (raw.size() >= 4) {
        const size_t offset = (size_t(raw[0]) << 24) | (size_t(raw[1]) << 16) | (size_t(raw[2]) << 8) | raw[3];
        if (tiff(offset)) base = offset;
        else if (offset <= raw.size() - 4 && tiff(offset + 4)) base = offset + 4;
    }
    if (base == std::string::npos) throw std::runtime_error("Invalid source EXIF TIFF header");
    return repack([&](size_t offset, size_t bytes) {
        if (offset > raw.size() - base || bytes > raw.size() - base - offset) {
            throw std::runtime_error("EXIF data outside source");
        }
        return std::vector<uint8_t>(raw.begin() + base + offset, raw.begin() + base + offset + bytes);
    }, width, height, false);
}

}  // namespace hdrbridge::metadata
