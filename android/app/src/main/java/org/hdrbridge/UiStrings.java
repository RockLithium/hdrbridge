package org.hdrbridge;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Resources;
import android.widget.EditText;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.WeakHashMap;

/** Presentation-only language selection. Never changes a process locale or task option. */
final class UiStrings {
  static final String PREFERENCES = "ui_language_v1";
  static final String LANGUAGE = "language";
  private static final Map<String, String> ZH = new LinkedHashMap<>();
  private static final Map<TextView, Binding> BINDINGS = new WeakHashMap<>();
  private static final class Binding {
    final String key;
    final Object[] args;
    Binding(String key, Object[] args) { this.key = key; this.args = args; }
    String render(Context context) {
      String translated = get(context, key);
      return args.length == 0 ? translated : String.format(Locale.ROOT, translated, args);
    }
  }
  static {
    add("Switch to English", "切换为 English");
    add("Switch to Simplified Chinese", "切换为简体中文");
    add("Reset settings", "重置设置");
    add("OUTPUT", "输出"); add("FILES", "文件"); add("TASKS", "任务");
    add("TASKS (%s)", "任务（%s）");
    add("Add files", "添加文件"); add("Remove", "移除"); add("Delete", "删除"); add("Clear", "清空");
    add("Add to queue", "加入队列"); add("Remove all", "全部移除");
    add("Stop", "停止"); add("Start all", "全部开始");
    add("Format", "格式"); add("Extract Gain Map", "提取 Gain Map"); add("Photo Frame", "照片边框");
    add("Color gamut", "色域"); add("Gamut", "色域");
    add("HDR representation", "HDR 模式"); add("HDR Presentation", "HDR 模式");
    add("Gain Map", "Gain Map"); add("Gain map", "Gain Map");
    add("Transfer / representation", "传递函数 / HDR 模式");
    add("TIFF compression", "TIFF 压缩"); add("Compressed", "压缩");
    add("Uncompressed", "不压缩"); add("None", "无");
    add("Gain map size", "Gain Map 尺寸"); add("Gain map resolution", "Gain Map 分辨率");
    add("Gain map channels", "Gain Map 通道"); add("Gain map channels: RGB", "Gain Map 通道：RGB");
    add("Quarter", "1/4 分辨率"); add("Half", "1/2 分辨率"); add("Full", "全分辨率");
    add("Channels", "通道"); add("Mono", "单通道"); add("Export", "导出");
    add("Gain map export", "导出 Gain Map"); add("Original", "原始格式");
    add("Lossless", "无损"); add("Copy EXIF", "复制 EXIF"); add("Copy XMP", "复制 XMP");
    add("Compression: %s", "压缩级别：%s"); add("Quality: %s", "质量：%s");
    add("Use suffix", "使用后缀"); add("Output suffix", "输出后缀");
    add("Compression level (1–9)", "压缩级别（1–9）");
    add("JPEG base and gain map quality (1–100)", "JPEG Base 与 Gain Map 质量（1–100）");
    add("Quality (1–100)", "质量（1–100）");
    add("Transfer: PQ / ST2084", "传递函数：PQ / ST2084");
    add("HDR input: Ultra HDR JPEG output", "HDR 输入：输出 Ultra HDR JPEG");
    add("Caption 1 (empty: camera from EXIF)", "第 1 行文字（留空：读取 EXIF 机型）");
    add("Caption 2 (empty: exposure from EXIF)", "第 2 行文字（留空：读取 EXIF 曝光参数）");
    add("Output folder", "输出文件夹"); add("Choose output folder", "选择输出文件夹");
    add("Save to: %s", "保存到：%s");
    add("Reset output settings?", "重置输出设置？");
    add("Restore defaults for all output formats.", "恢复所有输出格式的默认设置。");
    add("Cancel", "取消"); add("Reset", "重置"); add("Save", "保存");
    add("Close", "关闭"); add("OK", "确定"); add("Reset format", "重置当前格式");
    add("Output settings", "输出设置"); add("Edit queued task", "编辑队列任务");
    add("SOURCE INSPECTOR", "源文件信息");
    add("Inspecting…", "正在检查…"); add("Inspecting", "正在检查");
    add("Waiting", "等待中"); add("Inspection failed", "检查失败");
    add("Inspection failed: ", "检查失败：");
    add("Resolved color", "解析色彩"); add("Native color signal", "原生色彩信号");
    add("ICC profile signal", "ICC 配置文件信号"); add("Metadata status", "元数据状态");
    add("Gain map family", "Gain Map 类型"); add("Base rendition", "Base 图像");
    add("Gain map layout", "Gain Map 规格"); add("Gain map metadata", "Gain Map 元数据");
    add("Reconstructed HDR", "重建 HDR");
    add("HDR gain map", "HDR Gain Map"); add("Direct HDR", "单层 HDR");
    add("Image", "图像"); add("Ready", "就绪"); add("Completed", "已完成");
    add("Failed", "失败"); add("Skipped", "已跳过"); add("Cancelled", "已取消");
    add("Running", "运行中"); add("Saving", "正在保存"); add("Saved", "已保存");
    add("No HDR data", "无 HDR 数据"); add("Temporary cache cleanup pending", "临时缓存待清理");
    add("Ready %s • Completed %s • Skipped %s • Failed %s • Cancelled %s",
        "就绪 %s • 已完成 %s • 已跳过 %s • 失败 %s • 已取消 %s");
    add("Start task", "开始任务"); add("Clear status", "清除状态");
    add("Edit settings", "编辑设置"); add("Edit output base name", "编辑输出文件名");
    add("Move up", "上移"); add("Move down", "下移"); add("Open source", "打开源文件");
    add("Open output", "打开输出"); add("Details", "详情"); add("Task details", "任务详情");
    add("Output base name (extension is fixed)", "输出文件名（扩展名固定）");
    add("No output yet", "尚无输出"); add("Preview", "预览"); add("Inspect", "检查");
    add("Stop the queue before removing tasks", "请先停止队列，再移除任务");
    add("Stop the queue before editing tasks", "请先停止队列，再编辑任务");
    add("Suffix contains an invalid character", "后缀包含无效字符");
    add("Select an output folder first", "请先选择输出文件夹");
    add("Select files first", "请先选择文件"); add("Added to queue", "已加入队列");
    add("Folder permission was not granted", "未获得文件夹访问权限");
    add("Folder needs read and write access", "文件夹需要读写权限");
    add("Enter a valid base name", "请输入有效的文件名");
    add("No app can open this document", "没有可打开此文件的应用");
    add("Unable to start conversion: ", "无法开始转换：");
    add("HDR preview unavailable: ", "HDR 预览不可用：");
    add("Value must be between 1 and ", "数值必须介于 1 和以下上限之间：");
    add("An update is being installed", "正在安装更新");
    add("Cannot open source", "无法打开源文件"); add("Cannot open output", "无法打开输出文件");
    add("Cannot list output folder", "无法读取输出文件夹");
    add("Cannot create output document", "无法创建输出文件");
    add("Destination is the source document", "输出目标不能与源文件相同");
    add("App updates", "应用更新"); add("Download", "下载"); add("Install", "安装");
    add("Retry", "重试"); add("Cancel installation", "取消安装");
    add("Check for updates", "检查更新"); add("update available", "有可用更新");
    add("check for updates", "检查更新"); add("Later", "稍后");
    add("Version %s is available. Download the update?", "发现新版本 %s。要下载更新吗？");
    add("Downloading update: %s%%", "正在下载更新：%s%%");
    add("Wait for conversion to finish before installing.", "请等待转换完成后再安装。");
    add("Wait for inspection to finish before installing.", "请等待检查完成后再安装。");
    add("Update available", "有可用更新"); add("Up to date", "已是最新版本");
    add("Waiting for installer", "正在等待安装程序");
    add("Please wait %s seconds before checking again.", "请等待 %s 秒后再检查。");
    add("Checking for updates", "正在检查更新");
    add("No Android update available", "暂无 Android 更新");
    add("Update available (last successful check)", "有可用更新（上次成功检查的结果）");
    add("Update check failed: ", "检查更新失败："); add("Downloading update", "正在下载更新");
    add("Update ready to install", "更新已准备好安装"); add("Download failed: ", "下载失败：");
    add("Update installed", "更新已安装");
    add("The previous installation has ended. Check for updates again.", "上次安装已结束，请重新检查更新。");
    add("Cannot open installation settings", "无法打开安装设置");
    add("Installation permission not granted", "未获得安装权限");
    add("Wait for conversion to finish", "请等待转换完成");
    add("Wait for inspection to finish", "请等待检查完成");
    add("Installing update", "正在安装更新"); add("Installation failed: ", "安装失败：");
    add("Missing system confirmation", "缺少系统确认");
    add("Installation requires confirmation", "安装需要确认");
    add("Cannot open system installer. Cancel and retry the update.", "无法打开系统安装程序，请取消后重试更新。");
    add("Cannot cancel installation: ", "无法取消安装：");
    add("Installation cancelled. Check for updates to retry.", "安装已取消，请检查更新后重试。");
    add("Update confirmation", "更新确认");
    add("Update installation requires confirmation", "安装更新需要确认");
    add("No update checksum", "缺少更新校验值"); add("Cannot create update directory", "无法创建更新目录");
    add("Invalid APK size", "APK 大小无效"); add("Download timed out", "下载超时");
    add("APK too large", "APK 过大"); add("APK checksum or size mismatch", "APK 校验值或大小不匹配");
    add("Cannot replace update", "无法替换更新"); add("Cannot save update", "无法保存更新");
    add("Invalid APK checksum", "APK 校验值无效"); add("APK identity mismatch", "APK 应用身份不匹配");
    add("Missing APK signing information", "缺少 APK 签名信息");
    add("Invalid APK signer", "APK 签名无效"); add("APK signer mismatch", "APK 签名不匹配");
    add("APK incompatible with installed signing certificate", "APK 与已安装应用的签名证书不兼容");
    add("Untrusted update URL", "更新地址不受信任"); add("Request timed out", "请求超时");
    add("Invalid redirect", "重定向无效"); add("GitHub API rate limit reached", "已达到 GitHub API 请求频率限制");
    add("Too many redirects", "重定向次数过多"); add("Response too large or timed out", "响应过大或超时");
    add("Conversions", "转换任务"); add("HDR Bridge conversion running", "HDR Bridge 正在转换");
    add("Tap Stop to cancel the current conversion", "点按“停止”可取消当前转换");
    add("HDR preview", "HDR 预览"); add("Preview unavailable", "预览不可用");
    add("Native HDR preview requires Android 14 or later and an HDR display.", "原生 HDR 预览需要 Android 14 或更高版本及 HDR 显示屏。");
    add("No image URI provided.", "未提供图像地址。");
    add("HDR graphics initialization failed.", "HDR 图形初始化失败。");
    add("Unable to decode this image.", "无法解码此图像。");
    add("Invalid image dimensions.", "图像尺寸无效。");
    add("Unable to open this image for HDR preview.", "无法打开此图像进行 HDR 预览。");
    add("Cannot open image", "无法打开图像");
    add("This display cannot present native HDR preview.", "此显示屏无法呈现原生 HDR 预览。");
    add("Rendering frame background", "正在渲染边框背景");
    add("Encoding framed image", "正在编码边框图像");
    add("Frame output verified and committed", "照片边框输出已校验并保存");
    add("Inspecting HEIF container", "正在检查 HEIF 容器");
    add("Color signaling conflict: preserving container/codec YUV decode while using the resolved HDR RGB interpretation", "色彩信号冲突：保留容器及编码器的 YUV 解码，并采用解析后的 HDR RGB 色彩解释");
    add("HDR transfer/gamut resolved from ICC; codec/VUI remains responsible for stored YUV-to-RGB decoding", "已从 ICC 解析 HDR 传递函数与色域；存储的 YUV 到 RGB 解码仍由编解码器/VUI 处理");
    add("BT.2100 HLG OETF/OOTF reconstructed to linear HDR (1000-nit reference)", "已通过 BT.2100 HLG OETF/OOTF 重建线性 HDR（1000 nits 参考）");
    add("High-precision Rec.2020/PQ RGB ready", "高精度 Rec.2020/PQ RGB 已就绪");
    add("Matching native and ICC HDR signaling resolved", "已解析匹配的原生与 ICC HDR 信号");
    add("Decoding 20-tile HEVC image at 16-bit precision", "正在以 16-bit 精度解码 20 分块 HEVC 图像");
    add("Transforming Rec.2020/PQ to linear FP16 scRGB", "正在将 Rec.2020/PQ 转换为线性 FP16 scRGB");
    add("Linear FP16 scRGB transform complete", "线性 FP16 scRGB 转换完成");
    add("Resampling linear HDR preview", "正在重采样线性 HDR 预览");
    add("Parsing AVIF base/gain-map/tmap item graph and ISO metadata", "正在解析 AVIF Base/Gain Map/tmap 结构与 ISO 元数据");
    add("Adobe tmap gain-map reconstructed to canonical high-precision HDR master", "已将 Adobe tmap Gain Map 重建为高精度 HDR 母版");
    add("Parsing TIFF base/gain-map SubIFD and ISO metadata", "正在解析 TIFF Base/Gain Map SubIFD 与 ISO 元数据");
    add("Adobe TIFF gain map reconstructed to canonical high-precision HDR master", "已将 Adobe TIFF Gain Map 重建为高精度 HDR 母版");
    add("Parsing Apple HEIC base/auxiliary/tmap item graph and metadata", "正在解析 Apple HEIC Base/辅助图/tmap 结构与元数据");
    add("Apple HEIC gain map reconstructed to canonical high-precision HDR master", "已将 Apple HEIC Gain Map 重建为高精度 HDR 母版");
    add("Inspecting high-dynamic-range input", "正在检查 HDR 输入");
    add("Parsing JPEG XL jhgm base, gain-map codestream, and ISO metadata", "正在解析 JPEG XL jhgm Base、Gain Map 码流与 ISO 元数据");
    add("JPEG XL jhgm reconstructed to canonical high-precision HDR master", "已将 JPEG XL jhgm 重建为高精度 HDR 母版");
    add("Parsing Apple JPEG base/MPF gain map and ISO metadata", "正在解析 Apple JPEG Base/MPF Gain Map 与 ISO 元数据");
    add("Canonical high-precision HDR master ready", "高精度 HDR 母版已就绪");
    add("Source decoded; no HDR representation found", "源文件已解码；未检测到 HDR 数据");
    add("Reusing cached canonical high-precision HDR master", "正在复用已缓存的高精度 HDR 母版");
    add("Parsing Gain Map container and metadata", "正在解析 Gain Map 容器与元数据");
    add("Copying original Gain Map image", "正在复制原始 Gain Map");
    add("Writing decoded Gain Map image", "正在写入解码后的 Gain Map");
    add("Gain Map export committed", "Gain Map 导出已保存");
    add("Compositing source alpha over black in linear light for JPEG output", "正在线性光下将 Alpha 通道合成至黑色背景以输出 JPEG");
    add("Compositing source alpha over black in linear light for RGB-only encoder path", "正在线性光下将 Alpha 通道合成至黑色背景以供纯 RGB 编码");
    add("Encoding SDR base and ISO 21496-1 jhgm JPEG XL gain map", "正在编码 SDR Base 与 ISO 21496-1 jhgm JPEG XL Gain Map");
    add("Verifying JPEG XL jhgm metadata and HDR reconstruction", "正在校验 JPEG XL jhgm 元数据与 HDR 重建");
    add("Encoding JPEG XL BT.2100 HLG master", "正在编码 JPEG XL BT.2100 HLG 母版");
    add("Encoding JPEG XL lossless PQ master", "正在编码 JPEG XL 无损 PQ 母版");
    add("Verifying JPEG XL decode and RGB16 equality", "正在校验 JPEG XL 解码与 RGB16 一致性");
    add("Encoding JPEG XR FP16 scRGB with jxrlib", "正在使用 jxrlib 编码 JPEG XR FP16 scRGB");
    add("Encoding JPEG XR FP16 scRGB with Windows WIC", "正在使用 Windows WIC 编码 JPEG XR FP16 scRGB");
    add("Checking portable JPEG XR FP16 output", "正在检查跨平台兼容的 JPEG XR FP16 输出");
    add("Verifying JPEG XR through WIC FP16 decode", "正在通过 WIC FP16 解码校验 JPEG XR");
    add("Encoding faithful SDR base and ISO 21496-1 gain map with measured capacity", "正在按实测容量编码保真 SDR Base 与 ISO 21496-1 Gain Map");
    add("Resampling linear HDR to the JPEG 65500-pixel codec limit", "正在将线性 HDR 重采样至 JPEG 65500 像素尺寸上限内");
    add("Verifying gain-map capacity and linear-light HDR reconstruction", "正在校验 Gain Map 容量与线性光 HDR 重建");
    add("Encoding direct HDR TIFF RGB16 with Deflate and HDR CICP ICC", "正在编码单层 HDR TIFF RGB16（Deflate + HDR CICP ICC）");
    add("Encoding uncompressed direct HDR TIFF RGB16 with HDR CICP ICC", "正在编码未压缩单层 HDR TIFF RGB16（HDR CICP ICC）");
    add("Verifying TIFF RGB16 equality and direct-HDR ICC", "正在校验 TIFF RGB16 一致性与单层 HDR ICC");
    add("Encoding SDR base and ISO 21496-1 gain map AVIF", "正在编码 SDR Base 与 ISO 21496-1 Gain Map AVIF");
    add("Verifying AVIF gain-map item graph and HDR reconstruction", "正在校验 AVIF Gain Map 结构与 HDR 重建");
    add("Encoding direct 10-bit 4:4:4 BT.2100 HLG AVIF", "正在编码单层 10-bit 4:4:4 BT.2100 HLG AVIF");
    add("Encoding direct 10-bit 4:4:4 PQ AVIF compatibility candidate", "正在编码单层 10-bit 4:4:4 PQ AVIF 兼容候选格式");
    add("Verifying AV1 10-bit 4:4:4 and HDR CICP signaling", "正在校验 AV1 10-bit 4:4:4 与 HDR CICP 信号");
    add("Verified output committed atomically", "输出已校验并安全写入完成");
    for (String layout : new String[] {"RGB16", "RGBA16"}) {
      add("Encoding direct HDR PNG " + layout + " with BT.2100 HLG cICP", "正在以 BT.2100 HLG cICP 编码单层 HDR PNG " + layout);
      add("Encoding direct HDR PNG " + layout + " with PQ cICP and ICC", "正在以 PQ cICP 和 ICC 编码单层 HDR PNG " + layout);
      add("Encoding PNG " + layout + " with standard PQ cICP only", "正在仅以标准 PQ cICP 编码 PNG " + layout);
      add("Verifying PNG " + layout + " equality and cICP signaling", "正在校验 PNG " + layout + " 一致性与 cICP 信号");
    }
  }
  private static void add(String en, String zh) { ZH.put(en, zh); }

