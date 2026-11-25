#include "codecs/tiff.h"
#include "color/icc_profile.h"
#include "color/signaling.h"
#include "color/gamut.h"
#include "color/pq.h"
#include "codecs/gainmap/tiff.h"
#include "image/orientation.h"
#include "infrastructure/files.h"
#include "infrastructure/pixel_workers.h"
#include "metadata/exif_output.h"
#include "metadata/exif_payload.h"
#include "metadata/source_metadata.h"
#include "metadata/tiff_input_exif.h"
#include "hdrbridge_version.h"
#include <lcms2.h>
#include <tiffio.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace hdrbridge::detail {

void encode_gainmap_tiff(const std::filesystem::path& path,
                         const gainmap::GainMapRaster& raster, uint32_t) {
  TIFF* raw = nullptr;
#ifdef _WIN32
  raw = TIFFOpenW(path.c_str(), "w");
#else
  raw = TIFFOpen(path.string().c_str(), "w");
#endif
  if (!raw) throw std::runtime_error("cannot create gain-map TIFF");
  std::unique_ptr<TIFF, void(*)(TIFF*)> tiff(raw, TIFFClose);
  TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, raster.width);
  TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, raster.height);
  const bool rgb = raster.channels == 3;
  TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, rgb ? 3 : 1);
  TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tiff.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC,
               rgb ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
  TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, raster.height);
  std::vector<uint16_t> plane;
  const uint16_t* samples = raster.rgb16.data();
  size_t sample_count = raster.rgb16.size();
  if (!rgb) {
    plane.resize(static_cast<size_t>(raster.width) * raster.height);
    for (size_t p = 0; p < plane.size(); ++p) plane[p] = raster.rgb16[p * 3u];
    samples = plane.data();
    sample_count = plane.size();
  }
  if (TIFFWriteEncodedStrip(tiff.get(), 0, const_cast<uint16_t*>(samples),
      static_cast<tmsize_t>(sample_count * sizeof(uint16_t))) < 0) {
    throw std::runtime_error("cannot write gain-map TIFF");
  }
}

struct TiffDeleter { void operator()(TIFF* value) const { if (value) TIFFClose(value); } };
using TiffPtr = std::unique_ptr<TIFF, TiffDeleter>;

static TiffPtr open_tiff(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
  TIFF* value = TIFFOpenW(path.c_str(), mode);
#else
  TIFF* value = TIFFOpen(path.string().c_str(), mode);
#endif
  if (!value) throw std::runtime_error("cannot open TIFF: " + path.string());
  return TiffPtr(value);
}

void encode_tiff(const std::filesystem::path& path, const DecodedImage& decoded,
                 const ConversionOptions& options, const std::vector<uint16_t>& pixels) {
  auto tiff = open_tiff(path, "w");
  const bool p3 = options.output_gamut == "p3";
  auto icc = !options.diagnostic_icc_profile_path.empty()
      ? read_file(options.diagnostic_icc_profile_path)
      : make_windows_compatible_pq_icc(p3);
  TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, decoded.info.width);
  TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, decoded.info.height);
  TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tiff.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION,
               options.tiff_compressed ? COMPRESSION_ADOBE_DEFLATE : COMPRESSION_NONE);
#ifdef TIFFTAG_ZIPQUALITY
  if (options.tiff_compressed) {
    TIFFSetField(tiff.get(), TIFFTAG_ZIPQUALITY,
                 std::clamp(options.tiff_compression_level, 1, 9));
  }
