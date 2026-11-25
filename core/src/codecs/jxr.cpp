#include "codecs/jxr.h"
#include "color/pq.h"
#include "color/half_float.h"
#include "infrastructure/files.h"
#include "infrastructure/pixel_workers.h"
#include "metadata/source_metadata.h"
#include "image/orientation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include "infrastructure/wic.h"
#else
extern "C" {
#include <JXRGlue.h>
}
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace hdrbridge::detail {

#ifdef _WIN32
static bool wic_metadata_query_present(IWICMetadataQueryReader* reader, const wchar_t* query,
                                PROPVARIANT* value_out = nullptr) {
  if (!reader) return false;
  PROPVARIANT value{};
  PropVariantInit(&value);
  const HRESULT result = reader->GetMetadataByName(query, &value);
  if (FAILED(result)) {
    PropVariantClear(&value);
    return false;
  }
  if (value_out) {
    *value_out = value;
  } else {
    PropVariantClear(&value);
  }
  return true;
}

void encode_jxr(const std::filesystem::path& path, uint32_t width, uint32_t height,
                const std::vector<uint16_t>& rgba_half, const ConversionOptions& options) {
  ComScope com;
  auto factory = wic_factory();
  ComPtr<IWICStream> stream;
  require_hr(factory->CreateStream(&stream), "create WIC stream");
  require_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "open JXR output");
  ComPtr<IWICBitmapEncoder> encoder;
  require_hr(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder), "create JPEG XR encoder");
  require_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "initialize JPEG XR encoder");
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  require_hr(encoder->CreateNewFrame(&frame, &properties), "create JPEG XR frame");
  if (properties) {
    std::array<PROPBAG2, 2> prop{};
    prop[0].pstrName = const_cast<wchar_t*>(L"Lossless");
    prop[1].pstrName = const_cast<wchar_t*>(L"ImageQuality");
    std::array<VARIANT, 2> values{};
    VariantInit(&values[0]); VariantInit(&values[1]);
    values[0].vt = VT_BOOL; values[0].boolVal = options.lossless ? VARIANT_TRUE : VARIANT_FALSE;
    values[1].vt = VT_R4; values[1].fltVal = std::clamp(options.image_quality, 0.0f, 1.0f);
    require_hr(properties->Write(static_cast<ULONG>(prop.size()), prop.data(), values.data()), "set JPEG XR properties");
  }
  require_hr(frame->Initialize(properties.Get()), "initialize JPEG XR frame");
  require_hr(frame->SetSize(width, height), "set JPEG XR dimensions");
  WICPixelFormatGUID format = GUID_WICPixelFormat64bppRGBAHalf;
  require_hr(frame->SetPixelFormat(&format), "set JPEG XR FP16 pixel format");
  if (format != GUID_WICPixelFormat64bppRGBAHalf) throw std::runtime_error("WIC JPEG XR encoder rejected FP16 RGBA");
  const UINT stride = width * 8u;
  const UINT bytes = static_cast<UINT>(rgba_half.size() * sizeof(uint16_t));
  require_hr(frame->WritePixels(height, stride, bytes, reinterpret_cast<BYTE*>(const_cast<uint16_t*>(rgba_half.data()))), "write JPEG XR FP16 pixels");
  require_hr(frame->Commit(), "commit JPEG XR frame");
  require_hr(encoder->Commit(), "commit JPEG XR file");
}

