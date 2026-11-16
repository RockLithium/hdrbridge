package org.hdrbridge;

import static org.junit.Assert.*;

import org.json.JSONObject;
import org.junit.Test;

public class OptionsTest {
  @Test
  public void defaultsMapToExpectedNativeFormatsAndQualities() throws Exception {
    String[] modes = {"ultrahdr", "png-pq16", "tiff-pq16", "jxl-pq16", "jxr-scrgb-fp16",
        "avif-pq10", "gainmap-extract"};
    int[] qualities = {95, 4, 6, 100, 100, 90, 95};
    for (int i = 0; i < modes.length; i++) {
      Options option = Options.defaults(i);
      JSONObject nativeOptions = option.json();
      assertEquals(modes[i], nativeOptions.getString("mode"));
      assertEquals(qualities[i], option.quality);
      assertEquals(qualities[i] / 100.0, nativeOptions.getDouble("image_quality"), 0.000001);
      assertEquals(qualities[i], nativeOptions.getInt("base_quality"));
      assertEquals(i == 0 ? "p3" : "rec2020", nativeOptions.getString("output_gamut"));
      assertEquals("pq", nativeOptions.getString("output_transfer"));
      assertEquals("direct", nativeOptions.getString("output_representation"));
      assertTrue(nativeOptions.getBoolean("copy_exif"));
      assertTrue(nativeOptions.getBoolean("copy_xmp"));
    }
    assertEquals(4, Options.defaults(1).json().getInt("png_compression_level"));
    assertEquals(6, Options.defaults(2).json().getInt("tiff_compression_level"));
  }

  @Test
  public void gainMapSnapshotsPreserveRequestedRepresentation() throws Exception {
    for (int format : new int[] {3, 5}) {
      Options o = Options.defaults(format);
      o.transfer = 2;
      o.gamut = "p3";
      o.scale = 4;
      o.peak = 2;
      JSONObject j = o.json();
      assertEquals("gainmap", j.getString("output_representation"));
      assertEquals("p3", j.getString("output_gamut"));
      assertTrue(j.getBoolean("multi_channel_gainmap"));
      assertEquals(4, j.getInt("gainmap_scale"));
      assertEquals(2000, j.getInt("target_peak_nits"));
      assertEquals("_gainmap", Options.defaultSuffix(format, 2));
    }
    Options jpeg = Options.defaults(0);
    assertFalse(jpeg.json().getBoolean("multi_channel_gainmap"));
    jpeg.channels = 1;
    jpeg.gamut = "rec709";
    jpeg.scale = 1;
    assertTrue(jpeg.json().getBoolean("multi_channel_gainmap"));
    assertEquals("rec709", jpeg.json().getString("output_gamut"));
    assertEquals(1, jpeg.json().getInt("gainmap_scale"));
  }

  @Test
  public void copyIsDetachedAndPreservesOverrides() throws Exception {
    Options original = Options.defaults(5);
    original.transfer = 1;
    original.gamut = "p3";
    original.quality = 73;
    original.lossless = false;
    original.exif = false;
    original.xmp = false;
    original.suffix = "_custom";
    original.useSuffix = false;
    Options copy = original.copy();
    original.quality = 20;
    original.gamut = "rec2020";
    original.suffix = "_changed";
    JSONObject j = copy.json();
    assertEquals("hlg", j.getString("output_transfer"));
    assertEquals("p3", j.getString("output_gamut"));
    assertEquals(0.73, j.getDouble("image_quality"), 0.000001);
    assertFalse(j.getBoolean("copy_exif"));
    assertFalse(j.getBoolean("copy_xmp"));
    assertEquals("photo.avif", copy.filename("photo.heic"));
    copy.useSuffix = true;
    assertEquals("photo_custom.avif", copy.filename("photo.heic"));
  }

  @Test
  public void extractionAndSuffixChooseExpectedNamesAndOptions() throws Exception {
    Options o = Options.defaults(6);
    assertEquals("frame_gainmap.jpg", o.filename("frame.JPEG"));
    assertEquals("frame_gainmap.jxl", o.filename("frame.jxl"));
    assertEquals("frame_gainmap.png", o.filename("frame.avif"));
    o.extraction = 2;
    assertEquals("tiff", o.json().getString("gainmap_export_format"));
    assertEquals("frame_gainmap.tif", o.filename("frame.heic"));
    o.extraction = 3;
    assertEquals("jpeg", o.json().getString("gainmap_export_format"));
    assertEquals("frame_gainmap.jpg", o.filename("frame.heic"));
    assertEquals("_hdr-hlg", Options.defaultSuffix(1, 1));
    assertEquals("_direct-hlg10", Options.defaultSuffix(5, 1));
  }
  @Test public void frameSnapshotPreservesSettingsAndFilename() throws Exception {
    Options o = Options.defaults(7);
    assertEquals("photo-frame", o.json().getString("mode"));
    assertEquals("photo_frame.jpg", o.filename("photo.png"));
    o.caption1 = "Camera"; o.caption2 = "Exposure";
    Options copy = o.copy(); o.caption1 = "Changed";
    assertEquals("photo_frame.jpg", copy.filename("photo.jpg"));
    assertEquals("Camera", copy.json().getString("frame_caption1"));
    assertEquals("Exposure", copy.json().getString("frame_caption2"));
    assertEquals(95, copy.json().getInt("base_quality"));
    assertEquals("jpeg", copy.json().getString("frame_format"));
    assertEquals("p3", copy.json().getString("output_gamut"));
  }

}
