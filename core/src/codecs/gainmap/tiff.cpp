#include "codecs/gainmap/tiff.h"
#include "codecs/gainmap/icc_description.h"
#include "codecs/gainmap/iso_metadata.h"
#include "codecs/gainmap/decode_support.h"
#include "codecs/gainmap/extract_support.h"
#include "codecs/gainmap/reconstruction_math.h"
#include "color/icc_rgb_primaries.h"
#include "image/orientation.h"
#include "metadata/tiff_input_exif.h"
#include <tiffio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace hdrbridge::gainmap {

using namespace detail;

namespace {

struct TiffDeleter {
  void operator()(TIFF* value) const { if (value) TIFFClose(value); }
};

using TiffPtr = std::unique_ptr<TIFF, TiffDeleter>;

constexpr uint32_t kTiffGainMapMetadataTag = 52557;
constexpr uint16_t kTiffGainMapPhotometric = 52553;
constexpr uint32_t kTiffGainMapSubfileType = 32;

const TIFFFieldInfo kGainMapFields[] = {
    {kTiffGainMapMetadataTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED,
     FIELD_CUSTOM, TRUE, TRUE, const_cast<char*>("GainMapMetadata")}};

struct TiffGainMapData {
  SourceInfo info;
  IsoGainMapMetadata metadata;
  std::vector<uint16_t> base;
  std::vector<uint16_t> map;
  std::vector<uint8_t> exif;
  std::vector<uint8_t> xmp;
  CmsProfilePtr base_profile;
  std::array<const cmsToneCurve*, 3> base_curves{};
};

TiffPtr open_tiff_gain_map(const std::filesystem::path& path) {
#ifdef _WIN32
  TiffPtr tiff(TIFFOpenW(path.c_str(), "r"));
#else
  TiffPtr tiff(TIFFOpen(path.string().c_str(), "r"));
#endif
  if (!tiff) throw std::runtime_error("cannot open Adobe gain-map TIFF");
  if (TIFFMergeFieldInfo(tiff.get(), kGainMapFields,
                         static_cast<uint32_t>(std::size(kGainMapFields))) < 0) {
    throw std::runtime_error("cannot register TIFF gain-map metadata tag");
  }
  return tiff;
}

TiffGainMapData read_tiff_gain_map(const std::filesystem::path& path,
                                   bool pixels,
                                   std::atomic_bool* cancel) {
  auto tiff = open_tiff_gain_map(path);
  TiffGainMapData result;
  auto& info = result.info;
  info.path = path;
  info.format = "TIFF";
  info.container_brand = "TIFF/SubIFD";
  info.asset_kind = "gain-map-hdr";
  info.gain_map_present = true;
  info.gain_map_family = "adobe-iso-tiff";
  info.codec = "RGB16 base + RGB16 ISO gain map SubIFD";
  info.profile = "ISO 21496-1 gain-map HDR reconstruction";
  info.bit_depth = 16;
  info.chroma = "4:4:4 RGB";
  uint16_t base_bits = 0, base_samples = 0, base_format = SAMPLEFORMAT_UINT;
  uint16_t base_photo = 0, base_planar = 0, base_orientation = ORIENTATION_TOPLEFT;
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &info.width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &info.height);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &base_bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &base_samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &base_format);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PHOTOMETRIC, &base_photo);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &base_planar);
  const bool has_orientation = TIFFGetField(tiff.get(), TIFFTAG_ORIENTATION,
                                             &base_orientation) == 1;
  if (!info.width || !info.height || base_bits != 16 || base_samples != 3 ||
      base_format != SAMPLEFORMAT_UINT || base_photo != PHOTOMETRIC_RGB ||
      base_planar != PLANARCONFIG_CONTIG) {
    throw std::runtime_error("Adobe gain-map TIFF base must be contiguous RGB16");
  }
  info.base_width = info.width;
  info.base_height = info.height;
  info.base_bit_depth = base_bits;
  info.base_channels = base_samples;
  info.base_codec = "TIFF RGB";
  info.matrix = 0;
  info.full_range = true;
  info.range_known = true;
  info.original_orientation = static_cast<uint8_t>(std::clamp<uint16_t>(base_orientation, 1, 8));
  info.orientation_status = has_orientation ? "present" : "absent";
  uint32_t icc_size = 0;
  void* icc_data = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc_data) != 1 || !icc_data || !icc_size) {
    throw std::runtime_error("Adobe gain-map TIFF SDR base ICC profile is missing");
  }
  result.base_profile.reset(cmsOpenProfileFromMem(icc_data, icc_size));
  if (!result.base_profile) throw std::runtime_error("cannot parse Adobe gain-map TIFF base ICC");
  const auto profile = result.base_profile.get();
  info.primaries = icc::identify_rgb_primaries(profile);
  if (!info.primaries) {
    throw std::runtime_error("unsupported gain-map TIFF base ICC RGB primaries");
  }
  result.base_curves = {
      static_cast<const cmsToneCurve*>(cmsReadTag(profile, cmsSigRedTRCTag)),
      static_cast<const cmsToneCurve*>(cmsReadTag(profile, cmsSigGreenTRCTag)),
      static_cast<const cmsToneCurve*>(cmsReadTag(profile, cmsSigBlueTRCTag))};
  if (!result.base_curves[0] || !result.base_curves[1] || !result.base_curves[2]) {
    throw std::runtime_error("Adobe gain-map TIFF base ICC has no RGB transfer curves");
  }
  info.transfer = icc::identify_sdr_transfer(result.base_curves);
  info.base_transfer = info.transfer == 13 ? "sRGB" :
      info.transfer == 14 ? "BT.2020 SDR" : "ICC RGB transfer curve";
  info.base_color_space = info.primaries == 1 ? "BT.709 / sRGB primaries" :
      info.primaries == 12 ? "Display P3" : "BT.2020";
  info.icc_description = profile_description(profile);
  const auto version = cmsGetEncodedICCversion(profile);
  info.icc_version = std::to_string(version >> 24) + "." +
                     std::to_string((version >> 20) & 0x0f);
  info.profile = (info.icc_description.empty() ? info.base_color_space : info.icc_description) +
                 " base + ISO 21496-1 gain map";
  const auto* cicp = static_cast<const cmsVideoSignalType*>(cmsReadTag(profile, cmsSigcicpTag));
  if (cicp && (cicp->TransferCharacteristics == 16 || cicp->TransferCharacteristics == 18)) {
    throw std::runtime_error("HDR base ICC is unsupported for TIFF gain-map reconstruction");
  }
  info.icc_cicp_present = cicp != nullptr;
  info.icc_primaries = cicp ? cicp->ColourPrimaries : info.primaries;
  info.icc_transfer = cicp ? cicp->TransferCharacteristics : info.transfer;
  info.icc_matrix = cicp ? cicp->MatrixCoefficients : 0;
  info.icc_full_range = cicp ? cicp->VideoFullRangeFlag != 0 : true;
  info.icc_transfer_interpretation = info.base_transfer;
  info.icc_present = true;
  info.icc_status = "present";
  info.resolved_signaling_source = "ICC";
  info.color_signal_kind = "TIFF ICC + ISO gain-map metadata";
  info.pixel_format = "RGB16 integer base + RGB16 gain map";
