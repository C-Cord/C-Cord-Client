// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
package org.ccord.android

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.Rect
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.ConnectivityManager
import android.net.Network
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.provider.OpenableColumns
import android.text.InputType
import android.util.Log
import android.util.SparseArray
import android.view.Choreographer
import android.view.MotionEvent
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.ViewStructure
import android.view.WindowManager
import android.view.autofill.AutofillManager
import android.view.autofill.AutofillValue
import android.widget.FrameLayout
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.libsdl.app.SDLActivity
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import kotlin.math.max

/**
 * Main Android Activity hosting SDL3's native C-Cord client (`libmain.so`).
 * Handles edge-to-edge WindowInsets + IME height reporting, CA bundle extraction,
 * ConnectivityManager network change hooks, SAF file picking, notification deep-links,
 * and MediaProjection screen capture.
 */
class CCordActivity : SDLActivity() {
    companion object {
        private const val TAG = "CCordActivity"
        private const val STARTUP_TAG = "CCordStartup"
        private const val PREFS_STARTUP = "ccord_startup_prefs"
        private const val KEY_BG_ARGB = "bg_argb"
        private const val KEY_IS_LIGHT = "is_light"
        private const val DEFAULT_BG_ARGB = 0xFF141417.toInt() // Matte Graphite bg_primary
        private const val REQ_FILE_PICKER = 9101
        private const val REQ_MEDIA_PROJECTION = 9102
        private const val REQ_NOTIFICATIONS = 9103
    }

    private var connectivityManager: ConnectivityManager? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    // Cold-start OS SplashScreen hold + theme-matched resume backdrop synchronized with Choreographer.
    @Volatile
    private var keepSplashOnScreen = true
    private var splashDismissScheduled = false
    private var launchMonoMs: Long = 0L
    private var startupBgArgb: Int = DEFAULT_BG_ARGB
    private var startupIsLight: Boolean = false
    private var splashBackdrop: View? = null
    private val mainHandler = Handler(Looper.getMainLooper())
    private val splashTimeoutRunnable = Runnable { releaseSplashNow("timeout") }

    // Window & lifecycle tracking for edge-to-edge restoration and smooth foreground resume.
    private var isFullscreenMode = false
    private var wasStopped = false
    private var notifPermissionRequested = false
    private var lastInsets: WindowInsetsCompat? = null
    private var lastDispatchedInsetsKey = ""

    // Virtual Autofill overlay so Android password managers (Bitwarden, etc.) recognize email, password, and 2FA fields.
    private var autofillOverlay: CCordAutofillOverlay? = null

    // MediaProjection screen capture state
    private var mediaProjection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var imageReader: ImageReader? = null
    private var captureThread: HandlerThread? = null
    private var captureHandler: Handler? = null
    private var captureDirectBuf: ByteBuffer? = null
    private var captureRowBuf: ByteArray? = null
    private var captureOutRowBuf: ByteArray? = null
    @Volatile
    private var captureMinIntervalNs: Long = 33_333_333L // default ~30 fps capture cap
    private var captureLastFrameNs: Long = 0L

    fun setScreenCaptureFps(fps: Int) {
        val clamped = fps.coerceIn(5, 60)
        captureMinIntervalNs = 1_000_000_000L / clamped.toLong()
    }

    override fun getLibraries(): Array<String> = arrayOf(
        "c++_shared",
        "SDL3",
        "main"
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        launchMonoMs = SystemClock.elapsedRealtime()
        loadStartupThemePrefs()
        keepSplashOnScreen = true
        splashDismissScheduled = false

        // Keep Android's branded SplashScreen on screen until Frame 1 presents (with 1200ms failsafe)
        // instead of showing an unbranded blank View slab over SurfaceView.
        try {
            val splash = installSplashScreen()
            splash.setKeepOnScreenCondition { keepSplashOnScreen }
            splash.setOnExitAnimationListener { provider ->
                try {
                    provider.view.animate()
                        .alpha(0f)
                        .setDuration(80L)
                        .withEndAction {
                            try {
                                provider.remove()
                            } catch (_: Throwable) {
                            }
                        }
                        .start()
                } catch (_: Throwable) {
                    try {
                        provider.remove()
                    } catch (_: Throwable) {
                    }
                }
            }
        } catch (t: Throwable) {
            Log.w(TAG, "installSplashScreen unavailable, falling back", t)
            keepSplashOnScreen = false
        }

        // Fast-path no-op after first install; extracts Mozilla CA bundle asynchronously so onCreate never blocks.
        extractCaBundleAsync()
        mainHandler.postDelayed(splashTimeoutRunnable, 1200L)

        super.onCreate(savedInstanceState)
        Log.i(STARTUP_TAG, "t=+${SystemClock.elapsedRealtime() - launchMonoMs}ms libs_loaded")
        if (mBrokenLibraries) {
            releaseSplashNow("broken_libs")
            return
        }

        try {
            window.decorView.setBackgroundColor(startupBgArgb)
        } catch (_: Throwable) {
        }
        installAutofillOverlay()
        CCordBridge.attach(this)
        CCordBridge.nativeSetActivityActive(true)
        configureEdgeToEdge(fullscreen = false)
        installInsetsListener()
        registerNetworkCallback()
        window.decorView.post { requestNotificationPermissionIfNeeded() }
        handleNotificationIntent(intent)
    }

