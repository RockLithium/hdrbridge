#!/usr/bin/env bash
set -euo pipefail

web_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${web_root}/.." && pwd)"
cache_root="${web_root}/.cache"
vcpkg_root="${cache_root}/vcpkg"
install_root="${cache_root}/vcpkg_installed"
prefix="${install_root}/wasm32-emscripten"
build_root="${cache_root}/build/hdrbridge-full"
output_root="${web_root}/public/codecs/hdrbridge"
vcpkg_ref="ddd110b8a05cda14b8f1b0333a1d80c4fb6f16cd"

command -v emcmake >/dev/null || { echo "Emscripten is not active" >&2; exit 1; }
command -v ninja >/dev/null || { echo "Ninja is required" >&2; exit 1; }

mkdir -p "${cache_root}" "${output_root}" "${web_root}/public/licenses"
if [[ ! -d "${vcpkg_root}/.git" ]]; then
  git clone https://github.com/microsoft/vcpkg.git "${vcpkg_root}"
fi
git -C "${vcpkg_root}" fetch --quiet origin "${vcpkg_ref}"
git -C "${vcpkg_root}" checkout --quiet "${vcpkg_ref}"
"${vcpkg_root}/bootstrap-vcpkg.sh" -disableMetrics

"${vcpkg_root}/vcpkg" install \
  'libheif[core,aom]:wasm32-emscripten' 'libavif[core,aom]:wasm32-emscripten' \
  'libpng:wasm32-emscripten' 'tiff[core,jpeg,zip]:wasm32-emscripten' \
  'lcms:wasm32-emscripten' 'libjxl:wasm32-emscripten' \
  'jxrlib:wasm32-emscripten' 'nlohmann-json:wasm32-emscripten' \
  --classic "--x-install-root=${install_root}" \
  "--overlay-ports=${web_root}/vcpkg-overlay"

emcmake cmake -S "${web_root}/wasm" -B "${build_root}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=${prefix}" \
  "-DCMAKE_FIND_ROOT_PATH=${prefix}" \
  "-Dlibheif_DIR=${prefix}/share/libheif" \
  "-Dlibavif_DIR=${prefix}/share/libavif" \
  "-Dnlohmann_json_DIR=${prefix}/share/nlohmann_json" \
  "-Dlcms2_DIR=${prefix}/share/lcms2" \
  "-DHDRBRIDGE_VCPKG_PREFIX=${prefix}"
cmake --build "${build_root}" --parallel

cp "${build_root}/dist/hdrbridge-core.mjs" "${output_root}/"
cp "${build_root}/dist/hdrbridge-core.wasm" "${output_root}/"
cp "${repo_root}/THIRD_PARTY_NOTICES.md" "${web_root}/public/licenses/"
du -h "${output_root}/hdrbridge-core.wasm"
