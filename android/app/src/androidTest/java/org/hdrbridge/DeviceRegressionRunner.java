package org.hdrbridge;

import android.app.Instrumentation;
import android.os.Bundle;
import android.os.SystemClock;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Gainmap;
import android.net.Uri;
import android.provider.MediaStore;
import android.content.ContentValues;
import android.provider.DocumentsContract;
import android.content.SharedPreferences;
import android.media.MediaScannerConnection;
import android.os.Environment;
import android.database.Cursor;
import android.util.Log;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import org.json.JSONArray;
import org.json.JSONObject;

/** Device-local, path-based native regression harness; invoked only by instrumentation. */
public final class DeviceRegressionRunner extends Instrumentation {
  private Bundle args;
  private static native String codecSelfTest();

  @Override public void onCreate(Bundle arguments) {
    super.onCreate(arguments);
    args = arguments;
    start();
  }

  @Override public void onStart() {
    Bundle summary = new Bundle();
    int passed = 0;
    JSONArray results = new JSONArray();
    try {
      if (args.containsKey("uiSmokeSpec")) {
        JSONObject result = UiSmokeRunner.run(this, args.getString("uiSmokeSpec"));
        File specFile = new File(args.getString("uiSmokeSpec")).getCanonicalFile();
        File reportFile = new File(specFile.getParentFile(), "ui-smoke-report.json").getCanonicalFile();
        if (!reportFile.getParentFile().equals(specFile.getParentFile()) || reportFile.equals(specFile)) throw new IOException("Unsafe UI smoke report path");
        Files.write(reportFile.toPath(), result.toString(2).getBytes(StandardCharsets.UTF_8)); reportFile.setReadable(true, false);
        summary.putString("report", reportFile.getAbsolutePath());
        boolean uiPassed = result.optBoolean("passed", false);
        summary.putBoolean("passed", uiPassed);
        finish(uiPassed ? android.app.Activity.RESULT_OK : android.app.Activity.RESULT_CANCELED, summary);
        return;
      }
      if (args.containsKey("appConvertSpec")) {
        runAppConvertProbe(args.getString("appConvertSpec"), summary);
        return;
      }
      if (args.containsKey("publishSpec")) {
        runPublishProbe(args.getString("publishSpec"), summary);
        return;
      }
      if (args.containsKey("gallerySpec")) {
        runGalleryProbe(args.getString("gallerySpec"), summary);
        return;
      }
      if (Boolean.parseBoolean(args.getString("codecSelfTest", "false"))) {
        NativeBridge.nativeVersion();
        String result = codecSelfTest();
        summary.putString("result", result);
        finish(new JSONObject(result).optBoolean("passed") ? android.app.Activity.RESULT_OK : android.app.Activity.RESULT_CANCELED, summary);
        return;
      }
      File external = getTargetContext().getExternalFilesDir(null);
      if (external == null) throw new IOException("External files directory unavailable");
      if (args.containsKey("prepare")) {
        String name = args.getString("prepare");
        if (name == null || !name.matches("hdrbridge-device-tests-[A-Za-z0-9_-]+"))
          throw new IOException("Invalid test directory name");
        File directory = new File(external, name);
        if (!directory.mkdirs() && !directory.isDirectory()) throw new IOException("Cannot create test directory");
        directory.setReadable(true, false);
        directory.setExecutable(true, false);
        File[] existing = directory.listFiles();
        if (existing != null) for (File file : existing) {
          if (file.isFile() && file.getCanonicalFile().getParentFile().equals(directory.getCanonicalFile()))
            file.setReadable(true, false);
        }
        summary.putString("directory", directory.getCanonicalPath());
        finish(android.app.Activity.RESULT_OK, summary);
        return;
      }
      File spec = new File(args.getString("spec", "")).getCanonicalFile();
      String root = external.getCanonicalPath() + File.separator;
      if (!spec.getPath().startsWith(root) || !spec.isFile())
        throw new IOException("Spec must be a file inside target external files directory: expected=" + root + "; spec=" + spec + "; exists=" + spec.isFile());
      File dir = spec.getParentFile();
      JSONObject input = new JSONObject(new String(Files.readAllBytes(spec.toPath()), StandardCharsets.UTF_8));
      JSONArray jobs = input.getJSONArray("jobs");
      if (args.containsKey("job")) jobs = new JSONArray().put(jobs.getJSONObject(Integer.parseInt(args.getString("job"))));
      boolean inspectOnly = Boolean.parseBoolean(args.getString("inspectOnly", "false"));
      String reportName = args.getString("report", "report.json");
      if (reportName.isEmpty() || !reportName.equals(new File(reportName).getName()) || reportName.equals(".") || reportName.equals(".."))
        throw new IOException("Report must be a filename");
      File reportFile = new File(dir, reportName).getCanonicalFile();
      if (!reportFile.getParentFile().equals(dir) || reportFile.equals(spec))
        throw new IOException("Unsafe report path");
      writeReport(reportFile, results, passed, jobs.length());
      runOnMainSync(() -> AppModel.get(getTargetContext()));
      String previousInput = null;
      for (int i = 0; i < jobs.length(); i++) {
        long started = SystemClock.elapsedRealtime();
        JSONObject record = new JSONObject();
        try {
          JSONObject job = jobs.getJSONObject(i);
          String source = job.getString("input");
          record.put("input", source);
          if (!new File(source).isAbsolute()) throw new IOException("Input path must be absolute");
          // The production core invalidates its single-source cache by path
          // and revision. Follow the same path instead of explicit resets.
          previousInput = source;
          NativeBridge.resetCancel();
          record.put("inspect", new JSONObject(NativeBridge.inspect(source)));
          if (!inspectOnly) {
            String name = job.getString("output");
            if (name.isEmpty() || !name.equals(new File(name).getName()) || name.equals(".") || name.equals(".."))
              throw new IOException("Output must be a filename");
            File dest = new File(dir, name).getCanonicalFile();
            if (!dest.getParentFile().equals(dir) || dest.equals(spec) || dest.equals(reportFile) || dest.equals(new File(source).getCanonicalFile()))
              throw new IOException("Unsafe output path");
            int dot = name.lastIndexOf('.');
            String extension = dot < 0 ? "" : name.substring(dot);
            JSONObject options = job.optJSONObject("options");
            if (options == null) options = new JSONObject();
            try (ConversionWorkspace workspace = ConversionWorkspace.create(getTargetContext().getCacheDir(), extension)) {
              JSONObject converted = new JSONObject(NativeBridge.convert(source, workspace.output().getAbsolutePath(),
                  workspace.nativeOptions(options).toString(), (percent, message) ->
                      Log.i("DeviceRegression", percent + "% " + message)));
              record.put("result", converted);
              if (!converted.optBoolean("success", false))
                throw new IOException("Native conversion failed: " + converted.optString("error"));
              Files.copy(workspace.output().toPath(), dest.toPath(), StandardCopyOption.REPLACE_EXISTING);
              dest.setReadable(true, false);
              record.put("output", dest.getAbsolutePath());
            }
          }
          record.put("passed", true);
          passed++;
        } catch (Exception | LinkageError e) {
          record.put("passed", false);
          record.put("error", e.toString());
        }
        record.put("elapsedMs", SystemClock.elapsedRealtime() - started);
        results.put(record);
        writeReport(reportFile, results, passed, jobs.length());
        Bundle status = new Bundle();
        status.putInt("job", i);
        status.putBoolean("passed", record.optBoolean("passed"));
        sendStatus(1, status);
      }
      summary.putString("report", reportFile.getAbsolutePath());
      summary.putInt("passed", passed);
      summary.putInt("total", jobs.length());
      finish(passed == jobs.length() ? android.app.Activity.RESULT_OK : android.app.Activity.RESULT_CANCELED, summary);
    } catch (Exception | LinkageError e) {
      Log.e("DeviceRegression", "Harness failed", e);
      summary.putString("error", e.toString());
      finish(android.app.Activity.RESULT_CANCELED, summary);
    }
  }

