/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

package com.vibes.dsp.ui.dashboard

import android.content.Context
import android.os.Build
import android.view.View
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView

/** Pixel bounds of a centered camera cutout at the top of the display. */
data class TopCutoutBounds(
    val left: Int = 0,
    val right: Int = 0,
    val bottom: Int = 0,
) {
    val present: Boolean get() = right > left && bottom > 0

    internal fun isValidFor(screenWidth: Int): Boolean =
        present && left >= 0 && right <= screenWidth && left < screenWidth / 2 && right > screenWidth / 2
}

private const val CutoutPreferences = "top_cutout_bounds"

private fun cutoutCacheKey(context: Context, view: View): String {
    val metrics = context.resources.displayMetrics
    val configuration = context.resources.configuration
    return listOf(
        Build.MANUFACTURER,
        Build.MODEL,
        view.display?.displayId ?: 0,
        metrics.widthPixels,
        metrics.heightPixels,
        metrics.densityDpi,
        configuration.orientation,
    ).joinToString(separator = "_")
}

private fun readCachedCutout(context: Context, key: String, screenWidth: Int): TopCutoutBounds {
    val preferences = context.getSharedPreferences(CutoutPreferences, Context.MODE_PRIVATE)
    val bounds = TopCutoutBounds(
        left = preferences.getInt("${key}_left", 0),
        right = preferences.getInt("${key}_right", 0),
        bottom = preferences.getInt("${key}_bottom", 0),
    )
    return bounds.takeIf { it.isValidFor(screenWidth) } ?: TopCutoutBounds()
}

private fun cacheCutout(context: Context, key: String, bounds: TopCutoutBounds) {
    context.getSharedPreferences(CutoutPreferences, Context.MODE_PRIVATE)
        .edit()
        .putInt("${key}_left", bounds.left)
        .putInt("${key}_right", bounds.right)
        .putInt("${key}_bottom", bounds.bottom)
        .apply()
}

private fun clearCachedCutout(context: Context, key: String) {
    context.getSharedPreferences(CutoutPreferences, Context.MODE_PRIVATE)
        .edit()
        .remove("${key}_left")
        .remove("${key}_right")
        .remove("${key}_bottom")
        .apply()
}

/**
 * Returns cached geometry on the first composition, then replaces it with authoritative runtime
 * insets as soon as the window supplies them. The cache key includes the physical display and its
 * current pixel configuration so bounds are never reused for another screen or orientation.
 */
@Composable
fun rememberTopCutoutBounds(): TopCutoutBounds {
    val context = LocalContext.current
    val view = LocalView.current
    val screenWidth = context.resources.displayMetrics.widthPixels
    val cacheKey = remember(context, view, screenWidth) { cutoutCacheKey(context, view) }
    var bounds by remember(cacheKey) {
        mutableStateOf(readCachedCutout(context, cacheKey, screenWidth))
    }

    DisposableEffect(view, cacheKey, screenWidth) {
        fun updateFromRuntimeInsets() {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
                bounds = TopCutoutBounds()
                clearCachedCutout(context, cacheKey)
                return
            }
            val insets = view.rootWindowInsets ?: return
            val center = screenWidth / 2
            val centeredTopRects = insets.displayCutout
                ?.boundingRects
                .orEmpty()
                .filter { rect ->
                    rect.top <= 1 && rect.width() > 0 && center >= rect.left && center <= rect.right
                }
            val runtimeBounds = if (centeredTopRects.isEmpty()) {
                TopCutoutBounds()
            } else {
                TopCutoutBounds(
                    left = centeredTopRects.minOf { it.left },
                    right = centeredTopRects.maxOf { it.right },
                    bottom = centeredTopRects.maxOf { it.bottom },
                )
            }
            val resolvedBounds =
                runtimeBounds.takeIf { it.isValidFor(screenWidth) } ?: TopCutoutBounds()
            if (resolvedBounds != bounds) {
                bounds = resolvedBounds
                if (resolvedBounds.present) {
                    cacheCutout(context, cacheKey, resolvedBounds)
                } else {
                    clearCachedCutout(context, cacheKey)
                }
            }
        }

        val layoutListener = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            updateFromRuntimeInsets()
        }
        val deferredUpdate = Runnable { updateFromRuntimeInsets() }
        view.addOnLayoutChangeListener(layoutListener)
        updateFromRuntimeInsets()
        view.requestApplyInsets()
        view.post(deferredUpdate)
        onDispose {
            view.removeOnLayoutChangeListener(layoutListener)
            view.removeCallbacks(deferredUpdate)
        }
    }

    return bounds
}