#endif
  // Premiere interoperates reliably with ACR-style TIFFs that use one large
  // strip. Thousands of scanline-sized strips caused inserted black frames.
  TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, decoded.info.height);
  const std::string description = p3 ? "HDR Bridge direct RGB16 Display P3 PQ" :
                                       "HDR Bridge direct RGB16 Rec.2020 PQ";
  TIFFSetField(tiff.get(), TIFFTAG_IMAGEDESCRIPTION, description.c_str());
  TIFFSetField(tiff.get(), TIFFTAG_SOFTWARE, "HDR Bridge " HDRBRIDGE_VERSION_STRING);
  TIFFSetField(tiff.get(), TIFFTAG_ICCPROFILE, static_cast<uint32_t>(icc.size()), icc.data());
  std::vector<uint8_t> diagnostic_photoshop;
  std::vector<uint8_t> diagnostic_iptc;
  std::vector<uint8_t> diagnostic_xmp;
  if (!options.diagnostic_tiff_metadata_source.empty()) {
    auto source = open_tiff(options.diagnostic_tiff_metadata_source, "r");
#ifdef TIFFTAG_PHOTOSHOP
    uint32_t photoshop_size = 0; void* photoshop_data = nullptr;
    if (TIFFGetField(source.get(), TIFFTAG_PHOTOSHOP, &photoshop_size, &photoshop_data) == 1 && photoshop_data && photoshop_size) {
      diagnostic_photoshop.assign(static_cast<uint8_t*>(photoshop_data), static_cast<uint8_t*>(photoshop_data) + photoshop_size);
      TIFFSetField(tiff.get(), TIFFTAG_PHOTOSHOP, photoshop_size, diagnostic_photoshop.data());
    }
#endif
#ifdef TIFFTAG_RICHTIFFIPTC
    uint32_t iptc_words = 0; void* iptc_data = nullptr;
    if (TIFFGetField(source.get(), TIFFTAG_RICHTIFFIPTC, &iptc_words, &iptc_data) == 1 && iptc_data && iptc_words) {
      const uint32_t iptc_bytes = iptc_words * 4u;
      diagnostic_iptc.assign(static_cast<uint8_t*>(iptc_data), static_cast<uint8_t*>(iptc_data) + iptc_bytes);
      TIFFSetField(tiff.get(), TIFFTAG_RICHTIFFIPTC, iptc_words, diagnostic_iptc.data());
    }
#endif
#ifdef TIFFTAG_XMLPACKET
    uint32_t xmp_size = 0; void* xmp_data = nullptr;
    if (TIFFGetField(source.get(), TIFFTAG_XMLPACKET, &xmp_size, &xmp_data) == 1 && xmp_data && xmp_size) {
      diagnostic_xmp.assign(static_cast<uint8_t*>(xmp_data), static_cast<uint8_t*>(xmp_data) + xmp_size);
      TIFFSetField(tiff.get(), TIFFTAG_XMLPACKET, xmp_size, diagnostic_xmp.data());
    }
#endif
    float xres = 0.0f, yres = 0.0f; uint16_t unit = RESUNIT_NONE;
    if (TIFFGetField(source.get(), TIFFTAG_XRESOLUTION, &xres) == 1) TIFFSetField(tiff.get(), TIFFTAG_XRESOLUTION, xres);
    if (TIFFGetField(source.get(), TIFFTAG_YRESOLUTION, &yres) == 1) TIFFSetField(tiff.get(), TIFFTAG_YRESOLUTION, yres);
    if (TIFFGetField(source.get(), TIFFTAG_RESOLUTIONUNIT, &unit) == 1) TIFFSetField(tiff.get(), TIFFTAG_RESOLUTIONUNIT, unit);
  }
#ifdef TIFFTAG_XMLPACKET
  if (options.diagnostic_tiff_metadata_source.empty() &&
      options.copy_xmp && !decoded.xmp.empty()) {
    TIFFSetField(tiff.get(), TIFFTAG_XMLPACKET, static_cast<uint32_t>(decoded.xmp.size()), decoded.xmp.data());
  }
#endif
  for (uint32_t y = 0; y < decoded.info.height; ++y) {
    auto* row = const_cast<uint16_t*>(pixels.data() + static_cast<size_t>(y) * decoded.info.width * 3u);
    if (TIFFWriteScanline(tiff.get(), row, y, 0) < 0) throw std::runtime_error("TIFF scanline encoding failed");
  }
  if (!TIFFWriteDirectory(tiff.get())) throw std::runtime_error("cannot commit TIFF directory");
}

