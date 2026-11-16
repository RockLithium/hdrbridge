package org.hdrbridge;

import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import android.app.UiAutomation;
import android.provider.DocumentsContract;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Test-only, view-tag-driven end-to-end smoke through the real app queue and SAF output path. */
final class UiSmokeRunner {
  private final Instrumentation instrumentation;
  private final Context context;
  private final File appRoot;
  private final File testDir;
  private final JSONObject spec;
  private final JSONObject report = new JSONObject();
  private final JSONArray checks = new JSONArray();
  private final ArrayList<AppModel.Source> addedSources = new ArrayList<>();
  private final ArrayList<File> stagedFiles = new ArrayList<>();
  private final ArrayList<Uri> createdOutputs = new ArrayList<>();
  private final ArrayList<AppModel.Task> smokeTasks = new ArrayList<>();
  private AppModel model;
  private MainActivity activity;
  private ArrayList<AppModel.Task> savedTasks;
  private final ArrayList<Boolean> savedSelection = new ArrayList<>();
  private Map<String, ?> savedPreferences;
  private final ArrayList<AppModel.Source> savedSources = new ArrayList<>();

  static JSONObject run(Instrumentation instrumentation, String specPath) throws Exception {
    return new UiSmokeRunner(instrumentation, specPath).execute();
  }

  private UiSmokeRunner(Instrumentation instrumentation, String specPath) throws Exception {
    this.instrumentation = instrumentation;
    context = instrumentation.getTargetContext();
    File external = context.getExternalFilesDir(null);
    if (external == null) throw new IOException("External files directory unavailable");
    appRoot = external.getCanonicalFile();
    File specFile = new File(specPath).getCanonicalFile();
    if (!inside(appRoot, specFile) || !specFile.isFile()) throw new IOException("uiSmokeSpec must be inside app external files");
    spec = new JSONObject(new String(Files.readAllBytes(specFile.toPath()), StandardCharsets.UTF_8));
    File requestedDir = new File(spec.getString("testDir")).getCanonicalFile();
    if (!inside(appRoot, requestedDir)) throw new IOException("testDir must be inside app external files");
    if (!requestedDir.mkdirs() && !requestedDir.isDirectory()) throw new IOException("Cannot create UI smoke testDir");
    testDir = requestedDir;
  }

  private JSONObject execute() throws Exception {
    model = AppModel.get(context);
    require("AppModel idle before smoke", !model.running);
    snapshotModelAndPrefs();
    try {
      launchMainActivity();
      formatQualityChecks();
      String folderPref = context.getSharedPreferences("android_options_v2", 0).getString("folder", "");
      require("Persisted output folder available", !folderPref.isEmpty());
      report.put("outputFolder", folderPref);

      AppModel.Source hdr = stageAndAdd(spec.getString("hdr"), "HDRBridge_UI_Smoke_Xiaomi.jpg");
      AppModel.Source sdr = spec.has("sdr") ? stageAndAdd(spec.getString("sdr"), "HDRBridge_UI_Smoke_SDR.jpg") : null;
      AppModel.Source corrupt = spec.has("corrupt") ? stageAndAdd(spec.getString("corrupt"), "HDRBridge_UI_Smoke_Corrupt.jpg") : null;
      waitForInspect(hdr, false);
      if (sdr != null) waitForInspect(sdr, false);
      if (corrupt != null) waitForInspect(corrupt, true);
      if (sdr != null) require("SDR fixture inspected as non-HDR", !"gain-map-hdr".equals(sdr.info.optString("assetKind")) && !"direct-hdr".equals(sdr.info.optString("assetKind")));

      filesPageChecks(hdr);
      inspectCheck(hdr);
      previewCheck(hdr);
      tabRoundTrip();
      orderSmokeSourcesFailureFirst(hdr, sdr, corrupt);
      addQueueAndStart();
      awaitQueue();
      recordQueueResults(hdr, sdr, corrupt);
      repeatStartIsNoop();
      model.cancel();
      require("Idle cancel has no side effects", !model.cancelled && !model.running);
      resetSettingsCheck();
      stopAndRemoveGuardSmoke(hdr);
      report.put("passed", true);
    } catch (Exception | LinkageError e) {
      report.put("passed", false); report.put("error", e.toString());
    } finally {
      try { saveScreenshot("ui-smoke-final.png"); } catch (Exception e) { try { report.put("finalScreenshotError", e.toString()); } catch (Exception ignored) {} }
      cleanup();
    }
    report.put("checks", checks);
    return report;
  }

  private void snapshotModelAndPrefs() {
    synchronized (model) {
      savedTasks = new ArrayList<>(model.tasks);
      savedSources.addAll(model.sources);
      for (AppModel.Source source : savedSources) savedSelection.add(source.selected);
      model.tasks.clear();
      for (AppModel.Source source : savedSources) source.selected = false;
    }
    android.content.SharedPreferences prefs = context.getSharedPreferences("android_options_v2", 0);
    savedPreferences = new HashMap<>(prefs.getAll());
    if (!prefs.getBoolean("notificationsAsked", false)) prefs.edit().putBoolean("notificationsAsked", true).commit();
  }

