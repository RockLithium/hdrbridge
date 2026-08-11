import { readFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { inflateSync } from "node:zlib";

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

for (const path of process.argv.slice(2)) {
  const png = await readFile(path);
  if (png.subarray(0, 8).toString("hex") !== "89504e470d0a1a0a") throw new Error(`${path}: not PNG`);
  let offset = 8, width = 0, height = 0;
  const idat = [];
  while (offset + 12 <= png.length) {
    const length = png.readUInt32BE(offset);
    const type = png.toString("ascii", offset + 4, offset + 8);
    const data = png.subarray(offset + 8, offset + 8 + length);
    if (type === "IHDR") {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4);
      if (data[8] !== 16 || data[9] !== 2 || data[12] !== 0) {
        throw new Error(`${path}: expected non-interlaced RGB16 PNG`);
      }
    } else if (type === "IDAT") idat.push(data);
    else if (type === "IEND") break;
    offset += length + 12;
  }
  const packed = inflateSync(Buffer.concat(idat));
  const stride = width * 6;
  const pixels = Buffer.allocUnsafe(stride * height);
  for (let y = 0; y < height; y += 1) {
    const source = y * (stride + 1);
    const target = y * stride;
    const filter = packed[source];
    for (let x = 0; x < stride; x += 1) {
      const raw = packed[source + 1 + x];
      const left = x >= 6 ? pixels[target + x - 6] : 0;
      const above = y ? pixels[target - stride + x] : 0;
      const upperLeft = y && x >= 6 ? pixels[target - stride + x - 6] : 0;
      const predictor = filter === 0 ? 0 : filter === 1 ? left : filter === 2 ? above :
        filter === 3 ? Math.floor((left + above) / 2) :
        filter === 4 ? paeth(left, above, upperLeft) : NaN;
      if (!Number.isFinite(predictor)) throw new Error(`${path}: unsupported PNG filter ${filter}`);
      pixels[target + x] = (raw + predictor) & 255;
    }
  }
  console.log(`${createHash("sha256").update(pixels).digest("hex")}  ${path}`);
}
