const availableFormats = new Set(["uhdr", "png", "jxl", "jxr", "avif", "tiff"]);
const translations = {
  "en-US": {
    goBack: "← Go Back", subtitle: "Convert HDR still images locally in your browser.",
    selectHdr: "SELECT HDR IMAGE", dropText: "Click or drop one HDR image here...", localText: "Your image stays on this device.",
    representation: "Representation", raster: "Raster", signal: "Signal", metadata: "Metadata", outputFormat: "OUTPUT FORMAT",
    formatUhdr: "Ultra HDR JPEG", formatPng: "HDR PNG", formatAvif: "HDR AVIF", formatTiff: "HDR TIFF",
    colorSpace: "Color Space", displayP3: "Display P3", transfer: "Transfer", gainMap: "Gain Map", gainResolution: "Gain Map Resolution", gainChannels: "Gain Map Channels", mono: "Mono",
    faithful: "Faithful / Auto", lossless: "Lossless",
    peak: "Peak", preserve: "Preserve", convert: "Start Processing", stop: "Stop Processing", complete: "Processing Complete",
    selectBegin: "Select a supported HDR image to begin.", preview: "PREVIEW", noFile: "No file loaded", download: "DOWNLOAD",
    previewUnavailable: "Preview unavailable in this browser", inspecting: "Inspecting the HDR container locally...",
    ready: "Ready to convert locally.", detected: "HDR source detected. Ready to convert locally.", noHdr: "No supported HDR data was found.",
    unavailable: "This browser codec is not enabled yet.", processing: "Loading the required codecs and reconstructing the HDR raster locally...",
    aborted: "Processing aborted.", noneReported: "None reported", directHdr: "Direct HDR", gainMapHdr: "Gain-map HDR", localFile: "local file",
    compressionPng: "PNG Compression", compressionTiff: "TIFF Compression", jpegQuality: "JPEG Quality", quality: "Quality",
    converted: "Converted {value}", peakNits: "peak {value} nit",
    formatCopy: {
      uhdr: "Ultra HDR JPEG with a faithful ISO gain map.", png: "16-bit HDR PNG with standard cICP signaling.",
      jxl: "JPEG XL master output with HDR color signaling.", jxr: "FP16 linear scRGB JPEG XR for Windows workflows.",
      avif: "Compact direct 10-bit HDR AVIF output.", tiff: "Professional 16-bit PQ HDR TIFF interchange output.",
    },
  },
  "zh-Hans": {
    goBack: "← 返回", subtitle: "在浏览器中本地转换 HDR 静态图像。",
    selectHdr: "选择 HDR 图像", dropText: "点击或拖入一张 HDR 图像...", localText: "图像只在本设备上处理。",
    representation: "源表示", raster: "图像", signal: "信号", metadata: "元数据", outputFormat: "输出格式",
    formatUhdr: "Ultra HDR JPEG", formatPng: "HDR PNG", formatAvif: "HDR AVIF", formatTiff: "HDR TIFF",
    colorSpace: "色彩空间", displayP3: "Display P3", transfer: "传递函数", gainMap: "增益图", gainResolution: "增益图分辨率", gainChannels: "增益图通道", mono: "单通道",
    faithful: "保真 / 自动", lossless: "无损",
    peak: "峰值", preserve: "保留", convert: "开始转换", stop: "停止转换", complete: "转换完成",
    selectBegin: "请选择受支持的 HDR 图像。", preview: "预览", noFile: "未加载文件", download: "下载",
    previewUnavailable: "此浏览器无法预览该格式", inspecting: "正在本地检查 HDR 容器...",
    ready: "可以开始本地转换。", detected: "已检测到 HDR 源，可以开始本地转换。", noHdr: "未找到受支持的 HDR 数据。",
    unavailable: "此浏览器尚未启用该编解码器。", processing: "正在加载所需编解码器并在本地重建 HDR 图像...",
    aborted: "处理已中止。", noneReported: "未报告", directHdr: "Direct HDR", gainMapHdr: "Gain-map HDR", localFile: "本地文件",
    compressionPng: "PNG 压缩", compressionTiff: "TIFF 压缩", jpegQuality: "JPEG 质量", quality: "质量",
    converted: "已转换 {value}", peakNits: "峰值 {value} nit",
    formatCopy: {
      uhdr: "带保真 ISO 增益图的 Ultra HDR JPEG。", png: "带标准 cICP 信号的 16-bit HDR PNG。",
      jxl: "带 HDR 色彩信号的 JPEG XL 编辑/母版输出。", jxr: "FP16 线性 scRGB JPEG XR 编辑/母版输出。",
      avif: "Direct 10-bit HDR AVIF 输出。", tiff: "Direct 16-bit PQ HDR TIFF 输出。",
    },
  },
  "fr-FR": {
    goBack: "← Retour", subtitle: "Convertissez localement des images HDR dans votre navigateur.",
    selectHdr: "CHOISIR UNE IMAGE HDR", dropText: "Cliquez ou déposez une image HDR ici...", localText: "L’image reste sur cet appareil.",
    representation: "Représentation", raster: "Image", signal: "Signal", metadata: "Métadonnées", outputFormat: "FORMAT DE SORTIE",
    formatUhdr: "JPEG Ultra HDR", formatPng: "PNG HDR", formatAvif: "AVIF HDR", formatTiff: "TIFF HDR",
    colorSpace: "Espace colorimétrique", displayP3: "Display P3", transfer: "Transfert", gainMap: "Gain map", gainResolution: "Résolution de la gain map", gainChannels: "Canaux de la gain map", mono: "Mono",
    faithful: "Fidèle / Auto", lossless: "Sans perte",
    peak: "Pic", preserve: "Conserver", convert: "Lancer la conversion", stop: "Arrêter", complete: "Terminé",
    selectBegin: "Choisissez une image HDR prise en charge.", preview: "APERÇU", noFile: "Aucun fichier", download: "TÉLÉCHARGER",
    previewUnavailable: "Aperçu indisponible dans ce navigateur", inspecting: "Analyse locale du conteneur HDR...",
    ready: "Prêt pour la conversion locale.", detected: "Source HDR détectée. Prêt pour la conversion locale.", noHdr: "Aucune donnée HDR prise en charge.",
    unavailable: "Ce codec n’est pas encore activé dans ce navigateur.", processing: "Chargement des codecs et reconstruction locale de l’image HDR...",
    aborted: "Annulé.", noneReported: "Non indiqué", directHdr: "HDR direct", gainMapHdr: "HDR avec gain map", localFile: "fichier local",
    compressionPng: "Compression PNG", compressionTiff: "Compression TIFF", jpegQuality: "Qualité JPEG", quality: "Qualité",
    converted: "Converti : {value}", peakNits: "pic {value} nit",
    formatCopy: {
      uhdr: "JPEG Ultra HDR avec gain map ISO fidèle.", png: "PNG HDR 16 bits avec signalisation cICP standard.",
      jxl: "Sortie JPEG XL HDR pour édition/master.", jxr: "Sortie JPEG XR FP16 scRGB linéaire pour édition/master.",
      avif: "Sortie AVIF HDR directe 10 bits.", tiff: "Sortie TIFF HDR PQ directe 16 bits.",
    },
  },
  "ru-RU": {
    goBack: "← Назад", subtitle: "Локальное преобразование HDR-изображений в браузере.",
    selectHdr: "ВЫБЕРИТЕ HDR-ИЗОБРАЖЕНИЕ", dropText: "Нажмите или перетащите сюда HDR-изображение...", localText: "Изображение остаётся на этом устройстве.",
    representation: "Представление", raster: "Растр", signal: "Сигнал", metadata: "Метаданные", outputFormat: "ФОРМАТ ВЫВОДА",
    formatUhdr: "Ultra HDR JPEG", formatPng: "HDR PNG", formatAvif: "HDR AVIF", formatTiff: "HDR TIFF",
    colorSpace: "Цветовое пространство", displayP3: "Display P3", transfer: "Передаточная функция", gainMap: "Карта усиления", gainResolution: "Разрешение карты усиления", gainChannels: "Каналы карты усиления", mono: "Моно",
    faithful: "Точно / Авто", lossless: "Без потерь",
    peak: "Пик", preserve: "Сохранить", convert: "Запустить", stop: "Остановить", complete: "Готово",
    selectBegin: "Выберите поддерживаемое HDR-изображение.", preview: "ПРЕДПРОСМОТР", noFile: "Файл не выбран", download: "СКАЧАТЬ",
    previewUnavailable: "Предпросмотр недоступен в этом браузере", inspecting: "Локальная проверка HDR-контейнера...",
    ready: "Готово к локальному преобразованию.", detected: "Обнаружен HDR-источник. Можно преобразовывать.", noHdr: "Поддерживаемые HDR-данные не найдены.",
    unavailable: "Этот кодек пока не включён в браузере.", processing: "Загрузка кодеков и локальная реконструкция HDR-растра...",
    aborted: "Отменено.", noneReported: "Нет данных", directHdr: "Прямой HDR", gainMapHdr: "HDR с картой усиления", localFile: "локальный файл",
    compressionPng: "Сжатие PNG", compressionTiff: "Сжатие TIFF", jpegQuality: "Качество JPEG", quality: "Качество",
    converted: "Преобразовано: {value}", peakNits: "пик {value} нит",
    formatCopy: {
      uhdr: "Ultra HDR JPEG с точной ISO-картой усиления.", png: "16-битный HDR PNG со стандартной сигнализацией cICP.",
      jxl: "JPEG XL HDR для редактирования и мастер-копий.", jxr: "FP16 linear scRGB JPEG XR для редактирования и мастер-копий.",
      avif: "Прямой 10-битный HDR AVIF.", tiff: "Прямой 16-битный PQ HDR TIFF.",
    },
  },
};

