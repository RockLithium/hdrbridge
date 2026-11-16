package org.hdrbridge;
import android.content.*;
public final class UpdateStatusReceiver extends BroadcastReceiver {
  @Override public void onReceive(Context context, Intent intent) { UpdateManager.get(context).status(intent); }
}
