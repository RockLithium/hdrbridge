package org.hdrbridge;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.os.Build;
import android.os.Bundle;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.widget.FrameLayout;
import android.widget.ImageView;
import java.io.InputStream;

/** Optional system-decoder preview. Conversion never uses these decoded pixels. */
public final class HdrPreviewActivity extends Activity {
  private Bitmap bitmap;
  private ImageView image;
  private final Matrix transform = new Matrix();
  private boolean closing;
  private float fitScale = 1, scale = 1;

  @Override
  public void onCreate(Bundle saved) {
    super.onCreate(saved);
    setTitle("HDR preview");
    if (Build.VERSION.SDK_INT < 34 || getDisplay() == null || !getDisplay().isHdr()) {
      unavailable("Native Ultra HDR preview requires Android 14 or later and an HDR display.");
      return;
    }
    getWindow().setColorMode(ActivityInfo.COLOR_MODE_HDR);
    image = new ImageView(this);
    image.setScaleType(ImageView.ScaleType.MATRIX);
    image.setBackgroundColor(android.graphics.Color.BLACK);
    setContentView(image, new FrameLayout.LayoutParams(-1, -1));
    ScaleGestureDetector pinch =
        new ScaleGestureDetector(this, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
          @Override
          public boolean onScale(ScaleGestureDetector detector) {
            float next = Math.max(
                fitScale * .25f, Math.min(fitScale * 32, scale * detector.getScaleFactor()));
            transform.postScale(
                next / scale, next / scale, detector.getFocusX(), detector.getFocusY());
            scale = next;
            image.setImageMatrix(transform);
            return true;
          }
        });
    GestureDetector gestures =
        new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
          @Override
          public boolean onDown(MotionEvent e) {
            return true;
          }
          @Override
          public boolean onDoubleTap(MotionEvent e) {
            if (scale > fitScale * 1.1f)
              fit();
            else {
              transform.postScale(2, 2, e.getX(), e.getY());
              scale *= 2;
              image.setImageMatrix(transform);
            }
            return true;
          }
          @Override
          public boolean onScroll(MotionEvent e, MotionEvent now, float dx, float dy) {
            transform.postTranslate(-dx, -dy);
            image.setImageMatrix(transform);
            return true;
          }
        });
    image.setOnTouchListener((view, event) -> {
      pinch.onTouchEvent(event);
      if (!pinch.isInProgress())
        gestures.onTouchEvent(event);
      if (event.getAction() == MotionEvent.ACTION_UP)
        view.performClick();
      return true;
    });
    new Thread(() -> {
      Bitmap decoded = null;
      try (InputStream input = getContentResolver().openInputStream(getIntent().getData())) {
        decoded = BitmapFactory.decodeStream(input);
        if (decoded == null || !decoded.hasGainmap()) {
          if (decoded != null)
            decoded.recycle();
          runOnUiThread(()
                            -> unavailable("The Android decoder cannot present this file as native "
                                           + "Ultra HDR. Conversion remains available."));
          return;
        }
        final Bitmap ready = decoded;
        runOnUiThread(() -> {
          if (closing || isFinishing()) {
            ready.recycle();
            return;
          }
          bitmap = ready;
          image.setImageBitmap(bitmap);
          image.post(this::fit);
        });
      } catch (Exception | OutOfMemoryError error) {
        if (decoded != null && !decoded.isRecycled())
          decoded.recycle();
        runOnUiThread(() -> unavailable("Preview unavailable: " + error.getMessage()));
      }
    }, "hdr-preview").start();
  }
  private void fit() {
    if (bitmap == null || image.getWidth() == 0)
      return;
    fitScale = Math.min((float) image.getWidth() / bitmap.getWidth(),
        (float) image.getHeight() / bitmap.getHeight());
    scale = fitScale;
    transform.reset();
    transform.postScale(scale, scale);
    transform.postTranslate((image.getWidth() - bitmap.getWidth() * scale) / 2,
        (image.getHeight() - bitmap.getHeight() * scale) / 2);
    image.setImageMatrix(transform);
  }
  private void unavailable(String message) {
    if (closing || isFinishing())
      return;
    new AlertDialog.Builder(this)
        .setTitle("Preview unavailable")
        .setMessage(message)
        .setPositiveButton(android.R.string.ok, (dialog, which) -> finish())
        .setOnCancelListener(dialog -> finish())
        .show();
  }
  @Override
  public void onDestroy() {
    closing = true;
    if (image != null)
      image.setImageDrawable(null);
    if (bitmap != null)
      bitmap.recycle();
    super.onDestroy();
  }
}
