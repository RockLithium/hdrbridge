package org.hdrbridge;
import android.app.*;
import android.content.*;
import android.os.*;
public class ConversionService extends Service {
  static final String CHANNEL = "conversion";
  @Override
  public void onCreate() {
    super.onCreate();
    NotificationManager n = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
    n.createNotificationChannel(
        new NotificationChannel(CHANNEL, "Conversions", NotificationManager.IMPORTANCE_LOW));
    Intent stop = new Intent(this, ConversionService.class).setAction("stop");
    PendingIntent pi = PendingIntent.getService(
        this, 0, stop, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    Notification notification =
        new Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentTitle("HDR Bridge conversion running")
            .setContentIntent(open)
            .setContentText("Tap Stop to cancel the current conversion")
            .setOngoing(true)
            .addAction(new Notification.Action.Builder(null, "Stop", pi).build())
            .build();
    startForeground(1, notification);
  }
  @Override
  public int onStartCommand(Intent i, int flags, int id) {
    if (i != null && "stop".equals(i.getAction())) {
      AppModel.get(this).cancel();
    }
    return START_NOT_STICKY;
  }
  @Override
  public void onTimeout(int startId, int fgsType) {
    AppModel.get(this).cancel();
    stopSelf();
  }
  @Override
  public IBinder onBind(Intent i) {
    return null;
  }
}