static std::string icc_description(const void* data, uint32_t size) {
  if (!data || size == 0) return {};
  cmsHPROFILE profile = cmsOpenProfileFromMem(data, size);
  if (!profile) return {};
  std::array<char, 256> text{};
  cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", text.data(), static_cast<cmsUInt32Number>(text.size()));
  cmsCloseProfile(profile);
  return text.data();
}

static std::optional<cmsVideoSignalType> icc_cicp(const void* data, uint32_t size) {
  if (!data || size == 0) return std::nullopt;
  cmsHPROFILE profile = cmsOpenProfileFromMem(data, size);
  if (!profile) return std::nullopt;
  const auto* value = static_cast<const cmsVideoSignalType*>(
      cmsReadTag(profile, cmsSigcicpTag));
  const std::optional<cmsVideoSignalType> result = value
      ? std::optional<cmsVideoSignalType>(*value) : std::nullopt;
  cmsCloseProfile(profile);
  return result;
}

Verification decode_tiff(const std::filesystem::path& path,
                         const std::vector<uint16_t>* expected) {
  auto tiff = open_tiff(path, "r");
  Verification v;
  uint16_t samples = 0, bits = 0, sample_format = SAMPLEFORMAT_UINT;
  uint16_t photometric = 0, planar = 0, compression = 0;
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &v.width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &v.height);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
  TIFFGetField(tiff.get(), TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetField(tiff.get(), TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetField(tiff.get(), TIFFTAG_COMPRESSION, &compression);
  v.bit_depth = bits;
  v.pixel_format = samples == 3 && bits == 16 && sample_format == SAMPLEFORMAT_UINT &&
                   photometric == PHOTOMETRIC_RGB && planar == PLANARCONFIG_CONTIG
      ? "TIFF RGB uint16 contiguous" : "unexpected TIFF pixel format";
  uint32_t icc_size = 0; void* icc_data = nullptr;
  const bool has_icc = TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc_data) == 1 && icc_size > 0;
  const std::string description = has_icc ? icc_description(icc_data, icc_size) : std::string{};
  const auto cicp = has_icc ? icc_cicp(icc_data, icc_size) : std::nullopt;
  const bool rec2020 = cicp && cicp->ColourPrimaries == 9 &&
                       cicp->TransferCharacteristics == 16 &&
                       cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag == 1;
  const bool p3 = cicp && cicp->ColourPrimaries == 12 &&
                  cicp->TransferCharacteristics == 16 &&
                  cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag == 1;
  const bool rec2020_hlg = cicp && cicp->ColourPrimaries == 9 &&
                           cicp->TransferCharacteristics == 18 &&
                           cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag == 1;
  const bool p3_hlg = cicp && cicp->ColourPrimaries == 12 &&
                      cicp->TransferCharacteristics == 18 &&
                      cicp->MatrixCoefficients == 0 && cicp->VideoFullRangeFlag == 1;
  v.color_encoding = rec2020 ? "direct RGB16 Rec.2020/PQ ICC" :
                     p3 ? "direct RGB16 Display P3/PQ ICC" :
                     rec2020_hlg ? "direct RGB16 Rec.2020/HLG ICC" :
                     p3_hlg ? "direct RGB16 Display P3/HLG ICC" :
                     "missing/unexpected direct-HDR ICC";
  std::vector<uint16_t> pixels(static_cast<size_t>(v.width) * v.height * 3u);
  for (uint32_t y = 0; y < v.height; ++y) {
    if (TIFFReadScanline(tiff.get(), pixels.data() + static_cast<size_t>(y) * v.width * 3u, y, 0) < 0) {
      throw std::runtime_error("TIFF verification scanline decode failed");
    }
  }
  v.exact_roundtrip = expected && expected->size() == pixels.size() &&
                      std::memcmp(expected->data(), pixels.data(), pixels.size() * sizeof(uint16_t)) == 0;
  auto [minimum, maximum] = std::minmax_element(pixels.begin(), pixels.end());
  if (minimum != pixels.end()) { v.min_value = *minimum; v.max_value = *maximum; }
  v.checks.push_back(v.width && v.height ? "TIFF dimensions parse" : "TIFF dimensions missing");
  v.checks.push_back(v.pixel_format == "TIFF RGB uint16 contiguous" ? "direct unsigned RGB16 storage" : "TIFF is not direct RGB16");
  v.checks.push_back(compression == COMPRESSION_ADOBE_DEFLATE || compression == COMPRESSION_LZW || compression == COMPRESSION_NONE
      ? "lossless TIFF compression" : "unexpected TIFF compression");
  v.checks.push_back(rec2020 || p3 || rec2020_hlg || p3_hlg
      ? "HDR-aware ICC carries matching CICP signaling" : "direct-HDR CICP ICC missing");
  if (expected) v.checks.push_back(v.exact_roundtrip ? "exact RGB16 pixel roundtrip" : "RGB16 pixel roundtrip mismatch");
  v.passed = v.width > 0 && v.height > 0 && samples == 3 && bits == 16 &&
             sample_format == SAMPLEFORMAT_UINT && photometric == PHOTOMETRIC_RGB &&
             planar == PLANARCONFIG_CONTIG && (rec2020 || p3 || rec2020_hlg || p3_hlg) &&
             (!expected || v.exact_roundtrip);
  return v;
}

