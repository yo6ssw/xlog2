// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

package ro.scripca.xlog2

import android.app.ForegroundServiceStartNotAllowedException
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

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
        // A null intent means the system is restarting us — which, on Android 12+,
        // happens while the app is in the background where promoting to foreground
        // is illegal. Don't even try; bail so we never crash the process. We are
        // START_NOT_STICKY, so this restart path should not occur, but guard anyway.
        if (intent == null) {
            stopSelf()
            return START_NOT_STICKY
        }

        // Promoting to foreground is disallowed from the background (API 31+); if
        // that ever happens, catch it instead of crashing the whole process.
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

        // Only after a successful foreground promotion do we take resources.
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        multicastLock = wifi.createMulticastLock("xlog2-sync").apply {
            setReferenceCounted(false)
            acquire()
        }

        XlogRepository.get(applicationContext).start()
        return START_NOT_STICKY
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
