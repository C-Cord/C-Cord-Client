// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
package org.ccord.android

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
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
import android.provider.OpenableColumns
import android.text.InputType
import android.util.Log
import android.util.SparseArray
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.ViewStructure
import android.view.WindowManager
import android.view.autofill.AutofillManager
import android.view.autofill.AutofillValue
import android.widget.FrameLayout
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
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
        private const val REQ_FILE_PICKER = 9101
        private const val REQ_MEDIA_PROJECTION = 9102
        private const val REQ_NOTIFICATIONS = 9103
    }

    private var connectivityManager: ConnectivityManager? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    // Startup backdrop overlay: hides SDL's uninitialized black SurfaceView hole until frame 1 presents.
    private var splashBackdrop: View? = null
    private val mainHandler = Handler(Looper.getMainLooper())
    private val splashTimeoutRunnable = Runnable { dismissSplashBackdrop() }

    // Virtual Autofill overlay so Android password managers (Bitwarden, etc.) recognize email, password, and 2FA fields.
    private var autofillOverlay: CCordAutofillOverlay? = null

    // MediaProjection screen capture state
    private var mediaProjection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var imageReader: ImageReader? = null
    private var captureThread: HandlerThread? = null
    private var captureHandler: Handler? = null
    private var captureDirectBuf: ByteBuffer? = null

    override fun getLibraries(): Array<String> = arrayOf(
        "c++_shared",
        "SDL3",
        "main"
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        // Extract bundled Mozilla CA roots BEFORE super.onCreate() so SSL_CERT_FILE is ready
        // before SDLThread starts SDL_main().
        extractCaBundle()

        super.onCreate(savedInstanceState)
        if (mBrokenLibraries) return

        installSplashBackdrop()
        installAutofillOverlay()
        CCordBridge.attach(this)
        configureEdgeToEdge()
        installInsetsListener()
        registerNetworkCallback()
        requestNotificationPermissionIfNeeded()
        handleNotificationIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (!mBrokenLibraries) {
            handleNotificationIntent(intent)
        }
    }

    override fun onDestroy() {
        mainHandler.removeCallbacks(splashTimeoutRunnable)
        dismissSplashBackdrop()
        stopScreenCaptureInternal()
        unregisterNetworkCallback()
        CCordBridge.detach(this)
        super.onDestroy()
    }

    private fun installSplashBackdrop() {
        try {
            val content = findViewById<ViewGroup>(android.R.id.content) ?: return
            val backdrop = View(this).apply {
                setBackgroundColor(0xFF1E1F22.toInt())
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
            mainHandler.postDelayed(splashTimeoutRunnable, 1500L)
        } catch (t: Throwable) {
            Log.w(TAG, "installSplashBackdrop failed", t)
        }
    }

    fun dismissSplashBackdrop() {
        mainHandler.removeCallbacks(splashTimeoutRunnable)
        val v = splashBackdrop ?: return
        splashBackdrop = null
        try {
            (v.parent as? ViewGroup)?.removeView(v)
        } catch (_: Throwable) {
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

    private fun extractCaBundle() {
        val dst = File(filesDir, "cacert.pem")
        try {
            if (dst.exists() && dst.length() > 1024) return
            assets.open("cacert.pem").use { input ->
                val tmp = File(filesDir, "cacert.pem.tmp")
                FileOutputStream(tmp).use { out -> input.copyTo(out) }
                if (!tmp.renameTo(dst)) {
                    tmp.copyTo(dst, overwrite = true)
                    tmp.delete()
                }
            }
        } catch (t: Throwable) {
            Log.w(TAG, "Could not extract cacert.pem from assets", t)
        }
    }

    private fun configureEdgeToEdge() {
        try {
            WindowCompat.setDecorFitsSystemWindows(window, false)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                val lp = window.attributes
                lp.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                window.attributes = lp
            }
            val controller = WindowInsetsControllerCompat(window, window.decorView)
            controller.isAppearanceLightStatusBars = false
            controller.isAppearanceLightNavigationBars = false
        } catch (t: Throwable) {
            Log.w(TAG, "configureEdgeToEdge failed", t)
        }
    }

    private fun installInsetsListener() {
        val root = window.decorView
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val sysBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutout = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
            val ime = insets.getInsets(WindowInsetsCompat.Type.ime())

            val top = max(sysBars.top, cutout.top).toFloat()
            val right = max(sysBars.right, cutout.right).toFloat()
            val bottom = max(sysBars.bottom, cutout.bottom).toFloat()
            val left = max(sysBars.left, cutout.left).toFloat()
            // IME bottom includes navigation bar on Android; report full keyboard occlusion height.
            val imeBottom = max(0, ime.bottom).toFloat()

            if (CCordBridge.nativeReady) {
                CCordBridge.nativeOnInsetsChanged(top, right, bottom, left, imeBottom)
            }
            ViewCompat.onApplyWindowInsets(view, insets)
        }
        ViewCompat.requestApplyInsets(root)
    }

    private fun registerNetworkCallback() {
        try {
            val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return
            connectivityManager = cm
            var seenInitial = false
            val cb = object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: Network) {
                    if (!seenInitial) {
                        seenInitial = true
                        return
                    }
                    if (CCordBridge.nativeReady) CCordBridge.nativeOnNetworkChanged()
                }

                override fun onLost(network: Network) {
                    if (CCordBridge.nativeReady) CCordBridge.nativeOnNetworkChanged()
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
            reader.setOnImageAvailableListener({ r ->
                val img = try {
                    r.acquireLatestImage()
                } catch (_: Throwable) {
                    null
                } ?: return@setOnImageAvailableListener
                try {
                    val plane = img.planes[0]
                    val srcBuf = plane.buffer
                    val rowStride = plane.rowStride
                    val w = img.width
                    val h = img.height
                    val totalBytes = rowStride * h
                    var dst = captureDirectBuf
                    if (dst == null || dst.capacity() < totalBytes) {
                        dst = ByteBuffer.allocateDirect(totalBytes)
                        captureDirectBuf = dst
                    }
                    dst.clear()
                    srcBuf.rewind()
                    val copyLen = minOf(srcBuf.remaining(), totalBytes)
                    val savedLimit = srcBuf.limit()
                    srcBuf.limit(srcBuf.position() + copyLen)
                    dst.put(srcBuf)
                    srcBuf.limit(savedLimit)
                    dst.flip()
                    if (CCordBridge.nativeReady) {
                        CCordBridge.nativeOnCaptureFrame(dst, w, h, rowStride)
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