SourceInfo inspect_tiff_metadata(const std::filesystem::path& path,
                                 std::vector<uint8_t>* exif) {
  auto tiff = open_tiff(path, "r");
  SourceInfo info;
  info.path = path; info.format = "TIFF"; info.container_brand = "TIFF"; info.codec = "TIFF";
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &info.width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &info.height);
  uint16_t bits = 0, samples = 0, sample_format = SAMPLEFORMAT_UINT;
  uint16_t tiff_orientation = ORIENTATION_TOPLEFT;
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
  const bool has_orientation = TIFFGetField(tiff.get(), TIFFTAG_ORIENTATION,
                                             &tiff_orientation) == 1;
  info.original_orientation = static_cast<uint8_t>(std::clamp<uint16_t>(tiff_orientation, 1, 8));
  info.orientation_normalized = true;
  info.orientation_status = has_orientation ? "present" : "absent";
  info.bit_depth = bits; info.chroma = samples == 3 ? "4:4:4 RGB" : "unknown";
  info.pixel_format = samples == 3
      ? "RGB" + std::to_string(bits) + (sample_format == SAMPLEFORMAT_IEEEFP ? " float" : " integer")
      : "unknown";
  uint32_t icc_size = 0; void* icc_data = nullptr;
  const bool has_icc = TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc_data) == 1 &&
                       icc_data && icc_size > 0;
  store_icc_signal(info, parse_icc_signal(has_icc ? icc_data : nullptr,
                                           has_icc ? icc_size : 0));
  info.profile = info.icc_description.empty() ? "unprofiled" : info.icc_description;
  resolve_color_signaling(info, false);
  info.color_signal_kind = info.resolved_signaling_source;
  uint16_t subifd_count = 0; toff_t* subifd_offsets = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_SUBIFD, &subifd_count, &subifd_offsets) == 1 && subifd_count > 0) {
    info.gain_map_present = true; info.asset_kind = "gain-map-hdr";
  }
#ifdef TIFFTAG_XMLPACKET
  uint32_t xmp_size = 0; void* xmp = nullptr;
  info.xmp_present = TIFFGetField(tiff.get(), TIFFTAG_XMLPACKET, &xmp_size, &xmp) == 1 && xmp_size > 0;
  info.xmp_status = metadata_status(info.xmp_present);
#else
  info.xmp_status = "unsupported";
