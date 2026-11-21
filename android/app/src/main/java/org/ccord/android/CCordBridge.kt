// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
package org.ccord.android

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.media.AudioManager
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import android.view.WindowManager
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import java.lang.ref.WeakReference
import java.nio.ByteBuffer

/**
 * Static JNI bridge invoked from src/platform/desktop_android.c and src/platform/capture_android.c.
 */
object CCordBridge {
    private const val TAG = "CCordBridge"

    const val CHANNEL_MESSAGES = "ccord_messages"
    const val CHANNEL_VOICE = "ccord_voice"
    const val CHANNEL_CONNECTION = "ccord_connection"

    const val EXTRA_NOTIFY_ID = "org.ccord.android.NOTIFY_ID"
    const val EXTRA_GUILD_ID = "org.ccord.android.GUILD_ID"
    const val EXTRA_CHANNEL_ID = "org.ccord.android.CHANNEL_ID"
    const val EXTRA_MESSAGE_ID = "org.ccord.android.MESSAGE_ID"

    // Matches ccord_tray_action in src/platform/include/ccord/platform/platform.h
    const val VOICE_ACTION_ACTIVATE = 0
    const val VOICE_ACTION_TOGGLE_MUTE = 1
    const val VOICE_ACTION_TOGGLE_DEAFEN = 2
    const val VOICE_ACTION_QUIT = 3
    const val VOICE_ACTION_DISCONNECT = 4

    @Volatile
    private var activityRef: WeakReference<CCordActivity>? = null

    @Volatile
    private var appContext: Context? = null

    @Volatile
    var nativeReady: Boolean = false
        private set

    fun attach(activity: CCordActivity) {
        activityRef = WeakReference(activity)
        appContext = activity.applicationContext
        nativeReady = true
        ensureNotificationChannels(activity.applicationContext)
    }

    fun detach(activity: CCordActivity) {
        if (activityRef?.get() === activity) {
            activityRef = null
        }
    }

    fun currentActivity(): CCordActivity? = activityRef?.get()

    fun ensureNotificationChannels(context: Context) {
        val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
        val msgChannel = NotificationChannel(
            CHANNEL_MESSAGES,
            "Messages",
            NotificationManager.IMPORTANCE_HIGH
        ).apply {
            description = "Direct messages and mentions"
            enableVibration(true)
            setShowBadge(true)
        }
        val voiceChannel = NotificationChannel(
            CHANNEL_VOICE,
            "Voice & Screen Share",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Active voice call and screen sharing controls"
            setShowBadge(false)
        }
        val connChannel = NotificationChannel(
            CHANNEL_CONNECTION,
            "Background Connection",
            NotificationManager.IMPORTANCE_MIN
        ).apply {
            description = "Keeps C-Cord connected for real-time messages and notifications"
            setShowBadge(false)
            setSound(null, null)
            enableVibration(false)
        }
        nm.createNotificationChannel(msgChannel)
        nm.createNotificationChannel(voiceChannel)
        nm.createNotificationChannel(connChannel)
    }

    // ---- Native (Kotlin -> C) JNI entrypoints ----

    @JvmStatic
    external fun nativeOnInsetsChanged(top: Float, right: Float, bottom: Float, left: Float, imeBottom: Float)

    @JvmStatic
    external fun nativeOnFilePicked(path: String)

    @JvmStatic
    external fun nativeOnNotificationClicked(id: Int, guildId: Long, channelId: Long, messageId: Long)

    @JvmStatic
    external fun nativeOnVoiceAction(action: Int)

    @JvmStatic
    external fun nativeOnNetworkChanged()

    @JvmStatic
    external fun nativeOnCaptureFrame(buf: ByteBuffer, width: Int, height: Int, stride: Int)

    @JvmStatic
    external fun nativeOnCaptureStopped()

    @JvmStatic
    external fun nativeOnAutofillField(field: Int, text: String)

    // ---- Bridge methods called from C (desktop_android.c) ----

    @JvmStatic
    fun getFormFactor(): Int {
        val ctx = activityRef?.get() ?: appContext ?: return 1
        val swDp = ctx.resources.configuration.smallestScreenWidthDp
        return if (swDp >= 600) 2 else 1
    }