Verification decode_jxr(const std::filesystem::path& path, const std::vector<uint16_t>* expected) {
  ComScope com;
  auto factory = wic_factory();
  ComPtr<IWICBitmapDecoder> decoder;
  require_hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnDemand, &decoder), "open JPEG XR decoder");
  ComPtr<IWICBitmapFrameDecode> frame;
  require_hr(decoder->GetFrame(0, &frame), "read JPEG XR frame");
  Verification v;
  require_hr(frame->GetSize(&v.width, &v.height), "read JPEG XR dimensions");
  WICPixelFormatGUID format{};
  require_hr(frame->GetPixelFormat(&format), "read JPEG XR pixel format");
  if (format != GUID_WICPixelFormat64bppRGBAHalf) {
    v.pixel_format = "unexpected WIC pixel format";
    v.checks.push_back("decoder did not preserve GUID_WICPixelFormat64bppRGBAHalf");
    return v;
  }
  v.pixel_format = "GUID_WICPixelFormat64bppRGBAHalf";
  v.bit_depth = 16;
  const size_t count = static_cast<size_t>(v.width) * v.height * 4u;
  std::vector<uint16_t> pixels(count);
  const UINT stride = v.width * 8u;
  require_hr(frame->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size() * 2u), reinterpret_cast<BYTE*>(pixels.data())), "decode JPEG XR FP16 pixels");
  v.min_value = std::numeric_limits<double>::infinity();
  v.max_value = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < pixels.size(); i += 4u) {
    for (size_t c = 0; c < 3; ++c) {
      const float value = half_to_float(pixels[i + c]);
      if (!std::isfinite(value)) v.finite = false;
      v.min_value = std::min(v.min_value, static_cast<double>(value));
      v.max_value = std::max(v.max_value, static_cast<double>(value));
    }
  }
  if (expected && expected->size() == pixels.size()) {
    // The Windows JPEG XR codec canonicalizes IEEE-754 negative zero to
    // positive zero. They are numerically identical; every non-zero FP16 code
    // must remain bit-exact for the lossless claim.
    v.exact_roundtrip = std::equal(expected->begin(), expected->end(), pixels.begin(),
        [](uint16_t a, uint16_t b) {
          return a == b || ((a & 0x7fffu) == 0 && (b & 0x7fffu) == 0);
        });
  }
  v.color_encoding = "linear scRGB; 1.0 = 80 cd/m2; sRGB/BT.709 primaries";
  v.checks.push_back("WIC JPEG XR decoder opened output");
  v.checks.push_back("FP16 RGBA WIC pixel format preserved");
  v.checks.push_back(v.finite ? "all decoded RGB values finite" : "non-finite decoded value");
  v.checks.push_back(v.max_value > 1.0 ? "HDR values exceed SDR white" : "HDR range clipped to SDR white");
  if (expected) v.checks.push_back(v.exact_roundtrip
      ? "FP16 lossless roundtrip (allowing IEEE-754 signed-zero canonicalization)"
      : "FP16 lossless roundtrip mismatch");
  v.passed = v.width > 0 && v.height > 0 && v.finite && v.max_value > 1.0 && (!expected || v.exact_roundtrip);
  return v;
}

