import { readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

if (process.argv.length < 5) {
  console.error("Usage: node convert-heif.mjs <core.mjs> <input.heif> <output.png> [primaries=9] [transfer=16] [compression=4]");
  process.exit(2);
}

const modulePath = resolve(process.argv[2]);
const inputPath = resolve(process.argv[3]);
const outputPath = resolve(process.argv[4]);
const primaries = Number(process.argv[5] ?? 9);
const transfer = Number(process.argv[6] ?? 16);
const compression = Number(process.argv[7] ?? 4);
const createCore = (await import(pathToFileURL(modulePath))).default;
const core = await createCore({
  locateFile: (name) => resolve(dirname(modulePath), name),
});
const input = new Uint8Array(await readFile(inputPath));
const pointer = core._malloc(input.byteLength);

function readCString(address) {
  let end = address;
  while (core.HEAPU8[end] !== 0) end += 1;
  return new TextDecoder().decode(core.HEAPU8.subarray(address, end));
}

try {
  core.HEAPU8.set(input, pointer);
  const result = core._hb_convert_heif_to_png(
    pointer, input.byteLength, primaries, transfer, compression);
  if (result !== 0) {
    throw new Error(readCString(core._hb_last_error()));
  }
  const outputSize = core._hb_output_size();
  const output = core.HEAPU8.slice(
    core._hb_output_data(), core._hb_output_data() + outputSize);
  const pixelCrc = core._hb_png_pixel_crc32(
    core._hb_output_data(), outputSize) >>> 0;
  await writeFile(outputPath, output);
  console.log(readCString(core._hb_info_json()));
  console.log(`Pixel CRC-32 ${pixelCrc.toString(16).padStart(8, "0")}`);
  console.log(`Wrote ${outputSize} bytes to ${outputPath}`);
} finally {
  core._hb_clear_result();
  core._free(pointer);
}
