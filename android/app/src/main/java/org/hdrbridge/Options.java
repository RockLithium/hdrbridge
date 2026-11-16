package org.hdrbridge;

import android.content.SharedPreferences;
import org.json.*;

/** A detached task snapshot. Keys are also the native core option names. */
final class Options {
  static final String[] MODES = {"ultrahdr", "png-pq16", "tiff-pq16", "jxl-pq16", "jxr-scrgb-fp16",
      "avif-pq10", "gainmap-extract", "photo-frame"};
  static final String[] LABELS = {"Ultra HDR JPEG", "HDR PNG", "HDR TIFF", "JPEG XL",
      "JPEG XR FP16", "HDR AVIF", "Extract Gain Map", "Photo Frame"};
  int format, quality, transfer, scale, channels, peak, extraction;
  boolean lossless, compressed, exif, xmp, useSuffix;
  String gamut, suffix;
  String caption1 = "", caption2 = "";
  static Options defaults(int f) {
    Options o = new Options();
    o.format = f;
    o.quality = new int[] {95, 4, 6, 100, 100, 90, 95, 95}[f];
    o.lossless = f == 1 || f == 2 || f == 3 || f == 4;
    o.compressed = true;
    o.exif = o.xmp = f != 7;
    o.useSuffix = true;
    o.scale = f == 7 ? 1 : 2;
    o.channels = f == 7 ? 1 : 0;
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
  boolean gainmap() {
    return format == 0 || format == 7 || ((format == 3 || format == 5) && transfer == 2);
  }
  JSONObject json() throws JSONException {
    JSONObject j = new JSONObject();
    j.put("mode", MODES[format]);
    j.put("frame_format", "jpeg");
    j.put("frame_caption1", caption1);
    j.put("frame_caption2", caption2);
    j.put("lossless", format != 7 && (lossless || format == 1 || format == 2));
    j.put("image_quality", quality / 100.0);
    j.put("base_quality", quality);
    j.put("gainmap_quality", quality);
    j.put("png_compression_level", format == 7 ? 4 : quality);
    j.put("tiff_compression_level", quality);
    j.put("tiff_compressed", compressed);
    j.put("gainmap_scale", scale);
    j.put("multi_channel_gainmap", (format == 0 || format == 7) ? channels == 1 : gainmap());
    j.put("target_peak_nits", new int[] {0, 1000, 2000, 4000, 10000}[peak]);
    j.put("output_gamut", gamut);
    j.put("output_transfer", transfer == 1 ? "hlg" : "pq");
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
    j.put("transferIndex", transfer);
    j.put("scaleIndex", scale);
    j.put("channelsIndex", channels);
    j.put("peakIndex", peak);
    j.put("extractionIndex", extraction);
    j.put("useSuffix", useSuffix);
    j.put("suffix", suffix);
    return j;
  }
  static Options from(JSONObject j) {
    int f = Math.max(0, Math.min(7, j.optInt("format", 0)));
    Options o = defaults(f);
    o.caption1 = j.optString("frame_caption1", "");
    o.caption2 = j.optString("frame_caption2", "");
    o.quality = j.optInt("quality", o.quality);
    o.transfer = j.optInt("transferIndex", 0);
    o.scale = j.optInt("scaleIndex", o.scale);
    o.channels = j.optInt("channelsIndex", o.channels);
    o.peak = j.optInt("peakIndex", 0);
    o.extraction = j.optInt("extractionIndex", 0);
    o.lossless = j.optBoolean("lossless", o.lossless);
    o.compressed = j.optBoolean("tiff_compressed", true);
    o.exif = j.optBoolean("copy_exif", true);
    o.xmp = j.optBoolean("copy_xmp", true);
    if (f == 7) o.lossless = o.exif = o.xmp = false;
    o.gamut = j.optString("output_gamut", o.gamut);
    if (f == 7 && !o.gamut.equals("p3") && !o.gamut.equals("rec709")) o.gamut = "p3";
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
