// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
package org.ccord.android

import android.Manifest
import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat

/**
 * Foreground service keeping the Discord Gateway connection (real-time messages/notifications),
 * voice calls, and MediaProjection screen capture alive in the background.
 */
class CCordVoiceService : Service() {
    companion object {
        private const val TAG = "CCordVoiceService"
        private const val NOTIFICATION_ID = 4201

        private const val ACTION_SYNC = "org.ccord.android.action.VOICE_SYNC"
        private const val ACTION_TOGGLE_MUTE = "org.ccord.android.action.TOGGLE_MUTE"
        private const val ACTION_TOGGLE_DEAFEN = "org.ccord.android.action.TOGGLE_DEAFEN"
        private const val ACTION_DISCONNECT = "org.ccord.android.action.DISCONNECT"
        private const val ACTION_STOP_CAPTURE = "org.ccord.android.action.STOP_CAPTURE"

        @Volatile
        private var keepAliveActive = false
        @Volatile
        private var voiceActive = false
        @Volatile
        private var channelName = "Voice Call"
        @Volatile
        private var muted = false
        @Volatile
        private var deafened = false
        @Volatile
        private var screenShareActive = false
        @Volatile
        private var serviceRunning = false
        @Volatile
        private var pendingStartRetry = false
        @Volatile
        private var pendingCaptureReadyCallback: (() -> Unit)? = null

        fun updateKeepAliveState(context: Context, active: Boolean) {
            if (keepAliveActive == active && (active == serviceRunning || voiceActive || screenShareActive)) {
                return
            }
            keepAliveActive = active
            syncService(context)
        }

        fun updateVoiceState(
            context: Context,
            active: Boolean,
            channel: String,
            isMuted: Boolean,
            isDeafened: Boolean
        ) {
            voiceActive = active
            if (channel.isNotEmpty()) channelName = channel
            muted = isMuted
            deafened = isDeafened
            syncService(context)
        }

        fun updateScreenShareState(context: Context, active: Boolean, onReady: (() -> Unit)? = null) {
            screenShareActive = active
            if (active && onReady != null) {
                pendingCaptureReadyCallback = onReady
            } else if (!active) {
                pendingCaptureReadyCallback = null
            }
            syncService(context)
        }

        fun retrySyncIfNeeded(context: Context) {
            if (pendingStartRetry && (keepAliveActive || voiceActive || screenShareActive)) {
                pendingStartRetry = false
                syncService(context)
            }
        }

        private fun shouldRun(): Boolean = keepAliveActive || voiceActive || screenShareActive

        private fun syncService(context: Context) {
            try {
                val intent = Intent(context, CCordVoiceService::class.java).apply {
                    action = ACTION_SYNC
                }
                if (shouldRun()) {
                    pendingStartRetry = false
                    ContextCompat.startForegroundService(context, intent)
                } else {
                    pendingStartRetry = false
                    context.stopService(intent)
                }
            } catch (t: Throwable) {
                Log.w(TAG, "syncService failed (will retry on foreground)", t)
                if (shouldRun()) pendingStartRetry = true
                val cb = pendingCaptureReadyCallback
                pendingCaptureReadyCallback = null
                cb?.invoke()
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_TOGGLE_MUTE -> {
                if (CCordBridge.nativeReady) CCordBridge.nativeOnVoiceAction(CCordBridge.VOICE_ACTION_TOGGLE_MUTE)
            }
            ACTION_TOGGLE_DEAFEN -> {
                if (CCordBridge.nativeReady) CCordBridge.nativeOnVoiceAction(CCordBridge.VOICE_ACTION_TOGGLE_DEAFEN)
            }
            ACTION_DISCONNECT -> {
                if (CCordBridge.nativeReady) CCordBridge.nativeOnVoiceAction(CCordBridge.VOICE_ACTION_DISCONNECT)
            }
            ACTION_STOP_CAPTURE -> {
                CCordBridge.stopScreenCapture()
            }
        }

        if (!shouldRun()) {
            // Android O+ requires startForeground() after startForegroundService() even if stopping immediately.
            CCordBridge.ensureNotificationChannels(this)
            try {
                val notification = buildNotification()
                val stopType = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING
                } else {
                    0
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    ServiceCompat.startForeground(this, NOTIFICATION_ID, notification, stopType)
                } else {
                    startForeground(NOTIFICATION_ID, notification)
                }
            } catch (t: Throwable) {
                Log.w(TAG, "startForeground before immediate stop failed", t)
            }
            serviceRunning = false
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }

        CCordBridge.ensureNotificationChannels(this)
        val notification = buildNotification()
        val fgsType = computeForegroundServiceType()
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                ServiceCompat.startForeground(this, NOTIFICATION_ID, notification, fgsType)
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            serviceRunning = true
        } catch (t: Throwable) {
            Log.e(TAG, "startForeground failed", t)
            serviceRunning = false
        }

