$ErrorActionPreference = "Stop"

$webRoot = Split-Path -Parent $PSScriptRoot
$distRoot = Join-Path $webRoot "dist"
$wasm = Join-Path $webRoot "public\codecs\hdrbridge\hdrbridge-core.wasm"
if (-not (Test-Path -LiteralPath $wasm)) {
  throw "Run web/scripts/build-wasm.ps1 first"
}

if (Test-Path -LiteralPath $distRoot) {
  $resolvedDist = (Resolve-Path -LiteralPath $distRoot).Path
  $expectedDist = [System.IO.Path]::GetFullPath((Join-Path $webRoot "dist"))
  if ($resolvedDist -ne $expectedDist -or $expectedDist -eq [System.IO.Path]::GetPathRoot($expectedDist)) {
    throw "Refusing to clean an unexpected output path: $resolvedDist"
  }
  Remove-Item -LiteralPath $resolvedDist -Recurse -Force
}
New-Item -ItemType Directory -Path $distRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $webRoot "index.html") -Destination $distRoot
Copy-Item -LiteralPath (Join-Path $webRoot "src") -Destination $distRoot -Recurse
Copy-Item -LiteralPath (Join-Path $webRoot "public") -Destination $distRoot -Recurse
Copy-Item -LiteralPath (Join-Path $webRoot "README.md") -Destination (Join-Path $distRoot "WEB_README.md")
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $webRoot) "THIRD_PARTY_NOTICES.md") -Destination $distRoot
& node (Join-Path $PSScriptRoot "generate-bundled.mjs") `
  (Join-Path $webRoot "index.html") $wasm `
  (Join-Path $distRoot "bundled\index.html")
if ($LASTEXITCODE -ne 0) { throw "Bundled Web generation failed" }
New-Item -ItemType File -Force -Path (Join-Path $distRoot ".nojekyll") | Out-Null
