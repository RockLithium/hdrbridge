param()
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Join-Path $repo '.tools/libultrahdr'
$revision = 'b2aacb366e1542cfc29605cb0d8a0ebd06bb07f8'
if (-not (Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt'))) {
  if (Test-Path -LiteralPath $source) { throw "Incomplete Ultra HDR checkout: $source" }
  New-Item -ItemType Directory -Force -Path (Join-Path $repo '.tools') | Out-Null
  & git init $source
  if ($LASTEXITCODE -ne 0) { throw 'Ultra HDR source initialization failed.' }
  & git -C $source remote add origin https://github.com/google/libultrahdr.git
  if ($LASTEXITCODE -ne 0) { throw 'Ultra HDR remote setup failed.' }
  & git -C $source fetch --depth 1 origin $revision
  if ($LASTEXITCODE -ne 0) { throw 'Ultra HDR revision fetch failed.' }
  & git -C $source checkout --detach $revision
  if ($LASTEXITCODE -ne 0) { throw 'Ultra HDR checkout failed.' }
}
$actual = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual.Trim() -ne $revision) {
  throw "Ultra HDR must use revision $revision. Existing source was left unchanged."
}
