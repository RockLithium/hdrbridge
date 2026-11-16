package org.hdrbridge;

/** Synchronous path-based interface. Run inspection and conversion off the UI thread. */
public final class NativeBridge {
  static {
    System.loadLibrary("hdrbridge_jni");
  }
  private NativeBridge() {}

  public interface Progress {
    void onProgress(int percent, String message);
  }
  public static native String nativeVersion();
  public static native String inspect(String path);
  public static native String convert(
      String source, String output, String optionsJson, Progress callback);
  public static native void resetCancel();
  public static native void cancel();
  public static native void clearCache();
  public static native int previewMaxTextureSize();
  public static native long previewDecode(String path, int maxDimension);
  public static native float[] previewStats(long handle);
  public static native int previewAttachSurface(long handle, android.view.Surface surface);
  public static native int previewRender(long handle, int width, int height, float scale, float x, float y, float displayHeadroom);
  public static native void previewDetachSurface(long handle);
  public static native void previewDestroy(long handle);
}