DecodedImage decode_jxr_input(const std::filesystem::path& path) {
  ComScope com;
  auto factory = wic_factory();
  ComPtr<IWICBitmapDecoder> decoder;
  require_hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnDemand, &decoder), "open JPEG XR input");
  ComPtr<IWICBitmapFrameDecode> frame;
  require_hr(decoder->GetFrame(0, &frame), "read JPEG XR input frame");
  DecodedImage decoded;
  decoded.info.path = path; decoded.info.format = "JPEG XR"; decoded.info.container_brand = "JXR ";
  decoded.info.asset_kind = "non-HDR"; decoded.info.codec = "JPEG XR";
  require_hr(frame->GetSize(&decoded.info.width, &decoded.info.height), "read JPEG XR input dimensions");
  WICPixelFormatGUID format{};
  require_hr(frame->GetPixelFormat(&format), "read JPEG XR input pixel format");
  UINT color_context_count = 0;
  const HRESULT color_context_result = frame->GetColorContexts(0, nullptr, &color_context_count);
  decoded.info.icc_present = SUCCEEDED(color_context_result) && color_context_count > 0;
  decoded.info.icc_status = FAILED(color_context_result)
      ? "unsupported" : metadata_status(decoded.info.icc_present);
  ComPtr<IWICMetadataQueryReader> metadata_reader;
  const HRESULT metadata_result = frame->GetMetadataQueryReader(&metadata_reader);
  if (metadata_result == WINCODEC_ERR_UNSUPPORTEDOPERATION || metadata_result == E_NOINTERFACE) {
    decoded.info.exif_status = "unsupported";
    decoded.info.xmp_status = "unsupported";
    decoded.info.orientation_status = "unsupported";
  } else if (FAILED(metadata_result)) {
    decoded.info.exif_status = "read-error";
    decoded.info.xmp_status = "read-error";
    decoded.info.orientation_status = "read-error";
  } else {
    decoded.info.exif_present =
        wic_metadata_query_present(metadata_reader.Get(), L"/ifd/{ushort=34665}") ||
        wic_metadata_query_present(metadata_reader.Get(), L"/ifd/exif/{ushort=36867}");
    decoded.info.xmp_present =
        wic_metadata_query_present(metadata_reader.Get(), L"/ifd/{ushort=700}");
    decoded.info.exif_status = metadata_status(decoded.info.exif_present);
    decoded.info.xmp_status = metadata_status(decoded.info.xmp_present);
    PROPVARIANT orientation_value{};
    if (wic_metadata_query_present(metadata_reader.Get(), L"/ifd/{ushort=274}",
                                   &orientation_value)) {
      if (orientation_value.vt == VT_UI2) decoded.info.original_orientation =
          static_cast<uint8_t>(std::clamp<unsigned>(orientation_value.uiVal, 1, 8));
      else if (orientation_value.vt == VT_UI4) decoded.info.original_orientation =
          static_cast<uint8_t>(std::clamp<unsigned long>(orientation_value.ulVal, 1, 8));
      decoded.info.orientation_status = "present";
      PropVariantClear(&orientation_value);
    } else {
      decoded.info.orientation_status = "absent";
    }
  }
  const size_t count = static_cast<size_t>(decoded.info.width) * decoded.info.height;
  if (format == GUID_WICPixelFormat64bppRGBAHalf) {
    decoded.info.asset_kind = "direct-hdr";
    decoded.rgb.resize(count * 3u);
    decoded.alpha.resize(count);
    std::vector<uint16_t> rgba(count * 4u);
    require_hr(frame->CopyPixels(nullptr, decoded.info.width * 8u, static_cast<UINT>(rgba.size() * sizeof(uint16_t)),
                                 reinterpret_cast<BYTE*>(rgba.data())), "decode JPEG XR FP16 input");
    parallel_pixel_blocks(count, [&](size_t begin, size_t end) {
    for (size_t p = begin; p < end; ++p) {
      const double r = static_cast<double>(half_to_float(rgba[p * 4u])) * 80.0;
      const double g = static_cast<double>(half_to_float(rgba[p * 4u + 1u])) * 80.0;
      const double b = static_cast<double>(half_to_float(rgba[p * 4u + 2u])) * 80.0;
      const std::array<double, 3> rec2020{
          0.6274040 * r + 0.3292830 * g + 0.0433130 * b,
          0.0690970 * r + 0.9195400 * g + 0.0113620 * b,
          0.0163910 * r + 0.0880130 * g + 0.8955950 * b};
      for (size_t c = 0; c < 3; ++c) decoded.rgb[p * 3u + c] = static_cast<uint16_t>(std::llround(
          forward_pq(std::clamp(rec2020[c], 0.0, 10000.0)) * 65535.0));
      decoded.alpha[p] = static_cast<uint16_t>(std::llround(
          std::clamp(static_cast<double>(half_to_float(rgba[p * 4u + 3u])),
                     0.0, 1.0) * 65535.0));
    }
    });
    if (std::all_of(decoded.alpha.begin(), decoded.alpha.end(),
                    [](uint16_t value) { return value == 65535u; })) {
      decoded.alpha.clear();
    }
    decoded.info.profile = "FP16 linear scRGB (1.0 = 80 cd/m2)";
    decoded.info.bit_depth = 16; decoded.info.chroma = "4:4:4 RGBA FP16";
    decoded.info.pixel_format = "GUID_WICPixelFormat64bppRGBAHalf";
    decoded.info.color_signal_kind = "WIC pixel format / scRGB convention";
    decoded.info.primaries = 1; decoded.info.transfer = 8;
    decoded.info.matrix = 0; decoded.info.full_range = true; decoded.info.range_known = true;
  } else if (format == GUID_WICPixelFormat8bppGray ||
             format == GUID_WICPixelFormat16bppGray ||
             format == GUID_WICPixelFormat24bppBGR ||
             format == GUID_WICPixelFormat32bppBGR ||
             format == GUID_WICPixelFormat32bppBGRA ||
             format == GUID_WICPixelFormat48bppRGB ||
             format == GUID_WICPixelFormat64bppRGBA) {
    decoded.info.profile = "SDR JPEG XR";
    decoded.info.bit_depth = format == GUID_WICPixelFormat16bppGray ||
                             format == GUID_WICPixelFormat48bppRGB ||
                             format == GUID_WICPixelFormat64bppRGBA ? 16u : 8u;
    decoded.info.chroma = format == GUID_WICPixelFormat8bppGray ||
                          format == GUID_WICPixelFormat16bppGray
        ? "grayscale" : "RGB/RGBA";
    decoded.info.pixel_format = "SDR integer JPEG XR";
    decoded.info.color_signal_kind = "No validated HDR representation";
  } else {
    throw std::runtime_error("JPEG XR input is not a validated FP16 scRGB image");
  }
  return decoded;
}
#else
static void require_jxr(ERR error, const char* operation) {
  if (error != WMP_errSuccess) {
    throw std::runtime_error(std::string(operation) + " failed (JPEG XR error " +
                             std::to_string(error) + ")");
  }
}

