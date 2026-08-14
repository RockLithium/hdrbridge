import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const [indexPath, wasmPath, outputPath] = process.argv.slice(2)
  .map((value) => resolve(value));
if (!indexPath || !wasmPath || !outputPath) {
  throw new Error("usage: node generate-bundled.mjs <index> <wasm> <output>");
}

const [source, wasm] = await Promise.all([
  readFile(indexPath, "utf8"),
  readFile(wasmPath),
]);
const encoded = wasm.toString("base64");
if (!Buffer.from(encoded, "base64").equals(wasm)) {
  throw new Error("embedded WASM verification failed");
}

const marker = '<script type="module" src="./src/app.js?v=9"></script>';
if (!source.includes(marker)) throw new Error("application script marker not found");

const bundled = source
  .replace("<title>HDR Bridge Web</title>",
    "<title>HDR Bridge Web — Compatibility Mode</title>")
  .replace('href="../tools"', 'href="../../tools"')
  .replace('href="./src/styles.css?v=6"', 'href="../src/styles.css?v=6"')
  .replace(marker,
    `<script id="hdrbridge-embedded-core" type="application/octet-stream">${encoded}</script>\n` +
    '  <script type="module" src="../src/app.js?v=9"></script>');

await mkdir(dirname(outputPath), { recursive: true });
await writeFile(outputPath, bundled);

const digest = createHash("sha256").update(wasm).digest("hex");
console.log(`Bundled core: ${wasm.length} bytes, SHA-256 ${digest}`);
