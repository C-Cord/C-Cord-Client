// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
package org.ccord.android

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * Receives wakeup-capable AlarmManager broadcasts (ELAPSED_REALTIME_WAKEUP) to drive
 * Companion Wake WebSocket heartbeats and reconnect backoff across Android CPU suspend.
 * Acquires a short PARTIAL_WAKE_LOCK before returning so the SoC stays awake until
 * wake_thread completes its heartbeat ACK or reconnect and releases the lock.
 */
class CCordWakeAlarmReceiver : BroadcastReceiver() {
    companion object {
        const val ACTION_CLOUD_WAKE_ALARM = "org.ccord.android.action.CLOUD_WAKE_ALARM"
    }

    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != ACTION_CLOUD_WAKE_ALARM) return
        CCordBridge.attachAppContext(context)
        if (!CCordBridge.nativeReady) return
        CCordBridge.setCloudWakeLock(context, 20_000L)
        CCordBridge.nativeOnCloudWakeAlarm()
    }
}
