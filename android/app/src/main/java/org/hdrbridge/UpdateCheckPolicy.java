package org.hdrbridge;

/** Pure time policy for deciding when an update check may run. */
final class UpdateCheckPolicy {
  static final long AUTO_INTERVAL_MS = 60L * 60L * 1000L;
  static final long FAILURE_RETRY_MS = 5L * 60L * 1000L;
  static final long MANUAL_INTERVAL_MS = 60L * 1000L;

  private UpdateCheckPolicy() {}

  static boolean shouldCheck(long now, long lastSuccess, long lastAttempt, boolean manual) {
    if (lastAttempt > 0 && now < lastAttempt) return true;
    if (lastAttempt > 0 && now - lastAttempt < MANUAL_INTERVAL_MS) return false;
    if (manual) return true;
    if (lastAttempt > lastSuccess) return elapsed(now, lastAttempt, FAILURE_RETRY_MS);
    if (lastSuccess == 0) return true;
    return elapsed(now, lastSuccess, AUTO_INTERVAL_MS);
  }

  static long delayUntilAutomatic(long now, long lastSuccess, long lastAttempt) {
    if (lastAttempt > 0 && now < lastAttempt) return 0;
    if (lastAttempt > 0 && now - lastAttempt < MANUAL_INTERVAL_MS)
      return MANUAL_INTERVAL_MS - (now - lastAttempt);
    long base = lastAttempt > lastSuccess ? lastAttempt : lastSuccess;
    long interval = lastAttempt > lastSuccess ? FAILURE_RETRY_MS : AUTO_INTERVAL_MS;
    if (base == 0) return 0;
    long elapsed = now - base;
    return elapsed >= interval ? 0 : interval - elapsed;
  }

  private static boolean elapsed(long now, long then, long interval) {
    return now >= then && now - then >= interval;
  }
}
