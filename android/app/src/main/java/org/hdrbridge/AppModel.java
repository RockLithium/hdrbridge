package org.hdrbridge;

import android.content.*;
import android.database.Cursor;
import android.net.Uri;
import android.provider.*;
import java.io.*;
import java.security.*;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;

/** Process-retained state; work uses application context only. All native calls are serialized. */
final class AppModel {
  static AppModel instance;
  static synchronized AppModel get(Context c) {
    if (instance == null)
      instance = new AppModel(c.getApplicationContext());
    return instance;
  }
  final Context context;
  final ExecutorService executor = Executors.newSingleThreadExecutor();
  final ArrayList<Source> sources = new ArrayList<>();
  final ArrayList<Task> tasks = new ArrayList<>();
  volatile boolean running, cancelled, updateInstalling;
  volatile Runnable listener;
  static class Source {
    final Uri uri;
    final String name;
    File file;
    JSONObject info;
    String error = "";
    boolean selected = true, inspecting;
    Source(Uri u, String n) {
      uri = u;
      name = n;
    }
  }
  static class Task {
    Source source;
    Options options;
    Uri folder, output;
    String filename, status = "Ready", detail = "", hash = "";
    int progress;
    Task(Source s, Options o, Uri f) {
      source = s;
      options = o;
      folder = f;
      filename = o.filename(s.name);
    }
  }
  AppModel(Context c) {
    context = c;
    try { ConversionWorkspace.cleanupAbandoned(c.getCacheDir()); }
    catch (IOException ignored) { /* Retry cleanup at a later launch. */ }
    File[] files = c.getCacheDir().listFiles();
    if (files != null)
      for (File f : files)
        if (f.getName().startsWith("hdr_source_") || f.getName().startsWith("hdr_output_"))
          f.delete();
  }
  void changed() {
    Runnable r = listener;
    if (r != null)
      r.run();
  }
  synchronized Source add(Uri uri, String name) {
    for (Source s : sources)
      if (s.uri.equals(uri)) {
        s.selected = true;
        return s;
      }
    Source s = new Source(uri, name);
    sources.add(s);
    return s;
  }
  void inspect(Source s) {
    synchronized (this) {
      if (s.inspecting || updateInstalling)
        return;
      s.inspecting = true;
    }
    changed();
    executor.execute(() -> {
      try {
        if (hasSourceReference(s)) inspectNow(s);
      } catch (Exception e) {
        s.error = message(e);
      } finally {
        s.inspecting = false;
        release(s);
        changed();
      }
    });
  }
  void inspectNow(Source s) throws Exception {
    if (s.info != null)
      return;
    if (s.file == null) {
      String ext = "";
      int d = s.name.lastIndexOf('.');
      if (d >= 0)
        ext = s.name.substring(d).replaceAll("[^A-Za-z0-9.]", "");
      File staged = File.createTempFile("hdr_source_", ext, context.getCacheDir());
      try (InputStream in = context.getContentResolver().openInputStream(s.uri);
          OutputStream out = new FileOutputStream(staged)) {
        if (in == null)
          throw new IOException("Cannot open source");
        copy(in, out);
      } catch (Exception e) {
        staged.delete();
        throw e;
      }
      s.file = staged;
    }
    s.info = new JSONObject(NativeBridge.inspect(s.file.getAbsolutePath()));
    s.error = "";
  }
  synchronized void addTasks(Options o, Uri folder) {
    for (Source s : sources)
      if (s.selected)
        tasks.add(new Task(s, o.copy(), folder));
    changed();
  }
  boolean start(Task only) {
    final ArrayList<Task> batch = new ArrayList<>();
    synchronized (this) {
      if (running || updateInstalling) return false;
      if (only != null) {
        if (tasks.contains(only) && "Ready".equals(only.status)) batch.add(only);
      } else {
        for (Task task : tasks) if ("Ready".equals(task.status)) batch.add(task);
      }
      if (batch.isEmpty()) return false;
      running = true;
      cancelled = false;
      NativeBridge.resetCancel();
    }
    try {
      context.startForegroundService(new Intent(context, ConversionService.class));
    } catch (RuntimeException e) {
      running = false;
      changed();
      throw e;
    }
    executor.execute(() -> {
      try {
        for (Task t : batch) {
          if (cancelled)
            break;
          if (t.status.equals("Ready"))
            convert(t);
        }
      } finally {
        running = false;
        context.stopService(new Intent(context, ConversionService.class));
        changed();
      }
    });
    changed();
    return true;
  }
  void cancel() {
    synchronized (this) {
      if (!running) return;
      cancelled = true;
    }
    NativeBridge.cancel();
    changed();
  }
  void convert(Task t) {
    t.status = "Running";
    t.progress = 0;
    t.detail = "Inspecting";
    changed();
    File output = null;
    ConversionWorkspace workspace = null;
    try {
      inspectNow(t.source);
      if (cancelled)
        throw new InterruptedException("Cancelled");
      String kind = t.source.info.optString("assetKind");
      if (!kind.equals("direct-hdr") && !kind.equals("gain-map-hdr")) {
        t.status = "Skipped";
        t.detail = "No HDR data";
        return;
      }
      int dot = t.filename.lastIndexOf('.');
      String extension = dot < 0 ? "" : t.filename.substring(dot);
      workspace = ConversionWorkspace.create(context.getCacheDir(), extension);
      output = workspace.output();
      JSONObject nativeOptions = t.options.json();
      if (t.options.format == 7) {
        File font = new File(context.getFilesDir(), "Roboto-Bold.ttf");
        try (InputStream in = context.getAssets().open("fonts/Roboto-Bold.ttf");
             OutputStream out = new FileOutputStream(font)) { copy(in, out); }
        nativeOptions.put("frame_font_path", font.getAbsolutePath());
        File logoDir = new File(context.getFilesDir(), "frame-logos");
        if (logoDir.exists() && !logoDir.isDirectory())
          throw new IOException("Frame logo path is not a directory");
        if (!logoDir.exists() && !logoDir.mkdirs())
          throw new IOException("Cannot create frame logo directory");
        String[] logoAssets = context.getAssets().list("frame-logos");
        if (logoAssets == null) throw new IOException("Cannot list frame logos");
        java.util.Set<String> copiedLogos = new java.util.HashSet<>();
        for (String name : logoAssets) {
          if (!name.endsWith(".svg")) continue;
          File logo = new File(logoDir, name);
          try (InputStream in = context.getAssets().open("frame-logos/" + name);
               OutputStream out = new FileOutputStream(logo)) { copy(in, out); }
          copiedLogos.add(name);
        }
        File[] staleLogos = logoDir.listFiles((dir, name) -> name.endsWith(".svg") && !copiedLogos.contains(name));
        if (staleLogos != null) for (File stale : staleLogos) stale.delete();
        nativeOptions.put("frame_logo_path", logoDir.getAbsolutePath());
      }
      String result = NativeBridge.convert(t.source.file.getAbsolutePath(),
          output.getAbsolutePath(), workspace.nativeOptions(nativeOptions).toString(), (p, m) -> {
            t.progress = p;
            t.detail = m;
            changed();
          });
      if (cancelled)
        throw new InterruptedException("Cancelled");
      JSONObject res = new JSONObject(result);
      if (!res.optBoolean("success", false))
        throw new IOException(
            "Native conversion did not succeed: " + res.optString("error", result));
      t.detail = "Saving";
      changed();
      if (t.options.format == 6 && t.options.extraction == 0)
        t.filename = actualExtension(output, t.filename);
      Uri dest = createUnique(t.folder, t.filename);
      if (dest.equals(t.source.uri))
        throw new IOException("Destination is the source document");
      try (InputStream in = new FileInputStream(output);
          OutputStream out = context.getContentResolver().openOutputStream(dest, "w")) {
        if (out == null)
          throw new IOException("Cannot open output");
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (DigestInputStream digest = new DigestInputStream(in, md)) {
          copyOutput(digest, out);
        }
        StringBuilder h = new StringBuilder();
        for (byte b : md.digest()) h.append(String.format(Locale.ROOT, "%02x", b & 255));
        t.hash = h.toString();
      } catch (Exception e) {
        boolean removed = false;
        try {
          removed = DocumentsContract.deleteDocument(context.getContentResolver(), dest);
        } catch (Exception ignored) {
        }
        if (!removed)
          throw new IOException(AppModel.message(e) + "; partial output could not be removed", e);
        throw e;
      }
      t.output = dest;
      t.progress = 100;
      t.status = "Completed";
      JSONObject verify = res.optJSONObject("verification");
      t.detail = verify == null
          ? "Saved"
          : ("Saved • " + verify.optInt("width") + " × " + verify.optInt("height") + " • "
                + verify.optInt("bitDepth") + " bit");
    } catch (Exception e) {
      t.status = cancelled ? "Cancelled" : "Failed";
      t.detail = message(e);
    } finally {
      if (workspace != null) {
        try { workspace.close(); }
        catch (IOException cleanupError) {
          t.detail += " • Temporary cache cleanup pending";
        }
      }
      changed();
    }
  }
  Uri createUnique(Uri folder, String name) throws Exception {
    String id = DocumentsContract.getTreeDocumentId(folder);
    Uri parent = DocumentsContract.buildDocumentUriUsingTree(folder, id),
        children = DocumentsContract.buildChildDocumentsUriUsingTree(folder, id);
    Set<String> names = new HashSet<>();
    try (Cursor c = context.getContentResolver().query(children,
             new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
      if (c == null)
        throw new IOException("Cannot list output folder");
      while (c.moveToNext()) names.add(c.getString(0));
    }
    int d = name.lastIndexOf('.');
    String base = d < 0 ? name : name.substring(0, d), ext = d < 0 ? "" : name.substring(d),
           candidate = name;
    for (int n = 2; names.contains(candidate); n++) candidate = base + " (" + n + ")" + ext;
    Uri u = DocumentsContract.createDocument(
        context.getContentResolver(), parent, mime(candidate), candidate);
    if (u == null)
      throw new IOException("Cannot create output document");
    return u;
  }
  synchronized void clearSources() {
    ArrayList<Source> removed = new ArrayList<>(sources);
    sources.clear();
    for (Source s : removed) release(s);
    executor.execute(NativeBridge::clearCache);
  }
  synchronized void removeSource(Source s) {
    if (!sources.remove(s)) return;
    release(s);
    executor.execute(NativeBridge::clearCache);
  }
  synchronized void removeTask(Task t) {
    if (running || "Running".equals(t.status)) return;
    tasks.remove(t);
    release(t.source);
  }
  synchronized void removeAllTasks() {
    if (running) return;
    ArrayList<Task> removed = new ArrayList<>();
    for (Task t : new ArrayList<>(tasks))
      if (!t.status.equals("Running")) {
        tasks.remove(t);
        removed.add(t);
      }
    for (Task t : removed) release(t.source);
  }
  synchronized boolean resetTask(Task t) {
    if (running || !tasks.contains(t) || "Running".equals(t.status)) return false;
    t.status = "Ready";
    t.progress = 0;
    t.detail = "";
    t.hash = "";
    t.output = null;
    changed();
    return true;
  }
  private synchronized boolean hasSourceReference(Source s) {
    if (sources.contains(s)) return true;
    for (Task t : tasks)
      if (t.source == s) return true;
    return false;
  }
  synchronized void release(Source s) {
    if (s.inspecting || hasSourceReference(s)) return;
    if (s.file != null) {
      s.file.delete();
      s.file = null;
    }
  }
  void copyOutput(InputStream in, OutputStream out) throws IOException {
    byte[] b = new byte[65536];
    int n;
    while ((n = in.read(b)) != -1) {
      if (cancelled)
        throw new IOException("Cancelled");
      out.write(b, 0, n);
    }
    if (cancelled)
      throw new IOException("Cancelled");
  }
  static String mime(String n) {
    String s = n.toLowerCase(Locale.ROOT);
    if (s.endsWith(".jpg") || s.endsWith(".jpeg"))
      return "image/jpeg";
    if (s.endsWith(".png"))
      return "image/png";
    if (s.endsWith(".tif") || s.endsWith(".tiff"))
      return "image/tiff";
    if (s.endsWith(".avif"))
      return "image/avif";
    if (s.endsWith(".jxl"))
      return "image/jxl";
    if (s.endsWith(".jxr"))
      return "image/vnd.ms-photo";
    return "application/octet-stream";
  }
  static String actualExtension(File file, String name) throws IOException {
    byte[] b = new byte[12];
    int n;
    try (InputStream in = new FileInputStream(file)) {
      n = in.read(b);
    }
    String ext;
    if (n >= 2 && (b[0] & 255) == 255 && (b[1] & 255) == 216)
      ext = ".jpg";
    else if (n >= 2 && ((b[0] == 'I' && b[1] == 'I') || (b[0] == 'M' && b[1] == 'M')))
      ext = ".tif";
    else if (n >= 2 && (b[0] & 255) == 255 && (b[1] & 255) == 10
        || n >= 8 && b[4] == 'J' && b[5] == 'X' && b[6] == 'L')
      ext = ".jxl";
    else if (n >= 8 && (b[0] & 255) == 137 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
      ext = ".png";
    else
      throw new IOException("Unknown extracted gain map format");
    int dot = name.lastIndexOf('.');
    return (dot < 0 ? name : name.substring(0, dot)) + ext;
  }
  static void copy(InputStream in, OutputStream out) throws IOException {
    byte[] b = new byte[65536];
    int n;
    while ((n = in.read(b)) != -1) out.write(b, 0, n);
  }
  static String message(Exception e) {
    return e.getMessage() == null ? e.toString() : e.getMessage();
  }
}
