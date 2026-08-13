import { readFile } from "node:fs/promises";
import { dirname, extname, resolve } from "node:path";
import { pathToFileURL } from "node:url";

if (process.argv.length < 4) {
  console.error("Usage: node inspect-asset.mjs <core.mjs> <input>");
  process.exit(2);
}

const modulePath = resolve(process.argv[2]);
const inputPath = resolve(process.argv[3]);
const createCore = (await import(pathToFileURL(modulePath))).default;
const core = await createCore({ locateFile: (name) => resolve(dirname(modulePath), name) });
const input = new Uint8Array(await readFile(inputPath));
const pointer = core._malloc(input.byteLength);
const extension = extname(inputPath).slice(1).toLowerCase();
const extensionSize = core.lengthBytesUTF8(extension) + 1;
const extensionPointer = core._malloc(extensionSize);

function readCString(address) {
  let end = address;
  while (core.HEAPU8[end] !== 0) end += 1;
  return new TextDecoder().decode(core.HEAPU8.subarray(address, end));
}

try {
  core.HEAPU8.set(input, pointer);
  core.stringToUTF8(extension, extensionPointer, extensionSize);
  if (core._hb_inspect_asset(pointer, input.byteLength, extensionPointer) !== 0) {
    throw new Error(readCString(core._hb_last_error()));
  }
  console.log(readCString(core._hb_info_json()));
} finally {
  core._hb_clear_result();
  core._free(extensionPointer);
  core._free(pointer);
}
