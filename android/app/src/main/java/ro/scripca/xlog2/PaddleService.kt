package ro.scripca.xlog2

import android.app.ForegroundServiceStartNotAllowedException
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

/**
 * Foreground service that keeps the CW paddle keyer + USB HID reader alive with
 * the screen off (the operator's chosen behaviour). Owns no state — it drives
 * [XlogRepository.startPaddle]/[stopPaddle] on the process-wide singleton and
 * holds the process up so the native keyer's UDP session and the USB read loop
 * keep going. Same crash-safe pattern as [SyncService]/[AudioService].
 *
 * Type dataSync (not connectedDevice): the keyer's job is streaming key edges over
 * UDP, and dataSync has no start-time device precondition to trip on.
 */
class PaddleService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (intent.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        try {
            ServiceCompat.startForeground(
                this, NOTIF_ID, buildNotification(),
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
                else 0
            )
        } catch (e: Exception) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                e is ForegroundServiceStartNotAllowedException
            ) {
                stopSelf()
                return START_NOT_STICKY
            }
            throw e
        }

        XlogRepository.get(applicationContext).startPaddle()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        XlogRepository.get(applicationContext).stopPaddle()
        super.onDestroy()
    }

    private fun buildNotification(): Notification {
        val mgr = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL, "CW keyer", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val openApp = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this, 3, Intent(this, PaddleService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL)
            .setContentTitle("xlog2 CW keyer")
            .setContentText("Paddle keyer active")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(openApp)
            .addAction(android.R.drawable.ic_media_pause, "Stop", stop)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val CHANNEL = "paddle"
        private const val NOTIF_ID = 3
        private const val ACTION_STOP = "ro.scripca.xlog2.PADDLE_STOP"

        fun start(context: Context) {
            val i = Intent(context, PaddleService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                context.startForegroundService(i)
            else
                context.startService(i)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, PaddleService::class.java))
        }
    }
}