const state = {
  file: null,
  sourceSupported: false,
  format: "png",
  primaries: 9,
  transfer: 16,
  outputRepresentation: "direct",
  gainResolution: 2,
  gainChannels: "mono",
  peak: 0,
  lossless: true,
  copyExif: true,
  copyXmp: true,
  tiffCompression: "compressed",
  encodingValues: { uhdr: 95, png: 4, jxl: 95, jxr: 95, avif: 95, tiff: 6 },
  requestId: 0,
  outputUrl: null,
  outputName: "",
  worker: null,
  busy: false,
  language: "en-US",
  statusMessage: { key: "selectBegin", kind: "", replacements: {} },
  inspectInfo: null,
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
  gainmapTransferOption: document.querySelector("#gainmap-transfer-option"),
  colorRow: document.querySelector("#color-space-row"),
  transferRow: document.querySelector("#transfer-row"),
  gainResolutionRow: document.querySelector("#gain-resolution-row"),
  gainChannelsRow: document.querySelector("#gain-channels-row"),
  peakRow: document.querySelector("#peak-row"),
  losslessOption: document.querySelector("#lossless-option"),
  tiffCompressionRow: document.querySelector("#tiff-compression-row"),
  encodingRow: document.querySelector("#encoding-row"),
  sliderLabel: document.querySelector("#slider-label"),
  slider: document.querySelector("#encoding-slider"),
  sliderValue: document.querySelector("#encoding-value"),
  convert: document.querySelector("#convert-button"),
  progress: document.querySelector("#progress-track"),
  status: document.querySelector("#status"),
  preview: document.querySelector("#preview"),
  download: document.querySelector("#download-button"),
  language: document.querySelector("#language-select"),
};

