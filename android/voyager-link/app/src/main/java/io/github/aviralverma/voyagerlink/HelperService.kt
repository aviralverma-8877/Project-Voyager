package io.github.aviralverma.voyagerlink

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.os.Build
import android.os.IBinder

/** What the activity shows. Written by the service, read on the main thread. */
object HelperState {
    @Volatile var running = false
    @Volatile var port = 0
    @Volatile var pages = 0
    @Volatile var openPort: String? = null
    @Volatile var error: String? = null

    /** Called (from any thread) whenever something above changes. */
    @Volatile var onChange: (() -> Unit)? = null

    fun changed() {
        onChange?.invoke()
    }
}

/**
 * Foreground service that keeps [HelperServer] running while the browser is in
 * front. Started and stopped from [MainActivity] or from the notification.
 */
class HelperService : Service() {

    companion object {
        const val ACTION_STOP = "io.github.aviralverma.voyagerlink.STOP"
        private const val CHANNEL_ID = "helper"
        private const val NOTIFICATION_ID = 1
    }

    private var server: HelperServer? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        startInForeground(getString(R.string.notif_idle))
        if (server == null) startServer()
        return START_STICKY
    }

    private fun startServer() {
        val version = try {
            "android-" + packageManager.getPackageInfo(packageName, 0).versionName
        } catch (e: Exception) {
            "android"
        }
        val ports = SerialPorts(applicationContext)
        var lastError: String? = null
        // First free helper port, the same list the page probes.
        for (port in HelperServer.HELPER_PORTS) {
            val candidate = HelperServer(port, version, ports) { pages, openPort ->
                HelperState.pages = pages
                HelperState.openPort = openPort
                HelperState.changed()
                updateNotification(pages, openPort)
            }
            try {
                // No read timeout: a WebSocket may be idle for as long as the page is open.
                candidate.start(0, true)
                server = candidate
                HelperState.running = true
                HelperState.port = port
                HelperState.error = null
                HelperState.changed()
                return
            } catch (e: Exception) {
                lastError = e.message
            }
        }
        HelperState.running = false
        HelperState.error = "Could not start: " + (lastError ?: "no free port")
        HelperState.changed()
        stopSelf()
    }

    override fun onDestroy() {
        server?.shutdown()
        server = null
        HelperState.running = false
        HelperState.pages = 0
        HelperState.openPort = null
        HelperState.changed()
        super.onDestroy()
    }

    // --------------------------------------------------------- notification

    private fun buildNotification(text: String): Notification {
        val manager = getSystemService(NotificationManager::class.java)
        if (manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW)
            )
        }
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, HelperService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_link)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null as Icon?, getString(R.string.notif_stop), stop).build())
            .build()
    }

    private fun startInForeground(text: String) {
        val notification = buildNotification(text)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun updateNotification(pages: Int, openPort: String?) {
        val text = when {
            openPort != null -> "Connected: $openPort"
            pages > 0 -> "Device page connected. No serial port open."
            else -> getString(R.string.notif_idle)
        }
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, buildNotification(text))
    }
}