  static boolean isSimplifiedChinese(Locale locale) {
    if (locale == null || !"zh".equalsIgnoreCase(locale.getLanguage())) return false;
    String script = locale.getScript(), region = locale.getCountry();
    if ("Hant".equalsIgnoreCase(script)) return false;
    if ("Hans".equalsIgnoreCase(script)) return true;
    if (!script.isEmpty()) return false;
    return "CN".equalsIgnoreCase(region) || "SG".equalsIgnoreCase(region)
        || "MY".equalsIgnoreCase(region);
  }
  static synchronized String language(Context context) {
    SharedPreferences prefs = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
    String saved = prefs.getString(LANGUAGE, "");
    if ("zh-CN".equals(saved) || "en".equals(saved)) return saved;
    Locale system = Resources.getSystem().getConfiguration().getLocales().get(0);
    saved = isSimplifiedChinese(system) ? "zh-CN" : "en";
    prefs.edit().putString(LANGUAGE, saved).commit();
    return saved;
  }
  static boolean chinese(Context context) { return "zh-CN".equals(language(context)); }
  static void toggle(Context context) {
    context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
        .putString(LANGUAGE, chinese(context) ? "en" : "zh-CN").commit();
  }
  static String get(Context context, String english) { return translate(chinese(context), english); }
  static String format(Context context, String english, Object... args) {
    return String.format(Locale.ROOT, get(context, english), args);
  }
  static String[] choices(Context context, String... values) {
    String[] translated = new String[values.length];
    for (int i = 0; i < values.length; i++) translated[i] = get(context, values[i]);
    return translated;
  }
  /** Only known presentation phrases are translated; codec errors and raw data pass through. */
  static String translate(boolean chinese, String english) {
    if (english == null) return "";
    if (!chinese) return english;
    String exact = ZH.get(english);
    if (exact != null) return exact;
    for (Map.Entry<String, String> pair : ZH.entrySet()) {
      String key = pair.getKey();
      if (key.endsWith(": ") && english.startsWith(key))
        return pair.getValue() + translate(true, english.substring(key.length()));
    }
    if (english.startsWith("Value must be between 1 and "))
      return "数值必须介于 1 和 " + english.substring("Value must be between 1 and ".length()) + " 之间";
    if (english.startsWith("Please wait ") && english.endsWith(" seconds before checking again."))
      return "请等待 " + english.substring("Please wait ".length(),
          english.length() - " seconds before checking again.".length()) + " 秒后再检查。";
    // Detail text contains only engine status and technical diagnostics, never user filenames.
    String suffix = " • Temporary cache cleanup pending";
    if (english.endsWith(suffix))
      return translate(true, english.substring(0, english.length() - suffix.length())) + " • 临时缓存待清理";
    if (english.startsWith("Saved • ")) return "已保存 • " + english.substring(8);
    return english;
  }
  static void bind(TextView view, String english, Object... args) {
    // Editable content is user data and must never participate in language refresh.
    if (view instanceof EditText) throw new IllegalArgumentException("Cannot bind editable content");
    Binding binding = new Binding(english, args);
    BINDINGS.put(view, binding);
    view.setText(binding.render(view.getContext()));
  }
  static void refresh(Activity activity) {
    for (Map.Entry<TextView, Binding> entry : new ArrayList<>(BINDINGS.entrySet())) {
      TextView view = entry.getKey();
      if (view != null && view.getContext() == activity)
        view.setText(entry.getValue().render(activity));
    }
  }
  private UiStrings() { }
}