#endif
  auto metadata = read_tiff_input_exif(tiff.get(), info);
  if (exif) *exif = std::move(metadata);
  return info;
}

DecodedImage decode_tiff_input(const std::filesystem::path& path) {
  DecodedImage decoded;
  decoded.info = inspect_tiff_metadata(path, &decoded.exif);
  if (decoded.info.gain_map_present) {
    throw std::runtime_error("gain-map TIFF detected; Adobe SubIFD reconstruction is not yet a validated direct-HDR input path");
  }
  if ((decoded.info.transfer != 16 && decoded.info.transfer != 18) ||
      (decoded.info.primaries != 1 && decoded.info.primaries != 9 &&
       decoded.info.primaries != 12)) {
    return decoded;
  }
  auto tiff = open_tiff(path, "r");
  uint16_t samples = 0, bits = 0, sample_format = SAMPLEFORMAT_UINT, photometric = 0, planar = 0;
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
  TIFFGetField(tiff.get(), TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetField(tiff.get(), TIFFTAG_PLANARCONFIG, &planar);
  if (samples != 3 || bits != 16 || sample_format != SAMPLEFORMAT_UINT ||
      photometric != PHOTOMETRIC_RGB || planar != PLANARCONFIG_CONTIG) {
    throw std::runtime_error("direct HDR TIFF input must be contiguous unsigned RGB16");
  }
  decoded.rgb.resize(static_cast<size_t>(decoded.info.width) * decoded.info.height * 3u);
  for (uint32_t y = 0; y < decoded.info.height; ++y) {
    if (TIFFReadScanline(tiff.get(), decoded.rgb.data() + static_cast<size_t>(y) * decoded.info.width * 3u, y, 0) < 0) {
      throw std::runtime_error("direct HDR TIFF input scanline decode failed");
    }
  }
#ifdef TIFFTAG_XMLPACKET
  uint32_t xmp_size = 0; void* xmp = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_XMLPACKET, &xmp_size, &xmp) == 1 && xmp && xmp_size > 0) {
    const auto* first = static_cast<const uint8_t*>(xmp);
    decoded.xmp.assign(first, first + xmp_size);
  }
#endif
  if (decoded.info.original_orientation != 1) {
    const auto transformed = orientation::normalize_rgb16(
        decoded.rgb, decoded.info.width, decoded.info.height,
        decoded.info.original_orientation);
    decoded.info.width = transformed.width;
    decoded.info.height = transformed.height;
  }
  orientation::set_xmp_orientation_to_one(decoded.xmp);
  if (decoded.info.transfer == 18) {
    const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
    parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
      for (size_t p = begin; p < end; ++p) {
        const auto nits = transfer::hlg_to_linear_nits({
            decoded.rgb[p * 3u] / 65535.0,
            decoded.rgb[p * 3u + 1u] / 65535.0,
            decoded.rgb[p * 3u + 2u] / 65535.0});
        for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] =
            static_cast<uint16_t>(std::llround(
                transfer::nits_to_pq(nits[c]) * 65535.0));
      }
    });
  }
  if (decoded.info.primaries == 12) convert_p3_pq_to_rec2020_pq(decoded.rgb);
  else if (decoded.info.primaries == 1) convert_rec709_pq_to_rec2020_pq(decoded.rgb);
  return decoded;
}


void verify_gainmap_tiff(const std::filesystem::path& path,
                        const gainmap::GainMapRaster& raster, size_t channels) {
  auto tiff = open_tiff(path, "r");
  uint32_t width = 0, height = 0;
  uint16_t depth = 0, count = 0, planar = 0;
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height);
  TIFFGetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, &depth);
  TIFFGetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &count);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &planar);
  if (width != raster.width || height != raster.height || depth != 16 ||
      count != channels || planar != PLANARCONFIG_CONTIG)
    throw std::runtime_error("gain-map TIFF metadata readback mismatch");
}

}  // namespace hdrbridge::detail