void encode_jxr(const std::filesystem::path& path, uint32_t width, uint32_t height,
                const std::vector<uint16_t>& rgba_half,
                const ConversionOptions& options) {
  PKFactory* factory = nullptr;
  PKCodecFactory* codecs = nullptr;
  PKImageEncode* encoder = nullptr;
  WMPStream* stream = nullptr;
  auto cleanup = [&] {
    if (encoder) encoder->Release(&encoder);
    if (codecs) codecs->Release(&codecs);
    if (factory) factory->Release(&factory);
  };
  try {
    require_jxr(PKCreateFactory(&factory, PK_SDK_VERSION), "create JPEG XR stream factory");
    require_jxr(factory->CreateStreamFromFilename(&stream, path.string().c_str(), "wb"),
                "open JPEG XR output");
    require_jxr(PKCreateCodecFactory(&codecs, WMP_SDK_VERSION),
                "create JPEG XR codec factory");
    require_jxr(codecs->CreateCodec(&IID_PKImageWmpEncode,
                                    reinterpret_cast<void**>(&encoder)),
                "create JPEG XR encoder");

    CWMIStrCodecParam parameters{};
    parameters.cfColorFormat = YUV_444;
    parameters.bdBitDepth = BD_LONG;
    parameters.bfBitstreamFormat = FREQUENCY;
    parameters.bProgressiveMode = TRUE;
    parameters.olOverlap = options.lossless ? OL_NONE : OL_ONE;
    parameters.sbSubband = SB_ALL;
    parameters.uAlphaMode = 2;
    parameters.uiDefaultQPIndexAlpha = 1;
    if (options.lossless) {
      parameters.uiDefaultQPIndex = 1;
      parameters.uiDefaultQPIndexU = 1;
      parameters.uiDefaultQPIndexV = 1;
      parameters.uiDefaultQPIndexYHP = 1;
      parameters.uiDefaultQPIndexUHP = 1;
      parameters.uiDefaultQPIndexVHP = 1;
    } else {
      static constexpr int kQp16f[11][6] = {
        {148,177,171,165,187,191}, {133,155,153,147,172,181},
        {114,133,138,130,157,167}, {97,118,120,109,137,144},
        {76,98,103,85,115,121}, {63,86,91,62,96,99},
        {46,68,71,43,73,75}, {29,48,52,27,48,51},
        {16,30,35,14,29,34}, {8,14,17,7,13,17}, {3,5,7,3,5,6}};
      const float quality = std::clamp(options.image_quality, 0.0f, 1.0f) * 10.0f;
      const int lower = std::clamp(static_cast<int>(quality), 0, 10);
      const int upper = std::min(lower + 1, 10);
      const float fraction = quality - lower;
      std::array<U8*, 6> targets{
          &parameters.uiDefaultQPIndex, &parameters.uiDefaultQPIndexU,
          &parameters.uiDefaultQPIndexV, &parameters.uiDefaultQPIndexYHP,
          &parameters.uiDefaultQPIndexUHP, &parameters.uiDefaultQPIndexVHP};
      for (size_t index = 0; index < targets.size(); ++index) {
        *targets[index] = static_cast<U8>(std::lround(
            kQp16f[lower][index] * (1.0f - fraction) +
            kQp16f[upper][index] * fraction));
      }
    }
    require_jxr(encoder->Initialize(encoder, stream, &parameters,
                                    sizeof(parameters)),
                "initialize JPEG XR encoder");
    require_jxr(encoder->SetPixelFormat(encoder, GUID_PKPixelFormat64bppRGBAHalf),
                "set JPEG XR FP16 RGBA format");
    require_jxr(encoder->SetSize(encoder, static_cast<I32>(width),
                                static_cast<I32>(height)),
                "set JPEG XR dimensions");
    require_jxr(encoder->SetResolution(encoder, 96.0f, 96.0f),
                "set JPEG XR resolution");
    require_jxr(encoder->WritePixels(encoder, height,
                reinterpret_cast<U8*>(const_cast<uint16_t*>(rgba_half.data())),
                width * 8u), "write JPEG XR FP16 pixels");
    cleanup();
  } catch (...) {
    cleanup();
    throw;
  }
}