#ifdef TIFFTAG_XMLPACKET
  uint32_t xmp_size = 0;
  void* xmp_data = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_XMLPACKET, &xmp_size, &xmp_data) == 1 && xmp_data && xmp_size) {
    result.xmp.assign(static_cast<uint8_t*>(xmp_data), static_cast<uint8_t*>(xmp_data) + xmp_size);
    info.xmp_present = true;
  }
  info.xmp_status = info.xmp_present ? "present" : "absent";
#endif
  result.exif = read_tiff_input_exif(tiff.get(), info);
  uint16_t subifd_count = 0;
  toff_t* subifd_offsets = nullptr;
  if (TIFFGetField(tiff.get(), TIFFTAG_SUBIFD, &subifd_count, &subifd_offsets) != 1 ||
      subifd_count != 1 || !subifd_offsets) {
    throw std::runtime_error("Adobe gain-map TIFF requires exactly one gain-map SubIFD");
  }
  const toff_t gain_map_offset = subifd_offsets[0];
  if (pixels) {
    result.base.resize(static_cast<size_t>(info.width) * info.height * 3u);
    for (uint32_t y = 0; y < info.height; ++y) {
      if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
      if (TIFFReadScanline(tiff.get(), result.base.data() + static_cast<size_t>(y) * info.width * 3u, y, 0) < 0) {
        throw std::runtime_error("Adobe gain-map TIFF base scanline decode failed");
      }
    }
  }
  if (!TIFFSetSubDirectory(tiff.get(), gain_map_offset)) {
    throw std::runtime_error("cannot open Adobe gain-map TIFF SubIFD");
  }
  uint32_t map_width = 0, map_height = 0;
  uint16_t map_bits = 0, map_samples = 0, map_format = SAMPLEFORMAT_UINT;
  uint16_t map_photo = 0, map_planar = 0;
  uint32_t subfile_type = 0;
  TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &map_width);
  TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &map_height);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &map_bits);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &map_samples);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &map_format);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PHOTOMETRIC, &map_photo);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &map_planar);
  TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SUBFILETYPE, &subfile_type);
  if (!map_width || !map_height || map_bits != 16 || map_samples != 3 ||
      map_format != SAMPLEFORMAT_UINT || map_photo != kTiffGainMapPhotometric ||
      map_planar != PLANARCONFIG_CONTIG || (subfile_type & kTiffGainMapSubfileType) == 0) {
    throw std::runtime_error("Adobe gain-map TIFF SubIFD has an unsupported pixel layout");
  }
  info.gain_map_width = map_width;
  info.gain_map_height = map_height;
  info.gain_map_channels = map_samples;
  info.gain_map_scale_x = info.base_width / static_cast<double>(map_width);
  info.gain_map_scale_y = info.base_height / static_cast<double>(map_height);
  uint32_t metadata_size = 0;
  void* metadata_data = nullptr;
  if (TIFFGetField(tiff.get(), kTiffGainMapMetadataTag, &metadata_size, &metadata_data) != 1) {
    throw std::runtime_error("Adobe gain-map TIFF ISO metadata tag is missing");
  }
  result.metadata = parse_tiff_gain_map_metadata(metadata_data, metadata_size);
  info.base_hdr_headroom = result.metadata.base_hdr_headroom;
  info.alternate_hdr_headroom = result.metadata.alternate_hdr_headroom;
  info.hdr_capacity_min = std::exp2(info.base_hdr_headroom);
  info.hdr_capacity_max = std::exp2(info.alternate_hdr_headroom);
  info.gain_map_uses_base_color_space = result.metadata.use_base_color_space;
  info.gain_map_min = result.metadata.minimum;
  info.gain_map_max = result.metadata.maximum;
  info.gain_map_gamma = result.metadata.gamma;
  info.base_offset = result.metadata.base_offset;
  info.alternate_offset = result.metadata.alternate_offset;
  if (!result.metadata.use_base_color_space) {
    throw std::runtime_error("TIFF gain map using alternate color space is not safely reconstructable without its profile");
  }
  if (pixels) {
    result.map.resize(static_cast<size_t>(map_width) * map_height * 3u);
    for (uint32_t y = 0; y < map_height; ++y) {
      if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
      if (TIFFReadScanline(tiff.get(), result.map.data() + static_cast<size_t>(y) * map_width * 3u, y, 0) < 0) {
        throw std::runtime_error("Adobe gain-map TIFF SubIFD scanline decode failed");
      }
    }
  }
  if (info.original_orientation >= 5) {
    std::swap(info.width, info.height);
    std::swap(info.base_width, info.base_height);
    info.gain_map_scale_x = info.base_width / static_cast<double>(map_width);
    info.gain_map_scale_y = info.base_height / static_cast<double>(map_height);
  }
  info.orientation_normalized = true;
  return result;
}

