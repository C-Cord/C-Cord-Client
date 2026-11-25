// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
package org.ccord.android

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.ClipData
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaScannerConnection
import android.os.Build
import android.os.Environment
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.provider.MediaStore
import android.util.Log
import android.view.WindowManager
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import java.io.ByteArrayOutputStream
import java.io.File
import java.lang.ref.WeakReference
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.util.Locale

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

    private val audioFocusLock = Any()
    private var audioFocusRequest: AudioFocusRequest? = null
    private var audioFocusHeld: Boolean = false
    private val audioFocusChangeListener = AudioManager.OnAudioFocusChangeListener { _ -> }

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
    external fun nativeOnNetworkAvailable(available: Boolean)

    @JvmStatic
    external fun nativeOnCaptureFrame(buf: ByteBuffer, width: Int, height: Int, stride: Int)

    @JvmStatic
    external fun nativeOnCaptureStopped()

    @JvmStatic
    external fun nativeOnAutofillField(field: Int, text: String)

    @JvmStatic
    external fun nativeWakeMainLoop()

    @JvmStatic
    external fun nativeSetActivityActive(active: Boolean)

    @JvmStatic
    external fun nativeOnOpenUrl(url: String)

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

    private fun detectImageMimeAndExt(filename: String, bytes: ByteArray?): Pair<String, String> {
        if (bytes != null && bytes.size >= 12) {
            val b0 = bytes[0].toInt() and 0xFF
            val b1 = bytes[1].toInt() and 0xFF
            val b2 = bytes[2].toInt() and 0xFF
            val b3 = bytes[3].toInt() and 0xFF
            if (b0 == 0x89 && b1 == 0x50 && b2 == 0x4E && b3 == 0x47) return "image/png" to "png"
            if (b0 == 0xFF && b1 == 0xD8 && b2 == 0xFF) return "image/jpeg" to "jpg"
            if (b0 == 0x47 && b1 == 0x49 && b2 == 0x46 && b3 == 0x38) return "image/gif" to "gif"
            if (b0 == 0x52 && b1 == 0x49 && b2 == 0x46 && b3 == 0x46 &&
                bytes[8].toInt() == 0x57 && bytes[9].toInt() == 0x45 &&
                bytes[10].toInt() == 0x42 && bytes[11].toInt() == 0x50
            ) {
                return "image/webp" to "webp"
            }
        }
        val lower = filename.lowercase(Locale.ROOT)
        return when {
            lower.endsWith(".jpg") || lower.endsWith(".jpeg") -> "image/jpeg" to "jpg"
            lower.endsWith(".webp") -> "image/webp" to "webp"
            lower.endsWith(".gif") -> "image/gif" to "gif"
            else -> "image/png" to "png"
        }
    }

    private fun sanitizeImageFilename(raw: String, ext: String): String {
        val base = raw.substringAfterLast('/').substringAfterLast('\\').substringBefore('?').trim()
        val cleaned = buildString {
            for (ch in base) {
                if (ch.isLetterOrDigit() || ch == '.' || ch == '-' || ch == '_') append(ch)
                else append('_')
            }
        }.trimStart('.').take(96)
        if (cleaned.isEmpty()) return "ccord_${System.currentTimeMillis()}.$ext"
        return if (cleaned.contains('.')) cleaned else "$cleaned.$ext"
    }

    private fun writeBytesToGallery(ctx: Context, filename: String, bytes: ByteArray): Boolean {
        if (bytes.isEmpty()) return false
        val (mime, ext) = detectImageMimeAndExt(filename, bytes)
        val safeName = sanitizeImageFilename(filename, ext)
        return try {
            val resolver = ctx.contentResolver
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, safeName)
                    put(MediaStore.Images.Media.MIME_TYPE, mime)
                    put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/C-Cord")
                    put(MediaStore.Images.Media.IS_PENDING, 1)
                }
                val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: return false
                try {
                    resolver.openOutputStream(uri)?.use { out ->
                        out.write(bytes)
                        out.flush()
                    } ?: run {
                        resolver.delete(uri, null, null)
                        return false
                    }
                    val done = ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }
                    resolver.update(uri, done, null, null)
                    true
                } catch (t: Throwable) {
                    resolver.delete(uri, null, null)
                    throw t
                }
            } else {
                val values = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, safeName)
                    put(MediaStore.Images.Media.MIME_TYPE, mime)
                }
                val uri = try {
                    resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                } catch (_: SecurityException) {
                    null
                }
                if (uri != null) {
                    resolver.openOutputStream(uri)?.use { out ->
                        out.write(bytes)
                        out.flush()
                    } ?: return false
                    true
                } else {
                    val dir = File(ctx.getExternalFilesDir(Environment.DIRECTORY_PICTURES) ?: ctx.filesDir, "C-Cord")
                    if (!dir.exists()) dir.mkdirs()
                    val outFile = File(dir, safeName)
                    outFile.writeBytes(bytes)
                    MediaScannerConnection.scanFile(ctx, arrayOf(outFile.absolutePath), arrayOf(mime), null)
                    true
                }
            }
        } catch (t: Throwable) {
            Log.w(TAG, "writeBytesToGallery failed", t)
            false
        }
    }

    @JvmStatic
    fun saveImageToGallery(filename: String, bytes: ByteArray?, fallbackUrl: String): Boolean {
        val ctx = activityRef?.get()?.applicationContext ?: appContext ?: return false
        if (bytes != null && bytes.isNotEmpty()) {
            return writeBytesToGallery(ctx, filename, bytes)
        }
        val urlStr = fallbackUrl.trim()
        if (!urlStr.startsWith("https://") && !urlStr.startsWith("http://")) return false
        Thread({
            var conn: HttpURLConnection? = null
            try {
                conn = (URL(urlStr).openConnection() as? HttpURLConnection) ?: return@Thread
                conn.connectTimeout = 10_000
                conn.readTimeout = 20_000
                conn.instanceFollowRedirects = true
                if (conn.responseCode in 200..299) {
                    val maxBytes = 50 * 1024 * 1024
                    val baos = ByteArrayOutputStream()
                    val buf = ByteArray(16 * 1024)
                    conn.inputStream.use { inp ->
                        while (true) {
                            val n = inp.read(buf)
                            if (n <= 0) break
                            if (baos.size() + n > maxBytes) return@Thread
                            baos.write(buf, 0, n)
                        }
                    }
                    writeBytesToGallery(ctx, filename, baos.toByteArray())
                }
            } catch (t: Throwable) {
                Log.w(TAG, "saveImageToGallery fallback fetch failed", t)
            } finally {
                conn?.disconnect()
            }
        }, "ccord-save-img").start()
        return true
    }

    @JvmStatic
    fun shareImage(filename: String, bytes: ByteArray?, fallbackUrl: String): Boolean {
        val act = activityRef?.get() ?: return false
        if (bytes == null || bytes.isEmpty()) {
            return shareText(filename, fallbackUrl)
        }
        val (mime, ext) = detectImageMimeAndExt(filename, bytes)
        val safeName = sanitizeImageFilename(filename, ext)
        return try {
            val shareDir = File(act.cacheDir, "shared")
            if (!shareDir.exists()) shareDir.mkdirs()
            // Prune stale temp files older than 1 hour so cache/shared stays tiny.
            val cutoff = System.currentTimeMillis() - 3_600_000L
            shareDir.listFiles()?.forEach { f ->
                if (f.isFile && f.lastModified() < cutoff) f.delete()
            }
            val outFile = File(shareDir, safeName)
            outFile.writeBytes(bytes)
            val uri = FileProvider.getUriForFile(act, "${act.packageName}.fileprovider", outFile)
            act.runOnUiThread {
                try {
                    val send = Intent(Intent.ACTION_SEND).apply {
                        type = mime
                        putExtra(Intent.EXTRA_STREAM, uri)
                        clipData = ClipData.newRawUri(safeName, uri)
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    val chooser = Intent.createChooser(send, "Share image")
                    act.startActivity(chooser)
                } catch (t: Throwable) {
                    Log.w(TAG, "shareImage chooser failed", t)
                }
            }
            true
        } catch (t: Throwable) {
            Log.w(TAG, "shareImage failed", t)
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
    fun cancelNotification(id: Int) {
        if (id <= 0) return
        val ctx = activityRef?.get() ?: appContext ?: return
        try {
            val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
            nm.cancel(id)
        } catch (t: Throwable) {
            Log.w(TAG, "cancelNotification failed", t)
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
        synchronized(audioFocusLock) {
            try {
                val am = ctx.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return
                if (inCall) {
                    if (!audioFocusHeld) {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                            val attrs = AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                                .build()
                            val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                                .setAudioAttributes(attrs)
                                .setAcceptsDelayedFocusGain(false)
                                .setWillPauseWhenDucked(false)
                                .setOnAudioFocusChangeListener(audioFocusChangeListener)
                                .build()
                            am.requestAudioFocus(req)
                            audioFocusRequest = req
                        } else {
                            @Suppress("DEPRECATION")
                            am.requestAudioFocus(
                                audioFocusChangeListener,
                                AudioManager.STREAM_VOICE_CALL,
                                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
                            )
                        }
                        audioFocusHeld = true
                    }
                    am.mode = AudioManager.MODE_IN_COMMUNICATION
                } else {
                    am.mode = AudioManager.MODE_NORMAL
                    if (audioFocusHeld) {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                            audioFocusRequest?.let { am.abandonAudioFocusRequest(it) }
                            audioFocusRequest = null
                        } else {
                            @Suppress("DEPRECATION")
                            am.abandonAudioFocus(audioFocusChangeListener)
                        }
                        audioFocusHeld = false
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "setAudioCommMode failed", t)
            }
        }
    }

    @JvmStatic
    fun setScreenCaptureFps(fps: Int) {
        activityRef?.get()?.setScreenCaptureFps(fps)
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
    fun setStartupTheme(argb: Int, isLight: Boolean) {
        val act = activityRef?.get() ?: return
        act.runOnUiThread { act.onStartupThemeChanged(argb, isLight) }
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