  private void launchMainActivity() throws Exception {
    final MainActivity[] resumed = new MainActivity[1];
    Application app = (Application) context.getApplicationContext();
    Application.ActivityLifecycleCallbacks callbacks = new Application.ActivityLifecycleCallbacks() {
      public void onActivityCreated(Activity a, Bundle b) {}
      public void onActivityStarted(Activity a) {}
      public void onActivityResumed(Activity a) { if (a instanceof MainActivity) resumed[0] = (MainActivity) a; }
      public void onActivityPaused(Activity a) {}
      public void onActivityStopped(Activity a) {}
      public void onActivitySaveInstanceState(Activity a, Bundle b) {}
      public void onActivityDestroyed(Activity a) { if (a == resumed[0]) resumed[0] = null; }
    };
    app.registerActivityLifecycleCallbacks(callbacks);
    boolean registered = true;
    try {
    Intent intent = new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
    context.startActivity(intent);
      long deadline = SystemClock.elapsedRealtime() + 15000;
      MainActivity candidate = null;
      long stableSince = 0;
      while (SystemClock.elapsedRealtime() < deadline) {
        final MainActivity[] current = new MainActivity[1];
        instrumentation.runOnMainSync(() -> {
          MainActivity a = resumed[0];
          if (a != null && !a.isFinishing() && !a.isDestroyed() && a.getWindow() != null) {
            View decor = a.getWindow().getDecorView();
            if (decor.isAttachedToWindow() && decor.hasWindowFocus()) current[0] = a;
          }
        });
        if (current[0] == candidate && candidate != null) {
          if (stableSince == 0) stableSince = SystemClock.elapsedRealtime();
          if (SystemClock.elapsedRealtime() - stableSince >= 200) { activity = candidate; break; }
        } else {
          candidate = current[0];
          stableSince = candidate == null ? 0 : SystemClock.elapsedRealtime();
        }
        SystemClock.sleep(50);
      }
      require("Resumed MainActivity window attached and focused", activity != null);
      report.put("activity", activity.getClass().getName());
    } finally {
      if (registered) app.unregisterActivityLifecycleCallbacks(callbacks);
    }
  }