function tr(key) {
  return translations[state.language]?.[key] ?? translations["en-US"][key] ?? key;
}

function applyLanguage(language) {
  state.language = translations[language] ? language : "en-US";
  document.documentElement.lang = state.language;
  document.querySelectorAll("[data-i18n]").forEach((element) => {
    element.textContent = tr(element.dataset.i18n);
  });
  elements.language.value = state.language;
  if (!state.file) {
    elements.fileName.textContent = tr("dropText");
    elements.fileState.textContent = tr("localText");
  } else {
    elements.fileState.textContent = `${humanBytes(state.file.size)} · ${tr("localFile")}`;
  }
  if (state.inspectInfo) showInspector(state.inspectInfo);
  renderTranslatedStatus();
  configureParameters();
  elements.availability.textContent = translations[state.language].formatCopy[state.format];
  if (state.busy) elements.convert.textContent = tr("stop");
  else if (elements.convert.classList.contains("is-complete")) elements.convert.textContent = tr("complete");
}

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
  state.statusMessage = null;
  elements.status.textContent = text;
  elements.status.className = `status-text status ${kind}`.trim();
}

function renderTranslatedStatus() {
  if (!state.statusMessage) return;
  let text = tr(state.statusMessage.key);
  Object.entries(state.statusMessage.replacements).forEach(([key, value]) => {
    text = text.replace(`{${key}}`, value);
  });
  elements.status.textContent = text;
  elements.status.className = `status-text status ${state.statusMessage.kind}`.trim();
}

