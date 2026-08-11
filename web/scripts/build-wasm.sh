#!/usr/bin/env bash
set -euo pipefail

web_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${web_root}/.." && pwd)"
cache_root="${web_root}/.cache"
source_root="${cache_root}/src"
build_root="${cache_root}/build"
prefix_root="${cache_root}/prefix"
output_root="${web_root}/public/codecs/heif-png"

command -v emcmake >/dev/null || { echo "Emscripten is not active" >&2; exit 1; }
command -v ninja >/dev/null || { echo "Ninja is required" >&2; exit 1; }

mkdir -p "${source_root}" "${build_root}" "${prefix_root}" "${output_root}"

pinned_source() {
  local name="$1" repository="$2" tag="$3" destination="${source_root}/$1"
  if [[ ! -d "${destination}/.git" ]]; then
    git clone --depth 1 --branch "${tag}" "${repository}" "${destination}" >&2
  fi
  [[ "$(git -C "${destination}" describe --tags --exact-match)" == "${tag}" ]] || {
    echo "${name} cache is not pinned to ${tag}" >&2
    exit 1
  }
  printf '%s' "${destination}"
}

de265_source="$(pinned_source libde265 https://github.com/strukturag/libde265.git v1.0.16)"
heif_source="$(pinned_source libheif https://github.com/strukturag/libheif.git v1.23.1)"
de265_build="${build_root}/libde265"
heif_build="${build_root}/libheif"
bridge_build="${build_root}/hdrbridge"
de265_prefix="${prefix_root}/libde265"
heif_prefix="${prefix_root}/libheif"

emcmake cmake -S "${de265_source}" -B "${de265_build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${de265_prefix}" \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_SDL=OFF \
  -DENABLE_DECODER=OFF \
  -DENABLE_ENCODER=OFF
cmake --build "${de265_build}" --target install --parallel

emcmake cmake -S "${heif_source}" -B "${heif_build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${heif_prefix}" \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_PLUGIN_LOADING=OFF \
  -DENABLE_MULTITHREADING_SUPPORT=OFF \
  -DENABLE_PARALLEL_TILE_DECODING=OFF \
  -DBUILD_TESTING=OFF \
  -DBUILD_DOCUMENTATION=OFF \
  -DWITH_EXAMPLES=OFF \
  -DWITH_GDK_PIXBUF=OFF \
  -DWITH_LIBSHARPYUV=OFF \
  -DWITH_LIBDE265=ON \
  -DWITH_LIBDE265_PLUGIN=OFF \
  -DLIBDE265_INCLUDE_DIR="${de265_prefix}/include" \
  -DLIBDE265_LIBRARY="${de265_prefix}/lib/libde265.a" \
  -DWITH_X265=OFF \
  -DWITH_X264=OFF \
  -DWITH_OpenH264_DECODER=OFF \
  -DWITH_AOM_DECODER=OFF \
  -DWITH_AOM_ENCODER=OFF \
  -DWITH_DAV1D=OFF \
  -DWITH_JPEG_DECODER=OFF \
  -DWITH_JPEG_ENCODER=OFF \
  -DWITH_OpenJPEG_DECODER=OFF \
  -DWITH_OpenJPEG_ENCODER=OFF
cmake --build "${heif_build}" --target install --parallel

emcmake cmake -S "${web_root}/wasm" -B "${bridge_build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHDRBRIDGE_HEIF_ROOT="${heif_prefix}" \
  -DHDRBRIDGE_DE265_ROOT="${de265_prefix}"
cmake --build "${bridge_build}" --parallel

cp "${bridge_build}/dist/hdrbridge-core.mjs" "${output_root}/"
cp "${bridge_build}/dist/hdrbridge-core.wasm" "${output_root}/"
mkdir -p "${web_root}/public/licenses"
cp "${heif_source}/COPYING" "${web_root}/public/licenses/libheif-LGPL-3.0.txt"
cp "${de265_source}/COPYING" "${web_root}/public/licenses/libde265-LGPL-3.0.txt"
printf 'Built %s\n' "$(du -h "${output_root}/hdrbridge-core.wasm" | cut -f1) HDR Bridge Web HEIF module"
