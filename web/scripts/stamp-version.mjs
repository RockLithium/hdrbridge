import { readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";

const [versionArg, ...htmlArgs] = process.argv.slice(2);
const versionPath = versionArg ? resolve(versionArg) : "";
const htmlPaths = htmlArgs.map((path) => resolve(path));
if (!versionPath || htmlPaths.length === 0) {
  throw new Error("usage: node stamp-version.mjs <VERSION> <html>...");
}

const version = (await readFile(versionPath, "utf8")).trim();
if (!/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(version)) {
  throw new Error(`Invalid application version: ${version}`);
}
for (const path of htmlPaths) {
  const html = await readFile(path, "utf8");
  const marker = /(<span\b[^>]*\bdata-app-version\b[^>]*>)[^<]*(<\/span>)/;
  if (!marker.test(html)) throw new Error(`Version marker not found in ${path}`);
  let stamped = html.replace(marker, `$1${version}$2`);
  const releaseBase = "https://github.com/RockLithium/hdrbridge/releases/download";
  const windowsHref = `${releaseBase}/v${version}/HDRBridge-v${version}-Windows-x64-Setup.exe`;
  const androidHref = `${releaseBase}/v${version}/HDRBridge-v${version}-Android-arm64.apk`;
  stamped = stamped.replace(/(<a\b(?=[^>]*\bdata-platform\s*=\s*(["'])windows\2)[^>]*\bhref\s*=\s*)(["'])[^"']*\3/i,
    `$1"${windowsHref}"`);
  stamped = stamped.replace(/(<a\b(?=[^>]*\bdata-platform\s*=\s*(["'])android\2)[^>]*\bhref\s*=\s*)(["'])[^"']*\3/i,
    `$1"${androidHref}"`);
  await writeFile(path, stamped);
}