function setTranslatedStatus(key, kind = "", replacements = {}) {
  state.statusMessage = { key, kind, replacements };
  renderTranslatedStatus();
}

function refreshConvertAvailability() {
  elements.convert.disabled = !state.busy &&
    (!state.file || !state.sourceSupported || !availableFormats.has(state.format));
}

function setBusy(busy) {
  state.busy = busy;
  elements.progress.hidden = !busy;
  refreshConvertAvailability();
  elements.convert.textContent = busy ? tr("stop") : tr("convert");
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
    elements.convert.textContent = tr("convert");
    refreshConvertAvailability();
  }
}

function optionValue(setting, rawValue) {
  return ["primaries", "gainResolution", "peak"].includes(setting)
    ? Number(rawValue)
    : setting === "transfer" && rawValue !== "gainmap" ? Number(rawValue)
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
  const supportsRepresentation = isJxl || state.format === "avif";
  const isGainMap = isUhdr || (supportsRepresentation && state.outputRepresentation === "gainmap");
  const rgbOnlyGainMap = supportsRepresentation && state.outputRepresentation === "gainmap";
  const supportsColor = ["uhdr", "png", "jxl", "avif", "tiff"].includes(state.format);
  const supportsTransfer = ["png", "jxl", "avif"].includes(state.format);
  elements.gainmapTransferOption.hidden = !supportsRepresentation;
  elements.colorRow.hidden = !supportsColor;
  elements.transferRow.hidden = !supportsTransfer;
  elements.gainResolutionRow.hidden = !isGainMap;
  elements.gainChannelsRow.hidden = !isGainMap;
  const monoChannel = elements.gainChannelsRow.querySelector('[data-value="mono"]');
  monoChannel.hidden = rgbOnlyGainMap;
  if (rgbOnlyGainMap) state.gainChannels = "rgb";
  elements.peakRow.hidden = !isGainMap;
  const parameters = elements.colorRow.parentElement;
  const preservationRow = document.querySelector("#preservation-row");
  if (isGainMap && supportsRepresentation) {
    parameters.insertBefore(elements.transferRow, preservationRow);
  } else {
    parameters.insertBefore(elements.transferRow, elements.gainResolutionRow);
  }
  elements.losslessOption.hidden = !(isJxl || isJxr || state.format === "png" || state.format === "tiff");
  elements.losslessOption.textContent = tr("lossless");
  elements.losslessOption.dataset.toggle = "lossless";
  const fixedLossless = state.format === "png" || state.format === "tiff";
  elements.losslessOption.classList.toggle("active", fixedLossless || state.lossless);
  elements.losslessOption.disabled = fixedLossless;
  elements.tiffCompressionRow.hidden = state.format !== "tiff";
  elements.encodingRow.hidden = (isJxl || isJxr) && state.lossless;
  document.querySelector('[data-setting="primaries"] [data-value="1"]').hidden = !isGainMap;

  if (state.format === "png" || state.format === "tiff") {
    elements.sliderLabel.textContent = tr(state.format === "png" ? "compressionPng" : "compressionTiff");
    elements.slider.min = "1";
    elements.slider.max = "9";
    elements.slider.step = "1";
  } else {
    elements.sliderLabel.textContent = tr(isUhdr ? "jpegQuality" : "quality");
    elements.slider.min = "1";
    elements.slider.max = "100";
    elements.slider.step = "1";
  }
  elements.slider.value = String(state.encodingValues[state.format]);
  elements.sliderValue.value = elements.slider.value;
  elements.slider.disabled = state.format === "tiff" && state.tiffCompression === "uncompressed";
  elements.sliderValue.classList.toggle("disabled",
    state.format === "tiff" && state.tiffCompression === "uncompressed");

  updateOptionButtons("primaries");
  updateOptionButtons("transfer");
  updateOptionButtons("peak");
  updateOptionButtons("gainChannels");
  updateOptionButtons("tiffCompression");
}