    override fun onStart() {
        super.onStart()
        if (!mBrokenLibraries && CCordBridge.nativeReady) {
            CCordBridge.nativeSetActivityActive(true)
        }
    }

    override fun onPause() {
        if (!mBrokenLibraries && CCordBridge.nativeReady) {
            CCordBridge.nativeSetActivityActive(false)
        }
        super.onPause()
    }

    override fun onStop() {
        wasStopped = true
        if (!mBrokenLibraries && CCordBridge.nativeReady) {
            CCordBridge.nativeSetActivityActive(false)
        }
        super.onStop()
    }

    override fun onResume() {
        super.onResume()
        if (mBrokenLibraries) return
        if (CCordBridge.nativeReady) CCordBridge.nativeSetActivityActive(true)
        if (wasStopped) {
            wasStopped = false
            // Cover the freshly recreating SurfaceView with our theme-matched backdrop until the resumed frame presents.
            installSplashBackdrop(300L)
        }
        CCordVoiceService.retrySyncIfNeeded(this)
        configureEdgeToEdge(fullscreen = isFullscreenMode)
        window.decorView.post {
            configureEdgeToEdge(fullscreen = isFullscreenMode)
            requestLatestInsets(force = true)
            if (!notifPermissionRequested) requestNotificationPermissionIfNeeded()
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!mBrokenLibraries && hasFocus) {
            if (CCordBridge.nativeReady) CCordBridge.nativeWakeMainLoop()
            // Post after SDLActivity's command handler so our edge-to-edge flags win over SDL's SYSTEM_UI_FLAG_VISIBLE.
            window.decorView.post {
                configureEdgeToEdge(fullscreen = isFullscreenMode)
                requestLatestInsets(force = false)
            }
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        if (!mBrokenLibraries) {
            window.decorView.post {
                configureEdgeToEdge(fullscreen = isFullscreenMode)
                requestLatestInsets(force = true)
            }
        }
    }

    fun onNativeFullscreenChanged(fullscreen: Boolean) {
        isFullscreenMode = fullscreen
        configureEdgeToEdge(fullscreen = fullscreen)
        window.decorView.post {
            configureEdgeToEdge(fullscreen = fullscreen)
            requestLatestInsets(force = true)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (!mBrokenLibraries) {
            if (CCordBridge.nativeReady) CCordBridge.nativeWakeMainLoop()
            handleNotificationIntent(intent)
        }
    }

    override fun onDestroy() {
        mainHandler.removeCallbacks(splashTimeoutRunnable)
        dismissSplashBackdrop()
        stopScreenCaptureInternal()
        unregisterNetworkCallback()
        if (!mBrokenLibraries && CCordBridge.nativeReady) {
            CCordBridge.nativeSetActivityActive(false)
        }
        CCordBridge.detach(this)
        super.onDestroy()
    }

    private fun loadStartupThemePrefs() {
        try {
            val prefs = getSharedPreferences(PREFS_STARTUP, Context.MODE_PRIVATE)
            startupBgArgb = prefs.getInt(KEY_BG_ARGB, DEFAULT_BG_ARGB)
            startupIsLight = prefs.getBoolean(KEY_IS_LIGHT, false)
        } catch (_: Throwable) {
            startupBgArgb = DEFAULT_BG_ARGB
            startupIsLight = false
        }
    }

    fun onStartupThemeChanged(argb: Int, isLight: Boolean) {
        val opaque = argb or (0xFF shl 24)
        if (startupBgArgb == opaque && startupIsLight == isLight) return
        startupBgArgb = opaque
        startupIsLight = isLight
        try {
            getSharedPreferences(PREFS_STARTUP, Context.MODE_PRIVATE)
                .edit()
                .putInt(KEY_BG_ARGB, opaque)
                .putBoolean(KEY_IS_LIGHT, isLight)
                .apply()
            window.decorView.setBackgroundColor(opaque)
            splashBackdrop?.setBackgroundColor(opaque)
            val controller = WindowInsetsControllerCompat(window, window.decorView)
            controller.isAppearanceLightStatusBars = isLight
            controller.isAppearanceLightNavigationBars = isLight
        } catch (t: Throwable) {
            Log.w(TAG, "onStartupThemeChanged failed", t)
        }
    }

    private fun installSplashBackdrop(timeoutMs: Long) {
        try {
            mainHandler.removeCallbacks(splashTimeoutRunnable)
            splashDismissScheduled = false
            if (splashBackdrop == null) {
                val content = findViewById<ViewGroup>(android.R.id.content) ?: return
                val backdrop = View(this).apply {
                    setBackgroundColor(startupBgArgb)
                    alpha = 1f
                    isClickable = false
                    isFocusable = false
                    importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
                }
                content.addView(
                    backdrop,
                    FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT
                    )
                )
                splashBackdrop = backdrop
            } else {
                splashBackdrop?.apply {
                    animate().cancel()
                    alpha = 1f
                    setBackgroundColor(startupBgArgb)
                }
            }
            mainHandler.postDelayed(splashTimeoutRunnable, timeoutMs)
        } catch (t: Throwable) {
            Log.w(TAG, "installSplashBackdrop failed", t)
        }
    }

    fun dismissSplashBackdrop() {
        if (splashDismissScheduled && !keepSplashOnScreen && splashBackdrop == null) return
        splashDismissScheduled = true
        Log.i(STARTUP_TAG, "t=+${SystemClock.elapsedRealtime() - launchMonoMs}ms first_frame_jni")
        // Wait one Choreographer vsync so SurfaceFlinger has latched SDL's newly swapped GLES frame.
        try {
            Choreographer.getInstance().postFrameCallback {
                releaseSplashNow("first_frame")
            }
        } catch (_: Throwable) {
            releaseSplashNow("first_frame_fallback")
        }
    }

    private fun releaseSplashNow(reason: String) {
        mainHandler.removeCallbacks(splashTimeoutRunnable)
        splashDismissScheduled = false
        if (keepSplashOnScreen) {
            keepSplashOnScreen = false
            Log.i(STARTUP_TAG, "t=+${SystemClock.elapsedRealtime() - launchMonoMs}ms splash_released reason=$reason")
        }
        val v = splashBackdrop ?: return
        splashBackdrop = null
        try {
            if (isFinishing || isDestroyed) {
                (v.parent as? ViewGroup)?.removeView(v)
            } else {
                v.animate()
                    .alpha(0f)
                    .setDuration(80L)
                    .withEndAction {
                        try {
                            (v.parent as? ViewGroup)?.removeView(v)
                        } catch (_: Throwable) {
                        }
                    }
                    .start()
            }
        } catch (_: Throwable) {
            try {
                (v.parent as? ViewGroup)?.removeView(v)
            } catch (_: Throwable) {
            }
        }
    }

    private fun installAutofillOverlay() {
        try {
            val content = findViewById<ViewGroup>(android.R.id.content) ?: return
            val overlay = CCordAutofillOverlay(this)
            content.addView(
                overlay,
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT
                )
            )
            autofillOverlay = overlay
        } catch (t: Throwable) {
            Log.w(TAG, "installAutofillOverlay failed", t)
        }
    }

