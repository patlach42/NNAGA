/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 */
package com.vibes.dsp.ui.layout

import android.content.Context
import android.content.SharedPreferences
import android.content.pm.ActivityInfo
import android.os.Build
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext

sealed class DisplayOrientation(
    val persisted: String,
    val requestedOrientation: Int,
) {
    data object Portrait : DisplayOrientation("portrait", ActivityInfo.SCREEN_ORIENTATION_PORTRAIT)
    data object Landscape : DisplayOrientation("landscape", ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)
    data object ReverseLandscape : DisplayOrientation(
        "reverse_landscape",
        ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE,
    )

    companion object {
        fun fromPersisted(value: String?): DisplayOrientation = when (value) {
            Portrait.persisted -> Portrait
            Landscape.persisted -> Landscape
            ReverseLandscape.persisted -> ReverseLandscape
            else -> Portrait
        }
    }
}

object DisplayLayoutPreferences {
    private const val PREFERENCES_NAME = "display_layout"
    const val KEY_ORIENTATION = "orientation"
    const val KEY_USE_VERTICAL_CAMERA_STRIP_LANDSCAPE = "use_vertical_camera_strip_landscape"

    private fun preferences(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)

    fun getOrientation(context: Context): DisplayOrientation =
        DisplayOrientation.fromPersisted(preferences(context).getString(KEY_ORIENTATION, DisplayOrientation.Portrait.persisted))

    fun setOrientation(context: Context, orientation: DisplayOrientation) {
        preferences(context).edit().putString(KEY_ORIENTATION, orientation.persisted).apply()
    }

    fun getUseVerticalCameraStrip(context: Context): Boolean =
        preferences(context).getBoolean(KEY_USE_VERTICAL_CAMERA_STRIP_LANDSCAPE, false)

    fun setUseVerticalCameraStrip(context: Context, enabled: Boolean) {
        preferences(context).edit().putBoolean(KEY_USE_VERTICAL_CAMERA_STRIP_LANDSCAPE, enabled).apply()
    }

    fun registerListener(
        context: Context,
        listener: SharedPreferences.OnSharedPreferenceChangeListener,
    ) {
        preferences(context).registerOnSharedPreferenceChangeListener(listener)
    }

    fun unregisterListener(
        context: Context,
        listener: SharedPreferences.OnSharedPreferenceChangeListener,
    ) {
        preferences(context).unregisterOnSharedPreferenceChangeListener(listener)
    }
}

@Composable
fun rememberUseVerticalCameraStrip(): Boolean {
    val context = LocalContext.current
    var enabled by remember(context) {
        mutableStateOf(DisplayLayoutPreferences.getUseVerticalCameraStrip(context))
    }
    DisposableEffect(context) {
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == DisplayLayoutPreferences.KEY_USE_VERTICAL_CAMERA_STRIP_LANDSCAPE) {
                enabled = DisplayLayoutPreferences.getUseVerticalCameraStrip(context)
            }
        }
        DisplayLayoutPreferences.registerListener(context, listener)
        onDispose { DisplayLayoutPreferences.unregisterListener(context, listener) }
    }
    return enabled
}

object NnagaWindowPolicy {
    fun install(activity: ComponentActivity, fixedOrientation: DisplayOrientation? = null) {
        activity.enableEdgeToEdge()
        WindowCompat.setDecorFitsSystemWindows(activity.window, false)
        WindowInsetsControllerCompat(activity.window, activity.window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            activity.window.attributes = activity.window.attributes.apply {
                layoutInDisplayCutoutMode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                } else {
                    android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                }
            }
        }

        val preferenceListener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (fixedOrientation == null && key == DisplayLayoutPreferences.KEY_ORIENTATION) {
                applyOrientation(activity, DisplayLayoutPreferences.getOrientation(activity))
            }
        }
        val observer = object : DefaultLifecycleObserver {
            override fun onStart(owner: LifecycleOwner) {
                DisplayLayoutPreferences.registerListener(activity, preferenceListener)
                applyOrientation(activity, fixedOrientation ?: DisplayLayoutPreferences.getOrientation(activity))
            }

            override fun onStop(owner: LifecycleOwner) {
                DisplayLayoutPreferences.unregisterListener(activity, preferenceListener)
            }
        }
        activity.lifecycle.addObserver(observer)
    }

    private fun applyOrientation(activity: ComponentActivity, orientation: DisplayOrientation) {
        if (activity.requestedOrientation != orientation.requestedOrientation) {
            activity.requestedOrientation = orientation.requestedOrientation
        }
    }
}
