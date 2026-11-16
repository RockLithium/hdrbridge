package org.hdrbridge;

import android.app.Activity;
import android.os.Bundle;

/** Private notification handoff for the system package installer. */
public final class UpdateConfirmActivity extends Activity {
  @Override protected void onCreate(Bundle state) {
    super.onCreate(state);
    UpdateManager.get(this).launchNotificationConfirmation(this, getIntent());
    finish();
  }
}
