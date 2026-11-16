package org.hdrbridge;

import android.app.*;
import android.content.*;
import android.content.pm.*;
import android.net.Uri;
import android.os.*;
import android.provider.Settings;
import java.io.*;
import java.net.*;
import java.security.*;
import java.util.*;
import java.util.concurrent.*;

final class UpdateManager {
  enum State { IDLE, CHECKING, UP_TO_DATE, AVAILABLE, DOWNLOADING, READY, INSTALLING, ERROR, UNAVAILABLE }
  private static UpdateManager instance;
  static synchronized UpdateManager get(Context c) { if (instance == null) instance = new UpdateManager(c.getApplicationContext()); return instance; }
  volatile State state = State.IDLE;
  volatile String message = "";
  volatile int progress;
  volatile UpdateManifest.Release release;
  private final Context context;
  private final ExecutorService worker = Executors.newSingleThreadExecutor();
  private volatile Runnable listener;
  private volatile boolean permissionPending;
  private volatile int sessionId = -1;
  private File apk;
  private Intent confirmation;
  private PendingIntent confirmationPendingIntent;
  private boolean recoveredSession;
  private static final int CONFIRM_NOTIFICATION = 8742;
  private static final String CONFIRM_CHANNEL = "update_confirmation";
  private static final String API = "https://api.github.com/repos/RockLithium/hdrbridge/releases/latest";
  private UpdateManager(Context c) {
    context = c;
    sessionId = context.getSharedPreferences("update_private", 0).getInt("session", -1);
    if (sessionId >= 0) {
      recoveredSession = true;
      state = State.INSTALLING;
      message = "Waiting for installer";
      AppModel.get(context).updateInstalling = true;
      reconcileSession();
    }
  }
  void setListener(Runnable r) { listener = r; }
  private void changed() { Runnable r = listener; if (r != null) r.run(); }
  private void set(State s, String m) { state = s; message = m; changed(); }
  synchronized void maybeCheck() { if (System.currentTimeMillis() - context.getSharedPreferences("update_private", 0).getLong("checked", 0) >= 86400000L) check(false); }
  synchronized void check(boolean manual) {
    if (state == State.CHECKING || state == State.DOWNLOADING || state == State.INSTALLING) return;
    set(State.CHECKING, "Checking for updates");
    worker.execute(() -> { try {
      long deadline = System.currentTimeMillis() + 60000;
      UpdateManifest.Release r = UpdateManifest.parseLatest(read(API, 1048576, deadline));
      if (r == null) { set(State.UNAVAILABLE, "No Android update available"); return; }
      if (r.apkSha256 == null || r.apkSha256.isEmpty()) {
        if (r.checksumUrl == null || r.checksumUrl.isEmpty()) throw new IOException("No update checksum");
        r = UpdateManifest.withChecksums(r, read(r.checksumUrl, 1048576, deadline));
      }
      release = r;
      long current = currentVersionCode();
      set(r.versionCode > current ? State.AVAILABLE : State.UP_TO_DATE, r.versionCode > current ? "Update available" : "Up to date");
    } catch (Exception e) { set(manual ? State.ERROR : State.UNAVAILABLE, "Update check failed: " + e.getMessage()); }
      finally { context.getSharedPreferences("update_private", 0).edit().putLong("checked", System.currentTimeMillis()).apply(); }
    });
  }
  synchronized void download() {
    if (state != State.AVAILABLE || release == null) return;
    UpdateManifest.Release r = release; set(State.DOWNLOADING, "Downloading update");
    worker.execute(() -> { File part = null; progress = 0; try {
      File dir = new File(context.getFilesDir(), "updates"); if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create update directory");
      part = new File(dir, "update.apk.part"); File dest = new File(dir, "update.apk");
      long deadline = System.currentTimeMillis() + 600000;
      if (r.apkSize <= 0 || r.apkSize > 512L * 1024 * 1024) throw new IOException("Invalid APK size");
      HttpURLConnection c = open(r.apkUrl, deadline);
      MessageDigest digest = MessageDigest.getInstance("SHA-256"); long count = 0;
      try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(part)) {
        byte[] b = new byte[65536]; int n; while ((n = in.read(b)) != -1) { if (System.currentTimeMillis() > deadline) throw new IOException("Download timed out"); count += n; if (count > r.apkSize) throw new IOException("APK too large"); digest.update(b, 0, n); out.write(b, 0, n); progress = (int)(count * 100 / r.apkSize); changed(); }
      } finally { c.disconnect(); }
      if (count != r.apkSize || !hex(digest.digest()).equalsIgnoreCase(r.apkSha256)) throw new IOException("APK checksum or size mismatch");
      verify(part, r); if (dest.exists() && !dest.delete()) throw new IOException("Cannot replace update"); if (!part.renameTo(dest)) throw new IOException("Cannot save update"); apk = dest; set(State.READY, "Update ready to install");
    } catch (Exception e) { if (part != null) part.delete(); set(State.ERROR, "Download failed: " + e.getMessage()); } });
  }
  synchronized boolean installPending() {
    reconcileSession();
    return state == State.INSTALLING || permissionPending;
  }
  private synchronized void reconcileSession() {
    if (sessionId < 0) return;
    PackageInstaller.SessionInfo info = context.getPackageManager().getPackageInstaller().getSessionInfo(sessionId);
    if (info == null) {
      clearSession();
      set(State.ERROR, "The previous installation has ended. Check for updates again.");
    }
  }
  void install(Activity activity) {
    if (state != State.READY || apk == null) return;
    if (!context.getPackageManager().canRequestPackageInstalls()) { permissionPending = true; try { activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + context.getPackageName()))); } catch (Exception e) { permissionPending = false; set(State.ERROR, "Cannot open installation settings"); } return; }
    beginInstall();
  }
  void resumeInstall(Activity activity) { if (!permissionPending) return; permissionPending = false; if (context.getPackageManager().canRequestPackageInstalls()) beginInstall(); else { message = "Installation permission not granted"; changed(); } }
  private void beginInstall() {
    AppModel model = AppModel.get(context);
    synchronized (model) { if (model.running) { message = "Wait for conversion to finish"; changed(); return; } for (AppModel.Source s : model.sources) if (s.inspecting) { message = "Wait for inspection to finish"; changed(); return; } model.updateInstalling = true; }
    set(State.INSTALLING, "Installing update");
    worker.execute(() -> { int id = -1; try {
      verify(apk, release);
      PackageInstaller installer = context.getPackageManager().getPackageInstaller();
      PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
      params.setAppPackageName(context.getPackageName());
      if (Build.VERSION.SDK_INT >= 31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
      id = installer.createSession(params); sessionId = id;
      context.getSharedPreferences("update_private", 0).edit().putInt("session", id).remove("confirmation_launched").commit();
      try (PackageInstaller.Session session = installer.openSession(id); InputStream in = new FileInputStream(apk); OutputStream out = session.openWrite("base.apk", 0, apk.length())) { byte[] b = new byte[65536]; int n; while ((n = in.read(b)) != -1) out.write(b, 0, n); session.fsync(out);
        Intent callback = new Intent(context, UpdateStatusReceiver.class).setAction("org.hdrbridge.UPDATE_STATUS").putExtra("session", id);
        PendingIntent pi = PendingIntent.getBroadcast(context, id, callback, PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0)); session.commit(pi.getIntentSender()); }
    } catch (Exception e) { if (id >= 0) try { context.getPackageManager().getPackageInstaller().abandonSession(id); } catch (Exception ignored) {} finishFailure("Installation failed: " + e.getMessage()); } });
  }
  synchronized void status(Intent intent) {
    if (!"org.hdrbridge.UPDATE_STATUS".equals(intent.getAction()) || sessionId < 0
        || intent.getIntExtra("session", -1) != sessionId || state != State.INSTALLING) return;
    int reportedSession = intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, sessionId);
    if (reportedSession != sessionId) return;
    int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
    if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
      if (context.getSharedPreferences("update_private", 0).getBoolean("confirmation_launched", false)) return;
      Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
      if (confirm == null) { finishFailure("Missing system confirmation"); return; }
      synchronized (this) { confirmation = confirm; recoveredSession = false; }
      message = "Installation requires confirmation";
      postConfirmation(confirm);
      changed();
    }
    else if (status == PackageInstaller.STATUS_SUCCESS) { clearSession(); set(State.IDLE, "Update installed"); }
    else finishFailure("Installation failed: " + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
  }
  synchronized boolean canCancelInstall() {
    return state == State.INSTALLING && sessionId >= 0 && (confirmation != null || recoveredSession);
  }
  synchronized void launchConfirmation(Activity activity) {
    if (confirmation == null) return;
    launchConfirmationOnce(activity, sessionId, confirmation);
  }
  synchronized void launchNotificationConfirmation(Activity activity, Intent intent) {
    if (intent == null) return;
    Intent confirm = intent.getParcelableExtra("confirmation");
    launchConfirmationOnce(activity, intent.getIntExtra("session", -1), confirm);
  }
  private void launchConfirmationOnce(Activity activity, int id, Intent confirm) {
    if (confirm == null || id < 0 || id != sessionId || state != State.INSTALLING) return;
    android.content.SharedPreferences prefs = context.getSharedPreferences("update_private", 0);
    if (prefs.getInt("session", -1) != id || prefs.getBoolean("confirmation_launched", false)) return;
    if (!prefs.edit().putBoolean("confirmation_launched", true).commit()) return;
    try {
      activity.startActivity(confirm);
      confirmation = null;
      cancelConfirmationNotification();
      message = "Waiting for installer";
    } catch (Exception e) {
      prefs.edit().remove("confirmation_launched").commit();
      confirmation = confirm;
      message = "Cannot open system installer. Cancel and retry the update.";
    }
  }
  synchronized void cancelInstall() {
    if (!canCancelInstall()) return;
    int id = sessionId;
    try { context.getPackageManager().getPackageInstaller().abandonSession(id); }
    catch (Exception e) { message = "Cannot cancel installation: " + e.getMessage(); changed(); return; }
    finishFailure("Installation cancelled. Check for updates to retry.");
  }
  private void postConfirmation(Intent confirm) {
    try {
      NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
      if (Build.VERSION.SDK_INT >= 33 && context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return;
      if (Build.VERSION.SDK_INT >= 26) manager.createNotificationChannel(new NotificationChannel(CONFIRM_CHANNEL, "Update confirmation", NotificationManager.IMPORTANCE_LOW));
      Intent handoff = new Intent(context, UpdateConfirmActivity.class)
          .putExtra("session", sessionId).putExtra("confirmation", confirm);
      PendingIntent pending = PendingIntent.getActivity(context, CONFIRM_NOTIFICATION, handoff,
          PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0));
      synchronized (this) { confirmationPendingIntent = pending; }
      Notification.Builder builder = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(context, CONFIRM_CHANNEL) : new Notification.Builder(context);
      manager.notify(CONFIRM_NOTIFICATION, builder.setSmallIcon(android.R.drawable.stat_sys_download_done)
          .setContentTitle("HDR Bridge").setContentText("Update installation requires confirmation")
          .setContentIntent(pending).setAutoCancel(true).build());
    } catch (Exception ignored) { /* Confirmation remains available when the activity resumes. */ }
  }
  private void cancelConfirmationNotification() {
    ((NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE)).cancel(CONFIRM_NOTIFICATION);
    if (confirmationPendingIntent != null) { confirmationPendingIntent.cancel(); confirmationPendingIntent = null; }
  }
  private synchronized void clearSession() {
    confirmation = null;
    recoveredSession = false;
    cancelConfirmationNotification();
    sessionId = -1;
    context.getSharedPreferences("update_private", 0).edit().remove("session").remove("confirmation_launched").apply();
    AppModel.get(context).updateInstalling = false;
  }
  private void finishFailure(String m) {
    clearSession();
    set(apk != null && apk.isFile() && release != null ? State.READY : State.ERROR, m);
  }
  private int currentVersionCode() throws Exception {
    PackageInfo p = currentPackage();
    return Build.VERSION.SDK_INT >= 28 ? (int)p.getLongVersionCode() : p.versionCode;
  }
  private PackageInfo currentPackage() throws Exception {
    int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
    return context.getPackageManager().getPackageInfo(context.getPackageName(), flags);
  }
  private void verify(File f, UpdateManifest.Release r) throws Exception {
    if (r == null || f.length() != r.apkSize) throw new IOException("Invalid APK size");
    MessageDigest d = MessageDigest.getInstance("SHA-256"); try (InputStream in = new FileInputStream(f)) { byte[] b = new byte[65536]; int n; while ((n = in.read(b)) != -1) d.update(b, 0, n); }
    if (!hex(d.digest()).equalsIgnoreCase(r.apkSha256)) throw new IOException("Invalid APK checksum");
    int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
    PackageInfo p = context.getPackageManager().getPackageArchiveInfo(f.getAbsolutePath(), flags), current = currentPackage();
    long candidateCode = p == null ? -1 : Build.VERSION.SDK_INT >= 28 ? p.getLongVersionCode() : p.versionCode;
    if (p == null || !context.getPackageName().equals(p.packageName) || !r.version.equals(p.versionName) || candidateCode != r.versionCode || candidateCode <= currentVersionCode()) throw new IOException("APK identity mismatch");
    android.content.pm.Signature[] signers, installedSigners;
    if (Build.VERSION.SDK_INT >= 28) {
      if (p.signingInfo == null || current.signingInfo == null) throw new IOException("Missing APK signing information");
      signers = p.signingInfo.getApkContentsSigners();
      installedSigners = current.signingInfo.getApkContentsSigners();
    } else {
      signers = p.signatures;
      installedSigners = current.signatures;
    }
    if (signers == null || signers.length != 1 || installedSigners == null || installedSigners.length != 1) throw new IOException("Invalid APK signer");
    String pin = BuildConfig.RELEASE_CERT_SHA256.replace(":", "").trim();
    if (pin.isEmpty() || !hex(MessageDigest.getInstance("SHA-256").digest(signers[0].toByteArray())).equalsIgnoreCase(pin)) throw new IOException("APK signer mismatch");
    if (!installedSigners[0].equals(signers[0])) throw new IOException("APK incompatible with installed signing certificate");
  }
  private static String hex(byte[] data) { StringBuilder s = new StringBuilder(); for (byte b : data) s.append(String.format(Locale.ROOT, "%02x", b & 255)); return s.toString(); }
  private static HttpURLConnection open(String value, long deadline) throws Exception {
    for (int redirects = 0; redirects < 6; redirects++) { URL url = new URL(value); String host = url.getHost().toLowerCase(Locale.ROOT); if (!"https".equals(url.getProtocol()) || url.getUserInfo() != null || url.getPort() != -1 || !(host.equals("api.github.com") || host.equals("github.com") || host.equals("objects.githubusercontent.com") || host.equals("release-assets.githubusercontent.com"))) throw new IOException("Untrusted update URL");
      if (System.currentTimeMillis() > deadline) throw new IOException("Request timed out"); HttpURLConnection c = (HttpURLConnection)url.openConnection(); c.setInstanceFollowRedirects(false); c.setConnectTimeout(10000); c.setReadTimeout(10000); c.setRequestProperty("Accept", "application/vnd.github+json"); int status = c.getResponseCode(); if (status >= 300 && status < 400) { String location = c.getHeaderField("Location"); c.disconnect(); if (location == null) throw new IOException("Invalid redirect"); value = new URL(url, location).toString(); continue; } if (status != 200) { c.disconnect(); throw new IOException("HTTP " + status); } return c; }
    throw new IOException("Too many redirects");
  }
  private static String read(String url, int limit, long deadline) throws Exception { HttpURLConnection c = open(url, deadline); try (InputStream in = c.getInputStream(); ByteArrayOutputStream out = new ByteArrayOutputStream()) { byte[] b = new byte[8192]; int n; while ((n = in.read(b)) != -1) { if (System.currentTimeMillis() > deadline || out.size() + n > limit) throw new IOException("Response too large or timed out"); out.write(b, 0, n); } return out.toString("UTF-8"); } finally { c.disconnect(); } }
}