double sample_map_bilinear(const std::vector<uint16_t>& map, uint32_t map_width,
                           uint32_t map_height, uint32_t base_width,
                           uint32_t base_height, uint32_t x, uint32_t y,
                           size_t channel) {
  if (map_width == base_width && map_height == base_height) {
    return map[(static_cast<size_t>(y) * map_width + x) * 3u + channel] / 65535.0;
  }
  const double mx = (static_cast<double>(x) + 0.5) * map_width / base_width - 0.5;
  const double my = (static_cast<double>(y) + 0.5) * map_height / base_height - 0.5;
  const int x0 = std::clamp(static_cast<int>(std::floor(mx)), 0, static_cast<int>(map_width) - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor(my)), 0, static_cast<int>(map_height) - 1);
  const int x1 = std::min(x0 + 1, static_cast<int>(map_width) - 1);
  const int y1 = std::min(y0 + 1, static_cast<int>(map_height) - 1);
  const double fx = std::clamp(mx - std::floor(mx), 0.0, 1.0);
  const double fy = std::clamp(my - std::floor(my), 0.0, 1.0);
  const auto at = [&](int px, int py) {
    return map[(static_cast<size_t>(py) * map_width + static_cast<size_t>(px)) * 3u + channel] / 65535.0;
  };
  return (at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx) * (1.0 - fy) +
         (at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx) * fy;
}

}  // namespace

bool is_adobe_gainmap_tiff(const std::filesystem::path& path) {
  try {
    (void)read_tiff_gain_map(path, false, nullptr);
    return true;
  } catch (...) {
    return false;
  }
}

SourceInfo inspect_adobe_gainmap_tiff(const std::filesystem::path& path) {
  return read_tiff_gain_map(path, false, nullptr).info;
}

