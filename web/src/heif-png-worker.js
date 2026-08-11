let corePromise;

const formatIds = { uhdr: 0, png: 1, jxl: 2, jxr: 3, avif: 4, tiff: 5 };
const formatInfo = {
  uhdr: { extension: "jpg", mime: "image/jpeg" },
  png: { extension: "png", mime: "image/png" },
  jxl: { extension: "jxl", mime: "image/jxl" },
  jxr: { extension: "jxr", mime: "image/vnd.ms-photo" },
  avif: { extension: "avif", mime: "image/avif" },
  tiff: { extension: "tiff", mime: "image/tiff" },
};

async function getCore() {
  if (!corePromise) {
    corePromise = import("../public/codecs/hdrbridge/hdrbridge-core.mjs")
      .then(({ default: createCore }) => createCore({
        locateFile: (name) => new URL(
          `../public/codecs/hdrbridge/${name}`, import.meta.url).href,
      }));
  }
  return corePromise;
}

function readCString(core, address) {
  let end = address;
  while (core.HEAPU8[end] !== 0) end += 1;
  return new TextDecoder().decode(core.HEAPU8.subarray(address, end));
}

function extensionOf(name) {
  const match = /\.([a-z0-9]{1,8})$/i.exec(name || "");
  if (!match) throw new Error("The selected file has no usable extension.");
  return match[1].toLowerCase();
}

function withCString(core, value, operation) {
  const size = core.lengthBytesUTF8(value) + 1;
  const pointer = core._malloc(size);
  if (!pointer) throw new Error("The browser could not allocate enough memory.");
  try {
    core.stringToUTF8(value, pointer, size);
    return operation(pointer);
  } finally {
    core._free(pointer);
  }
}

async function withInput(buffer, operation) {
  const core = await getCore();
  const input = new Uint8Array(buffer);
  const pointer = core._malloc(input.byteLength);
  if (!pointer) throw new Error("The browser could not allocate enough memory.");
  try {
    core.HEAPU8.set(input, pointer);
    return await operation(core, pointer, input.byteLength);
  } finally {
    core._free(pointer);
  }
}

self.addEventListener("message", async ({ data }) => {
  try {
    const extension = extensionOf(data.fileName);
    if (data.type === "inspect") {
      const info = await withInput(data.buffer, (core, pointer, size) =>
        withCString(core, extension, (extensionPointer) => {
          if (core._hb_inspect_asset(pointer, size, extensionPointer) !== 0) {
            throw new Error(readCString(core, core._hb_last_error()));
          }
          return JSON.parse(readCString(core, core._hb_info_json()));
        }));
      self.postMessage({ type: "inspected", requestId: data.requestId, info });
      return;
    }
    if (data.type === "convert") {
      if (!(data.format in formatIds)) throw new Error("Unknown output format.");
      const result = await withInput(data.buffer, (core, pointer, size) =>
        withCString(core, extension, (extensionPointer) => {
          const status = core._hb_convert_asset(
            pointer, size, extensionPointer, formatIds[data.format],
            data.primaries, data.transfer, data.encodingValue,
            data.lossless ? 1 : 0, data.copyExif ? 1 : 0,
            data.copyXmp ? 1 : 0, data.gainResolution,
            data.gainChannels === "rgb" ? 1 : 0, data.peak);
          if (status !== 0) {
            throw new Error(readCString(core, core._hb_last_error()));
          }
          const outputSize = core._hb_output_size();
          const outputPointer = core._hb_output_data();
          const output = core.HEAPU8.slice(outputPointer, outputPointer + outputSize);
          const info = JSON.parse(readCString(core, core._hb_info_json()));
          core._hb_clear_result();
          return { output, info };
        }));
      self.postMessage({
        type: "converted",
        requestId: data.requestId,
        buffer: result.output.buffer,
        info: result.info,
        format: data.format,
        ...formatInfo[data.format],
      }, [result.output.buffer]);
    }
  } catch (error) {
    self.postMessage({
      type: "error",
      requestId: data.requestId,
      message: error instanceof Error ? error.message : String(error),
    });
  }
});
