package org.hdrbridge;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.OpenableColumns;
import android.view.Display;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.Surface;
import android.view.SurfaceControl;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/** Full-source native HDR preview. Decoding starts only when this activity opens. */
public final class HdrPreviewActivity extends Activity {
  private final Object lock = new Object();
  private HandlerThread worker;
  private Handler handler;
  private SurfaceView view;
  private long nativeSession;
  private int imageWidth, imageHeight, width, height;
  private float fitScale = 1, scale = 1, x, y, peak = 1, displayHeadroom;
  private boolean closing, surfaceReady, attached, renderPending, warned, hdrListenerRegistered;
  private int retries;
  private final Consumer<Display> hdrRatioListener = display -> {
    float next = readDisplayHeadroom(display);
    boolean changed;
    synchronized (lock) {
      changed = Math.abs(next - displayHeadroom) > 0.01f;
      if (changed) displayHeadroom = next;
    }
    if (changed) requestRender();
  };

  private static float readDisplayHeadroom(Display display) {
    if (Build.VERSION.SDK_INT < 34 || display == null || !display.isHdrSdrRatioAvailable()) {
      return 0;
    }
    float ratio = display.getHdrSdrRatio();
    return Float.isFinite(ratio) && ratio > 0 ? Math.max(1f, ratio) : 1f;
  }

