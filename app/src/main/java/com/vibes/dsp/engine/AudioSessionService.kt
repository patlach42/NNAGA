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
        runCatching { startForegroundCompat() }.onFailure {
            Log.w(TAG, "Audio foreground service could not start: ${it.message}")
            stopSelf()
        }
        // The session is owned by DirectUsbAudioManager, not by this service:
        // if the platform kills us, restarting an empty service would only
        // post a notification for audio that is not playing.
        return START_NOT_STICKY
    }

    private fun startForegroundCompat() {
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
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
            runCatching {
                val intent = Intent(context, AudioSessionService::class.java)
                context.startForegroundService(intent)
            }.onFailure { Log.w(TAG, "start failed: ${it.message}") }
        }

        fun stop(context: Context) {
            runCatching {
                context.stopService(Intent(context, AudioSessionService::class.java))
            }.onFailure { Log.w(TAG, "stop failed: ${it.message}") }
        }
    }
}