function selectFormat(format) {
  state.format = format;
  state.outputRepresentation = "direct";
  state.transfer = 16;
  state.primaries = format === "uhdr" ? 12 : 9;
  if (format === "uhdr") state.gainChannels = "mono";
  document.querySelectorAll("[data-format]").forEach((button) => {
    button.classList.toggle("active", button.dataset.format === format);
  });
  const available = availableFormats.has(format);
  elements.availability.textContent = translations[state.language].formatCopy[format];
  elements.availability.classList.toggle("unavailable", !available);
  configureParameters();
  clearOutput();
  setBusy(false);
  if (!available) setTranslatedStatus("unavailable");
  else if (state.file && state.sourceSupported) setTranslatedStatus("ready");
}

async function inspectFile(file) {
  const requestId = ++state.requestId;
  setTranslatedStatus("inspecting");
  try {
    const buffer = await file.arrayBuffer();
    codecWorker().postMessage({
      type: "inspect", requestId, buffer, fileName: file.name,
    }, [buffer]);
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
  elements.fileState.textContent = `${humanBytes(file.size)} · ${tr("localFile")}`;
  elements.inspector.hidden = true;
  elements.preview.innerHTML = `<span class="preview-placeholder">${tr("previewUnavailable")}</span>`;
  setBusy(false);
  inspectFile(file);
}

function showInspector(info) {
  state.inspectInfo = info;
  elements.inspector.hidden = false;
  const kind = info.assetKind === "gain-map-hdr" ? tr("gainMapHdr") :
    info.assetKind === "direct-hdr" ? tr("directHdr") : "Non-HDR / SDR";
  elements.inspectKind.textContent = `${kind} · ${info.format || info.containerBrand}`;
  elements.inspectRaster.textContent = `${info.width}×${info.height} · ${info.bitDepth || "?"}-bit · ${info.pixelFormat || info.chroma}`;
  const gamutName = (value) => value === 1 ? "BT.709" : value === 9 ? "BT.2020" :
    value === 12 ? "Display P3" : `CICP ${value ?? "?"}`;
  const primaries = gamutName(info.color?.primaries);
  const transfer = info.color?.transferName || `CICP ${info.color?.transfer ?? "?"}`;
  const native = info.nativeSignal?.present
    ? `Native ${gamutName(info.nativeSignal.primaries)} / ${info.nativeSignal.transfer}`
    : "Native absent";
  const icc = info.iccSignal?.present
    ? `ICC ${info.iccSignal.description || "present"}${info.iccSignal.cicpPresent
      ? ` (${gamutName(info.iccSignal.primaries)} / ${info.iccSignal.transferName})` : ""}`
    : "ICC absent";
  const resolved = info.resolvedColor
    ? `Resolved ${primaries} / ${transfer} from ${info.resolvedColor.source}${info.resolvedColor.conflict ? " (conflict)" : ""}`
    : `${primaries} / ${transfer}`;
  elements.inspectSignal.textContent = info.gainMapPresent
    ? `${info.gainMapFamily} · ${info.gainMapSize.width}×${info.gainMapSize.height} · ${info.gainMapLayout.channels} channel`
    : `${native} · ${icc} · ${resolved} · ${info.range}`;
  const metadata = Object.entries(info.metadata || {})
    .filter(([, status]) => status === "present")
    .map(([name]) => name.toUpperCase());
  elements.inspectMetadata.textContent = metadata.length ? metadata.join(" · ") : tr("noneReported");
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
    state.sourceSupported = data.info.assetKind === "direct-hdr" ||
      data.info.assetKind === "gain-map-hdr";
    setTranslatedStatus(state.sourceSupported ? "detected" : "noHdr", state.sourceSupported ? "" : "error");
    refreshConvertAvailability();
    return;
  }
  if (data.type === "converted") {
    setBusy(false);
    const blob = new Blob([data.buffer], { type: data.mime });
    state.outputUrl = URL.createObjectURL(blob);
    if (["uhdr", "png", "avif"].includes(data.format)) {
      const image = new Image();
      image.alt = "Converted HDR output";
      image.src = state.outputUrl;
      elements.preview.replaceChildren(image);
    } else {
      elements.preview.innerHTML = `<span class="preview-placeholder">${tr("previewUnavailable")}</span>`;
    }
    const stem = state.file.name.replace(/\.[^.]+$/, "");
    const suffixes = {
      uhdr: "_ultrahdr.jpg",
      png: state.transfer === 18 ? "_hdr-hlg.png" : "_hdr-pq16.png",
      jxl: state.outputRepresentation === "gainmap" ? "_gainmap.jxl" :
        state.transfer === 18 ? "_hlg16.jxl" : "_pq16.jxl",
      jxr: "_scrgb-fp16.jxr",
      avif: state.outputRepresentation === "gainmap" ? "_gainmap.avif" :
        state.transfer === 18 ? "_direct-hlg10.avif" : "_direct-pq10.avif",
      tiff: "_hdr-pq16.tiff",
    };
    state.outputName = `${stem}${suffixes[data.format]}`;
    elements.download.disabled = false;
    const peak = data.info.verification?.hdrDiagnostics?.maxChannelNits;
    const conversionSummary = `${data.info.verification?.pixelFormat || state.format.toUpperCase()} · ${humanBytes(blob.size)}${Number.isFinite(peak) ? ` · ${tr("peakNits").replace("{value}", peak.toFixed(1))}` : ""}`;
    setTranslatedStatus("converted", "success", { value: conversionSummary });
    elements.convert.textContent = tr("complete");
    elements.convert.className = "main-btn process-button is-complete";
    refreshConvertAvailability();
  }
}