ReconstructedHdr reconstruct_adobe_gainmap_tiff(
    const std::filesystem::path& path, std::atomic_bool* cancel) {
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  const auto decode_start = Clock::now();
  TiffGainMapData tiff = read_tiff_gain_map(path, true, cancel);
  ReconstructedHdr output;
  output.info = tiff.info;
  output.exif = std::move(tiff.exif);
  output.xmp = std::move(tiff.xmp);
  output.decode_ms = elapsed_ms(decode_start);

  const uint32_t width = output.info.original_orientation >= 5
      ? output.info.height : output.info.width;
  const uint32_t height = output.info.original_orientation >= 5
      ? output.info.width : output.info.height;
  if (tiff.base.size() != static_cast<size_t>(width) * height * 3u) {
    throw std::runtime_error("Adobe gain-map TIFF base dimensions are inconsistent");
  }
  const auto reconstruction_start = Clock::now();
  output.rec2020_pq_rgb.resize(tiff.base.size());
  const double base_headroom = tiff.metadata.base_hdr_headroom;
  const double alternate_headroom = tiff.metadata.alternate_hdr_headroom;
  const double full_headroom = std::max(base_headroom, alternate_headroom);
  double weight = 0.0;
  if (alternate_headroom != base_headroom) {
    weight = std::clamp((full_headroom - base_headroom) /
                            (alternate_headroom - base_headroom), 0.0, 1.0);
    if (alternate_headroom < base_headroom) weight = -weight;
  }
  double maximum_linear = 0.0;
  long double sum_max_linear = 0.0;
  for (uint32_t y = 0; y < height; ++y) {
    if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
    for (uint32_t x = 0; x < width; ++x) {
      const size_t pixel = static_cast<size_t>(y) * width + x;
      std::array<double, 3> base_linear_rgb{};
      for (size_t channel = 0; channel < 3; ++channel) {
        const float encoded_base = tiff.base[pixel * 3u + channel] / 65535.0f;
        const double base_linear = cmsEvalToneCurveFloat(tiff.base_curves[channel], encoded_base);
        const double encoded_map = sample_map_bilinear(
            tiff.map, output.info.gain_map_width, output.info.gain_map_height,
            width, height, x, y, channel);
        const double log_gain = tiff.metadata.minimum[channel] +
            (tiff.metadata.maximum[channel] - tiff.metadata.minimum[channel]) *
                std::pow(encoded_map, 1.0 / tiff.metadata.gamma[channel]);
        base_linear_rgb[channel] =
            (base_linear + tiff.metadata.base_offset[channel]) *
                std::exp2(log_gain * weight) -
            tiff.metadata.alternate_offset[channel];
      }
      const auto rec2020 = rec2020_from_gainmap_base(base_linear_rgb, output.info.primaries);
      const double pixel_max = std::max({rec2020[0], rec2020[1], rec2020[2], 0.0});
      maximum_linear = std::max(maximum_linear, pixel_max);
      sum_max_linear += pixel_max;
      for (size_t channel = 0; channel < 3; ++channel) {
        output.rec2020_pq_rgb[pixel * 3u + channel] = static_cast<uint16_t>(std::llround(
            forward_pq(std::max(rec2020[channel], 0.0) * 203.0) * 65535.0));
      }
    }
  }
  output.max_cll = static_cast<uint16_t>(std::clamp(
      std::llround(maximum_linear * 203.0), 0ll, 65535ll));
  const double average_linear = static_cast<double>(
      sum_max_linear / static_cast<long double>(static_cast<uint64_t>(width) * height));
  output.max_pall = static_cast<uint16_t>(std::clamp(
      std::llround(average_linear * 203.0), 0ll, 65535ll));
  output.reconstruction_ms = elapsed_ms(reconstruction_start);

  const auto orientation_start = Clock::now();
  const auto transformed = orientation::normalize_rgb16(
      output.rec2020_pq_rgb, width, height, output.info.original_orientation);
  output.info.width = transformed.width;
  output.info.height = transformed.height;
  output.info.orientation_normalized = true;
  orientation::set_xmp_orientation_to_one(output.xmp);
  output.orientation_ms = elapsed_ms(orientation_start);
  if (cancel && cancel->load()) throw std::runtime_error("conversion cancelled");
  return output;
}

GainMapRaster extract_tiff_gainmap(const std::filesystem::path& path,
                                   std::atomic_bool* cancel) {
  const auto begin = Clock::now();
  auto tiff = read_tiff_gain_map(path, true, cancel);
  const uint32_t map_width = tiff.info.gain_map_width;
  const uint32_t map_height = tiff.info.gain_map_height;
  const uint32_t map_channels = tiff.info.gain_map_channels;
  const uint8_t orientation = tiff.info.original_orientation;
  auto result = make_raster(std::move(tiff.info), map_width, map_height,
                            map_channels, std::move(tiff.map),
                            "TIFF gain-map SubIFD", cancel);
  normalize_gainmap_orientation(result, orientation);
  result.decode_ms = elapsed_ms(begin);
  return result;
}

}  // namespace hdrbridge::gainmap
