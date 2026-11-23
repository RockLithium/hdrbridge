package org.hdrbridge;

import static org.junit.Assert.*;

import org.json.JSONObject;
import org.junit.Test;

public class OptionsTest {
  @Test
  public void defaultsMapToExpectedNativeFormatsAndQualities() throws Exception {
    String[] modes = {"ultrahdr", "png-pq16", "tiff-pq16", "jxl-pq16", "jxr-scrgb-fp16",
        "avif-pq10", "gainmap-extract"};
    int[] qualities = {95, 4, 6, 90, 100, 95, 95};
    for (int i = 0; i < modes.length; i++) {
      Options option = Options.defaults(i);
      JSONObject nativeOptions = option.json();
      assertEquals("mode at index " + i, modes[i], nativeOptions.getString("mode"));
      assertEquals(modes[i], qualities[i] / 100.0, nativeOptions.getDouble("image_quality"), 0.000001);
      assertEquals(i == 0 ? "p3" : "rec2020", nativeOptions.getString("output_gamut"));
      assertEquals("pq", nativeOptions.getString("output_transfer"));
      assertEquals("direct", nativeOptions.getString("output_representation"));
    }
    JSONObject jpeg = Options.defaults(0).json();
    assertEquals(95, jpeg.getInt("base_quality"));
    assertEquals(95, jpeg.getInt("gainmap_quality"));
    assertTrue(jpeg.getBoolean("copy_exif"));
    assertTrue(jpeg.getBoolean("copy_xmp"));
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
      JSONObject j = o.json();
      assertEquals("gainmap", j.getString("output_representation"));
      assertEquals("p3", j.getString("output_gamut"));
      assertTrue(j.getBoolean("multi_channel_gainmap"));
      assertEquals(4, j.getInt("gainmap_scale"));
      assertEquals("p3", Options.from(o.save().put("output_gamut", "rec709")).gamut);
      assertEquals("rec2020", Options.from(o.save().put("output_gamut", "rec2020")).gamut);
    }
    Options jpeg = Options.defaults(0);
    assertTrue(jpeg.json().getBoolean("multi_channel_gainmap"));
    assertEquals(1, jpeg.json().getInt("gainmap_scale"));
    jpeg.channels = 0;
    jpeg.scale = 2;
    assertFalse(jpeg.json().getBoolean("multi_channel_gainmap"));
    assertEquals("p3", Options.from(jpeg.save().put("output_gamut", "rec709")).gamut);
    assertEquals(2, jpeg.json().getInt("gainmap_scale"));
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
    assertEquals("rec2020", Options.from(copy.save().put("output_gamut", "rec709")).gamut);
  }

  @Test
  public void outputNamesFollowFormatAndTransfer() throws Exception {
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

    assertTransferSuffixes(1, "_hdr-pq16", "_hdr-hlg");
    assertTransferSuffixes(3, "_pq16", "_hlg16", "_gainmap");
    assertTransferSuffixes(5, "_direct-pq10", "_direct-hlg10", "_gainmap");

    Options hlg = Options.defaults(1);
    hlg.suffix = "_HDR";
    hlg.applyTransfer(1);
    assertEquals("photo_HDR.png", hlg.filename("photo.heic"));

    Options frame = Options.defaults(7);
    frame.suffix = "_HDR";
    frame.setFrameFormat("png");
    frame.applyTransfer(1);
    assertEquals("photo_HDR.png", frame.filename("photo.heic"));

    Options avif = Options.defaults(5);
    avif.carryCustomSuffixFrom(Options.defaults(1));
    assertEquals("_direct-pq10", avif.suffix);
    avif.carryCustomSuffixFrom(frame);
    assertEquals("photo_HDR.avif", avif.filename("photo.png"));
  }

  private static void assertTransferSuffixes(int format, String... suffixes) {
    Options option = Options.defaults(format);
    for (int transfer = 1; transfer < suffixes.length; transfer++) {
      option.applyTransfer(transfer);
      assertEquals(Options.LABELS[format], suffixes[transfer], option.suffix);
    }
    option.applyTransfer(0);
    assertEquals(Options.LABELS[format], suffixes[0], option.suffix);
  }

  @Test public void frameSnapshotPreservesSettingsAndFilename() throws Exception {
    Options o = Options.defaults(7);
    assertEquals("photo_frame.jpg", o.filename("photo.png"));
    o.caption1 = "Camera"; o.caption2 = "Exposure";
    Options copy = o.copy(); o.caption1 = "Changed";
    JSONObject jpeg = copy.json();
    assertEquals("photo-frame", jpeg.getString("mode"));
    assertEquals("Camera", jpeg.getString("frame_caption1"));
    assertEquals("Exposure", jpeg.getString("frame_caption2"));
    assertEquals("jpeg", jpeg.getString("frame_format"));
    assertEquals("p3", jpeg.getString("output_gamut"));
    assertFalse(jpeg.getBoolean("copy_exif"));
    assertFalse(jpeg.getBoolean("copy_xmp"));
    assertEquals("p3", Options.from(o.save().put("output_gamut", "rec709")).gamut);

    o.quality = 87;
    o.setFrameFormat("png");
    assertEquals("rec2020", o.gamut);
    assertEquals(4, o.quality);
    o.gamut = "p3";
    o.quality = 7;
    o.applyTransfer(1);
    Options pngCopy = o.copy();
    JSONObject png = pngCopy.json();
    assertEquals("png", png.getString("frame_format"));
    assertEquals("p3", png.getString("output_gamut"));
    assertEquals("hlg", png.getString("output_transfer"));
    assertEquals(7, png.getInt("png_compression_level"));
    assertTrue(png.getBoolean("lossless"));
    pngCopy.setFrameFormat("jpeg");
    assertEquals(87, pngCopy.quality);
    assertEquals("pq", pngCopy.json().getString("output_transfer"));
    pngCopy.setFrameFormat("png");
    assertEquals(7, pngCopy.quality);
  }

}
