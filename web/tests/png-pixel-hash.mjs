import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { pathToFileURL } from "node:url";

if (process.argv.length < 4) {
  console.error("Usage: node png-pixel-hash.mjs <core.mjs> <image.png> [...]");
  process.exit(2);
}

const modulePath = resolve(process.argv[2]);
const createCore = (await import(pathToFileURL(modulePath))).default;
const core = await createCore({
  locateFile: (name) => resolve(dirname(modulePath), name),
});

function readCString(address) {
  let end = address;
  while (core.HEAPU8[end] !== 0) end += 1;
  return new TextDecoder().decode(core.HEAPU8.subarray(address, end));
}

for (const source of process.argv.slice(3)) {
  const input = new Uint8Array(await readFile(source));
  const pointer = core._malloc(input.byteLength);
  try {
    core.HEAPU8.set(input, pointer);
    const crc = core._hb_png_pixel_crc32(pointer, input.byteLength) >>> 0;
    if (crc === 0) throw new Error(readCString(core._hb_last_error()));
    console.log(`${crc.toString(16).padStart(8, "0")}  ${source}`);
  } finally {
    core._free(pointer);
  }
}