  private AppModel.Source stageAndAdd(String input, String name) throws Exception {
    File source = new File(input).getCanonicalFile();
    require("Fixture is app-local", inside(appRoot, source) && source.isFile());
    File staged = new File(testDir, name).getCanonicalFile();
    require("Staged filename stays in testDir", staged.getParentFile().equals(testDir) && name.startsWith("HDRBridge_UI_Smoke_") && !staged.equals(source));
    Files.copy(source.toPath(), staged.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    stagedFiles.add(staged); staged.setReadable(true, false);
    final AppModel.Source[] holder = new AppModel.Source[1];
    instrumentation.runOnMainSync(() -> {
      holder[0] = model.add(Uri.fromFile(staged), name);
      holder[0].selected = true;
      model.inspect(holder[0]);
      activity.refresh();
    });
    addedSources.add(holder[0]);
    return holder[0];
  }

  private void waitForInspect(AppModel.Source source, boolean expectFailure) throws Exception {
    long end = SystemClock.elapsedRealtime() + 30000;
    while (source.inspecting && SystemClock.elapsedRealtime() < end) SystemClock.sleep(50);
    require("Native inspect completed: " + source.name, !source.inspecting && (expectFailure ? !source.error.isEmpty() : source.info != null));
    JSONObject item = new JSONObject(); item.put("name", source.name); item.put("inspect", source.info);
    item.put("error", source.error);
    checks.put(item);
  }

  private void filesPageChecks(AppModel.Source hdr) throws Exception {
    require("FILES tab visible", tagVisible("files_tab"));
    require("OUTPUT folder control visible", tagVisible("output_format"));
    require("FILES add control visible", tagVisible("add_files"));
    require("FILES queue control visible", tagVisible("add_to_queue"));
    final boolean[] sourceVisible = new boolean[1], sourceInViewport = new boolean[1];
    instrumentation.runOnMainSync(() -> {
      View sourceName = findText(activity.getWindow().getDecorView(), hdr.name);
      sourceVisible[0] = sourceName != null && sourceName.getVisibility() == View.VISIBLE;
      Rect rect = new Rect(); sourceInViewport[0] = sourceName != null && sourceName.getGlobalVisibleRect(rect) && rect.height() > 0;
    });
    require("Staged source name visible", sourceVisible[0]);
    require("Source row in visible lower FILES region", sourceInViewport[0]);
    measureFilesLayout(hdr.name);
    require("Preview has not queued or created output", model.tasks.isEmpty());
    saveScreenshot("ui-smoke-files.png");
  }

  private void formatQualityChecks() throws Exception {
    JSONArray formatResults = new JSONArray();
    try {
      for (int format = 0; format <= 5; format++) {
        final int selected = format;
        instrumentation.runOnMainSync(() -> {
          activity.current = Options.defaults(selected);
          if (selected == 2) activity.current.compressed = false;
          activity.renderInline();
        });
        instrumentation.waitForIdleSync();
        QualitySample q = awaitLayout(() -> {
          View seek = findTag(activity.getWindow().getDecorView(), "output_quality");
          View outputArea = findTag(activity.getWindow().getDecorView(), "output_region");
          if (seek == null || outputArea == null || seek.getHeight() == 0 || !seek.isAttachedToWindow()
              || outputArea.getHeight() == 0 || !outputArea.isAttachedToWindow()) return null;
          android.graphics.Rect actual = new android.graphics.Rect(), output = new android.graphics.Rect();
          boolean visible = seek.getGlobalVisibleRect(actual);
          boolean withinOutput = outputArea.getGlobalVisibleRect(output) && output.contains(actual);
          int fullyVisible = visible && actual.height() == seek.getHeight() && withinOutput ? 1 : 0;
          View lossless = findTag(activity.getWindow().getDecorView(), "output_lossless");
          View compression = findTag(activity.getWindow().getDecorView(), "output_compression");
          int losslessEnabled = lossless != null && lossless.isEnabled() ? 1 : 0;
          int losslessChecked = lossless instanceof android.widget.CheckBox && ((android.widget.CheckBox) lossless).isChecked() ? 1 : 0;
          int compressionEnabled = compression != null && compression.isEnabled() ? 1 : 0;
          return new QualitySample(fullyVisible == 1, seek.isEnabled(), losslessEnabled == 1,
              losslessChecked == 1, compressionEnabled == 1);
        });
        boolean expectQualityEnabled = selected == 0 || selected == 1 || selected == 5;
        require("Format " + selected + " quality fully visible in OUTPUT", q.visible);
        require("Format " + selected + " quality enabled state", q.enabled == expectQualityEnabled);
        if (selected != 4) {
          String secondTag = selected == 0 ? "output_peak" : selected == 2 ? "output_compression" : "output_transfer";
          require("Format " + selected + " keeps paired options in two columns", hasTwoColumnPair("output_gamut", secondTag));
        }
        if (selected == 1 || selected == 2) {
          require("Format " + selected + " lossless forced and locked", !q.losslessEnabled && q.losslessChecked);
        }
        if (selected == 3 || selected == 4)
          require("Format " + selected + " lossless default disables quality", q.losslessEnabled && q.losslessChecked);
        if (selected == 2) require("Uncompressed TIFF disables quality", !q.enabled && q.compressionEnabled);
        formatResults.put(new JSONObject().put("format", selected).put("qualityVisible", q.visible)
            .put("qualityEnabled", q.enabled).put("losslessEnabled", q.losslessEnabled)
            .put("losslessChecked", q.losslessChecked).put("compressionEnabled", q.compressionEnabled));
      }
    } finally {
      instrumentation.runOnMainSync(() -> { activity.current = Options.defaults(0); activity.renderInline(); });
      instrumentation.waitForIdleSync();
    }
    report.put("formatQualityChecks", formatResults);
  }

  private void resetSettingsCheck() throws Exception {
    final android.content.SharedPreferences prefs = context.getSharedPreferences("android_options_v2", 0);
    final String folderBefore = prefs.getString("folder", "");
    final ArrayList<AppModel.Task> tasksBefore = new ArrayList<>(smokeTasks);
    final ArrayList<String> taskOptionsBefore = new ArrayList<>();
    final ArrayList<AppModel.Source> sourcesBefore;
    final ArrayList<AppModel.Task> modelTasksBefore;
    synchronized (model) {
      sourcesBefore = new ArrayList<>(model.sources);
      modelTasksBefore = new ArrayList<>(model.tasks);
      for (AppModel.Task task : smokeTasks) taskOptionsBefore.add(task.options.save().toString());
    }
    require("Reset settings button is present", tagVisible("reset_settings"));
    Options custom0 = Options.defaults(0);
    custom0.quality = 61; custom0.gamut = "rec709"; custom0.scale = 4; custom0.channels = 1;
    custom0.peak = 3; custom0.exif = false; custom0.xmp = false;
    Options custom5 = Options.defaults(5);
    custom5.quality = 47; custom5.gamut = "p3"; custom5.transfer = 1; custom5.exif = false; custom5.xmp = false;
    prefs.edit().putInt("format", 0).putString("options_0", custom0.save().toString())
        .putString("options_5", custom5.save().toString()).commit();
    instrumentation.runOnMainSync(() -> { activity.current = custom0.copy(); activity.renderInline(); });
    instrumentation.waitForIdleSync();
    instrumentation.runOnMainSync(activity::resetOutputSettings);
    instrumentation.waitForIdleSync();
    Options defaults0 = Options.defaults(0);
    require("Reset restores Ultra HDR default quality/gamut/peak/scale/channels/EXIF/XMP",
        activity.current.quality == defaults0.quality && activity.current.gamut.equals(defaults0.gamut)
            && activity.current.peak == defaults0.peak && activity.current.scale == defaults0.scale
            && activity.current.channels == defaults0.channels && activity.current.exif && activity.current.xmp);
    for (int format = 0; format < Options.LABELS.length; format++)
      require("Reset restores format " + format + " defaults", Options.load(prefs, format).save().toString().equals(Options.defaults(format).save().toString()));
    require("Reset preserves output folder", folderBefore.equals(prefs.getString("folder", ""))
        && (folderBefore.isEmpty() || folderBefore.equals(activity.folder.toString())));
    require("Reset leaves Files/Tasks and task options unchanged", activity.tasksSelected
        && sameReferences(sourcesBefore, model.sources) && sameReferences(modelTasksBefore, model.tasks)
        && sameReferences(tasksBefore, smokeTasks));
    synchronized (model) {
      for (int i = 0; i < smokeTasks.size(); i++)
        require("Reset does not alter queued task option snapshot " + i, taskOptionsBefore.get(i).equals(smokeTasks.get(i).options.save().toString()));
    }
    checks.put(new JSONObject().put("resetSettings", "passed"));
  }

  private boolean hasTwoColumnPair(String firstTag, String secondTag) throws Exception {
    final boolean[] result = new boolean[1];
    for (int retry = 0; retry < 12 && !result[0]; retry++) {
      instrumentation.waitForIdleSync();
      instrumentation.runOnMainSync(() -> {
        View first = findTag(activity.getWindow().getDecorView(), firstTag);
        View second = findTag(activity.getWindow().getDecorView(), secondTag);
        if (first == null || second == null || first.getWidth() == 0 || second.getWidth() == 0) return;
        View firstCell = (View) first.getParent(), secondCell = (View) second.getParent();
        View pair = (View) firstCell.getParent();
        if (secondCell.getParent() != pair || pair.getWidth() == 0) return;
        android.graphics.Rect a = new android.graphics.Rect(), b = new android.graphics.Rect(), p = new android.graphics.Rect();
        if (!firstCell.getGlobalVisibleRect(a) || !secondCell.getGlobalVisibleRect(b) || !pair.getGlobalVisibleRect(p)) return;
        result[0] = a.width() > 0 && b.width() > 0 && p.width() > a.width() && p.width() > b.width()
            && (a.right <= b.left || b.right <= a.left);
      });
      if (!result[0]) SystemClock.sleep(50);
    }
    return result[0];
  }

  private static boolean sameReferences(List<?> expected, List<?> actual) {
    if (expected.size() != actual.size()) return false;
    for (int i = 0; i < expected.size(); i++) if (expected.get(i) != actual.get(i)) return false;
    return true;
  }

  private void inspectCheck(AppModel.Source hdr) throws Exception {
    clickInSourceRow(hdr.name, "source_inspect");
    waitUi(250);
    require("Inspector selected source", activity.activeSource == hdr && hdr.info != null);
    require("Inspector native asset kind", hdr.info.optString("assetKind").equals("gain-map-hdr") || hdr.info.optString("assetKind").equals("direct-hdr"));
    final boolean[] dialogDetailsVisible = new boolean[1];
    instrumentation.runOnMainSync(() -> {
      TextView content = activity.details;
      if (content == null || !content.isAttachedToWindow()) return;
      View dialogRoot = content.getRootView();
      View title = findText(dialogRoot, "SOURCE INSPECTOR");
      dialogDetailsVisible[0] = title != null && content.getText().toString().contains(hdr.name);
    });
    require("Inspector dialog exposes source details", dialogDetailsVisible[0]);
    checks.put(new JSONObject().put("inspector", hdr.name).put("assetKind", hdr.info.optString("assetKind")));
    // Dismiss the real dialog through its positive button.
    final View[] dialogButton = new View[1];
    instrumentation.runOnMainSync(() -> {
      if (activity.details != null) dialogButton[0] = activity.details.getRootView().findViewById(android.R.id.button1);
    });
    require("Inspector close button found in dialog window", dialogButton[0] != null);
    click(dialogButton[0]);
    waitUi(200);
  }

  private void previewCheck(AppModel.Source hdr) throws Exception {
    final Instrumentation.ActivityMonitor monitor = instrumentation.addMonitor(HdrPreviewActivity.class.getName(), null, false);
    clickInSourceRow(hdr.name, "source_preview");
    Activity preview = instrumentation.waitForMonitorWithTimeout(monitor, 10000);
    instrumentation.removeMonitor(monitor);
    require("Native preview activity launched", preview instanceof HdrPreviewActivity);
    try {
      Field sessionField = HdrPreviewActivity.class.getDeclaredField("nativeSession"); sessionField.setAccessible(true);
      Field widthField = HdrPreviewActivity.class.getDeclaredField("imageWidth"); widthField.setAccessible(true);
      Field heightField = HdrPreviewActivity.class.getDeclaredField("imageHeight"); heightField.setAccessible(true);
      Field lockField = HdrPreviewActivity.class.getDeclaredField("lock"); lockField.setAccessible(true);
      Object stateLock = lockField.get(preview);
      long session = 0;
      int width = 0, height = 0;
      long end = SystemClock.elapsedRealtime() + 20000;
      while (SystemClock.elapsedRealtime() < end) {
        synchronized (stateLock) {
          session = sessionField.getLong(preview);
          width = widthField.getInt(preview);
          height = heightField.getInt(preview);
        }
        if (session != 0 && width > 0 && height > 0) break;
        SystemClock.sleep(50);
      }
      require("Preview native session decoded", session != 0 && width > 0 && height > 0);
      checks.put(new JSONObject().put("preview", "nativeDecoded").put("width", width).put("height", height));
    } finally {
      instrumentation.runOnMainSync(preview::finish);
      waitUi(250);
    }
  }

  private void tabRoundTrip() throws Exception {
    clickTag("tasks_tab");
    require("TASKS page visible", activity.tasksPage.getVisibility() == View.VISIBLE && activity.filesPage.getVisibility() != View.VISIBLE);
    clickTag("files_tab");
    require("FILES page visible after round trip", activity.filesPage.getVisibility() == View.VISIBLE && activity.tasksPage.getVisibility() != View.VISIBLE);
    checks.put(new JSONObject().put("tabs", "FILES↔TASKS"));
  }

  private void orderSmokeSourcesFailureFirst(AppModel.Source hdr, AppModel.Source sdr, AppModel.Source corrupt) {
    synchronized (model) {
      model.sources.removeAll(addedSources);
      if (corrupt != null) model.sources.add(corrupt);
      if (sdr != null) model.sources.add(sdr);
      model.sources.add(hdr);
    }
    instrumentation.runOnMainSync(activity::refresh);
  }

  private void addQueueAndStart() throws Exception {
    clickTag("add_to_queue");
    waitUi(200);
    clickTag("tasks_tab");
    require("TASKS tab visible after queue add", activity.tasksPage.getVisibility() == View.VISIBLE);
    synchronized (model) {
      for (AppModel.Task task : model.tasks) {
        require("Smoke output name has prefix", task.filename.startsWith("HDRBridge_UI_Smoke_"));
        smokeTasks.add(task);
      }
    }
    require("Tasks created", !smokeTasks.isEmpty());
    require("TASKS remove control visible", tagVisible("remove_all"));
    require("Start all control visible", tagVisible("start_all"));
    require("One progress indicator per task", countProgress(activity.taskList) == smokeTasks.size());
    measureTasksLayout();
    saveScreenshot("ui-smoke-tasks-ready.png");
    clickTag("start_all");
  }

  private void awaitQueue() throws Exception {
    long end = SystemClock.elapsedRealtime() + 180000;
    while ((model.running || hasReadyOrRunning(smokeTasks)) && SystemClock.elapsedRealtime() < end) SystemClock.sleep(100);
    require("Queue completed before timeout", !model.running && !hasReadyOrRunning(smokeTasks));
    for (AppModel.Task task : smokeTasks) if (task.output != null) createdOutputs.add(task.output);
    instrumentation.runOnMainSync(activity::refresh);
    saveScreenshot("ui-smoke-tasks-finished.png");
  }

  private void recordQueueResults(AppModel.Source hdr, AppModel.Source sdr, AppModel.Source corrupt) throws Exception {
    JSONArray results = new JSONArray();
    java.util.HashSet<String> outputUris = new java.util.HashSet<>();
    for (AppModel.Task task : smokeTasks) {
      JSONObject item = new JSONObject().put("source", task.source.name).put("filename", task.filename)
          .put("status", task.status).put("detail", task.detail).put("progress", task.progress)
          .put("outputUri", task.output == null ? JSONObject.NULL : task.output.toString()).put("sha256", task.hash);
      results.put(item);
      if (task.output != null) outputUris.add(task.output.toString());
    }
    report.put("queueResults", results);
    require("HDR conversion completed", hasStatus(hdr, "Completed"));
    require("HDR task produced a new SAF output", hasOutput(hdr));
    if (sdr != null) require("SDR task skipped", hasStatus(sdr, "Skipped"));
    if (corrupt != null) require("Corrupt task failed without aborting batch", hasStatus(corrupt, "Failed"));
    require("All smoke output URIs are new and recorded", createdOutputs.size() == countOutputs(smokeTasks));
    require("Every smoke output URI is unique", outputUris.size() == countOutputs(smokeTasks));
  }

  private void repeatStartIsNoop() throws Exception {
    JSONArray before = statuses(smokeTasks);
    clickTag("start_all"); waitUi(300);
    JSONArray after = statuses(smokeTasks);
    require("Repeat Start all leaves terminal queue untouched", before.toString().equals(after.toString()));
    checks.put(new JSONObject().put("repeatStart", "terminal states unchanged"));
  }

  private void stopAndRemoveGuardSmoke(AppModel.Source hdr) throws Exception {
    CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
    model.executor.execute(() -> { entered.countDown(); try { release.await(5, TimeUnit.SECONDS); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); } });
    require("Executor gate entered", entered.await(3, TimeUnit.SECONDS));
    AppModel.Task probe = new AppModel.Task(hdr, hdr.info == null ? Options.defaults(0) : activity.current.copy(), Uri.parse(context.getSharedPreferences("android_options_v2", 0).getString("folder", "")));
    probe.filename = "HDRBridge_UI_Smoke_StopProbe.jpg";
    synchronized (model) { model.tasks.add(probe); }
    instrumentation.runOnMainSync(activity::refresh);
    try {
      clickTag("start_all");
      require("Queue enters running state before task execution", model.running);
      clickTag("remove_all"); waitUi(150);
      require("Remove all is blocked during active queue", model.tasks.contains(probe));
      clickTag("start_all");
      require("Stop requests only active user cancellation", model.cancelled);
    } finally {
      if (model.running && !model.cancelled) model.cancel();
      release.countDown();
    }
    long end = SystemClock.elapsedRealtime() + 5000;
    while (model.running && SystemClock.elapsedRealtime() < end) SystemClock.sleep(50);
    require("Cancelled queued task remains pending", !model.running && "Ready".equals(probe.status));
    synchronized (model) { model.tasks.remove(probe); }
    checks.put(new JSONObject().put("stopRemoveGuard", "passed"));
    instrumentation.runOnMainSync(activity::refresh);
  }

