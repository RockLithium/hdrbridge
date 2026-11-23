import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const script = fileURLToPath(new URL("../scripts/stamp-version.mjs", import.meta.url));
const dir = await mkdtemp(join(tmpdir(), "version stamp "));
try {
  const version = join(dir, "VERSION file");
  const first = join(dir, "normal page.html");
  const second = join(dir, "bundled page.html");
  await writeFile(version, "2.3.4\n");
  const page = '<span data-app-version>1.4.1</span><a data-platform="windows" href="old.exe">Windows</a><a href="old.apk" data-platform="android">Android</a><a href="https://github.com/RockLithium/hdrbridge/releases/latest">latest</a>';
  await writeFile(first, page);
  await writeFile(second, page);

  const run = (...args) => spawnSync(process.execPath, [script, ...args], { encoding: "utf8" });
  let result = run(version, first, second);
  assert.equal(result.status, 0, result.stderr);
  const html = await readFile(first, "utf8");
  assert.equal(await readFile(second, "utf8"), html);
  assert.match(html, />2\.3\.4</);
  assert.match(html, /data-platform="windows" href="https:\/\/github\.com\/RockLithium\/hdrbridge\/releases\/download\/v2\.3\.4\/HDRBridge-v2\.3\.4-Windows-x64-Setup\.exe"/);
  assert.match(html, /href="https:\/\/github\.com\/RockLithium\/hdrbridge\/releases\/download\/v2\.3\.4\/HDRBridge-v2\.3\.4-Android-arm64\.apk" data-platform="android"/);
  assert.match(html, /href="https:\/\/github\.com\/RockLithium\/hdrbridge\/releases\/latest"/);

  result = run(version, first);
  assert.equal(result.status, 0, result.stderr, "already-current version should be accepted");
  assert.equal(await readFile(first, "utf8"), html);

  const missing = join(dir, "missing marker.html");
  await writeFile(missing, "<p>No version marker</p>");
  result = run(version, missing);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /Version marker not found/);
} finally {
  await rm(dir, { recursive: true, force: true });
}

console.log("Version stamping tests passed");
