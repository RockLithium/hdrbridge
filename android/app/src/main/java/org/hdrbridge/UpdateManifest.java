package org.hdrbridge;

import java.net.URI;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

/** Strict parser for the GitHub Releases metadata used by the Android updater. */
final class UpdateManifest {
  private static final long MAX_APK_SIZE = 512L * 1024L * 1024L;
  private static final Pattern VERSION = Pattern.compile("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)");
  private static final Pattern HASH = Pattern.compile("(?i)[0-9a-f]{64}");
  private UpdateManifest() {}

  static final class Release {
    final String version, apkName, apkUrl, apkSha256, checksumUrl;
    final long versionCode, apkSize;
    Release(String version, String apkName, String apkUrl, String apkSha256, String checksumUrl,
        long versionCode, long apkSize) {
      this.version = version; this.apkName = apkName; this.apkUrl = apkUrl;
      this.apkSha256 = apkSha256; this.checksumUrl = checksumUrl;
      this.versionCode = versionCode; this.apkSize = apkSize;
    }
  }

  static Release parseLatest(String json) {
    try {
      JSONObject root = new JSONObject(json);
      if (root.optBoolean("draft", false) || root.optBoolean("prerelease", false)) return null;
      String version = versionFromTag(root.optString("tag_name", ""));
      long code = versionCode(version);
      JSONArray assets = root.optJSONArray("assets");
      if (assets == null) return null;
      String expected = "HDRBridge-v" + version + "-Android-arm64.apk";
      JSONObject apk = null;
      JSONObject androidSums = null, generalSums = null;
      boolean duplicateAndroidSums = false, duplicateGeneralSums = false;
      for (int i = 0; i < assets.length(); i++) {
        JSONObject asset = assets.optJSONObject(i);
        if (asset == null) continue;
        String name = asset.optString("name", "");
        if (expected.equals(name)) {
          if (apk != null) throw new IllegalArgumentException("duplicate Android APK asset");
          apk = asset;
        }
        if ("SHA256SUMS-Android.txt".equals(name)) {
          if (androidSums != null) duplicateAndroidSums = true;
          androidSums = asset;
        }
        if ("SHA256SUMS.txt".equals(name)) {
          if (generalSums != null) duplicateGeneralSums = true;
          generalSums = asset;
        }
      }
      // A release without the exact Android artifact is a normal GitHub release shape.
      if (apk == null) return null;
      if ((androidSums != null && duplicateAndroidSums) || (androidSums == null && duplicateGeneralSums))
        throw new IllegalArgumentException("duplicate checksum asset");
      long size = apk.optLong("size", -1);
      if (size <= 0 || size > MAX_APK_SIZE) throw new IllegalArgumentException("invalid APK size");
      String apkUrl = releaseUrl(apk.optString("browser_download_url", ""), version, expected);
      String digest = apk.optString("digest", "");
      String sha = "";
      if (!digest.isEmpty()) {
        if (!digest.matches("(?i)sha256:[0-9a-f]{64}")) throw new IllegalArgumentException("invalid APK digest");
        sha = digest.substring(7).toLowerCase(Locale.ROOT);
      }
      JSONObject sums = androidSums != null ? androidSums : generalSums;
      String checksumUrl = sums == null ? "" : releaseUrl(sums.optString("browser_download_url", ""), version,
          sums.optString("name", ""));
      return new Release(version, expected, apkUrl, sha, checksumUrl, code, size);
    } catch (org.json.JSONException e) {
      throw new IllegalArgumentException("invalid GitHub release JSON", e);
    }
  }

  static Release withChecksums(Release release, String contents) {
    if (release == null) return null;
    if (release.apkSha256 != null && !release.apkSha256.isEmpty()) return release;
    if (contents == null) throw new IllegalArgumentException("missing checksum contents");
    String found = null;
    String[] lines = contents.split("\\R");
    for (String line : lines) {
      String trimmed = line.trim();
      if (trimmed.isEmpty()) continue;
      Matcher m = Pattern.compile("(?i)^([0-9a-f]{64})[ \\t]+\\*?([^ \\t]+)$").matcher(trimmed);
      if (!m.matches()) continue;
      String filename = m.group(2);
      if (filename.contains("/") || filename.contains("\\") || filename.equals(".") || filename.equals(".."))
        throw new IllegalArgumentException("checksum filename must be a basename");
      if (release.apkName.equals(filename)) {
        String hash = m.group(1).toLowerCase(Locale.ROOT);
        if (found != null) throw new IllegalArgumentException("duplicate APK checksum entry");
        found = hash;
      }
    }
    if (found == null) throw new IllegalArgumentException("APK checksum not found");
    return new Release(release.version, release.apkName, release.apkUrl, found, release.checksumUrl,
        release.versionCode, release.apkSize);
  }

  static int compareVersions(String a, String b) {
    String[] x = parts(a), y = parts(b);
    versionCode(a); versionCode(b);
    for (int i = 0; i < 3; i++) {
      int c = Integer.compare(Integer.parseInt(x[i]), Integer.parseInt(y[i]));
      if (c != 0) return c;
    }
    return 0;
  }

  static long versionCode(String version) {
    String[] p = parts(version);
    long major = Long.parseLong(p[0]), minor = Long.parseLong(p[1]), patch = Long.parseLong(p[2]);
    if (major > 999 || minor > 999 || patch > 999) throw new IllegalArgumentException("version component out of range");
    return major * 1000000L + minor * 1000L + patch;
  }

  private static String[] parts(String v) {
    Matcher m = VERSION.matcher(v == null ? "" : v);
    if (!m.matches()) throw new IllegalArgumentException("version must be stable x.y.z");
    return new String[] {m.group(1), m.group(2), m.group(3)};
  }
  private static String versionFromTag(String tag) {
    String v = tag.startsWith("v") ? tag.substring(1) : tag;
    versionCode(v);
    return v;
  }
  private static String releaseUrl(String raw, String version, String filename) {
    try {
      URI uri = new URI(raw);
      String path = "/RockLithium/hdrbridge/releases/download/v" + version + "/" + filename;
      if (!"https".equals(uri.getScheme()) || !"github.com".equalsIgnoreCase(uri.getHost()) ||
          uri.getUserInfo() != null || uri.getPort() != -1 || uri.getQuery() != null || uri.getFragment() != null ||
          !path.equals(uri.getRawPath()) || raw.indexOf('%') >= 0)
        throw new IllegalArgumentException("untrusted release asset URL");
      return uri.toASCIIString();
    } catch (java.net.URISyntaxException e) { throw new IllegalArgumentException("invalid release asset URL", e); }
  }
}
