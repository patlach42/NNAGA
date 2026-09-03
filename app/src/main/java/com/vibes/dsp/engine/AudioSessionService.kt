/*
 * Copyright (C) 2026 patlach42
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

package com.vibes.dsp.engine

import android.app.ActivityManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import com.vibes.dsp.R

/**
 * Keeps the process in the foreground for as long as audio is running.
 *
 * Without this the platform is entitled to treat a backgrounded app as
 * cached: its threads lose CPU share, and on recent releases the whole
 * process can be frozen. A render loop with a fixed deadline does not
 * survive either. Declaring media playback tells the system this process is
 * doing continuous work a user is listening to.
 *
 * This is the one privilege that matters most and costs nothing: the
 * permissions are normal, granted at install, so it needs no root and no
 * prompt. The notification is required by the platform, not by us.
 */
class AudioSessionService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Once startForegroundService() has been accepted the platform demands
        // a startForeground() from here within a few seconds, and stopping
        // instead does NOT lift that obligation - it kills the process with
        // ForegroundServiceDidNotStartInTimeException. So every path through
        // this method has to end in a real startForeground() call.
        //
        // The typed call is the one we want, but it is also the one the system
        // can refuse: on recent releases a media-playback type is checked
        // against whether the app is currently allowed to hold one. An untyped
        // notification keeps the process out of the cached state just as well,
        // so it is tried next rather than giving up.
        val promoted = runCatching { startForegroundCompat(typed = true) }.isSuccess ||
            runCatching { startForegroundCompat(typed = false) }.isSuccess
        if (!promoted) {
            // Nothing left to try. Stopping now may still cost us the process,
            // but staying started without a notification certainly would.
            Log.w(TAG, "Audio foreground service could not be promoted")
            stopSelf()
        }
        // The session is owned by DirectUsbAudioManager, not by this service:
        // if the platform kills us, restarting an empty service would only
        // post a notification for audio that is not playing.
        return START_NOT_STICKY
    }

    private fun startForegroundCompat(typed: Boolean) {
        val manager = getSystemService(NotificationManager::class.java)
        if (manager?.getNotificationChannel(CHANNEL_ID) == null) {
            manager?.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    "Audio engine",
                    NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = "Shown while the audio engine is running"
                    setShowBadge(false)
                }
            )
        }
        val notification: Notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("NNAGA audio engine")
            .setContentText("Running")
            .setSmallIcon(R.mipmap.ic_launcher)
            .setOngoing(true)
            .build()
        if (typed && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    companion object {
        private const val TAG = "AudioSessionService"
        private const val CHANNEL_ID = "nnaga.audio.engine"
        private const val NOTIFICATION_ID = 0x4E4E41

        /** Failures are logged, never fatal: audio must still run without it. */
        fun start(context: Context) {
            // A background process is not allowed to start a foreground
            // service, and asking anyway is worse than not asking: the call is
            // what creates the obligation the platform later kills us over.
            // Checking our own importance first keeps that out of the picture
            // when the engine is started from a test or from a receiver.
            if (!canStartForegroundService(context)) {
                Log.i(TAG, "not in the foreground; leaving the process unpromoted")
                return
            }
            runCatching {
                val intent = Intent(context, AudioSessionService::class.java)
                context.startForegroundService(intent)
            }.onFailure { Log.w(TAG, "start failed: ${it.message}") }
        }

        private fun canStartForegroundService(context: Context): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true
            return runCatching {
                val state = ActivityManager.RunningAppProcessInfo()
                ActivityManager.getMyMemoryState(state)
                state.importance <=
                    ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND_SERVICE
            }.getOrDefault(true)
        }

        fun stop(context: Context) {
            runCatching {
                context.stopService(Intent(context, AudioSessionService::class.java))
            }.onFailure { Log.w(TAG, "stop failed: ${it.message}") }
        }
    }
}
