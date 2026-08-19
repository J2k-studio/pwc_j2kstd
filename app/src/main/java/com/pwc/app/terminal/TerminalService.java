package com.pwc.app.terminal;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * Foreground Service — กัน Android ฆ่า process ตอนสลับแอพ / จอดับ
 * ขณะมี terminal session ทำงานอยู่
 *
 * ผูกจาก MainActivity: start เมื่อ session เริ่ม, stop เมื่อออกจากแอพถาวร
 */
public class TerminalService extends Service {

    private static final String TAG = "TerminalService";
    public static final String CHANNEL_ID = "pwc_terminal";
    public static final int NOTIF_ID = 42;

    @Override
    public void onCreate() {
        super.onCreate();
        ensureChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        PendingIntent pi = PendingIntent.getActivity(
                this, 0,
                getPackageManager().getLaunchIntentForPackage(getPackageName()),
                PendingIntent.FLAG_UPDATE_CURRENT
                        | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0));

        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            b = new Notification.Builder(this, CHANNEL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        Notification n = b
                .setContentTitle("PowerCode")
                .setContentText("Terminal session running")
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();

        startForeground(NOTIF_ID, n);
        Log.i(TAG, "foreground started");
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "foreground stopped");
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void ensureChannel() {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "Terminal", NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Keeps the PowerCode shell alive in the background");
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.createNotificationChannel(ch);
    }
}
