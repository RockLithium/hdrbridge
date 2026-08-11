let corePromise;

async function getCore() {
  if (!corePromise) {
    corePromise = import("../public/codecs/heif-png/hdrbridge-core.mjs")
      .then(({ default: createCore }) => createCore({
        locateFile: (name) => new URL(
          `../public/codecs/heif-png/${name}`, import.meta.url).href,
      }));
  }
  return corePromise;
}

function readCString(core, address) {
  let end = address;
  while (core.HEAPU8[end] !== 0) end += 1;
  return new TextDecoder().decode(core.HEAPU8.subarray(address, end));
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
    if (data.type === "inspect") {
      const info = await withInput(data.buffer, (core, pointer, size) => {
        if (core._hb_inspect_heif(pointer, size) !== 0) {
          throw new Error(readCString(core, core._hb_last_error()));
        }
        return JSON.parse(readCString(core, core._hb_info_json()));
      });
      self.postMessage({ type: "inspected", requestId: data.requestId, info });
      return;
    }
    if (data.type === "convert") {
      const result = await withInput(data.buffer, (core, pointer, size) => {
        const status = core._hb_convert_heif_to_png(
          pointer, size, data.primaries, data.transfer, data.compression);
        if (status !== 0) {
          throw new Error(readCString(core, core._hb_last_error()));
        }
        const outputSize = core._hb_output_size();
        const outputPointer = core._hb_output_data();
        const output = core.HEAPU8.slice(outputPointer, outputPointer + outputSize);
        const info = JSON.parse(readCString(core, core._hb_info_json()));
        core._hb_clear_result();
        return { output, info };
      });
      self.postMessage({
        type: "converted",
        requestId: data.requestId,
        buffer: result.output.buffer,
        info: result.info,
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
