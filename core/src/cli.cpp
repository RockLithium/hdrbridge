#include "hdrbridge_core.h"

#include <atomic>
#include <charconv>
#include <exception>
#include <iostream>
#include <string>

namespace {
void usage() {
  std::cerr << "HDR Bridge CLI\n"
            << "  hdrbridge-cli inspect <HDR input>\n"
            << "  hdrbridge-cli convert <HDR input> <output> [--mode=jxl-pq16|jxr-scrgb-fp16|ultrahdr|png-pq16|tiff-pq16|avif-pq10|gainmap-extract]\n"
            << "      [--gamut=rec2020|p3] [--transfer=pq|hlg] [--quality=1..100]\n"
            << "      [--representation=direct|gainmap] [--effort=1..10 (JPEG XL)] [--avif-speed=0..9]\n"
            << "      [--base-quality=0..100] [--gainmap-quality=0..100] [--gainmap-scale=1|2|4]\n"
            << "      [--single-channel-gainmap|--rgb-gainmap] [--no-icc] [--png-icc-name=<description>] [--png-compression=1..9]\n"
            << "      [--tiff-compression=1..9|--tiff-uncompressed] [--diagnostic-icc-only] [--lossy] [--overwrite]\n"
            << "      [--mode=photo-frame --frame-format=jpeg|png --frame-font=<path> --frame-caption1=<text> --frame-caption2=<text>]\n"
            << "  hdrbridge-cli verify <output> <mode>\n"
            << "  hdrbridge-cli benchmark-cache <input> <output-a> <mode-a> <output-b> <mode-b>\n";
}
}

int main(int argc, char** argv) {
  try {
    if (argc < 3) { usage(); return 2; }
    const std::string command = argv[1];
    if (command == "inspect") {
      std::cout << hdrbridge::to_json(hdrbridge::inspect(argv[2])) << '\n';
      return 0;
    }
    if (command == "verify" && argc >= 4) {
      const auto result = hdrbridge::verify(argv[2], argv[3]);
      std::cout << hdrbridge::to_json(result) << '\n';
      return result.passed ? 0 : 1;
    }
    if (command == "benchmark-cache" && argc >= 7) {
      hdrbridge::ConversionOptions first_options;
      first_options.mode = argv[4];
      first_options.overwrite = true;
      const auto first = hdrbridge::convert(argv[2], argv[3], first_options);
      hdrbridge::ConversionOptions second_options;
      second_options.mode = argv[6];
      second_options.overwrite = true;
      const auto second = hdrbridge::convert(argv[2], argv[5], second_options);
      std::cout << "[" << hdrbridge::to_json(first) << ",\n"
                << hdrbridge::to_json(second) << "]\n";
      return first.success && second.success ? 0 : 1;
    }
    if (command == "convert" && argc >= 4) {
      hdrbridge::ConversionOptions options;
      bool gamut_explicit = false;
      for (int i = 4; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--lossy") options.lossless = false;
        else if (flag == "--overwrite") options.overwrite = true;
        else if (flag == "--single-channel-gainmap") options.multi_channel_gainmap = false;
        else if (flag == "--rgb-gainmap") options.multi_channel_gainmap = true;
        else if (flag == "--no-icc") options.embed_hdr_icc = false;
        else if (flag.rfind("--mode=", 0) == 0) options.mode = flag.substr(7);
        else if (flag.rfind("--frame-format=", 0) == 0) options.frame_format = flag.substr(15);
        else if (flag.rfind("--frame-font=", 0) == 0) options.frame_font_path = flag.substr(13);
        else if (flag.rfind("--frame-caption1=", 0) == 0) options.frame_caption1 = flag.substr(17);
        else if (flag.rfind("--frame-caption2=", 0) == 0) options.frame_caption2 = flag.substr(17);
        else if (flag.rfind("--gainmap-format=", 0) == 0) options.gainmap_export_format = flag.substr(17);
        else if (flag.rfind("--gamut=", 0) == 0) {
          options.output_gamut = flag.substr(8);
          gamut_explicit = true;
          if (options.output_gamut != "rec2020" && options.output_gamut != "p3") {
            throw std::runtime_error("--gamut expects rec2020 or p3");
          }
        }
        else if (flag.rfind("--transfer=", 0) == 0) options.output_transfer = flag.substr(11);
        else if (flag.rfind("--representation=", 0) == 0) options.output_representation = flag.substr(17);
        else if (flag.rfind("--quality=", 0) == 0) options.image_quality = std::stoi(flag.substr(10)) / 100.0f;
        else if (flag.rfind("--effort=", 0) == 0) {
          const char* end = flag.data() + flag.size();
          const auto parsed = std::from_chars(flag.data() + 9, end, options.effort);
          if (parsed.ec != std::errc{} || parsed.ptr != end ||
              options.effort < 1 || options.effort > 10) {
            throw std::runtime_error("--effort expects an integer from 1 to 10");
          }
        }
        else if (flag.rfind("--avif-speed=", 0) == 0) {
          const char* end = flag.data() + flag.size();
          const auto parsed = std::from_chars(flag.data() + 13, end, options.avif_speed);
          if (parsed.ec != std::errc{} || parsed.ptr != end ||
              options.avif_speed < 0 || options.avif_speed > 9) {
            throw std::runtime_error("--avif-speed expects an integer from 0 to 9");
          }
        }
        else if (flag.rfind("--base-quality=", 0) == 0) options.base_quality = std::stoi(flag.substr(15));
        else if (flag.rfind("--gainmap-quality=", 0) == 0) options.gainmap_quality = std::stoi(flag.substr(18));
        else if (flag.rfind("--gainmap-scale=", 0) == 0) options.gainmap_scale = std::stoi(flag.substr(16));
        else if (flag.rfind("--png-icc-name=", 0) == 0) options.png_icc_name_override = flag.substr(15);
        else if (flag.rfind("--png-compression=", 0) == 0) options.png_compression_level = std::stoi(flag.substr(18));
        else if (flag.rfind("--tiff-compression=", 0) == 0) options.tiff_compression_level = std::stoi(flag.substr(19));
        else if (flag == "--tiff-uncompressed") options.tiff_compressed = false;
        else if (flag == "--diagnostic-icc-only") options.diagnostic_icc_only = true;
        else if (flag.rfind("--diagnostic-icc-profile=", 0) == 0)
          options.diagnostic_icc_profile_path = flag.substr(25);
        else if (flag.rfind("--diagnostic-tiff-metadata=", 0) == 0)
          options.diagnostic_tiff_metadata_source = flag.substr(27);
        else if (flag == "--diagnostic-windows-compatible-icc")
          options.diagnostic_windows_compatible_icc = true;
        else throw std::runtime_error("unknown option: " + flag);
      }
      const bool p3_default = options.mode == "ultrahdr" ||
          (options.mode == "photo-frame" && options.frame_format == "jpeg") ||
          options.output_representation == "gainmap";
      if (!gamut_explicit) {
        options.output_gamut = p3_default ? "p3" : "rec2020";
      }
      std::atomic_bool cancel{false};
      const auto result = hdrbridge::convert(argv[2], argv[3], options,
          [](int value, const std::string& stage) { std::cerr << value << "% " << stage << '\n'; }, &cancel);
      std::cout << hdrbridge::to_json(result) << '\n';
      return result.success ? 0 : 1;
    }
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << '\n';
    return 1;
  }
}