        val cb = pendingCaptureReadyCallback
        if (cb != null && screenShareActive) {
            pendingCaptureReadyCallback = null
            cb.invoke()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        serviceRunning = false
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }

    private fun computeForegroundServiceType(): Int {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return 0
        var type = 0
        if (voiceActive) {
            type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            val hasMic = ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
                PackageManager.PERMISSION_GRANTED
            if (hasMic && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            }
        }
        if (screenShareActive) {
            type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && (keepAliveActive || type == 0)) {
            type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING
        }
        return type
    }

    private fun serviceActionIntent(actionName: String, reqCode: Int): PendingIntent {
        val i = Intent(this, CCordVoiceService::class.java).apply { action = actionName }
        return PendingIntent.getService(
            this,
            reqCode,
            i,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
    }

    private fun buildNotification(): Notification {
        val openIntent = Intent(this, CCordActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val openPi = PendingIntent.getActivity(
            this,
            0,
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        if (!voiceActive && !screenShareActive) {
            return NotificationCompat.Builder(this, CCordBridge.CHANNEL_CONNECTION)
                .setSmallIcon(R.drawable.ic_stat_ccord)
                .setColor(0xFF5865F2.toInt())
                .setContentTitle("C-Cord")
                .setContentText("Connected · Listening for messages")
                .setOngoing(true)
                .setSilent(true)
                .setCategory(NotificationCompat.CATEGORY_SERVICE)
                .setPriority(NotificationCompat.PRIORITY_MIN)
                .setContentIntent(openPi)
                .build()
        }

        val title = when {
            voiceActive && screenShareActive -> "Voice & Screen Share · $channelName"
            voiceActive -> "Voice Connected · $channelName"
            else -> "Sharing Screen"
        }
        val statusText = when {
            voiceActive && deafened -> "Deafened"
            voiceActive && muted -> "Muted"
            voiceActive -> "Microphone active"
            else -> "C-Cord is streaming your screen"
        }

        val builder = NotificationCompat.Builder(this, CCordBridge.CHANNEL_VOICE)
            .setSmallIcon(R.drawable.ic_stat_ccord)
            .setColor(0xFF5865F2.toInt())
            .setContentTitle(title)
            .setContentText(statusText)
            .setOngoing(true)
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setContentIntent(openPi)

        if (voiceActive) {
            builder.addAction(
                0,
                if (muted) "Unmute" else "Mute",
                serviceActionIntent(ACTION_TOGGLE_MUTE, 1)
            )
            builder.addAction(
                0,
                if (deafened) "Undeafen" else "Deafen",
                serviceActionIntent(ACTION_TOGGLE_DEAFEN, 2)
            )
            builder.addAction(
                0,
                "Disconnect",
                serviceActionIntent(ACTION_DISCONNECT, 3)
            )
        } else if (screenShareActive) {
            builder.addAction(
                0,
                "Stop Sharing",
                serviceActionIntent(ACTION_STOP_CAPTURE, 4)
            )
        }

        return builder.build()
    }
}