  private void cleanup() {
    try {
      if (model != null && model.running) {
        model.cancel();
        long end = SystemClock.elapsedRealtime() + 15000;
        while (model.running && SystemClock.elapsedRealtime() < end) SystemClock.sleep(50);
        if (model.running) throw new IOException("Could not stop smoke queue during cleanup; preserving artifacts");
      }
      for (AppModel.Task task : smokeTasks) if (task.output != null && !createdOutputs.contains(task.output)) createdOutputs.add(task.output);
      if (model != null) {
        synchronized (model) {
          model.tasks.clear(); model.tasks.addAll(savedTasks == null ? new ArrayList<>() : savedTasks);
          for (AppModel.Source source : addedSources) model.sources.remove(source);
          for (int i = 0; i < savedSources.size(); i++) savedSources.get(i).selected = savedSelection.get(i);
        }
        for (AppModel.Source source : addedSources) model.release(source);
      }
      for (Uri uri : createdOutputs) try { DocumentsContract.deleteDocument(context.getContentResolver(), uri); } catch (Exception ignored) {}
      for (File file : stagedFiles) file.delete();
      android.content.SharedPreferences prefs = context.getSharedPreferences("android_options_v2", 0);
      android.content.SharedPreferences.Editor edit = prefs.edit().clear();
      if (savedPreferences != null) for (Map.Entry<String, ?> entry : savedPreferences.entrySet()) {
        Object value = entry.getValue(); String key = entry.getKey();
        if (value instanceof String) edit.putString(key, (String) value);
        else if (value instanceof Boolean) edit.putBoolean(key, (Boolean) value);
        else if (value instanceof Integer) edit.putInt(key, (Integer) value);
        else if (value instanceof Long) edit.putLong(key, (Long) value);
        else if (value instanceof Float) edit.putFloat(key, (Float) value);
        else if (value instanceof Set) {
          @SuppressWarnings("unchecked") Set<String> strings = (Set<String>) value;
          edit.putStringSet(key, strings);
        }
      }
      if (!edit.commit()) throw new IOException("Could not restore android_options_v2 preferences");
      if (activity != null) instrumentation.runOnMainSync(activity::finish);
    } catch (Exception e) {
      try { report.put("cleanupError", e.toString()); } catch (Exception ignored) {}
    }
  }

