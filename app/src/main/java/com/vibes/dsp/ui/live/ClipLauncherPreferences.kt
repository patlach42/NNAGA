package com.vibes.dsp.ui.live

import android.content.Context

object ClipLauncherPreferences {
    private const val PREFERENCES_NAME = "clip_launcher"
    private const val KEY_AUTO_DETECT_BPM_FROM_FILENAME = "auto_detect_bpm_from_filename"

    fun getAutoDetectBpmFromFilename(context: Context): Boolean =
        preferences(context).getBoolean(KEY_AUTO_DETECT_BPM_FROM_FILENAME, true)

    fun setAutoDetectBpmFromFilename(context: Context, enabled: Boolean) {
        preferences(context).edit().putBoolean(KEY_AUTO_DETECT_BPM_FROM_FILENAME, enabled).apply()
    }

    private fun preferences(context: Context) =
        context.applicationContext.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
}
