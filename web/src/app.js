const availableFormats = new Set(["png"]);
const formatCopy = {
  uhdr: "Ultra HDR JPEG codec is not part of this Web build yet.",
  png: "HDR PNG is available now for direct PQ/HLG HEIF/HIF input.",
  jxl: "JPEG XL Web codec is not part of this build yet.",
  jxr: "Portable FP16 JPEG XR encoding remains under technical evaluation.",
  avif: "HDR AVIF Web encoding is not part of this build yet.",
  tiff: "HDR TIFF Web encoding is not part of this build yet.",
};

const state = {
  file: null,
  sourceSupported: false,
  format: "png",
  primaries: 9,
  transfer: 16,
  gainResolution: 2,
  gainChannels: "mono",
  peak: 0,
  lossless: true,
  copyExif: true,
  copyXmp: true,
  encodingValues: { uhdr: 95, png: 4, jxl: 95, jxr: 95, avif: 95, tiff: 6 },
  requestId: 0,
  outputUrl: null,
  outputName: "",
  worker: null,
  busy: false,
};

const elements = {
  input: document.querySelector("#file-input"),
  drop: document.querySelector("#drop-zone"),
  fileName: document.querySelector("#file-name"),
  fileState: document.querySelector("#file-state"),
  inspector: document.querySelector("#inspector"),
  inspectKind: document.querySelector("#inspect-kind"),
  inspectRaster: document.querySelector("#inspect-raster"),
  inspectSignal: document.querySelector("#inspect-signal"),
  inspectMetadata: document.querySelector("#inspect-metadata"),
  availability: document.querySelector("#availability"),
  colorRow: document.querySelector("#color-space-row"),
  transferRow: document.querySelector("#transfer-row"),
  gainResolutionRow: document.querySelector("#gain-resolution-row"),
  gainChannelsRow: document.querySelector("#gain-channels-row"),
  peakRow: document.querySelector("#peak-row"),
  losslessOption: document.querySelector("#lossless-option"),
  encodingRow: document.querySelector("#encoding-row"),
  sliderLabel: document.querySelector("#slider-label"),
  slider: document.querySelector("#encoding-slider"),
  sliderValue: document.querySelector("#encoding-value"),
  convert: document.querySelector("#convert-button"),
  progress: document.querySelector("#progress-track"),
  status: document.querySelector("#status"),
  preview: document.querySelector("#preview"),
  download: document.querySelector("#download-button"),
};

function codecWorker() {
  if (!state.worker) {
    state.worker = new Worker(new URL("./heif-png-worker.js", import.meta.url), {
      type: "module",
    });
    state.worker.addEventListener("message", handleWorkerMessage);
  }
  return state.worker;
}