  private void saveScreenshot(String name) throws Exception {
    Bitmap screenshot = instrumentation.getUiAutomation(UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES).takeScreenshot();
    if (screenshot == null) throw new IOException("UiAutomation screenshot unavailable: " + name);
    File output = new File(testDir, name);
    try (FileOutputStream stream = new FileOutputStream(output)) { screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream); }
    screenshot.recycle(); output.setReadable(true, false);
    if (name.endsWith("finished.png")) report.put("finishedScreenshot", output.getAbsolutePath());
  }

  private void measureFilesLayout(String sourceName) throws Exception {
    LayoutSample sample = awaitLayout(() -> {
      View add = findTag(activity.getWindow().getDecorView(), "add_files");
      View queue = findTag(activity.getWindow().getDecorView(), "add_to_queue");
      View sourceLabel = findText(activity.getWindow().getDecorView(), sourceName);
      View sourceRow = sourceLabel;
      while (sourceRow != null && findTag(sourceRow, "source_preview") == null) sourceRow = (View) sourceRow.getParent();
      View preview = sourceRow == null ? null : findTag(sourceRow, "source_preview");
      View inspect = sourceRow == null ? null : findTag(sourceRow, "source_inspect");
      if (add == null || queue == null || preview == null || inspect == null
          || add.getHeight() == 0 || queue.getHeight() == 0 || preview.getHeight() == 0 || inspect.getHeight() == 0
          || activity.filesScroll.getHeight() == 0 || !add.isAttachedToWindow() || !queue.isAttachedToWindow()
          || !preview.isAttachedToWindow() || !inspect.isAttachedToWindow() || !activity.filesScroll.isAttachedToWindow()) return null;
      View addRow = (View) add.getParent(), queueRow = (View) queue.getParent();
      int[] previewXY = new int[2], inspectXY = new int[2];
      preview.getLocationOnScreen(previewXY); inspect.getLocationOnScreen(inspectXY);
      int addGap = activity.filesScroll.getTop() - addRow.getBottom();
      int queueGap = queueRow.getTop() - activity.filesScroll.getBottom();
      return new LayoutSample(addGap, queueGap, preview.getHeight(), inspect.getHeight(),
          Math.abs(previewXY[1] - inspectXY[1]));
    });
    double density = context.getResources().getDisplayMetrics().density;
    JSONObject m = new JSONObject();
    m.put("files.addToScrollGapDp", sample.firstGap / density);
    m.put("files.scrollToQueueGapDp", sample.secondGap / density);
    m.put("files.previewHeightDp", sample.previewHeight / density);
    m.put("files.inspectHeightDp", sample.inspectHeight / density);
    m.put("files.previewInspectTopDeltaDp", sample.actionsTopDelta / density);
    JSONObject layout = report.optJSONObject("layoutMetrics"); if (layout == null) layout = new JSONObject();
    layout.put("files", m); report.put("layoutMetrics", layout);
    require("Files scroll separated from Add files by >=4dp", sample.firstGap >= context.getResources().getDisplayMetrics().density * 4);
    require("Add queue separated from files scroll by >=6dp", sample.secondGap >= context.getResources().getDisplayMetrics().density * 6);
    require("Preview and Inspect heights equal", sample.previewHeight == sample.inspectHeight);
    require("Preview and Inspect heights are 32dp", sample.previewHeight == Math.round(32 * density));
    require("Preview and Inspect controls share vertical alignment", sample.actionsTopDelta <= 1);
  }

  private void measureTasksLayout() throws Exception {
    LayoutSample sample = awaitLayout(() -> {
      View remove = findTag(activity.getWindow().getDecorView(), "remove_all");
      View start = findTag(activity.getWindow().getDecorView(), "start_all");
      if (remove == null || start == null || remove.getHeight() == 0 || start.getHeight() == 0 || activity.tasksScroll.getHeight() == 0
          || !remove.isAttachedToWindow() || !start.isAttachedToWindow() || !activity.tasksScroll.isAttachedToWindow()) return null;
      View removeRow = (View) remove.getParent();
      View tabsRow = (View) activity.filesTab.getParent();
      int[] barXY = new int[2], tabsXY = new int[2];
      removeRow.getLocationOnScreen(barXY); tabsRow.getLocationOnScreen(tabsXY);
      int globalBarBottom = barXY[1] + removeRow.getHeight();
      int globalTabsTop = tabsXY[1];
      return new LayoutSample(removeRow.getTop() - activity.tasksScroll.getBottom(),
          globalTabsTop - globalBarBottom, remove.getHeight(), start.getHeight(), Math.abs(remove.getHeight() - start.getHeight()));
    });
    double density = context.getResources().getDisplayMetrics().density;
    JSONObject m = new JSONObject();
    m.put("tasks.scrollToBottomBarGapDp", sample.firstGap / density);
    m.put("tasks.bottomBarToTabsGapDp", sample.secondGap / density);
    m.put("tasks.removeAllHeightDp", sample.previewHeight / density);
    m.put("tasks.startAllHeightDp", sample.inspectHeight / density);
    JSONObject layout = report.optJSONObject("layoutMetrics"); if (layout == null) layout = new JSONObject();
    layout.put("tasks", m); report.put("layoutMetrics", layout);
    require("Tasks scroll separated from bottom bar by >=6dp", sample.firstGap >= density * 6);
    require("Bottom bar separated from tabs by >=4dp", sample.secondGap >= density * 4);
    require("Remove all and Start all heights equal", sample.actionsTopDelta == 0);
    require("Queue buttons are 36dp", sample.previewHeight == Math.round(36 * density));
  }

  private <T> T awaitLayout(java.util.concurrent.Callable<T> capture) throws Exception {
    for (int i = 0; i < 12; i++) {
      instrumentation.waitForIdleSync();
      AtomicReference<T> result = new AtomicReference<>();
      instrumentation.runOnMainSync(() -> {
        try { result.set(capture.call()); } catch (Exception e) { throw new RuntimeException(e); }
      });
      if (result.get() != null) return result.get();
      SystemClock.sleep(50);
    }
    throw new IOException("Layout remained unmeasured after short wait");
  }

  private static final class LayoutSample {
    final int firstGap, secondGap, previewHeight, inspectHeight, actionsTopDelta;
    LayoutSample(int firstGap, int secondGap, int previewHeight, int inspectHeight, int actionsTopDelta) {
      this.firstGap = firstGap; this.secondGap = secondGap; this.previewHeight = previewHeight;
      this.inspectHeight = inspectHeight; this.actionsTopDelta = actionsTopDelta;
    }
  }
  private static final class QualitySample {
    final boolean visible, enabled, losslessEnabled, losslessChecked, compressionEnabled;
    QualitySample(boolean visible, boolean enabled, boolean losslessEnabled, boolean losslessChecked, boolean compressionEnabled) {
      this.visible = visible; this.enabled = enabled; this.losslessEnabled = losslessEnabled;
      this.losslessChecked = losslessChecked; this.compressionEnabled = compressionEnabled;
    }
  }

  private View tagged(String tag) {
    final View[] result = new View[1];
    instrumentation.runOnMainSync(() -> result[0] = findTag(activity.getWindow().getDecorView(), tag));
    if (result[0] == null) throw new IllegalStateException("Missing view tag: " + tag);
    return result[0];
  }
  private boolean tagVisible(String tag) { return tagged(tag).getVisibility() == View.VISIBLE; }
  private void clickTag(String tag) throws Exception { click(tagged(tag)); }
  private void click(View view) throws Exception {
    final boolean[] clicked = new boolean[1];
    instrumentation.runOnMainSync(() -> clicked[0] = view.performClick());
    require("View click accepted: " + view.getTag(), clicked[0]);
    waitUi(120);
  }
  private void clickInSourceRow(String name, String tag) throws Exception {
    final View[] target = new View[1];
    instrumentation.runOnMainSync(() -> {
      View row = findText(activity.getWindow().getDecorView(), name);
      while (row != null && findTag(row, tag) == null) row = (View) row.getParent();
      if (row != null) target[0] = findTag(row, tag);
    });
    require("Source action found: " + tag, target[0] != null);
    click(target[0]);
  }
  private static View findTag(View root, String tag) {
    if (tag.equals(root.getTag())) return root;
    if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
      View found = findTag(((ViewGroup) root).getChildAt(i), tag); if (found != null) return found;
    }
    return null;
  }
  private static View findText(View root, String text) {
    if (root instanceof TextView && text.contentEquals(((TextView) root).getText())) return root;
    if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
      View found = findText(((ViewGroup) root).getChildAt(i), text); if (found != null) return found;
    }
    return null;
  }
  private void waitUi(long ms) throws Exception { SystemClock.sleep(ms); }
  private void require(String name, boolean passed) throws Exception { checks.put(new JSONObject().put("name", name).put("passed", passed)); if (!passed) throw new IOException("UI smoke failed: " + name); }
  private static boolean inside(File root, File file) throws IOException { return file.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator); }
  private static boolean hasReadyOrRunning(List<AppModel.Task> tasks) { for (AppModel.Task t : tasks) if ("Ready".equals(t.status) || "Running".equals(t.status)) return true; return false; }
  private static int countProgress(View root) { int n = root instanceof android.widget.ProgressBar ? 1 : 0; if (root instanceof ViewGroup) for (int i=0;i<((ViewGroup)root).getChildCount();i++) n += countProgress(((ViewGroup)root).getChildAt(i)); return n; }
  private boolean hasStatus(AppModel.Source source, String status) { for (AppModel.Task t : smokeTasks) if (t.source == source && status.equals(t.status)) return true; return false; }
  private boolean hasOutput(AppModel.Source source) { for (AppModel.Task t : smokeTasks) if (t.source == source && t.output != null) return true; return false; }
  private int countOutputs(List<AppModel.Task> tasks) { int n=0; for (AppModel.Task task:tasks) if(task.output!=null)n++; return n; }
  private static JSONArray statuses(List<AppModel.Task> tasks) throws Exception { JSONArray out=new JSONArray(); for(AppModel.Task t:tasks) out.put(t.status+"|"+(t.output==null?"":t.output.toString())); return out; }
}
