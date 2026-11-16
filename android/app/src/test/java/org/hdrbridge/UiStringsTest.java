package org.hdrbridge;

import static org.junit.Assert.*;
import java.util.Locale;
import org.junit.Test;

public class UiStringsTest {
  @Test public void onlySimplifiedSystemLocalesChooseChinese() {
    for (String tag : new String[] {"zh-CN", "zh-SG", "zh-MY", "zh-Hans", "zh-Hans-CN", "zh-Hans-TW"})
      assertTrue(tag, UiStrings.isSimplifiedChinese(Locale.forLanguageTag(tag)));
    for (String tag : new String[] {"zh-TW", "zh-HK", "zh-MO", "zh-Hant", "zh-Hant-CN",
        "zh", "zh-US", "zh-Latn-CN", "en", "en-US", "ja-JP", "fr-FR", "ar", "de-DE"})
      assertFalse(tag, UiStrings.isSimplifiedChinese(Locale.forLanguageTag(tag)));
    assertFalse(UiStrings.isSimplifiedChinese(null));
  }

  @Test public void ordinaryStatusAndUpdateMessagesArePresentationOnly() {
    assertEquals("已完成", UiStrings.translate(true, "Completed"));
    assertEquals("正在下载更新", UiStrings.translate(true, "Downloading update"));
    assertEquals("请等待 59 秒后再检查。",
        UiStrings.translate(true, "Please wait 59 seconds before checking again."));
    assertEquals("数值必须介于 1 和 100 之间",
        UiStrings.translate(true, "Value must be between 1 and 100"));
    assertEquals("已保存 • 3000 × 2000 • 16 bit • 临时缓存待清理",
        UiStrings.translate(true, "Saved • 3000 × 2000 • 16 bit • Temporary cache cleanup pending"));
    assertEquals("下载失败：下载超时", UiStrings.translate(true, "Download failed: Download timed out"));
  }

  @Test public void technicalDataIsPreservedAndEnglishIsExact() {
    for (String raw : new String[] {"rec2020", "p3", "_gainmap", "PQ / ST2084", "JPEG XL",
        "IMG_2026.jpg", "content://media/external/images/media/42", "jxrlib: unsupported pixel format",
        "Saved custom caption", "gainMapMetadata", "10-bit RGB 4:4:4"}) {
      assertEquals(raw, UiStrings.translate(true, raw));
      assertEquals(raw, UiStrings.translate(false, raw));
    }
    assertEquals("Completed", UiStrings.translate(false, "Completed"));
    assertEquals("Download failed: Download timed out",
        UiStrings.translate(false, "Download failed: Download timed out"));
  }
}
