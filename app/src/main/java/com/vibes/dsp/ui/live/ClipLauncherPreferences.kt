package com.vibes.dsp.ui.live

import android.content.Context
import android.net.Uri

object ClipLauncherPreferences {
    private const val PREFERENCES_NAME = "clip_launcher"
    private const val KEY_COPY_CLIPS_INTO_PROJECT = "copy_clips_into_project"
    private const val KEY_AUTO_DETECT_BPM_FROM_FILENAME = "auto_detect_bpm_from_filename"
    private const val KEY_AUTO_DETECT_LOOP_TEMPO = "auto_detect_loop_tempo"
    private const val KEY_BROWSER_ROOT_URI = "browser_root_uri"

    fun getCopyClipsIntoProject(context: Context): Boolean =
        preferences(context).getBoolean(KEY_COPY_CLIPS_INTO_PROJECT, false)

    fun setCopyClipsIntoProject(context: Context, enabled: Boolean) {
        preferences(context).edit().putBoolean(KEY_COPY_CLIPS_INTO_PROJECT, enabled).apply()
    }

    fun getAutoDetectBpmFromFilename(context: Context): Boolean =
        preferences(context).getBoolean(KEY_AUTO_DETECT_BPM_FROM_FILENAME, true)

    fun setAutoDetectBpmFromFilename(context: Context, enabled: Boolean) {
        preferences(context).edit().putBoolean(KEY_AUTO_DETECT_BPM_FROM_FILENAME, enabled).apply()
    }

    fun getAutoDetectLoopTempo(context: Context): Boolean =
        preferences(context).getBoolean(KEY_AUTO_DETECT_LOOP_TEMPO, true)

    fun setAutoDetectLoopTempo(context: Context, enabled: Boolean) {
        preferences(context).edit().putBoolean(KEY_AUTO_DETECT_LOOP_TEMPO, enabled).apply()
    }

    fun getBrowserRootUri(context: Context): Uri? =
        runCatching { preferences(context).getString(KEY_BROWSER_ROOT_URI, null) }
            .getOrNull()
            ?.let { raw -> runCatching { Uri.parse(raw) }.getOrNull() }

    fun setBrowserRootUri(context: Context, uri: Uri?) {
        val prefs = preferences(context).edit()
        if (uri == null) {
            prefs.remove(KEY_BROWSER_ROOT_URI)
        } else {
            prefs.putString(KEY_BROWSER_ROOT_URI, uri.toString())
        }
        prefs.apply()
    }

    private fun preferences(context: Context) =
        context.applicationContext.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
}