Verification decode_jxr(const std::filesystem::path& path,
                        const std::vector<uint16_t>* expected) {
  PKCodecFactory* codecs = nullptr;
  PKImageDecode* decoder = nullptr;
  PKFormatConverter* converter = nullptr;
  auto cleanup = [&] {
    if (converter) converter->Release(&converter);
    if (decoder) decoder->Release(&decoder);
    if (codecs) codecs->Release(&codecs);
  };
  try {
    require_jxr(PKCreateCodecFactory(&codecs, WMP_SDK_VERSION),
                "create JPEG XR decoder factory");
    require_jxr(codecs->CreateDecoderFromFile(path.string().c_str(), &decoder),
                "open JPEG XR input");
    require_jxr(codecs->CreateFormatConverter(&converter),
                "create JPEG XR format converter");
    char extension[] = ".jxr";
    require_jxr(converter->Initialize(converter, decoder, extension,
                                      GUID_PKPixelFormat64bppRGBAHalf),
                "configure JPEG XR FP16 decode");
    I32 width = 0, height = 0;
    require_jxr(converter->GetSize(converter, &width, &height),
                "read JPEG XR dimensions");
    if (width <= 0 || height <= 0) throw std::runtime_error("invalid JPEG XR dimensions");
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * 4u);
    const PKRect rect{0, 0, width, height};
    require_jxr(converter->Copy(converter, &rect,
                reinterpret_cast<U8*>(pixels.data()), static_cast<U32>(width) * 8u),
                "decode JPEG XR FP16 pixels");

    Verification result;
    result.width = static_cast<uint32_t>(width);
    result.height = static_cast<uint32_t>(height);
    result.bit_depth = 16;
    result.pixel_format = "64bpp RGBA half-float";
    result.color_encoding = "linear scRGB; 1.0 = 80 cd/m2; sRGB/BT.709 primaries";
    result.min_value = std::numeric_limits<double>::infinity();
    result.max_value = -std::numeric_limits<double>::infinity();
    for (size_t index = 0; index < pixels.size(); index += 4u) {
      for (size_t channel = 0; channel < 3; ++channel) {
        const float value = half_to_float(pixels[index + channel]);
        result.finite = result.finite && std::isfinite(value);
        result.min_value = std::min(result.min_value, static_cast<double>(value));
        result.max_value = std::max(result.max_value, static_cast<double>(value));
      }
    }
    if (expected && expected->size() == pixels.size()) {
      result.exact_roundtrip = std::equal(expected->begin(), expected->end(), pixels.begin(),
          [](uint16_t left, uint16_t right) {
            return left == right || ((left & 0x7fffu) == 0 && (right & 0x7fffu) == 0);
          });
    }
    result.checks.push_back("portable JPEG XR decoder opened FP16 output");
    result.checks.push_back(result.finite ? "all decoded RGB values finite" :
                                           "non-finite decoded value");
    result.checks.push_back(result.max_value > 1.0 ? "HDR values exceed SDR white" :
                                                   "HDR range clipped to SDR white");
    result.passed = result.finite && result.max_value > 1.0 &&
        (!expected || result.exact_roundtrip);
    cleanup();
    return result;
  } catch (...) {
    cleanup();
    throw;
  }
}