  @Override public void onCreate(Bundle saved) {
    super.onCreate(saved);
    setTitle("HDR preview");
    if (Build.VERSION.SDK_INT < 34 || getDisplay() == null || !getDisplay().isHdr()) {
      unavailable("Native HDR preview requires Android 14 or later and an HDR display.");
      return;
    }
    Display display = getDisplay();
    getWindow().setColorMode(ActivityInfo.COLOR_MODE_HDR);
    displayHeadroom = readDisplayHeadroom(display);
    if (display.isHdrSdrRatioAvailable()) {
      display.registerHdrSdrRatioChangedListener(getMainExecutor(), hdrRatioListener);
      hdrListenerRegistered = true;
    }
    FrameLayout root = new FrameLayout(this);
    root.setBackgroundColor(Color.BLACK);
    view = new SurfaceView(this);
    root.addView(view, new FrameLayout.LayoutParams(-1, -1));
    setContentView(root);
    worker = new HandlerThread("hdr-preview-gl");
    worker.start();
    handler = new Handler(worker.getLooper());
    setupGestures();
    view.getHolder().addCallback(new SurfaceHolder.Callback() {
      @Override public void surfaceCreated(SurfaceHolder holder) {
        synchronized (lock) { surfaceReady = true; }
        attach(holder.getSurface());
      }
      @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        synchronized (lock) {
          boolean wasFit = Math.abs(scale - fitScale) < .001f;
          width = w; height = h; surfaceReady = true;
          if (wasFit) fitLocked();
        }
        attach(holder.getSurface());
        requestRender();
      }
      @Override public void surfaceDestroyed(SurfaceHolder holder) {
        CountDownLatch latch = new CountDownLatch(1);
        boolean posted = false;
        synchronized (lock) {
          surfaceReady = false;
          if (handler != null) {
            posted = handler.post(() -> {
              try {
                long session;
                boolean detach;
                synchronized (lock) {
                  session = nativeSession;
                  detach = attached;
                  attached = false;
                }
                if (session != 0 && detach) NativeBridge.previewDetachSurface(session);
              } finally {
                latch.countDown();
              }
            });
          }
        }
        if (posted) {
          try { latch.await(2, TimeUnit.SECONDS); }
          catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
        }
      }
    });
    decode();
  }

  private void setupGestures() {
    ScaleGestureDetector pinch = new ScaleGestureDetector(this,
        new ScaleGestureDetector.SimpleOnScaleGestureListener() {
          @Override public boolean onScale(ScaleGestureDetector d) {
            synchronized (lock) { zoomLocked(d.getScaleFactor(), d.getFocusX(), d.getFocusY()); }
            requestRender(); return true;
          }
        });
    GestureDetector gestures = new GestureDetector(this,
        new GestureDetector.SimpleOnGestureListener() {
          @Override public boolean onDown(MotionEvent e) { return true; }
          @Override public boolean onDoubleTap(MotionEvent e) {
            synchronized (lock) {
              if (scale > fitScale * 1.1f) fitLocked();
              else zoomLocked(2, e.getX(), e.getY());
            }
            requestRender(); return true;
          }
          @Override public boolean onScroll(MotionEvent a, MotionEvent b, float dx, float dy) {
            synchronized (lock) { x -= dx; y -= dy; }
            requestRender(); return true;
          }
        });
    view.setOnTouchListener((v, event) -> {
      pinch.onTouchEvent(event);
      if (!pinch.isInProgress()) gestures.onTouchEvent(event);
      if (event.getAction() == MotionEvent.ACTION_UP) v.performClick();
      return true;
    });
  }
  private void zoomLocked(float factor, float fx, float fy) {
    if (imageWidth <= 0) return;
    float next = Math.max(fitScale * .25f, Math.min(fitScale * 32, scale * factor));
    float ratio = next / scale;
    x = fx - (fx - x) * ratio; y = fy - (fy - y) * ratio; scale = next;
  }
  private void fitLocked() {
    if (width <= 0 || height <= 0 || imageWidth <= 0 || imageHeight <= 0) return;
    fitScale = Math.min((float) width / imageWidth, (float) height / imageHeight);
    scale = fitScale; x = (width - imageWidth * scale) / 2; y = (height - imageHeight * scale) / 2;
  }

  private void decode() {
    Uri uri = getIntent().getData();
    if (uri == null) { unavailable("No image URI provided."); return; }
    new Thread(() -> {
      File staged = null;
      long handle = 0;
      try {
        String path;
        if ("file".equals(uri.getScheme()) && uri.getPath() != null) path = uri.getPath();
        else { staged = stage(uri); path = staged.getAbsolutePath(); }
        int limit = NativeBridge.previewMaxTextureSize();
        if (limit <= 0) throw new IOException("HDR graphics initialization failed.");
        handle = NativeBridge.previewDecode(path, Math.min(limit, 16384));
        if (handle == 0) throw new IOException("Unable to decode this image.");
        float[] stats = NativeBridge.previewStats(handle);
        if (stats == null || stats.length < 3 || stats[0] <= 0 || stats[1] <= 0)
          throw new IOException("Invalid image dimensions.");
        synchronized (lock) {
          if (!closing) {
            nativeSession = handle; handle = 0;
            imageWidth = Math.round(stats[0]); imageHeight = Math.round(stats[1]);
            peak = Math.max(1, stats[2]); fitLocked();
            runOnUiThread(() -> { if (!closing && view != null) attach(view.getHolder().getSurface()); });
          }
        }
      } catch (Exception | OutOfMemoryError error) {
        String detail = error.getMessage();
        String msg = (detail != null && !detail.isEmpty())
            ? detail : "Unable to open this image for HDR preview.";
        runOnUiThread(() -> unavailable(msg));
      } finally {
        if (handle != 0) NativeBridge.previewDestroy(handle);
        if (staged != null) staged.delete();
      }
    }, "hdr-preview-decode").start();
  }
  private File stage(Uri uri) throws IOException {
    String suffix = ".img";
    try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
      if (c != null && c.moveToFirst()) {
        String name = c.getString(0);
        int dot = name == null ? -1 : name.lastIndexOf('.');
        if (dot >= 0) suffix = name.substring(dot).replaceAll("[^A-Za-z0-9.]", "");
      }
    } catch (RuntimeException ignored) { }
    File file = File.createTempFile("hdr_preview_", suffix, getCacheDir());
    try (InputStream in = getContentResolver().openInputStream(uri); FileOutputStream out = new FileOutputStream(file)) {
      if (in == null) throw new IOException("Cannot open image");
      byte[] buffer = new byte[65536]; int n;
      while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
      return file;
    } catch (IOException | RuntimeException e) { file.delete(); throw e; }
  }
  private void attach(Surface surface) {
    synchronized (lock) {
      if (closing || !surfaceReady || nativeSession == 0 || handler == null) return;
      handler.post(() -> {
        long session;
        synchronized (lock) {
          if (closing || !surfaceReady || nativeSession == 0 || attached || !surface.isValid()) return;
          session = nativeSession;
        }
        int mode = NativeBridge.previewAttachSurface(session, surface);
        synchronized (lock) {
          if (closing || !surfaceReady) {
            if (mode > 0) NativeBridge.previewDetachSurface(session);
            return;
          }
          if (mode <= 0) { runOnUiThread(() -> unavailable("This display cannot present native HDR preview.")); return; }
          attached = true;
        }
        runOnUiThread(() -> applyHeadroom(mode));
        requestRender();
      });
    }
  }
  private void applyHeadroom(int mode) {
    if (closing || view == null) return;
    float target = Math.max(1f, Math.min(16f, peak));
    if (Build.VERSION.SDK_INT >= 35) {
      getWindow().setDesiredHdrHeadroom(target);
      if (mode != 1) view.setDesiredHdrHeadroom(target);
    } else if (Build.VERSION.SDK_INT >= 34 && mode != 1) {
      SurfaceControl sc = view.getSurfaceControl();
      if (sc != null && sc.isValid()) try (SurfaceControl.Transaction tx = new SurfaceControl.Transaction()) {
        tx.setExtendedRangeBrightness(sc, 1f, target); tx.apply();
      }
    }
  }
  private void requestRender() {
    synchronized (lock) { retries = 0; scheduleLocked(0); }
  }
  private void scheduleLocked(long delay) {
    if (closing || handler == null || renderPending) return;
    renderPending = true;
    if (!handler.postDelayed(this::render, delay)) renderPending = false;
  }
  private void render() {
    long session; int w, h; float zoom, tx, ty, hdr;
    synchronized (lock) {
      renderPending = false;
      if (closing || !surfaceReady || !attached || nativeSession == 0 || width <= 0 || height <= 0) return;
      session = nativeSession; w = width; h = height; zoom = scale; tx = x; ty = y; hdr = displayHeadroom;
    }
    int result = NativeBridge.previewRender(session, w, h, zoom, tx, ty, hdr);
    synchronized (lock) {
      if (closing || !surfaceReady) return;
      if (result == 0 && retries++ < 10) scheduleLocked(16);
      else if (result < 0) runOnUiThread(() -> unavailable("This display cannot present native HDR preview."));
    }
  }
  private void unavailable(String message) {
    synchronized (lock) { if (closing || warned || isFinishing()) return; warned = true; }
    new AlertDialog.Builder(this).setTitle("Preview unavailable").setMessage(message)
        .setPositiveButton(android.R.string.ok, (d, w) -> finish())
        .setOnCancelListener(d -> finish()).show();
  }
  @Override public void onDestroy() {
    if (hdrListenerRegistered && Build.VERSION.SDK_INT >= 34) {
      Display display = getDisplay();
      if (display != null) display.unregisterHdrSdrRatioChangedListener(hdrRatioListener);
      hdrListenerRegistered = false;
    }
    long fallback = 0;
    synchronized (lock) {
      closing = true;
      long session = nativeSession;
      nativeSession = 0;
      if (handler != null) {
        if (session != 0 && !handler.post(() -> NativeBridge.previewDestroy(session))) fallback = session;
        worker.quitSafely();
        handler = null;
      } else {
        fallback = session;
      }
    }
    if (fallback != 0) NativeBridge.previewDestroy(fallback);
    super.onDestroy();
  }
}