  /** Runs one actual AppModel conversion task, using the app's ordinary default Ultra HDR options. */
  private void runAppConvertProbe(String specPath, Bundle summary) throws Exception {
    File external = getTargetContext().getExternalFilesDir(null);
    if (external == null) throw new IOException("External files directory unavailable");
    File spec = new File(specPath == null ? "" : specPath).getCanonicalFile();
    String root = external.getCanonicalPath() + File.separator;
    if (!spec.getPath().startsWith(root) || !spec.isFile()) throw new IOException("appConvertSpec must be inside target external files directory");
    File dir = spec.getParentFile();
    JSONObject input = new JSONObject(new String(Files.readAllBytes(spec.toPath()), StandardCharsets.UTF_8));
    String uriString = input.getString("inputUri"), outputName = input.getString("outputName");
    Uri inputUri = Uri.parse(uriString);
    if ("file".equals(inputUri.getScheme())) {
      File inputFile = new File(inputUri.getPath() == null ? "" : inputUri.getPath()).getCanonicalFile();
      if (!inputFile.getPath().startsWith(root) || !inputFile.isFile()) throw new IOException("file inputUri must name an existing app-local staged file");
    } else if (!"content".equals(inputUri.getScheme())) throw new IOException("inputUri must be a content URI or an app-local file URI");
    if (outputName.isEmpty() || !outputName.equals(new File(outputName).getName()) || outputName.equals(".") || outputName.equals("..")
        || !outputName.toLowerCase(java.util.Locale.ROOT).endsWith(".jpg")) throw new IOException("outputName must be a .jpg filename");
    String folderValue = getTargetContext().getSharedPreferences("android_options_v2", 0).getString("folder", "");
    if (folderValue.isEmpty()) throw new IOException("No persisted SAF output folder in app preferences");
    Uri folder = Uri.parse(folderValue);
    Options appOptions = Options.defaults(0);
    if (input.has("gainmapScale")) {
      int scale = input.getInt("gainmapScale");
      if (scale != 1 && scale != 2 && scale != 4) throw new IOException("gainmapScale must be 1, 2, or 4");
      appOptions.scale = scale;
    }
    if (input.has("rgbGainmap")) appOptions.channels = input.getBoolean("rgbGainmap") ? 1 : 0;
    final Options taskOptions = appOptions;
    final AppModel[] modelHolder = new AppModel[1];
    final AppModel.Task[] taskHolder = new AppModel.Task[1];
    String inputName = input.optString("inputName", inputUri.getLastPathSegment());
    if (inputName == null || inputName.isEmpty()) inputName = "HDRBridge_Input.jpg";
    inputName = new File(inputName).getName().replaceAll("[^A-Za-z0-9._-]", "_");
    final String safeInputName = inputName;
    runOnMainSync(() -> {
      AppModel model = AppModel.get(getTargetContext());
      AppModel.Source source = model.add(inputUri, safeInputName);
      AppModel.Task task = new AppModel.Task(source, taskOptions.copy(), folder);
      task.filename = outputName;
      modelHolder[0] = model;
      taskHolder[0] = task;
      model.start(task);
    });
    AppModel model = modelHolder[0]; AppModel.Task task = taskHolder[0];
    if (task == null) throw new IOException("Could not create app task");
    long deadline = SystemClock.elapsedRealtime() + 90000;
    while (SystemClock.elapsedRealtime() < deadline) {
      String status = task.status;
      if ("Completed".equals(status) || "Failed".equals(status) || "Skipped".equals(status) || "Cancelled".equals(status)) break;
      SystemClock.sleep(50);
    }
    JSONObject report = new JSONObject();
    report.put("inputUri", uriString); report.put("outputName", outputName);
    report.put("options", taskOptions.json());
    report.put("optionsSnapshot", taskOptions.save());
    report.put("inputName", safeInputName);
    report.put("status", task.status); report.put("detail", task.detail); report.put("progress", task.progress);
    report.put("taskHash", task.hash);
    report.put("actualOutputFilename", task.filename);
    if (task.source.info != null) report.put("nativeInputInspect", task.source.info);
    else if (!task.source.error.isEmpty()) report.put("nativeInputInspectError", task.source.error);
    if (task.output != null) {
      report.put("outputUri", task.output.toString());
      try { report.put("uriReadbackSha256", sha256(getTargetContext().getContentResolver().openInputStream(task.output))); }
      catch (Exception e) { report.put("uriReadbackError", e.toString()); }
      try { report.put("mediaStore", queryMediaStoreUri(task.output)); }
      catch (Exception e) { report.put("mediaStoreError", e.toString()); }
      File outputProbe = File.createTempFile("hdr-app-output-inspect-", ".jpg", getTargetContext().getCacheDir());
      try (InputStream in = getTargetContext().getContentResolver().openInputStream(task.output);
           java.io.OutputStream out = Files.newOutputStream(outputProbe.toPath())) {
        if (in == null) throw new IOException("Cannot read output URI for native inspect");
        copyBytes(in, out);
        report.put("nativeOutputInspect", new JSONObject(NativeBridge.inspect(outputProbe.getAbsolutePath())));
      } catch (Exception e) { report.put("nativeOutputInspectError", e.toString()); }
      finally { outputProbe.delete(); }
    }
    boolean passed = "Completed".equals(task.status) && task.output != null;
    report.put("passed", passed);
    File reportFile = new File(dir, "app-convert-report.json").getCanonicalFile();
    if (!reportFile.getParentFile().equals(dir) || reportFile.equals(spec)) throw new IOException("Unsafe app-convert report path");
    Files.write(reportFile.toPath(), report.toString(2).getBytes(StandardCharsets.UTF_8)); reportFile.setReadable(true, false);
    summary.putString("report", reportFile.getAbsolutePath()); summary.putString("status", task.status);
    summary.putBoolean("passed", passed);
    finish(passed ? android.app.Activity.RESULT_OK : android.app.Activity.RESULT_CANCELED, summary);
  }

