#include "hdrbridge_core.h"

#include <atomic>
#include <exception>
#include <iostream>
#include <string>

namespace {
void usage() {
  std::cerr << "HDR Bridge CLI\n"
            << "  hdrbridge-cli inspect <HDR input>\n"
            << "  hdrbridge-cli convert <HDR input> <output> [--mode=jxl-pq16|jxr-scrgb-fp16|ultrahdr|png-pq16|tiff-pq16|avif-pq10|jxr-rgb10-experimental]\n"
            << "      [--gamut=rec2020|p3] [--transfer=pq|hlg] [--target-peak=auto|203..10000] [--quality=1..100]\n"
            << "      [--base-quality=0..100] [--gainmap-quality=0..100] [--gainmap-scale=1|2|4]\n"
            << "      [--single-channel-gainmap|--rgb-gainmap] [--no-icc] [--png-icc-name=<description>] [--png-compression=1..9]\n"
            << "      [--tiff-compression=1..9] [--lossy] [--overwrite]\n"
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
      for (int i = 4; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--lossy") options.lossless = false;
        else if (flag == "--overwrite") options.overwrite = true;
        else if (flag == "--single-channel-gainmap") options.multi_channel_gainmap = false;
        else if (flag == "--rgb-gainmap") options.multi_channel_gainmap = true;
        else if (flag == "--no-icc") options.embed_hdr_icc = false;
        else if (flag.rfind("--mode=", 0) == 0) options.mode = flag.substr(7);
        else if (flag.rfind("--gamut=", 0) == 0) options.output_gamut = flag.substr(8);
        else if (flag.rfind("--transfer=", 0) == 0) options.output_transfer = flag.substr(11);
        else if (flag.rfind("--target-peak=", 0) == 0) {
          const auto value = flag.substr(14);
          options.target_peak_nits = value == "auto" ? 0.0f : std::stof(value);
        }
        else if (flag.rfind("--quality=", 0) == 0) options.image_quality = std::stoi(flag.substr(10)) / 100.0f;
        else if (flag.rfind("--base-quality=", 0) == 0) options.base_quality = std::stoi(flag.substr(15));
        else if (flag.rfind("--gainmap-quality=", 0) == 0) options.gainmap_quality = std::stoi(flag.substr(18));
        else if (flag.rfind("--gainmap-scale=", 0) == 0) options.gainmap_scale = std::stoi(flag.substr(16));
        else if (flag.rfind("--png-icc-name=", 0) == 0) options.png_icc_name_override = flag.substr(15);
        else if (flag.rfind("--png-compression=", 0) == 0) options.png_compression_level = std::stoi(flag.substr(18));
        else if (flag.rfind("--tiff-compression=", 0) == 0) options.tiff_compression_level = std::stoi(flag.substr(19));
        else throw std::runtime_error("unknown option: " + flag);
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