    fun updateAuthAutofill(
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
        val overlay = autofillOverlay ?: return
        overlay.updateState(
            mode, activeField, commit,
            Rect(emailX, emailY, emailX + max(1, emailW), emailY + max(1, emailH)),
            Rect(pwX, pwY, pwX + max(1, pwW), pwY + max(1, pwH)),
            Rect(otpX, otpY, otpX + max(1, otpW), otpY + max(1, otpH)),
            emailText, pwText, otpText
        )
    }

    private fun suppressSdlDummyEditAutofill(root: View?) {
        if (root == null || root === autofillOverlay) return
        val name = root.javaClass.simpleName
        if (name.contains("DummyEdit", ignoreCase = true)) {
            if (root.importantForAutofill != View.IMPORTANT_FOR_AUTOFILL_NO) {
                root.importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
            }
        }
        if (root is ViewGroup) {
            for (i in 0 until root.childCount) {
                suppressSdlDummyEditAutofill(root.getChildAt(i))
            }
        }
    }

    /**
     * Non-interactive virtual View hierarchy exposing C-Cord's login (email/username, password)
     * and 2FA/TOTP fields to Android's AutofillManager so password managers like Bitwarden
     * recognize and autofill them natively.
     */
    private inner class CCordAutofillOverlay(context: Context) : View(context) {
        private val VIRTUAL_ID_EMAIL = 1
        private val VIRTUAL_ID_PASSWORD = 2
        private val VIRTUAL_ID_OTP = 3

        private var mode: Int = 0
        private var activeField: Int = 0
        private var emailRect: Rect = Rect(0, 0, 1, 1)
        private var pwRect: Rect = Rect(0, 0, 1, 1)
        private var otpRect: Rect = Rect(0, 0, 1, 1)
        private var emailText: String = ""
        private var pwText: String = ""
        private var otpText: String = ""

        init {
            isClickable = false
            isFocusable = false
            isFocusableInTouchMode = false
            importantForAutofill = IMPORTANT_FOR_AUTOFILL_YES
            visibility = GONE
        }

        override fun onTouchEvent(event: MotionEvent?): Boolean = false

        private fun rectForField(field: Int): Rect = when (field) {
            VIRTUAL_ID_EMAIL -> emailRect
            VIRTUAL_ID_PASSWORD -> pwRect
            VIRTUAL_ID_OTP -> otpRect
            else -> Rect(0, 0, 1, 1)
        }

        fun updateState(
            newMode: Int,
            newActive: Int,
            commit: Boolean,
            newEmailRect: Rect,
            newPwRect: Rect,
            newOtpRect: Rect,
            newEmailText: String,
            newPwText: String,
            newOtpText: String
        ) {
            val afm = context.getSystemService(AutofillManager::class.java)
            val prevMode = mode
            val prevActive = activeField
            val prevEmail = emailText
            val prevPw = pwText
            val prevOtp = otpText

            mode = newMode
            activeField = newActive
            emailRect = newEmailRect
            pwRect = newPwRect
            otpRect = newOtpRect
            emailText = newEmailText
            pwText = newPwText
            otpText = newOtpText

            if (newMode == 0) {
                if (prevActive != 0) {
                    try {
                        afm?.notifyViewExited(this, prevActive)
                    } catch (_: Throwable) {
                    }
                }
                if (prevMode != 0) {
                    try {
                        if (commit) afm?.commit() else afm?.cancel()
                    } catch (_: Throwable) {
                    }
                }
                visibility = GONE
                return
            }

            visibility = VISIBLE
            suppressSdlDummyEditAutofill(window?.decorView)

            if (prevMode != 0 && prevMode != newMode) {
                if (prevActive != 0) {
                    try {
                        afm?.notifyViewExited(this, prevActive)
                    } catch (_: Throwable) {
                    }
                }
                try {
                    afm?.commit()
                } catch (_: Throwable) {
                }
            } else if (prevActive != 0 && prevActive != newActive) {
                try {
                    afm?.notifyViewExited(this, prevActive)
                } catch (_: Throwable) {
                }
            }

            if (prevMode == newMode) {
                try {
                    if (newMode == 1) {
                        if (prevEmail != newEmailText) {
                            afm?.notifyValueChanged(this, VIRTUAL_ID_EMAIL, AutofillValue.forText(newEmailText))
                        }
                        if (prevPw != newPwText) {
                            afm?.notifyValueChanged(this, VIRTUAL_ID_PASSWORD, AutofillValue.forText(newPwText))
                        }
                    } else if (newMode == 2 && prevOtp != newOtpText) {
                        afm?.notifyValueChanged(this, VIRTUAL_ID_OTP, AutofillValue.forText(newOtpText))
                    }
                } catch (_: Throwable) {
                }
            }

            if (newActive != 0 && (newActive != prevActive || newMode != prevMode)) {
                val targetMode = newMode
                val targetField = newActive
                post {
                    if (mode != targetMode || activeField != targetField) return@post
                    suppressSdlDummyEditAutofill(window?.decorView)
                    try {
                        val loc = IntArray(2)
                        getLocationOnScreen(loc)
                        val r = Rect(rectForField(targetField))
                        r.offset(loc[0], loc[1])
                        afm?.notifyViewEntered(this, targetField, r)
                    } catch (t: Throwable) {
                        Log.w(TAG, "notifyViewEntered failed", t)
                    }
                }
            }
        }

        override fun onProvideAutofillVirtualStructure(structure: ViewStructure, flags: Int) {
            super.onProvideAutofillVirtualStructure(structure, flags)
            if (mode == 0) return
            val rootAutofillId = autofillId ?: return
            structure.setClassName("android.view.ViewGroup")

            if (mode == 1) {
                val idx = structure.addChildCount(2)
                structure.newChild(idx).apply {
                    setAutofillId(rootAutofillId, VIRTUAL_ID_EMAIL)
                    setAutofillType(AUTOFILL_TYPE_TEXT)
                    setAutofillHints(arrayOf(AUTOFILL_HINT_USERNAME, AUTOFILL_HINT_EMAIL_ADDRESS))
                    setInputType(InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS)
                    setClassName("android.widget.EditText")
                    setId(VIRTUAL_ID_EMAIL, context.packageName, "id", "discord_login_email")
                    setHint("Email or Phone Number")
                    setDimens(
                        emailRect.left, emailRect.top, 0, 0,
                        max(1, emailRect.width()), max(1, emailRect.height())
                    )
                    setVisibility(VISIBLE)
                    setEnabled(true)
                    setFocusable(true)
                    setFocused(activeField == VIRTUAL_ID_EMAIL)
                    setAutofillValue(AutofillValue.forText(emailText))
                }
                structure.newChild(idx + 1).apply {
                    setAutofillId(rootAutofillId, VIRTUAL_ID_PASSWORD)
                    setAutofillType(AUTOFILL_TYPE_TEXT)
                    setAutofillHints(arrayOf(AUTOFILL_HINT_PASSWORD))
                    setInputType(InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
                    setClassName("android.widget.EditText")
                    setId(VIRTUAL_ID_PASSWORD, context.packageName, "id", "discord_login_password")
                    setHint("Password")
                    setDimens(
                        pwRect.left, pwRect.top, 0, 0,
                        max(1, pwRect.width()), max(1, pwRect.height())
                    )
                    setVisibility(VISIBLE)
                    setEnabled(true)
                    setFocusable(true)
                    setFocused(activeField == VIRTUAL_ID_PASSWORD)
                    setAutofillValue(AutofillValue.forText(pwText))
                }
            } else if (mode == 2) {
                val idx = structure.addChildCount(1)
                structure.newChild(idx).apply {
                    setAutofillId(rootAutofillId, VIRTUAL_ID_OTP)
                    setAutofillType(AUTOFILL_TYPE_TEXT)
                    setAutofillHints(arrayOf("2faAppOTPCode", "smsOTPCode", "oneTimeCode"))
                    setInputType(InputType.TYPE_CLASS_NUMBER)
                    setClassName("android.widget.EditText")
                    setId(VIRTUAL_ID_OTP, context.packageName, "id", "discord_login_2fa_code")
                    setHint("6-digit authenticator code")
                    setDimens(
                        otpRect.left, otpRect.top, 0, 0,
                        max(1, otpRect.width()), max(1, otpRect.height())
                    )
                    setVisibility(VISIBLE)
                    setEnabled(true)
                    setFocusable(true)
                    setFocused(activeField == VIRTUAL_ID_OTP)
                    setAutofillValue(AutofillValue.forText(otpText))
                }
            }
        }

        override fun autofill(values: SparseArray<AutofillValue>) {
            val afm = context.getSystemService(AutofillManager::class.java)
            for (i in 0 until values.size()) {
                val id = values.keyAt(i)
                val v = values.valueAt(i) ?: continue
                if (!v.isText) continue
                val text = v.textValue.toString()
                when (id) {
                    VIRTUAL_ID_EMAIL -> emailText = text
                    VIRTUAL_ID_PASSWORD -> pwText = text
                    VIRTUAL_ID_OTP -> otpText = text
                    else -> continue
                }
                try {
                    afm?.notifyValueChanged(this, id, v)
                } catch (_: Throwable) {
                }
                if (CCordBridge.nativeReady) {
                    CCordBridge.nativeOnAutofillField(id, text)
                }
            }
        }
    }

    private fun extractCaBundleAsync() {
        val dst = File(filesDir, "cacert.pem")
        try {
            if (dst.exists() && dst.length() > 1024) return
        } catch (_: Throwable) {
        }
        val appCtx = applicationContext
        Thread({
            try {
                if (dst.exists() && dst.length() > 1024) return@Thread
                appCtx.assets.open("cacert.pem").use { input ->
                    val tmp = File(appCtx.filesDir, "cacert.pem.tmp")
                    FileOutputStream(tmp).use { out -> input.copyTo(out) }
                    if (!tmp.renameTo(dst)) {
                        tmp.copyTo(dst, overwrite = true)
                        tmp.delete()
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "Could not extract cacert.pem from assets", t)
            }
        }, "CCordCaExtract").apply {
            isDaemon = true
            start()
        }
    }

    private fun findSurfaceView(root: View?): SurfaceView? {
        if (root is SurfaceView) return root
        if (root is ViewGroup) {
            for (i in 0 until root.childCount) {
                val found = findSurfaceView(root.getChildAt(i))
                if (found != null) return found
            }
        }
        return null
    }

    private fun clearEdgePadding(root: View?) {
        if (root == null) return
        if (root.fitsSystemWindows) root.fitsSystemWindows = false
        if (root.paddingLeft != 0 || root.paddingTop != 0 || root.paddingRight != 0 || root.paddingBottom != 0) {
            root.setPadding(0, 0, 0, 0)
        }
        if (root is ViewGroup) {
            for (i in 0 until root.childCount) {
                val child = root.getChildAt(i)
                if (child !== autofillOverlay && child !== splashBackdrop) {
                    clearEdgePadding(child)
                }
            }
        }
    }

    private fun configureEdgeToEdge(fullscreen: Boolean) {
        try {
            WindowCompat.setDecorFitsSystemWindows(window, false)
            window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS)
            @Suppress("DEPRECATION")
            window.clearFlags(
                WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS or
                    WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION
            )
            @Suppress("DEPRECATION")
            window.statusBarColor = Color.TRANSPARENT
            @Suppress("DEPRECATION")
            window.navigationBarColor = Color.TRANSPARENT
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                window.isStatusBarContrastEnforced = false
                window.isNavigationBarContrastEnforced = false
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                val lp = window.attributes
                lp.layoutInDisplayCutoutMode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                } else {
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                }
                window.attributes = lp
            }
            val controller = WindowInsetsControllerCompat(window, window.decorView)
            controller.isAppearanceLightStatusBars = startupIsLight
            controller.isAppearanceLightNavigationBars = startupIsLight
            if (fullscreen) {
                controller.systemBarsBehavior =
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                controller.hide(WindowInsetsCompat.Type.systemBars())
            } else {
                @Suppress("DEPRECATION")
                window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
                controller.show(WindowInsetsCompat.Type.systemBars())
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
                    @Suppress("DEPRECATION")
                    window.decorView.systemUiVisibility =
                        View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                }
            }
            val content = findViewById<ViewGroup>(android.R.id.content)
            clearEdgePadding(content)
        } catch (t: Throwable) {
            Log.w(TAG, "configureEdgeToEdge failed", t)
        }
    }

    private fun dispatchInsets(insets: WindowInsetsCompat, force: Boolean) {
        lastInsets = insets
        val sysBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
        val cutout = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
        val ime = insets.getInsets(WindowInsetsCompat.Type.ime())

        var top = max(sysBars.top, cutout.top)
        var right = max(sysBars.right, cutout.right)
        var bottom = max(sysBars.bottom, cutout.bottom)
        var left = max(sysBars.left, cutout.left)
        var imeBottom = max(0, ime.bottom)

        // Express insets relative to SDL's SurfaceView so adjustResize, split-screen, or OEM window
        // positioning can never double-count system bar or keyboard offsets.
        val content = findViewById<ViewGroup>(android.R.id.content)
        val surface = findSurfaceView(content)
        val decor = window?.decorView
        if (surface != null && decor != null && surface.width > 0 && surface.height > 0 &&
            decor.width > 0 && decor.height > 0
        ) {
            val loc = IntArray(2)
            surface.getLocationInWindow(loc)
            val offLeft = max(0, loc[0])
            val offTop = max(0, loc[1])
            val offRight = max(0, decor.width - (loc[0] + surface.width))
            val offBottom = max(0, decor.height - (loc[1] + surface.height))
            top = max(0, top - offTop)
            left = max(0, left - offLeft)
            right = max(0, right - offRight)
            bottom = max(0, bottom - offBottom)
            imeBottom = max(0, imeBottom - offBottom)
        }

        val key = "$top:$right:$bottom:$left:$imeBottom"
        if (!force && key == lastDispatchedInsetsKey) return
        lastDispatchedInsetsKey = key
        if (CCordBridge.nativeReady) {
            CCordBridge.nativeOnInsetsChanged(
                top.toFloat(),
                right.toFloat(),
                bottom.toFloat(),
                left.toFloat(),
                imeBottom.toFloat()
            )
        }
    }

    private fun requestLatestInsets(force: Boolean) {
        val content = findViewById<ViewGroup>(android.R.id.content) ?: return
        val cur = ViewCompat.getRootWindowInsets(content) ?: lastInsets
        if (cur != null) {
            dispatchInsets(cur, force = force)
        }
        ViewCompat.requestApplyInsets(content)
    }

    private fun installInsetsListener() {
        val content = findViewById<ViewGroup>(android.R.id.content) ?: return
        clearEdgePadding(content)
        ViewCompat.setOnApplyWindowInsetsListener(content) { view, insets ->
            clearEdgePadding(view)
            dispatchInsets(insets, force = false)
            insets
        }
        content.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            val cur = ViewCompat.getRootWindowInsets(content) ?: lastInsets
            if (cur != null) dispatchInsets(cur, force = false)
        }
        ViewCompat.requestApplyInsets(content)
    }

    private fun registerNetworkCallback() {
        try {
            val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return
            connectivityManager = cm
            var seenInitial = false
            var wasOffline = cm.activeNetwork == null
            if (wasOffline && CCordBridge.nativeReady) {
                CCordBridge.nativeOnNetworkAvailable(false)
            }
            val cb = object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: Network) {
                    val cameBackOnline = wasOffline
                    wasOffline = false
                    if (!seenInitial) {
                        seenInitial = true
                        if (cameBackOnline && CCordBridge.nativeReady) {
                            CCordBridge.nativeOnNetworkAvailable(true)
                        }
                        return
                    }
                    if (CCordBridge.nativeReady) {
                        if (cameBackOnline) {
                            CCordBridge.nativeOnNetworkAvailable(true)
                        } else {
                            CCordBridge.nativeOnNetworkChanged()
                        }
                    }
                }

                override fun onLost(network: Network) {
                    // Only declare offline when no active default network remains; Wi-Fi <-> Cellular
                    // handoffs deliver onAvailable() for the replacement network right away.
                    val hasActive = try {
                        cm.activeNetwork != null
                    } catch (_: Throwable) {
                        false
                    }
                    if (!hasActive) {
                        wasOffline = true
                        if (CCordBridge.nativeReady) CCordBridge.nativeOnNetworkAvailable(false)
                    }
                }
            }
            networkCallback = cb
            cm.registerDefaultNetworkCallback(cb)
        } catch (t: Throwable) {
            Log.w(TAG, "registerDefaultNetworkCallback failed", t)
        }
    }

    private fun unregisterNetworkCallback() {
        val cm = connectivityManager
        val cb = networkCallback
        if (cm != null && cb != null) {
            try {
                cm.unregisterNetworkCallback(cb)
            } catch (_: Throwable) {
            }
        }
        networkCallback = null
        connectivityManager = null
    }

    private fun requestNotificationPermissionIfNeeded() {
        notifPermissionRequested = true
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val granted = ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) ==
                PackageManager.PERMISSION_GRANTED
            if (!granted) {
                try {
                    ActivityCompat.requestPermissions(
                        this,
                        arrayOf(Manifest.permission.POST_NOTIFICATIONS),
                        REQ_NOTIFICATIONS
                    )
                } catch (t: Throwable) {
                    Log.w(TAG, "requestPermissions(POST_NOTIFICATIONS) failed", t)
                }
            }
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<String>,
        grantResults: IntArray
    ) {
        if (requestCode == REQ_NOTIFICATIONS) {
            val granted = grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED
            Log.i(TAG, "POST_NOTIFICATIONS permission result: granted=$granted")
            return
        }
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
    }

    private fun handleNotificationIntent(intent: Intent?) {
        if (intent == null) return
        val id = intent.getIntExtra(CCordBridge.EXTRA_NOTIFY_ID, 0)
        val guildId = intent.getLongExtra(CCordBridge.EXTRA_GUILD_ID, 0L)
        val channelId = intent.getLongExtra(CCordBridge.EXTRA_CHANNEL_ID, 0L)
        val messageId = intent.getLongExtra(CCordBridge.EXTRA_MESSAGE_ID, 0L)
        if (id != 0 || guildId != 0L || channelId != 0L || messageId != 0L) {
            intent.removeExtra(CCordBridge.EXTRA_NOTIFY_ID)
            intent.removeExtra(CCordBridge.EXTRA_GUILD_ID)
            intent.removeExtra(CCordBridge.EXTRA_CHANNEL_ID)
            intent.removeExtra(CCordBridge.EXTRA_MESSAGE_ID)
            if (CCordBridge.nativeReady) {
                CCordBridge.nativeOnNotificationClicked(id, guildId, channelId, messageId)
            }
        }
    }

    // ---- SAF Document / File Picker ----

    fun launchFilePicker() {
        try {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            }
            @Suppress("DEPRECATION")
            startActivityForResult(intent, REQ_FILE_PICKER)
        } catch (t: Throwable) {
            Log.w(TAG, "launchFilePicker failed", t)
        }
    }

    private fun queryDisplayName(uri: Uri): String {
        var name = "attachment"
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0) {
                        val v = c.getString(idx)
                        if (!v.isNullOrBlank()) name = v
                    }
                }
            }
        } catch (_: Throwable) {
        }
        // Sanitize filename against path separators.
        return name.replace('/', '_').replace('\\', '_').ifBlank { "attachment" }
    }

    private fun copyPickedUrisToCache(uris: List<Uri>) {
        Thread({
            val dir = File(cacheDir, "picked").apply { mkdirs() }
            for (uri in uris) {
                try {
                    val rawName = queryDisplayName(uri)
                    val stamp = System.currentTimeMillis()
                    val out = File(dir, "${stamp}_$rawName")
                    contentResolver.openInputStream(uri)?.use { input ->
                        FileOutputStream(out).use { fos -> input.copyTo(fos) }
                    } ?: continue
                    if (out.exists() && CCordBridge.nativeReady) {
                        CCordBridge.nativeOnFilePicked(out.absolutePath)
                    }
                } catch (t: Throwable) {
                    Log.w(TAG, "Failed copying picked URI: $uri", t)
                }
            }
        }, "CCordFileCopy").start()
    }

    // ---- MediaProjection Screen Capture ----

    fun launchScreenCaptureRequest() {
        try {
            val mgr = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as? MediaProjectionManager
            if (mgr == null) {
                if (CCordBridge.nativeReady) CCordBridge.nativeOnCaptureStopped()
                return
            }
            @Suppress("DEPRECATION")
            startActivityForResult(mgr.createScreenCaptureIntent(), REQ_MEDIA_PROJECTION)
        } catch (t: Throwable) {
            Log.w(TAG, "launchScreenCaptureRequest failed", t)
            if (CCordBridge.nativeReady) CCordBridge.nativeOnCaptureStopped()
        }
    }

    private fun startMediaProjectionCapture(resultCode: Int, data: Intent) {
        stopScreenCaptureInternal(notifyNative = false)
        CCordVoiceService.updateScreenShareState(this, true) {
            runOnUiThread {
                beginVirtualDisplay(resultCode, data)
            }
        }
    }

    private fun beginVirtualDisplay(resultCode: Int, data: Intent) {
        try {
            val mgr = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as? MediaProjectionManager
            val proj = mgr?.getMediaProjection(resultCode, data)
            if (proj == null) {
                stopScreenCaptureInternal(notifyNative = true)
                return
            }
            mediaProjection = proj

            val dm = resources.displayMetrics
            var capW = dm.widthPixels
            var capH = dm.heightPixels
            // Scale down very high-DPI screens so software/HW encoding stays responsive.
            val maxDim = 1280
            if (capW > maxDim || capH > maxDim) {
                val scale = maxDim.toFloat() / max(capW, capH).toFloat()
                capW = max(2, ((capW * scale).toInt() and 1.inv()))
                capH = max(2, ((capH * scale).toInt() and 1.inv()))
            } else {
                capW = max(2, capW and 1.inv())
                capH = max(2, capH and 1.inv())
            }
            val densityDpi = dm.densityDpi

            val thread = HandlerThread("CCordScreenCapture").apply { start() }
            captureThread = thread
            val handler = Handler(thread.looper)
            captureHandler = handler

            proj.registerCallback(object : MediaProjection.Callback() {
                override fun onStop() {
                    runOnUiThread { stopScreenCaptureInternal(notifyNative = true) }
                }
            }, handler)

            val reader = ImageReader.newInstance(capW, capH, PixelFormat.RGBA_8888, 2)
            imageReader = reader
            captureLastFrameNs = 0L
            reader.setOnImageAvailableListener({ r ->
                val img = try {
                    r.acquireLatestImage()
                } catch (_: Throwable) {
                    null
                } ?: return@setOnImageAvailableListener
                try {
                    val nowNs = SystemClock.elapsedRealtimeNanos()
                    val minIntervalNs = captureMinIntervalNs
                    if (captureLastFrameNs != 0L && nowNs - captureLastFrameNs < minIntervalNs) {
                        return@setOnImageAvailableListener
                    }
                    captureLastFrameNs = nowNs

                    val plane = img.planes[0]
                    val srcBuf = plane.buffer
                    val pixelStride = plane.pixelStride
                    val rowStride = plane.rowStride
                    val w = img.width
                    val h = img.height
                    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || pixelStride < 4 || rowStride < w * pixelStride) {
                        return@setOnImageAvailableListener
                    }
                    srcBuf.rewind()
                    if (pixelStride == 4) {
                        val needed = (h - 1) * rowStride + w * 4
                        if (srcBuf.remaining() < needed) return@setOnImageAvailableListener
                        val totalBytes = rowStride * h
                        var dst = captureDirectBuf
                        if (dst == null || dst.capacity() < totalBytes) {
                            dst = ByteBuffer.allocateDirect(totalBytes)
                            captureDirectBuf = dst
                        }
                        dst.clear()
                        val copyLen = minOf(srcBuf.remaining(), totalBytes)
                        val savedLimit = srcBuf.limit()
                        srcBuf.limit(srcBuf.position() + copyLen)
                        dst.put(srcBuf)
                        srcBuf.limit(savedLimit)
                        dst.flip()
                        if (CCordBridge.nativeReady) {
                            CCordBridge.nativeOnCaptureFrame(dst, w, h, rowStride)
                        }
                    } else {
                        val tightStride = w * 4
                        val totalBytes = tightStride * h
                        var dst = captureDirectBuf
                        if (dst == null || dst.capacity() < totalBytes) {
                            dst = ByteBuffer.allocateDirect(totalBytes)
                            captureDirectBuf = dst
                        }
                        dst.clear()
                        val needSrc = (h - 1) * rowStride + (w - 1) * pixelStride + 4
                        if (srcBuf.remaining() < needSrc) return@setOnImageAvailableListener
                        val rowNeed = (w - 1) * pixelStride + 4
                        var inRow = captureRowBuf
                        if (inRow == null || inRow.size < rowNeed) {
                            inRow = ByteArray(rowNeed)
                            captureRowBuf = inRow
                        }
                        var outRow = captureOutRowBuf
                        if (outRow == null || outRow.size < tightStride) {
                            outRow = ByteArray(tightStride)
                            captureOutRowBuf = outRow
                        }
                        for (y in 0 until h) {
                            srcBuf.position(y * rowStride)
                            srcBuf.get(inRow, 0, rowNeed)
                            var srcOff = 0
                            var dstOff = 0
                            for (x in 0 until w) {
                                outRow[dstOff] = inRow[srcOff]
                                outRow[dstOff + 1] = inRow[srcOff + 1]
                                outRow[dstOff + 2] = inRow[srcOff + 2]
                                outRow[dstOff + 3] = inRow[srcOff + 3]
                                srcOff += pixelStride
                                dstOff += 4
                            }
                            dst.put(outRow, 0, tightStride)
                        }
                        dst.flip()
                        if (CCordBridge.nativeReady) {
                            CCordBridge.nativeOnCaptureFrame(dst, w, h, tightStride)
                        }
                    }
                } catch (t: Throwable) {
                    Log.w(TAG, "capture frame copy failed", t)
                } finally {
                    img.close()
                }
            }, handler)

            virtualDisplay = proj.createVirtualDisplay(
                "CCordScreenShare",
                capW,
                capH,
                densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                reader.surface,
                null,
                handler
            )
        } catch (t: Throwable) {
            Log.e(TAG, "beginVirtualDisplay failed", t)
            stopScreenCaptureInternal(notifyNative = true)
        }
    }

    fun stopScreenCaptureInternal(notifyNative: Boolean = false) {
        try {
            virtualDisplay?.release()
        } catch (_: Throwable) {
        }
        virtualDisplay = null

        try {
            imageReader?.setOnImageAvailableListener(null, null)
            imageReader?.close()
        } catch (_: Throwable) {
        }
        imageReader = null

        try {
            mediaProjection?.stop()
        } catch (_: Throwable) {
        }
        mediaProjection = null

        captureThread?.quitSafely()
        captureThread = null
        captureHandler = null
        captureDirectBuf = null
        captureRowBuf = null
        captureOutRowBuf = null
        captureLastFrameNs = 0L

        CCordVoiceService.updateScreenShareState(this, false)
        if (notifyNative && CCordBridge.nativeReady) {
            CCordBridge.nativeOnCaptureStopped()
        }
    }

    @Deprecated("Overrides SDLActivity.onActivityResult")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == REQ_FILE_PICKER) {
            if (resultCode == RESULT_OK && data != null) {
                val uris = mutableListOf<Uri>()
                val clip = data.clipData
                if (clip != null && clip.itemCount > 0) {
                    for (i in 0 until clip.itemCount) {
                        clip.getItemAt(i)?.uri?.let { uris.add(it) }
                    }
                } else {
                    data.data?.let { uris.add(it) }
                }
                if (uris.isNotEmpty()) copyPickedUrisToCache(uris)
            }
            return
        }
        if (requestCode == REQ_MEDIA_PROJECTION) {
            if (resultCode == RESULT_OK && data != null) {
                startMediaProjectionCapture(resultCode, data)
            } else {
                if (CCordBridge.nativeReady) CCordBridge.nativeOnCaptureStopped()
            }
            return
        }
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
    }
}