  /** Test-only byte-preserving publication through existing SAF or MediaStore paths. */
  private void runPublishProbe(String specPath, Bundle summary) throws Exception {
    File external = getTargetContext().getExternalFilesDir(null);
    if (external == null) throw new IOException("External files directory unavailable");
    File spec = new File(specPath == null ? "" : specPath).getCanonicalFile();
    String root = external.getCanonicalPath() + File.separator;
    if (!spec.getPath().startsWith(root) || !spec.isFile()) throw new IOException("publishSpec must be inside target external files directory");
    File dir = spec.getParentFile();
    JSONObject input = new JSONObject(new String(Files.readAllBytes(spec.toPath()), StandardCharsets.UTF_8));
    JSONArray jobs = input.getJSONArray("jobs"), results = new JSONArray();
    String relativePath = input.optString("relativePath", "DCIM/HDR Bridge/");
    if (!relativePath.startsWith("DCIM/") || relativePath.contains("..") || !relativePath.endsWith("/"))
      throw new IOException("relativePath must be a DCIM/ path ending in /");
    for (int i = 0; i < jobs.length(); i++) {
      JSONObject job = jobs.getJSONObject(i), row = new JSONObject();
      Uri dest = null;
      boolean scan = "scan".equals(job.optString("method"));
      boolean pending = "pending".equals(job.optString("method"));
      boolean saf = "saf".equals(job.optString("method"));
      boolean retainMismatch = input.optBoolean("retainMismatch", false);
      boolean hashMismatch = false;
      try {
        if (!pending && !saf && !scan) throw new IOException("method must be saf, pending, or scan");
        File source = null;
        if (!scan) {
          source = new File(job.getString("source")).getCanonicalFile();
          if (!source.getPath().startsWith(root) || !source.isFile()) throw new IOException("source must be a staged app-local file: " + source);
        }
        String name = job.getString("name");
        if (name.isEmpty() || !name.equals(new File(name).getName()) || name.equals(".") || name.equals("..")) throw new IOException("name must be a filename");
        String lower = name.toLowerCase(java.util.Locale.ROOT);
        if (!lower.endsWith(".jpg") && !lower.endsWith(".jpeg")) throw new IOException("name must have a JPEG extension");
        if (source != null) row.put("source", source.getAbsolutePath()); row.put("name", name); row.put("method", job.getString("method"));
        if (!scan) row.put("sourceSha256", sha256(Files.newInputStream(source.toPath())));
        if (scan) {
          if (!name.startsWith("HDRBridge_Check_")) throw new IOException("scan name must start HDRBridge_Check_");
          File scanFile = new File(new File(Environment.getExternalStorageDirectory(), relativePath), name).getCanonicalFile();
          String expected = new File(Environment.getExternalStorageDirectory(), relativePath).getCanonicalPath() + File.separator;
          if (!scanFile.getPath().startsWith(expected) || !scanFile.isFile()) throw new IOException("Expected fresh PC-copied file missing at " + scanFile);
          final Object lock = new Object(); final Uri[] scanned = new Uri[1]; final boolean[] complete = new boolean[1];
          MediaScannerConnection.scanFile(getTargetContext(), new String[]{scanFile.getAbsolutePath()}, new String[]{"image/jpeg"}, (p, u) -> {
            synchronized (lock) { scanned[0] = u; complete[0] = true; lock.notifyAll(); }
          });
          synchronized (lock) {
            long end = SystemClock.elapsedRealtime() + 10000;
            while (!complete[0] && SystemClock.elapsedRealtime() < end) lock.wait(end - SystemClock.elapsedRealtime());
          }
          if (!complete[0]) throw new IOException("MediaScanner timed out after 10 seconds");
          dest = scanned[0];
          if (dest == null) throw new IOException("MediaScanner returned no content URI");
        } else if (saf) {
          String folderValue = getTargetContext().getSharedPreferences("android_options_v2", 0).getString("folder", "");
          if (folderValue.isEmpty()) throw new IOException("No persisted SAF output folder in app preferences");
          Uri folder = Uri.parse(folderValue);
          dest = AppModel.get(getTargetContext()).createUnique(folder, name);
          try (InputStream in = Files.newInputStream(source.toPath()); java.io.OutputStream out = getTargetContext().getContentResolver().openOutputStream(dest, "w")) {
            if (out == null) throw new IOException("Cannot open SAF destination");
            copyBytes(in, out);
          }
        } else {
          ContentValues values = new ContentValues();
          values.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
          values.put(MediaStore.MediaColumns.MIME_TYPE, "image/jpeg");
          values.put(MediaStore.MediaColumns.RELATIVE_PATH, relativePath);
          values.put(MediaStore.MediaColumns.IS_PENDING, 1);
          dest = getTargetContext().getContentResolver().insert(MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values);
          if (dest == null) throw new IOException("MediaStore insert failed");
          try (InputStream in = Files.newInputStream(source.toPath()); java.io.OutputStream out = getTargetContext().getContentResolver().openOutputStream(dest, "w")) {
            if (out == null) throw new IOException("Cannot open pending MediaStore destination");
            copyBytes(in, out);
          }
          ContentValues publish = new ContentValues(); publish.put(MediaStore.MediaColumns.IS_PENDING, 0);
          if (getTargetContext().getContentResolver().update(dest, publish, null, null) != 1) throw new IOException("Could not publish MediaStore row");
        }
        row.put("uri", dest.toString());
        if (!scan) {
          String readbackSha256 = sha256(getTargetContext().getContentResolver().openInputStream(dest));
          row.put("publishedSha256", readbackSha256);
          row.put("sameBytes", row.getString("sourceSha256").equals(row.getString("publishedSha256")));
          if (!row.getBoolean("sameBytes")) {
            hashMismatch = true;
            if (retainMismatch) {
              String readbackName = name.substring(0, name.lastIndexOf('.')) + "-uri-readback.jpg";
              File readback = new File(dir, readbackName).getCanonicalFile();
              if (!readback.getParentFile().equals(dir) || readback.equals(spec) || !readback.createNewFile())
                throw new IOException("Cannot safely create unique readback artifact: " + readbackName);
              try (InputStream in = getTargetContext().getContentResolver().openInputStream(dest);
                   java.io.OutputStream out = Files.newOutputStream(readback.toPath())) {
                if (in == null) throw new IOException("Cannot reopen published URI for readback artifact");
                copyBytes(in, out);
              }
              readback.setReadable(true, false);
              row.put("uriReadbackArtifact", readback.getAbsolutePath());
              row.put("uriReadbackArtifactSha256", sha256(Files.newInputStream(readback.toPath())));
            }
            throw new IOException("Published bytes differ from staged source");
          }
        }
        try { row.put("mediaStore", queryMediaStoreUri(dest)); }
        catch (Exception metadataError) { row.put("mediaStoreError", metadataError.toString()); }
        row.put("passed", true);
      } catch (Exception | LinkageError e) {
        row.put("passed", false); row.put("error", e.toString());
        if (dest != null) {
          try {
            if (hashMismatch && retainMismatch) { /* Keep the probe destination for raw comparison. */ }
            else if (pending) getTargetContext().getContentResolver().delete(dest, null, null);
            else if (saf) DocumentsContract.deleteDocument(getTargetContext().getContentResolver(), dest);
          } catch (Exception ignored) { row.put("cleanupError", ignored.toString()); }
        }
      }
      results.put(row);
    }
    JSONObject report = new JSONObject(); report.put("jobs", results);
    File reportFile = new File(dir, "publish-report.json").getCanonicalFile();
    if (!reportFile.getParentFile().equals(dir) || reportFile.equals(spec)) throw new IOException("Unsafe publish report path");
    Files.write(reportFile.toPath(), report.toString(2).getBytes(StandardCharsets.UTF_8)); reportFile.setReadable(true, false);
    summary.putString("report", reportFile.getAbsolutePath()); summary.putInt("jobs", results.length());
    boolean allPassed = true; for (int i = 0; i < results.length(); i++) allPassed &= results.getJSONObject(i).optBoolean("passed");
    finish(allPassed ? android.app.Activity.RESULT_OK : android.app.Activity.RESULT_CANCELED, summary);
  }