async function convert() {
  if (state.busy) {
    state.requestId += 1;
    if (state.worker) state.worker.terminate();
    state.worker = null;
    setBusy(false);
    setTranslatedStatus("aborted");
    return;
  }
  if (!state.file || !state.sourceSupported || !availableFormats.has(state.format)) return;
  clearOutput();
  setBusy(true);
  setTranslatedStatus("processing");
  const requestId = ++state.requestId;
  try {
    const buffer = await state.file.arrayBuffer();
    codecWorker().postMessage({
      type: "convert",
      requestId,
      buffer,
      fileName: state.file.name,
      format: state.format,
      primaries: state.primaries,
      transfer: state.transfer,
      outputRepresentation: state.outputRepresentation,
      encodingValue: state.encodingValues[state.format],
      lossless: state.format === "png" || state.format === "tiff" ? true : state.lossless,
      tiffCompressed: state.tiffCompression === "compressed",
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
    if (setting === "transfer") {
      state.outputRepresentation = state.transfer === "gainmap" ? "gainmap" : "direct";
      state.primaries = state.outputRepresentation === "gainmap" ? 12 : 9;
      if (state.outputRepresentation === "gainmap") state.gainChannels = "rgb";
    }
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
elements.download.addEventListener("click", async () => {
  if (!state.outputUrl || !state.outputName) return;
  if (window.showSaveFilePicker) {
    try {
      const response = await fetch(state.outputUrl);
      const blob = await response.blob();
      const handle = await window.showSaveFilePicker({ suggestedName: state.outputName });
      const writable = await handle.createWritable();
      await writable.write(blob);
      await writable.close();
      return;
    } catch (error) {
      if (error?.name === "AbortError") return;
    }
  }
  const link = document.createElement("a");
  link.href = state.outputUrl;
  link.download = state.outputName;
  link.click();
});
elements.language.addEventListener("change", () => applyLanguage(elements.language.value));

configureParameters();
applyLanguage("en-US");