DecodedImage decode_jxr_input(const std::filesystem::path& path) {
  PKCodecFactory* codecs = nullptr;
  PKImageDecode* decoder = nullptr;
  PKFormatConverter* converter = nullptr;
  auto cleanup = [&] {
    if (converter) converter->Release(&converter);
    if (decoder) decoder->Release(&decoder);
    if (codecs) codecs->Release(&codecs);
  };
  try {
    require_jxr(PKCreateCodecFactory(&codecs, WMP_SDK_VERSION),
                "create JPEG XR decoder factory");
    require_jxr(codecs->CreateDecoderFromFile(path.string().c_str(), &decoder),
                "open JPEG XR input");
    PKPixelFormatGUID source_format{};
    require_jxr(decoder->GetPixelFormat(decoder, &source_format),
                "read JPEG XR pixel format");
    const bool fp16 = IsEqualGUID(source_format, GUID_PKPixelFormat64bppRGBAHalf) ||
                      IsEqualGUID(source_format, GUID_PKPixelFormat64bppRGBHalf) ||
                      IsEqualGUID(source_format, GUID_PKPixelFormat48bppRGBHalf);
    const bool common_sdr =
        IsEqualGUID(source_format, GUID_PKPixelFormat8bppGray) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat16bppGray) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat16bppRGB555) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat16bppRGB565) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat24bppBGR) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat24bppRGB) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat32bppBGR) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat32bppBGRA) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat32bppRGB) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat32bppRGBA) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat48bppRGB) ||
        IsEqualGUID(source_format, GUID_PKPixelFormat64bppRGBA);
    if (!fp16 && !common_sdr) {
      throw std::runtime_error("JPEG XR input is not a validated FP16 scRGB image");
    }
    if (common_sdr) {
      I32 width = 0, height = 0;
      require_jxr(decoder->GetSize(decoder, &width, &height),
                  "read SDR JPEG XR dimensions");
      if (width <= 0 || height <= 0) throw std::runtime_error("invalid JPEG XR dimensions");
      DecodedImage decoded;
      decoded.info.path = path;
      decoded.info.format = "JPEG XR";
      decoded.info.container_brand = "JXR ";
      decoded.info.asset_kind = "non-HDR";
      decoded.info.codec = "JPEG XR";
      decoded.info.width = static_cast<uint32_t>(width);
      decoded.info.height = static_cast<uint32_t>(height);
      decoded.info.profile = "SDR JPEG XR";
      decoded.info.bit_depth = IsEqualGUID(source_format, GUID_PKPixelFormat16bppGray) ||
                               IsEqualGUID(source_format, GUID_PKPixelFormat48bppRGB) ||
                               IsEqualGUID(source_format, GUID_PKPixelFormat64bppRGBA) ? 16u : 8u;
      decoded.info.chroma = IsEqualGUID(source_format, GUID_PKPixelFormat8bppGray) ||
                            IsEqualGUID(source_format, GUID_PKPixelFormat16bppGray)
          ? "grayscale" : "RGB/RGBA";
      decoded.info.pixel_format = "SDR integer JPEG XR";
      decoded.info.color_signal_kind = "No validated HDR representation";
      decoded.info.exif_status = "unsupported";
      decoded.info.xmp_status = "unsupported";
      decoded.info.icc_status = "unsupported";
      decoded.info.orientation_status = "unsupported";
      cleanup();
      return decoded;
    }
    require_jxr(codecs->CreateFormatConverter(&converter),
                "create JPEG XR input converter");
    char extension[] = ".jxr";
    require_jxr(converter->Initialize(converter, decoder, extension,
                                      GUID_PKPixelFormat64bppRGBAHalf),
                "configure JPEG XR input conversion");
    I32 width = 0, height = 0;
    require_jxr(converter->GetSize(converter, &width, &height),
                "read JPEG XR input dimensions");
    if (width <= 0 || height <= 0) throw std::runtime_error("invalid JPEG XR dimensions");
    std::vector<uint16_t> rgba(static_cast<size_t>(width) * height * 4u);
    const PKRect rect{0, 0, width, height};
    require_jxr(converter->Copy(converter, &rect,
                reinterpret_cast<U8*>(rgba.data()), static_cast<U32>(width) * 8u),
                "decode JPEG XR input pixels");

    DecodedImage decoded;
    decoded.info.path = path;
    decoded.info.format = "JPEG XR";
    decoded.info.container_brand = "JXR ";
    decoded.info.asset_kind = "direct-hdr";
    decoded.info.codec = "JPEG XR";
    decoded.info.width = static_cast<uint32_t>(width);
    decoded.info.height = static_cast<uint32_t>(height);
    decoded.info.profile = "FP16 linear scRGB (1.0 = 80 cd/m2)";
    decoded.info.bit_depth = 16;
    decoded.info.chroma = "4:4:4 RGBA FP16";
    decoded.info.pixel_format = "64bpp RGBA half-float";
    decoded.info.color_signal_kind = "JPEG XR FP16 pixel format / scRGB convention";
    decoded.info.primaries = 1;
    decoded.info.transfer = 8;
    decoded.info.matrix = 0;
    decoded.info.full_range = true;
    decoded.info.range_known = true;
    decoded.info.exif_status = "unsupported";
    decoded.info.xmp_status = "unsupported";
    decoded.info.icc_status = "unsupported";
    decoded.info.orientation_status = "unsupported";
    decoded.rgb.resize(static_cast<size_t>(width) * height * 3u);
    for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel) {
      const double red = static_cast<double>(half_to_float(rgba[pixel * 4u])) * 80.0;
      const double green = static_cast<double>(half_to_float(rgba[pixel * 4u + 1u])) * 80.0;
      const double blue = static_cast<double>(half_to_float(rgba[pixel * 4u + 2u])) * 80.0;
      const std::array<double, 3> rec2020{
          0.6274040 * red + 0.3292830 * green + 0.0433130 * blue,
          0.0690970 * red + 0.9195400 * green + 0.0113620 * blue,
          0.0163910 * red + 0.0880130 * green + 0.8955950 * blue};
      for (size_t channel = 0; channel < 3; ++channel) {
        decoded.rgb[pixel * 3u + channel] = static_cast<uint16_t>(std::llround(
            forward_pq(std::clamp(rec2020[channel], 0.0, 10000.0)) * 65535.0));
      }
    }
    cleanup();
    return decoded;
  } catch (...) {
    cleanup();
    throw;
  }
}
#endif

}  // namespace hdrbridge::detail
