package org.hdrbridge;

import android.content.SharedPreferences;
import org.json.*;

/** A detached task snapshot. Keys are also the native core option names. */
final class Options {
  static final String[] MODES = {"ultrahdr", "png-pq16", "tiff-pq16", "jxl-pq16", "jxr-scrgb-fp16",
      "avif-pq10", "gainmap-extract", "photo-frame"};
  static final String[] LABELS = {"Ultra HDR JPEG", "HDR PNG", "HDR TIFF", "JPEG XL",
      "JPEG XR FP16", "HDR AVIF", "Extract Gain Map", "Photo Frame"};
  int format, quality, transfer, scale, channels, extraction;
  int frameJpegQuality = 95, framePngCompression = 4;
  boolean lossless, compressed, exif, xmp, useSuffix;
  String gamut, suffix;
  String frameFormat = "jpeg";
  String caption1 = "", caption2 = "";
  static Options defaults(int f) {
    Options o = new Options();
    o.format = f;
    o.quality = new int[] {95, 4, 6, 90, 100, 95, 95, 95}[f];
    o.lossless = f == 1 || f == 2 || f == 3 || f == 4;
    o.compressed = true;
    o.exif = o.xmp = f != 7;
    o.useSuffix = true;
    o.scale = 1;
    o.channels = (f == 0 || f == 7) ? 1 : 0;
    o.gamut = (f == 0 || f == 7) ? "p3" : "rec2020";
    o.suffix = defaultSuffix(f, 0);
    return o;
  }
  static String defaultSuffix(int f, int tr) {
    switch (f) {
      case 0:
        return "_ultrahdr";
      case 1:
        return tr == 1 ? "_hdr-hlg" : "_hdr-pq16";
      case 2:
        return "_hdr-pq16";
      case 3:
        return tr == 2 ? "_gainmap" : tr == 1 ? "_hlg16" : "_pq16";
      case 4:
        return "_scrgb-fp16";
      case 5:
        return tr == 2 ? "_gainmap" : tr == 1 ? "_direct-hlg10" : "_direct-pq10";
      case 7:
        return "_frame";
      default:
        return "_gainmap";
    }
  }
  boolean framePng() {
    return format == 7 && "png".equals(frameFormat);
  }
  boolean compressionFormat() {
    return format == 1 || format == 2 || framePng();
  }
  void applyTransfer(int nextTransfer) {
    String oldDefault = defaultSuffix(format, transfer);
    transfer = nextTransfer;
    if (suffix.equals(oldDefault)) {
      suffix = defaultSuffix(format, nextTransfer);
    }
  }
  void carryCustomSuffixFrom(Options previous) {
    if (previous != null && !previous.suffix.equals(defaultSuffix(previous.format, previous.transfer))) {
      suffix = previous.suffix;
    }
  }
  void setFrameFormat(String next) {
    String normalized = "png".equals(next) ? "png" : "jpeg";
    if (normalized.equals(frameFormat)) return;
    if ("png".equals(normalized)) {
      frameJpegQuality = Math.max(1, Math.min(100, quality));
      frameFormat = "png";
      quality = framePngCompression;
      gamut = "rec2020";
      lossless = true;
    } else {
      framePngCompression = Math.max(1, Math.min(9, quality));
      frameFormat = "jpeg";
      quality = frameJpegQuality;
      gamut = "p3";
      lossless = false;
    }
  }
  boolean gainmap() {
    return format == 0 || (format == 7 && !framePng())
        || ((format == 3 || format == 5) && transfer == 2);
  }
  private String effectiveGamut() {
    if (format == 0 || (format == 7 && !framePng()) || "p3".equals(gamut)) return "p3";
    if ("rec2020".equals(gamut)) return "rec2020";
    return gainmap() ? "p3" : "rec2020";
  }
  JSONObject json() throws JSONException {
    JSONObject j = new JSONObject();
    j.put("mode", MODES[format]);
    j.put("frame_format", framePng() ? "png" : "jpeg");
    j.put("frame_caption1", caption1);
    j.put("frame_caption2", caption2);
    j.put("lossless", framePng() || (format != 7 && (lossless || format == 1 || format == 2)));
    int jpegQuality = framePng() ? frameJpegQuality : quality;
    j.put("image_quality", jpegQuality / 100.0);
    j.put("base_quality", jpegQuality);
    j.put("gainmap_quality", jpegQuality);
    j.put("png_compression_level",
        format == 1 || framePng() ? Math.max(1, Math.min(9, quality)) : 4);
    j.put("tiff_compression_level", quality);
    j.put("tiff_compressed", compressed);
    j.put("gainmap_scale", scale);
    j.put("multi_channel_gainmap", (format == 0 || (format == 7 && !framePng())) ? channels == 1 : gainmap());
    j.put("output_gamut", effectiveGamut());
    j.put("output_transfer", (format == 7 && !framePng()) ? "pq" : (transfer == 1 ? "hlg" : "pq"));
    j.put("output_representation", transfer == 2 ? "gainmap" : "direct");
    j.put("gainmap_export_format", new String[] {"original", "png", "tiff", "jpeg"}[extraction]);
    j.put("copy_exif", format != 7 && exif);
    j.put("copy_xmp", format != 7 && xmp);
    return j;
  }
  JSONObject save() throws JSONException {
    JSONObject j = json();
    j.put("format", format);
    j.put("quality", quality);
    j.put("frameJpegQuality", framePng() ? frameJpegQuality : quality);
    j.put("framePngCompression", framePng() ? quality : framePngCompression);
    j.put("transferIndex", transfer);
    j.put("scaleIndex", scale);
    j.put("channelsIndex", channels);
    j.put("extractionIndex", extraction);
    j.put("useSuffix", useSuffix);
    j.put("suffix", suffix);
    return j;
  }
  static Options from(JSONObject j) {
    int f = Math.max(0, Math.min(7, j.optInt("format", 0)));
    Options o = defaults(f);
    o.frameFormat = "png".equals(j.optString("frame_format", "jpeg")) ? "png" : "jpeg";
    o.caption1 = j.optString("frame_caption1", "");
    o.caption2 = j.optString("frame_caption2", "");
    o.quality = j.optInt("quality", o.quality);
    o.transfer = j.optInt("transferIndex", 0);
    o.scale = j.optInt("scaleIndex", o.scale);
    o.channels = j.optInt("channelsIndex", o.channels);
    o.extraction = j.optInt("extractionIndex", 0);
    o.lossless = j.optBoolean("lossless", o.lossless);
    o.compressed = j.optBoolean("tiff_compressed", true);
    o.exif = j.optBoolean("copy_exif", true);
    o.xmp = j.optBoolean("copy_xmp", true);
    if (f == 7) {
      o.lossless = o.framePng();
      o.exif = o.xmp = false;
      int maxQuality = o.framePng() ? 9 : 100;
      o.quality = Math.max(1, Math.min(maxQuality,
          j.optInt("quality", o.framePng() ? 4 : 95)));
      o.frameJpegQuality = Math.max(1, Math.min(100,
          j.optInt("frameJpegQuality", o.framePng() ? 95 : o.quality)));
      o.framePngCompression = Math.max(1, Math.min(9,
          j.optInt("framePngCompression", o.framePng() ? o.quality : 4)));
    }
    o.gamut = j.optString("output_gamut", o.gamut);
    o.gamut = o.effectiveGamut();
    o.useSuffix = j.optBoolean("useSuffix", true);
    o.suffix = j.optString("suffix", o.suffix);
    return o;
  }
  static Options load(SharedPreferences p, int f) {
    try {
      return from(new JSONObject(p.getString("options_" + f, defaults(f).save().toString())));
    } catch (Exception e) {
      return defaults(f);
    }
  }
  Options copy() {
    try {
      return from(save());
    } catch (Exception e) {
      throw new IllegalStateException(e);
    }
  }
  String filename(String source) {
    int d = source.lastIndexOf('.');
    String base = d > 0 ? source.substring(0, d) : source;
    String ext = new String[] {".jpg", ".png", ".tif", ".jxl", ".jxr", ".avif", ".png", ".jpg"}[format];
    if (format == 7) {
      ext = framePng() ? ".png" : ".jpg";
    }
    if (format == 6) {
      if (extraction == 1)
        ext = ".png";
      else if (extraction == 2)
        ext = ".tif";
      else if (extraction == 3)
        ext = ".jpg";
      else {
        String x = d < 0 ? "" : source.substring(d).toLowerCase(java.util.Locale.ROOT);
        ext = (x.equals(".jpg") || x.equals(".jpeg") || x.equals(".jpe")) ? ".jpg"
            : x.equals(".jxl")                                            ? ".jxl"
            : (x.equals(".tif") || x.equals(".tiff"))                     ? ".tif"
                                                                          : ".png";
      }
    }
    return base + (useSuffix ? suffix : "") + ext;
  }
}
