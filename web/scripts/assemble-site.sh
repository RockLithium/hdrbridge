#!/usr/bin/env bash
set -euo pipefail

web_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dist_root="${web_root}/dist"

test -f "${web_root}/public/codecs/hdrbridge/hdrbridge-core.wasm" || {
  echo "Run web/scripts/build-wasm.sh first" >&2
  exit 1
}

[[ "${dist_root}" == "${web_root}/dist" && "${web_root}" != "/" ]] || {
  echo "Refusing to clean an unexpected output path" >&2
  exit 1
}
rm -rf "${dist_root}"
mkdir -p "${dist_root}"
cp "${web_root}/index.html" "${dist_root}/"
cp -R "${web_root}/src" "${dist_root}/src"
cp -R "${web_root}/public" "${dist_root}/public"
cp "${web_root}/README.md" "${dist_root}/WEB_README.md"
cp "${web_root}/../THIRD_PARTY_NOTICES.md" "${dist_root}/"
node "${web_root}/scripts/generate-bundled.mjs" \
  "${web_root}/index.html" \
  "${web_root}/public/codecs/hdrbridge/hdrbridge-core.wasm" \
  "${dist_root}/bundled/index.html"
touch "${dist_root}/.nojekyll"
