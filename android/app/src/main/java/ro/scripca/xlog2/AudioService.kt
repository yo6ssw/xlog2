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
 * Foreground service (type mediaPlayback) that keeps the rig-audio stream + the
 * frequency poll running with the screen off. Like [SyncService] it owns no
 * state: it drives [XlogRepository.startAudio]/[stopAudio] on the process-wide
 * repository singleton and holds the process alive so [AudioStreamClient]'s
 * AudioTrack and [RigctldClient]'s poll keep going in the background.
 */
class AudioService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // A null intent is a system restart in the background where foreground
        // promotion is illegal (API 31+); bail rather than crash. We are
        // START_NOT_STICKY so this should not happen, but guard anyway.
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
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
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

        XlogRepository.get(applicationContext).startAudio()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        XlogRepository.get(applicationContext).stopAudio()
        super.onDestroy()
    }

    private fun buildNotification(): Notification {
        val mgr = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL, "Rig audio", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val openApp = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, AudioService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL)
            .setContentTitle("xlog2 rig audio")
            .setContentText("Streaming rig audio")
            .setSmallIcon(android.R.drawable.stat_sys_headset)
            .setContentIntent(openApp)
            .addAction(android.R.drawable.ic_media_pause, "Stop", stop)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val CHANNEL = "audio"
        private const val NOTIF_ID = 2
        private const val ACTION_STOP = "ro.scripca.xlog2.AUDIO_STOP"

        fun start(context: Context) {
            val i = Intent(context, AudioService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                context.startForegroundService(i)
            else
                context.startService(i)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, AudioService::class.java))
        }
    }
}
