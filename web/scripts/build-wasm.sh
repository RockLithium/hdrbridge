#!/usr/bin/env bash
set -euo pipefail

web_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${web_root}/.." && pwd)"
cache_root="${web_root}/.cache"
vcpkg_root="${cache_root}/vcpkg"
downloads_root="${cache_root}/downloads"
install_root="${cache_root}/vcpkg_installed"
aom_overlay="${repo_root}/ports-overlay/aom"
prefix="${install_root}/wasm32-emscripten"
build_root="${cache_root}/build/hdrbridge-full"
output_root="${web_root}/public/codecs/hdrbridge"
vcpkg_ref="ddd110b8a05cda14b8f1b0333a1d80c4fb6f16cd"

command -v emcmake >/dev/null || { echo "Emscripten is not active" >&2; exit 1; }
command -v ninja >/dev/null || { echo "Ninja is required" >&2; exit 1; }

mkdir -p "${cache_root}" "${downloads_root}" "${output_root}" "${web_root}/public/licenses"
if [[ ! -d "${vcpkg_root}/.git" ]]; then
  # Older CI caches placed downloads inside this checkout path, leaving a
  # non-empty directory that git clone cannot reuse.
  rm -rf "${vcpkg_root}"
  git clone https://github.com/microsoft/vcpkg.git "${vcpkg_root}"
fi
git -C "${vcpkg_root}" fetch --quiet origin "${vcpkg_ref}"
git -C "${vcpkg_root}" checkout --quiet "${vcpkg_ref}"
"${vcpkg_root}/bootstrap-vcpkg.sh" -disableMetrics
aom_version=$(python3 -c 'import json, sys; p = json.load(open(sys.argv[1])); print(p["version-semver"] + "#" + str(p["port-version"]))' "${aom_overlay}/vcpkg.json")

for variant in scalar threaded; do
  triplet=wasm32-emscripten
  threaded=OFF
  if [[ "$variant" == threaded ]]; then triplet=wasm32-emscripten-pthreads; threaded=ON; fi
  prefix="${install_root}/${triplet}"
  build_root="${cache_root}/build/hdrbridge-${variant}"
# Classic vcpkg installs do not replace an already installed overlay version.
installed_aom=$("${vcpkg_root}/vcpkg" list "aom:${triplet}" --classic "--x-install-root=${install_root}" --x-json |
  python3 -c 'import json, sys; p = json.load(sys.stdin).get(sys.argv[1]); print(p["version"] + "#" + str(p["port_version"]) if p else "")' "aom:${triplet}")
if [[ -n "$installed_aom" && "$installed_aom" != "$aom_version" ]]; then
  "${vcpkg_root}/vcpkg" remove "aom:${triplet}" --classic --recurse "--x-install-root=${install_root}" \
    "--overlay-triplets=${web_root}/vcpkg-triplets"
fi
for attempt in 1 2 3; do
  if VCPKG_DOWNLOADS="${downloads_root}" "${vcpkg_root}/vcpkg" install \
    "aom:${triplet}" "libheif[core,aom]:${triplet}" "libavif[core,aom]:${triplet}" \
    "libpng:${triplet}" "tiff[core,jpeg,zip]:${triplet}" \
    "lcms:${triplet}" "liblzma:${triplet}" "libjxl:${triplet}" \
    "jxrlib:${triplet}" "nlohmann-json:${triplet}" \
    --classic "--x-install-root=${install_root}" \
    "--overlay-ports=${web_root}/vcpkg-overlay" "--overlay-ports=${aom_overlay}" \
    "--overlay-triplets=${web_root}/vcpkg-triplets"; then
    break
  fi
  if [[ "${attempt}" == 3 ]]; then
    echo "vcpkg dependency installation failed after ${attempt} attempts" >&2
    exit 1
  fi
  echo "vcpkg download/build attempt ${attempt} failed; retrying" >&2
  sleep 10
done

emcmake cmake -S "${web_root}/wasm" -B "${build_root}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  "-DHDRBRIDGE_PTHREADS=${threaded}" \
  "-DCMAKE_PREFIX_PATH=${prefix}" \
  "-DCMAKE_FIND_ROOT_PATH=${prefix}" \
  "-Dlibheif_DIR=${prefix}/share/libheif" \
  "-Dlibavif_DIR=${prefix}/share/libavif" \
  "-Dnlohmann_json_DIR=${prefix}/share/nlohmann_json" \
  "-Dlcms2_DIR=${prefix}/share/lcms2" \
  "-DHDRBRIDGE_VCPKG_PREFIX=${prefix}"
cmake --build "${build_root}" --parallel

  dest="${output_root}"
  if [[ "$variant" == threaded ]]; then dest="${output_root}/threaded"; fi
  mkdir -p "$dest"
  cp "${build_root}/dist/"* "$dest/"
done
cp "${repo_root}/THIRD_PARTY_NOTICES.md" "${web_root}/public/licenses/"
du -h "${output_root}/hdrbridge-core.wasm"
