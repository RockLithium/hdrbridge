package org.hdrbridge;

import static org.junit.Assert.*;
import org.junit.Test;

public class UpdateCheckPolicyTest {
  @Test public void firstLaunchChecksImmediately() {
    assertTrue(UpdateCheckPolicy.shouldCheck(1_000L, 0L, 0L, false));
    assertEquals(0L, UpdateCheckPolicy.delayUntilAutomatic(1_000L, 0L, 0L));
  }

  @Test public void successfulAutomaticCheckWaitsOneHour() {
    long success = 10_000L;
    assertFalse(UpdateCheckPolicy.shouldCheck(success + UpdateCheckPolicy.AUTO_INTERVAL_MS - 1, success, success, false));
    assertTrue(UpdateCheckPolicy.shouldCheck(success + UpdateCheckPolicy.AUTO_INTERVAL_MS, success, success, false));
    assertEquals(1L, UpdateCheckPolicy.delayUntilAutomatic(success + UpdateCheckPolicy.AUTO_INTERVAL_MS - 1, success, success));
  }

  @Test public void failedAttemptRetriesAfterFiveMinutes() {
    long attempt = 20_000L;
    assertFalse(UpdateCheckPolicy.shouldCheck(attempt + UpdateCheckPolicy.FAILURE_RETRY_MS - 1, 0L, attempt, false));
    assertTrue(UpdateCheckPolicy.shouldCheck(attempt + UpdateCheckPolicy.FAILURE_RETRY_MS, 0L, attempt, false));
  }

  @Test public void manualCheckStillHasOneMinuteAttemptCooldown() {
    long attempt = 30_000L;
    assertFalse(UpdateCheckPolicy.shouldCheck(attempt + UpdateCheckPolicy.MANUAL_INTERVAL_MS - 1, 0L, attempt, true));
    assertTrue(UpdateCheckPolicy.shouldCheck(attempt + UpdateCheckPolicy.MANUAL_INTERVAL_MS, 0L, attempt, true));
  }

  @Test public void rollbackAllowsCheckAndAutomaticDelayIsZero() {
    long future = 100_000L;
    assertTrue(UpdateCheckPolicy.shouldCheck(future - 1, 0L, future, false));
    assertEquals(0L, UpdateCheckPolicy.delayUntilAutomatic(future - 1, 0L, future));
  }
}
