import { readFile, writeFile } from "node:fs/promises";
import { dirname, extname, resolve } from "node:path";
import { pathToFileURL } from "node:url";

if (process.argv.length < 5) {
  console.error("Usage: node convert-asset.mjs <core.mjs> <input> <output> [format=1] [primaries=9] [transfer=16] [encoding=4]");
  process.exit(2);
}

const modulePath = resolve(process.argv[2]);
const inputPath = resolve(process.argv[3]);
const outputPath = resolve(process.argv[4]);
const format = Number(process.argv[5] ?? 1);
const primaries = Number(process.argv[6] ?? 9);
const transfer = Number(process.argv[7] ?? 16);
const encoding = Number(process.argv[8] ?? 4);
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
  const status = core._hb_convert_asset(
    pointer, input.byteLength, extensionPointer, format, primaries, transfer,
    encoding, 1, 1, 1, 2, 0, 0);
  if (status !== 0) throw new Error(readCString(core._hb_last_error()));
  const outputSize = core._hb_output_size();
  const outputPointer = core._hb_output_data();
  await writeFile(outputPath, core.HEAPU8.slice(outputPointer, outputPointer + outputSize));
  console.log(readCString(core._hb_info_json()));
} finally {
  core._hb_clear_result();
  core._free(extensionPointer);
  core._free(pointer);
}
