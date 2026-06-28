package ro.scripca.xlog2

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat

/**
 * Foreground service that keeps the sync mesh alive while the app is
 * backgrounded and — crucially — holds a [WifiManager.MulticastLock]. Without
 * the lock Android silently drops inbound multicast, so LAN peer auto-discovery
 * would never see other nodes. It owns the [XlogRepository] (which owns the
 * native core thread + sockets); the UI binds to the same singleton repository.
 */
class SyncService : Service() {

    private var multicastLock: WifiManager.MulticastLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIF_ID, buildNotification())

        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        multicastLock = wifi.createMulticastLock("xlog2-sync").apply {
            setReferenceCounted(false)
            acquire()
        }

        XlogRepository.get(applicationContext).start()
        return START_STICKY
    }

    override fun onDestroy() {
        // Release the multicast lock only. The core (logbook + mesh) is owned by
        // the repository for the process lifetime — logging must keep working
        // even with the foreground service stopped (e.g. sync toggled off). The
        // OS reclaims sockets when the process is killed.
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
        super.onDestroy()
    }

    private fun buildNotification(): Notification {
        val mgr = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL, "Logbook sync", NotificationManager.IMPORTANCE_LOW)
            )
        }
        return NotificationCompat.Builder(this, CHANNEL)
            .setContentTitle("xlog2 sync")
            .setContentText("Syncing the logbook with mesh peers")
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val CHANNEL = "sync"
        private const val NOTIF_ID = 1

        fun start(context: Context) {
            val i = Intent(context, SyncService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                context.startForegroundService(i)
            else
                context.startService(i)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, SyncService::class.java))
        }
    }
}
