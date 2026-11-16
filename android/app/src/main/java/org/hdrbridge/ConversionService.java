package org.hdrbridge;
import android.app.*;
import android.content.*;
import android.os.*;
public class ConversionService extends Service {
  static final String CHANNEL = "conversion";
  @Override
  public void onCreate() {
    super.onCreate();
    startForeground(1, notification(this));
  }
  private static Notification notification(Context context) {
    NotificationManager n = (NotificationManager) context.getSystemService(NOTIFICATION_SERVICE);
    n.createNotificationChannel(
        new NotificationChannel(CHANNEL, UiStrings.get(context, "Conversions"), NotificationManager.IMPORTANCE_LOW));
    Intent stop = new Intent(context, ConversionService.class).setAction("stop");
    PendingIntent pi = PendingIntent.getService(
        context, 0, stop, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    PendingIntent open = PendingIntent.getActivity(context, 0, new Intent(context, MainActivity.class),
        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    return new Notification.Builder(context, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentTitle(UiStrings.get(context, "HDR Bridge conversion running"))
            .setContentIntent(open)
            .setContentText(UiStrings.get(context, "Tap Stop to cancel the current conversion"))
            .setOngoing(true)
            .addAction(new Notification.Action.Builder(null, UiStrings.get(context, "Stop"), pi).build())
            .build();
  }
  /** Refresh only an existing notification; never starts or restarts conversion work. */
  static void refreshNotification(Context context) {
    NotificationManager n = (NotificationManager) context.getSystemService(NOTIFICATION_SERVICE);
    for (android.service.notification.StatusBarNotification active : n.getActiveNotifications()) {
      if (active.getId() == 1 && CHANNEL.equals(active.getNotification().getChannelId())) {
        n.notify(1, notification(context));
        break;
      }
    }
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