function humanBytes(bytes) {
  const units = ["B", "KB", "MB", "GB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1000 && unit < units.length - 1) {
    value /= 1000;
    unit += 1;
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}

function setStatus(text, kind = "") {
  elements.status.textContent = text;
  elements.status.className = `status-text status ${kind}`.trim();
}

function setBusy(busy) {
  state.busy = busy;
  elements.progress.hidden = !busy;
  elements.convert.disabled = !busy &&
    (!state.file || !state.sourceSupported || !availableFormats.has(state.format));
  elements.convert.textContent = busy ? "STOP PROCESSING" : "CONVERT";
  elements.convert.className = busy
    ? "main-btn process-button is-processing"
    : "main-btn process-button";
}

function clearOutput() {
  if (state.outputUrl) URL.revokeObjectURL(state.outputUrl);
  state.outputUrl = null;
  state.outputName = "";
  elements.download.disabled = true;
  if (!state.busy) {
    elements.convert.className = "main-btn process-button";
    elements.convert.textContent = "CONVERT";
  }
}

function optionValue(setting, rawValue) {
  return ["primaries", "transfer", "gainResolution", "peak"].includes(setting)
    ? Number(rawValue)
    : rawValue;
}

function updateOptionButtons(setting) {
  document.querySelectorAll(`[data-setting="${setting}"] .mini-option`).forEach((button) => {
    button.classList.toggle("active", optionValue(setting, button.dataset.value) === state[setting]);
  });
}

function configureParameters() {
  const isUhdr = state.format === "uhdr";
  const isJxr = state.format === "jxr";
  const isJxl = state.format === "jxl";
  const supportsColor = ["png", "jxl", "avif", "tiff"].includes(state.format);
  const supportsTransfer = ["png", "jxl", "avif"].includes(state.format);
  elements.colorRow.hidden = !supportsColor;
  elements.transferRow.hidden = !supportsTransfer;
  elements.gainResolutionRow.hidden = !isUhdr;
  elements.gainChannelsRow.hidden = !isUhdr;
  elements.peakRow.hidden = !isUhdr;
  elements.losslessOption.hidden = !(isJxl || isJxr);
  elements.encodingRow.hidden = (isJxl || isJxr) && state.lossless;

  if (state.format === "png" || state.format === "tiff") {
    elements.sliderLabel.textContent = `${state.format === "png" ? "PNG" : "TIFF"} Compression`;
    elements.slider.min = "1";
    elements.slider.max = "9";
    elements.slider.step = "1";
  } else {
    elements.sliderLabel.textContent = isUhdr ? "JPEG Quality" : "Quality";
    elements.slider.min = "1";
    elements.slider.max = "100";
    elements.slider.step = "1";
  }
  elements.slider.value = String(state.encodingValues[state.format]);
  elements.sliderValue.value = elements.slider.value;

  const p3Button = document.querySelector('[data-setting="primaries"] [data-value="12"]');
  p3Button.hidden = state.transfer === 18;
  if (state.transfer === 18 && state.primaries !== 9) state.primaries = 9;
  updateOptionButtons("primaries");
  updateOptionButtons("transfer");
  updateOptionButtons("peak");
}

function selectFormat(format) {
  state.format = format;
  document.querySelectorAll("[data-format]").forEach((button) => {
    button.classList.toggle("active", button.dataset.format === format);
  });
  const available = availableFormats.has(format);
  elements.availability.textContent = formatCopy[format];
  elements.availability.classList.toggle("unavailable", !available);
  configureParameters();
  clearOutput();
  setBusy(false);
  if (!available) setStatus("This browser codec is not enabled yet.");
  else if (state.file && state.sourceSupported) setStatus("Ready to convert locally.");
}

async function inspectFile(file) {
  const requestId = ++state.requestId;
  setStatus("Inspecting the HDR container locally...");
  try {
    const buffer = await file.arrayBuffer();
    codecWorker().postMessage({ type: "inspect", requestId, buffer }, [buffer]);
  } catch (error) {
    setStatus(error.message, "error");
  }
}

function selectFile(file) {
  if (!file) return;
  state.file = file;
  state.sourceSupported = false;
  clearOutput();
  elements.fileName.textContent = file.name;
  elements.fileState.textContent = `${humanBytes(file.size)} · local file`;
  elements.inspector.hidden = true;
  elements.preview.innerHTML = '<span class="preview-placeholder">Preview unavailable in this browser</span>';
  setBusy(false);
  inspectFile(file);
}

function showInspector(info) {
  const source = info.input;
  elements.inspector.hidden = false;
  elements.inspectKind.textContent = `${source.asset} · ${source.brand}`;
  elements.inspectRaster.textContent = `${source.width}×${source.height} · ${source.bitDepth}-bit`;
  elements.inspectSignal.textContent = `${source.primariesName} · ${source.transferName} · Matrix ${source.matrix} · ${source.fullRange ? "Full" : "Limited"}`;
  const metadata = Object.entries(source.metadata)
    .filter(([, present]) => present)
    .map(([name]) => name.toUpperCase());
  elements.inspectMetadata.textContent = metadata.length ? metadata.join(" · ") : "None reported";
}

function handleWorkerMessage({ data }) {
  if (data.requestId !== state.requestId) return;
  if (data.type === "error") {
    setBusy(false);
    setStatus(data.message, "error");
    return;
  }
  if (data.type === "inspected") {
    showInspector(data.info);
    state.sourceSupported = data.info.supported;
    setStatus(data.info.supported
      ? "Direct HDR signal detected. Ready to convert."
      : "No supported direct PQ/HLG HDR signal was found.",
    data.info.supported ? "" : "error");
    elements.convert.disabled = !data.info.supported || !availableFormats.has(state.format);
    return;
  }
  if (data.type === "converted") {
    setBusy(false);
    const blob = new Blob([data.buffer], { type: "image/png" });
    state.outputUrl = URL.createObjectURL(blob);
    const image = new Image();
    image.alt = "Converted HDR output";
    image.src = state.outputUrl;
    elements.preview.replaceChildren(image);
    const stem = state.file.name.replace(/\.[^.]+$/, "");
    state.outputName = `${stem}-hdrbridge.png`;
    elements.download.disabled = false;
    setStatus(`Converted ${data.info.output.bitDepth}-bit HDR PNG · ${humanBytes(blob.size)} · peak ${data.info.reconstructed.peakNits.toFixed(1)} nit`, "success");
    elements.convert.textContent = "PROCESSING COMPLETE";
    elements.convert.className = "main-btn process-button is-complete";
    elements.convert.disabled = true;
  }
}

async function convert() {
  if (state.busy) {
    state.requestId += 1;
    if (state.worker) state.worker.terminate();
    state.worker = null;
    setBusy(false);
    setStatus("Processing aborted.");
    return;
  }
  if (!state.file || !state.sourceSupported || !availableFormats.has(state.format)) return;
  clearOutput();
  setBusy(true);
  setStatus("Loading the HEIF decoder and reconstructing the HDR raster locally...");
  const requestId = ++state.requestId;
  try {
    const buffer = await state.file.arrayBuffer();
    codecWorker().postMessage({
      type: "convert",
      requestId,
      buffer,
      primaries: state.primaries,
      transfer: state.transfer,
      compression: state.encodingValues.png,
      quality: state.encodingValues[state.format],
      lossless: state.lossless,
      copyExif: state.copyExif,
      copyXmp: state.copyXmp,
      gainResolution: state.gainResolution,
      gainChannels: state.gainChannels,
      peak: state.peak,
    }, [buffer]);
  } catch (error) {
    setBusy(false);
    setStatus(error.message, "error");
  }
}

elements.input.addEventListener("change", () => selectFile(elements.input.files[0]));
for (const type of ["dragenter", "dragover"]) {
  elements.drop.addEventListener(type, (event) => {
    event.preventDefault();
    elements.drop.classList.add("drag-active");
  });
}
for (const type of ["dragleave", "drop"]) {
  elements.drop.addEventListener(type, (event) => {
    event.preventDefault();
    elements.drop.classList.remove("drag-active");
  });
}
elements.drop.addEventListener("drop", (event) => selectFile(event.dataTransfer.files[0]));
document.querySelectorAll("[data-format]").forEach((button) => {
  button.addEventListener("click", () => selectFormat(button.dataset.format));
});
document.querySelectorAll("[data-setting]").forEach((group) => {
  group.addEventListener("click", (event) => {
    const button = event.target.closest(".mini-option");
    if (!button) return;
    const setting = group.dataset.setting;
    state[setting] = optionValue(setting, button.dataset.value);
    if (setting === "transfer" && state.transfer === 18) state.primaries = 9;
    updateOptionButtons(setting);
    configureParameters();
    clearOutput();
  });
});
document.querySelectorAll("[data-toggle]").forEach((button) => {
  button.addEventListener("click", () => {
    const setting = button.dataset.toggle;
    state[setting] = !state[setting];
    button.classList.toggle("active", state[setting]);
    configureParameters();
    clearOutput();
  });
});
elements.slider.addEventListener("input", () => {
  elements.sliderValue.value = elements.slider.value;
  state.encodingValues[state.format] = Number(elements.slider.value);
  clearOutput();
});
elements.convert.addEventListener("click", convert);
elements.download.addEventListener("click", () => {
  if (!state.outputUrl || !state.outputName) return;
  const link = document.createElement("a");
  link.href = state.outputUrl;
  link.download = state.outputName;
  link.click();
});

configureParameters();