    private fun getVibrator(context: Context): Vibrator? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val vm = context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager
            vm?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }
    }

    @JvmStatic
    fun hapticImpact(style: Int): Boolean {
        val ctx = activityRef?.get() ?: appContext ?: return false
        return try {
            val vib = getVibrator(ctx) ?: return false
            if (!vib.hasVibrator()) return false
            val effect = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                when (style) {
                    0 -> VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK)
                    1 -> VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK)
                    2 -> VibrationEffect.createPredefined(VibrationEffect.EFFECT_HEAVY_CLICK)
                    3 -> VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK)
                    else -> VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK)
                }
            } else {
                val (ms, amp) = when (style) {
                    0 -> 12L to 70
                    1 -> 20L to 150
                    2 -> 30L to 230
                    3 -> 8L to 50
                    else -> 18L to 128
                }
                VibrationEffect.createOneShot(ms, amp)
            }
            vib.vibrate(effect)
            true
        } catch (t: Throwable) {
            Log.w(TAG, "hapticImpact failed", t)
            false
        }
    }

    @JvmStatic
    fun hapticNotify(kind: Int): Boolean {
        val ctx = activityRef?.get() ?: appContext ?: return false
        return try {
            val vib = getVibrator(ctx) ?: return false
            if (!vib.hasVibrator()) return false
            val effect = when (kind) {
                0 -> { // success
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        VibrationEffect.createPredefined(VibrationEffect.EFFECT_DOUBLE_CLICK)
                    } else {
                        VibrationEffect.createWaveform(longArrayOf(0, 14, 40, 18), intArrayOf(0, 120, 0, 180), -1)
                    }
                }
                1 -> VibrationEffect.createWaveform(longArrayOf(0, 18, 55, 22), intArrayOf(0, 150, 0, 180), -1)
                else -> VibrationEffect.createWaveform(
                    longArrayOf(0, 22, 45, 22, 45, 30),
                    intArrayOf(0, 180, 0, 180, 0, 240),
                    -1
                )
            }
            vib.vibrate(effect)
            true
        } catch (t: Throwable) {
            Log.w(TAG, "hapticNotify failed", t)
            false
        }
    }

    @JvmStatic
    fun shareText(title: String, text: String): Boolean {
        val act = activityRef?.get() ?: return false
        if (text.isEmpty()) return false
        return try {
            act.runOnUiThread {
                try {
                    val send = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_TEXT, text)
                        if (title.isNotEmpty()) putExtra(Intent.EXTRA_SUBJECT, title)
                    }
                    val chooser = Intent.createChooser(send, title.ifEmpty { "Share" })
                    act.startActivity(chooser)
                } catch (t: Throwable) {
                    Log.w(TAG, "shareText chooser failed", t)
                }
            }
            true
        } catch (t: Throwable) {
            Log.w(TAG, "shareText failed", t)
            false
        }
    }

    @JvmStatic
    fun openFilePicker(): Boolean {
        val act = activityRef?.get() ?: return false
        act.runOnUiThread { act.launchFilePicker() }
        return true
    }

    @JvmStatic
    fun setOrientation(mode: Int): Boolean {
        val act = activityRef?.get() ?: return false
        val req = when (mode) {
            1 -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            2 -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            else -> ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
        act.runOnUiThread {
            try {
                act.requestedOrientation = req
            } catch (t: Throwable) {
                Log.w(TAG, "setOrientation failed", t)
            }
        }
        return true
    }

    @JvmStatic
    fun setKeepScreenOn(on: Boolean): Boolean {
        val act = activityRef?.get() ?: return false
        act.runOnUiThread {
            try {
                if (on) {
                    act.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                } else {
                    act.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
            } catch (t: Throwable) {
                Log.w(TAG, "setKeepScreenOn failed", t)
            }
        }
        return true
    }

    @JvmStatic
    fun notifyMessage(
        id: Int,
        title: String,
        body: String,
        guildId: Long,
        channelId: Long,
        messageId: Long
    ): Boolean {
        val ctx = activityRef?.get() ?: appContext ?: return false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val granted = ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) ==
                PackageManager.PERMISSION_GRANTED
            if (!granted) {
                Log.w(TAG, "notifyMessage skipped: POST_NOTIFICATIONS not granted")
                return false
            }
        }
        return try {
            ensureNotificationChannels(ctx)
            val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return false
            if (!nm.areNotificationsEnabled()) {
                Log.w(TAG, "notifyMessage skipped: notifications disabled in Android settings")
                return false
            }
            val intent = Intent(ctx, CCordActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
                putExtra(EXTRA_NOTIFY_ID, id)
                putExtra(EXTRA_GUILD_ID, guildId)
                putExtra(EXTRA_CHANNEL_ID, channelId)
                putExtra(EXTRA_MESSAGE_ID, messageId)
            }
            val pi = PendingIntent.getActivity(
                ctx,
                id,
                intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            val notif = NotificationCompat.Builder(ctx, CHANNEL_MESSAGES)
                .setSmallIcon(R.drawable.ic_stat_ccord)
                .setColor(0xFF5865F2.toInt())
                .setContentTitle(title.ifEmpty { "C-Cord" })
                .setContentText(body)
                .setStyle(NotificationCompat.BigTextStyle().bigText(body))
                .setCategory(NotificationCompat.CATEGORY_MESSAGE)
                .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
                .setDefaults(NotificationCompat.DEFAULT_ALL)
                .setPriority(NotificationCompat.PRIORITY_HIGH)
                .setAutoCancel(true)
                .setContentIntent(pi)
                .build()
            nm.notify(if (id > 0) id else 1001, notif)
            true
        } catch (t: Throwable) {
            Log.w(TAG, "notifyMessage failed", t)
            false
        }
    }

    @JvmStatic
    fun setKeepAliveService(active: Boolean) {
        val ctx = activityRef?.get() ?: appContext ?: return
        CCordVoiceService.updateKeepAliveState(ctx, active)
    }

    @JvmStatic
    fun onFullscreenChanged(fullscreen: Boolean) {
        val act = activityRef?.get() ?: return
        act.runOnUiThread { act.onNativeFullscreenChanged(fullscreen) }
    }

    @JvmStatic
    fun setVoiceService(active: Boolean, channelName: String, muted: Boolean, deafened: Boolean) {
        val ctx = activityRef?.get() ?: appContext ?: return
        CCordVoiceService.updateVoiceState(ctx, active, channelName, muted, deafened)
    }

    @JvmStatic
    fun setAudioCommMode(inCall: Boolean) {
        val ctx = activityRef?.get() ?: appContext ?: return
        try {
            val am = ctx.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return
            am.mode = if (inCall) AudioManager.MODE_IN_COMMUNICATION else AudioManager.MODE_NORMAL
        } catch (t: Throwable) {
            Log.w(TAG, "setAudioCommMode failed", t)
        }
    }

    @JvmStatic
    fun requestScreenCapture(): Boolean {
        val act = activityRef?.get() ?: return false
        act.runOnUiThread { act.launchScreenCaptureRequest() }
        return true
    }

    @JvmStatic
    fun stopScreenCapture() {
        val act = activityRef?.get()
        if (act != null) {
            act.runOnUiThread { act.stopScreenCaptureInternal() }
        } else {
            val ctx = appContext
            if (ctx != null) CCordVoiceService.updateScreenShareState(ctx, false)
        }
    }

    @JvmStatic
    fun onFirstFrameRendered() {
        val act = activityRef?.get() ?: return
        act.runOnUiThread { act.dismissSplashBackdrop() }
    }

    @JvmStatic
    fun setAuthAutofill(
        mode: Int,
        activeField: Int,
        commit: Boolean,
        emailX: Int,
        emailY: Int,
        emailW: Int,
        emailH: Int,
        pwX: Int,
        pwY: Int,
        pwW: Int,
        pwH: Int,
        otpX: Int,
        otpY: Int,
        otpW: Int,
        otpH: Int,
        emailText: String,
        pwText: String,
        otpText: String
    ) {
        val act = activityRef?.get() ?: return
        act.runOnUiThread {
            act.updateAuthAutofill(
                mode, activeField, commit,
                emailX, emailY, emailW, emailH,
                pwX, pwY, pwW, pwH,
                otpX, otpY, otpW, otpH,
                emailText, pwText, otpText
            )
        }
    }
}