  private static void copyBytes(InputStream in, java.io.OutputStream out) throws IOException {
    byte[] buffer = new byte[65536]; int n; while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n); out.flush();
  }

  /** Read-only probe for Android's decoded Bitmap gainmap and matching MediaStore row. */
  private void runGalleryProbe(String specPath, Bundle summary) throws Exception {
    File external = getTargetContext().getExternalFilesDir(null);
    if (external == null) throw new IOException("External files directory unavailable");
    File spec = new File(specPath == null ? "" : specPath).getCanonicalFile();
    String root = external.getCanonicalPath() + File.separator;
    if (!spec.getPath().startsWith(root) || !spec.isFile())
      throw new IOException("gallerySpec must be inside target external files directory: " + root);
    JSONObject input = new JSONObject(new String(Files.readAllBytes(spec.toPath()), StandardCharsets.UTF_8));
    JSONArray files = input.getJSONArray("files");
    JSONArray rows = new JSONArray();
    for (int i = 0; i < files.length(); i++) {
      JSONObject item = files.getJSONObject(i);
      JSONObject row = new JSONObject();
      if (item.has("title")) row.put("title", item.getString("title"));
      String path = item.optString("path", "");
      String uriString = item.optString("uri", "");
      Uri uri = null;
      if (!uriString.isEmpty()) {
        uri = Uri.parse(uriString);
        if (!"content".equals(uri.getScheme())) throw new IOException("probe uri must use content scheme");
        row.put("uri", uriString);
      } else {
        File file = new File(path).getCanonicalFile();
        if (!file.isAbsolute() || !file.isFile()) throw new IOException("File path missing or not a file: " + path);
        path = file.getAbsolutePath();
        row.put("path", path);
      }
      row.put("sha256", sha256(uri == null ? Files.newInputStream(new File(path).toPath()) : getTargetContext().getContentResolver().openInputStream(uri)));
      BitmapFactory.Options options = new BitmapFactory.Options();
      options.inSampleSize = 16;
      options.inScaled = false;
      Bitmap bitmap;
      try (InputStream stream = uri == null ? Files.newInputStream(new File(path).toPath()) : getTargetContext().getContentResolver().openInputStream(uri)) {
        if (stream == null) throw new IOException("Unable to open input stream");
        bitmap = BitmapFactory.decodeStream(stream, null, options);
      }
      if (bitmap == null) throw new IOException("BitmapFactory could not decode image");
      row.put("width", bitmap.getWidth()); row.put("height", bitmap.getHeight());
      row.put("colorSpaceName", bitmap.getColorSpace() == null ? JSONObject.NULL : bitmap.getColorSpace().getName());
      row.put("hasGainmap", bitmap.hasGainmap());
      if (bitmap.hasGainmap()) {
        Gainmap gainmap = bitmap.getGainmap();
        JSONObject gm = new JSONObject();
        gm.put("ratioMin", floats(gainmap.getRatioMin())); gm.put("ratioMax", floats(gainmap.getRatioMax()));
        gm.put("gamma", floats(gainmap.getGamma())); gm.put("epsilonSdr", floats(gainmap.getEpsilonSdr())); gm.put("epsilonHdr", floats(gainmap.getEpsilonHdr()));
        gm.put("displayRatioForFullHdr", gainmap.getDisplayRatioForFullHdr());
        gm.put("minDisplayRatioForHdrTransition", gainmap.getMinDisplayRatioForHdrTransition());
        row.put("gainmap", gm);
      }
      if (uri != null) row.put("mediaStore", queryMediaStoreUri(uri));
      else row.put("mediaStore", queryMediaStorePath(path));
      if (Boolean.parseBoolean(args.getString("nativeInspect", "false")) && uri == null) {
        try { row.put("nativeInspect", new JSONObject(NativeBridge.inspect(path))); }
        catch (Exception | LinkageError e) { row.put("nativeInspectError", e.toString()); }
      }
      rows.put(row);
      bitmap.recycle();
    }
    JSONObject report = new JSONObject();
    report.put("files", rows);
    File reportFile = new File(spec.getParentFile(), input.optString("report", "gallery-report.json")).getCanonicalFile();
    if (!reportFile.getParentFile().equals(spec.getParentFile()) || reportFile.equals(spec)) throw new IOException("Unsafe gallery report filename");
    Files.write(reportFile.toPath(), report.toString(2).getBytes(StandardCharsets.UTF_8));
    reportFile.setReadable(true, false);
    summary.putString("report", reportFile.getAbsolutePath());
    summary.putInt("files", rows.length());
    finish(android.app.Activity.RESULT_OK, summary);
  }

  private static JSONArray floats(float[] values) throws Exception {
    JSONArray out = new JSONArray(); for (float value : values) out.put(value); return out;
  }
  private static String sha256(InputStream stream) throws Exception {
    if (stream == null) throw new IOException("Unable to open file stream");
    MessageDigest digest = MessageDigest.getInstance("SHA-256");
    try (InputStream in = stream) { byte[] buffer = new byte[65536]; int n; while ((n = in.read(buffer)) != -1) digest.update(buffer, 0, n); }
    StringBuilder out = new StringBuilder(); for (byte b : digest.digest()) out.append(String.format(java.util.Locale.ROOT, "%02x", b & 0xff)); return out.toString();
  }
  private JSONObject queryMediaStorePath(String path) throws Exception {
    JSONArray matches = new JSONArray();
    try (Cursor cursor = getTargetContext().getContentResolver().query(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, null, "_data=?", new String[]{path}, null)) {
      if (cursor != null) while (cursor.moveToNext()) matches.put(cursorRow(cursor));
    }
    JSONObject result = new JSONObject(); result.put("query", "_data=?"); result.put("matches", matches); return result;
  }
  private JSONObject queryMediaStoreUri(Uri uri) throws Exception {
    try (Cursor cursor = getTargetContext().getContentResolver().query(uri, null, null, null, null)) {
      if (cursor == null || !cursor.moveToFirst()) return new JSONObject().put("available", false);
      return cursorRow(cursor);
    }
  }
  private static JSONObject cursorRow(Cursor cursor) throws Exception {
    JSONObject result = new JSONObject();
    java.util.Set<String> allowed = new java.util.HashSet<>(java.util.Arrays.asList("_id", "_display_name", "mime_type", "_size", "date_modified", "width", "height", "orientation", "is_pending", "owner_package_name", "generation_modified"));
    for (String column : cursor.getColumnNames()) {
      if (!allowed.contains(column)) continue;
      int index = cursor.getColumnIndex(column);
      if (index >= 0) result.put(column, cursor.isNull(index) ? JSONObject.NULL : cursor.getString(index));
    }
    return result;
  }
  private void writeReport(File file, JSONArray results, int passed, int total) throws Exception {
    JSONObject report = new JSONObject();
    report.put("currentSettings", new JSONObject(getTargetContext().getSharedPreferences("android_options_v2", 0).getAll()));
    report.put("jobs", results);
    report.put("passed", passed);
    report.put("total", total);
    File temporary = new File(file.getParentFile(), file.getName() + ".tmp");
    Files.write(temporary.toPath(), report.toString(2).getBytes(StandardCharsets.UTF_8));
    temporary.setReadable(true, false);
    Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
    file.setReadable(true, false);
  }

}
