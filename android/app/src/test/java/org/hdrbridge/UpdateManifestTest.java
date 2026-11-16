package org.hdrbridge;

import static org.junit.Assert.*;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public class UpdateManifestTest {
  private static JSONObject asset(String name, String url, long size, String digest) throws Exception {
    JSONObject a = new JSONObject().put("name", name).put("browser_download_url", url).put("size", size);
    if (digest != null) a.put("digest", digest);
    return a;
  }
  private static JSONObject release(JSONArray assets) throws Exception {
    return new JSONObject().put("tag_name", "v1.2.3").put("draft", false).put("prerelease", false).put("assets", assets);
  }
  private static String apkName() { return "HDRBridge-v1.2.3-Android-arm64.apk"; }
  private static String apkUrl() { return "https://github.com/RockLithium/hdrbridge/releases/download/v1.2.3/" + apkName(); }

  @Test public void parsesAndroidAssetAndPrefersApiDigest() throws Exception {
    JSONObject json = release(new JSONArray().put(asset(apkName(), apkUrl(), 1234, "sha256:" + "a".repeat(64)))
        .put(asset("HDRBridge-v1.2.3-Windows-x64.zip", "https://example.com/x", 4, null)));
    UpdateManifest.Release r = UpdateManifest.parseLatest(json.toString());
    assertEquals("1.2.3", r.version); assertEquals(apkName(), r.apkName);
    assertEquals(1002003L, r.versionCode); assertEquals(1234, r.apkSize);
    assertEquals("a".repeat(64), r.apkSha256);
  }
  @Test public void ordinaryNoAndroidDraftAndPrereleaseReturnNull() throws Exception {
    assertNull(UpdateManifest.parseLatest(release(new JSONArray().put(asset("HDRBridge-v1.2.3-Windows.zip", "https://github.com/RockLithium/hdrbridge/releases/download/v1.2.3/x", 12, null))).toString()));
    assertNull(UpdateManifest.parseLatest(release(new JSONArray()).put("draft", true).toString()));
    assertNull(UpdateManifest.parseLatest(release(new JSONArray()).put("prerelease", true).toString()));
  }
  @Test public void rejectsDuplicateApkAndUnsafeUrls() throws Exception {
    try { UpdateManifest.parseLatest(release(new JSONArray().put(asset(apkName(), apkUrl(), 1, null)).put(asset(apkName(), apkUrl(), 1, null))).toString()); fail(); }
    catch (IllegalArgumentException expected) {}
    String[] bad = {"http://github.com/RockLithium/hdrbridge/releases/download/v1.2.3/" + apkName(),
        "https://github.com.evil/RockLithium/hdrbridge/releases/download/v1.2.3/" + apkName(),
        "https://user@github.com/RockLithium/hdrbridge/releases/download/v1.2.3/" + apkName(),
        "https://github.com:443/RockLithium/hdrbridge/releases/download/v1.2.3/" + apkName(),
        "https://github.com/RockLithium/hdrbridge/releases/download/v1.2.3/%2e%2e/x.apk?x=1"};
    for (String u : bad) try { UpdateManifest.parseLatest(release(new JSONArray().put(asset(apkName(), u, 1, null))).toString()); fail(u); }
    catch (IllegalArgumentException expected) {}
  }
  @Test public void checksumFallbackRequiresOneExactBasenameEntry() throws Exception {
    UpdateManifest.Release r = UpdateManifest.parseLatest(release(new JSONArray().put(asset(apkName(), apkUrl(), 100, null))
        .put(asset("SHA256SUMS.txt", "https://github.com/RockLithium/hdrbridge/releases/download/v1.2.3/SHA256SUMS.txt", 30, null))
        .put(asset("SHA256SUMS-Android.txt", "https://github.com/RockLithium/hdrbridge/releases/download/v1.2.3/SHA256SUMS-Android.txt", 20, null))).toString());
    assertTrue(r.checksumUrl.endsWith("SHA256SUMS-Android.txt"));
    assertEquals("b".repeat(64), UpdateManifest.withChecksums(r, "b".repeat(64) + "  " + apkName() + "\n").apkSha256);
    String[] invalid = {"", "c".repeat(64) + "  ../" + apkName(), "c".repeat(64) + "  " + apkName() + "\n" + "d".repeat(64) + " *" + apkName()};
    for (String content : invalid) try { UpdateManifest.withChecksums(r, content); fail(); } catch (IllegalArgumentException expected) {}
  }
  @Test public void validatesVersionArithmeticAndSizeBoundary() throws Exception {
    assertEquals(999999999L, UpdateManifest.versionCode("999.999.999"));
    assertTrue(UpdateManifest.compareVersions("1.10.0", "1.9.99") > 0);
    try { UpdateManifest.versionCode("1.1000.0"); fail(); } catch (IllegalArgumentException expected) {}
    for (long size : new long[] {0, 512L * 1024 * 1024 + 1}) try {
      UpdateManifest.parseLatest(release(new JSONArray().put(asset(apkName(), apkUrl(), size, null))).toString()); fail();
    } catch (IllegalArgumentException expected) {}
    assertNotNull(UpdateManifest.parseLatest(release(new JSONArray().put(asset(apkName(), apkUrl(), 512L * 1024 * 1024, null))).toString()));
  }
  @Test public void filenameNearMissesDoNotBecomeAndroidApks() throws Exception {
    String[] names = {"HDRBridge-v1.2.3-Android-arm64.apk.exe", "HDRBridge-v1.2.3-Android-arm64.apk/../x", "HDRBridge-v1.2.3-Windows-arm64.apk"};
    for (String n : names) assertNull(UpdateManifest.parseLatest(release(new JSONArray().put(asset(n, apkUrl(), 3, null))).toString()));
  }
}
