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
package com.vibes.dsp.ui.theme

import android.app.Activity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.compositeOver
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.runtime.collectAsState
import androidx.core.view.WindowCompat

private fun amoledColorScheme(accentArgb: Int) : androidx.compose.material3.ColorScheme {
    val accent = Color(accentArgb)
    val surface = Color(0xFF090909)
    val accentContainer = accent.copy(alpha = 0.28f).compositeOver(surface)
    val onAccent = Color(AppearancePreferences.contentArgbForAccent(accentArgb))
    return darkColorScheme(
        primary = accent,
        onPrimary = onAccent,
        primaryContainer = accentContainer,
        onPrimaryContainer = Color(AppearancePreferences.contentArgbForAccent(accentContainer.toArgb())),
        inversePrimary = accent,
        secondary = accent,
        onSecondary = onAccent,
        secondaryContainer = accent.copy(alpha = 0.20f).compositeOver(surface),
        onSecondaryContainer = Color(0xFFF4F4F4),
        tertiary = Color(0xFFB8C7D9),
        onTertiary = Color(0xFF1A2028),
        tertiaryContainer = Color(0xFF29313A),
        onTertiaryContainer = Color(0xFFD9E5F2),
        background = Color.Black,
        onBackground = Color(0xFFF5F5F5),
        surface = surface,
        onSurface = Color(0xFFF5F5F5),
        surfaceVariant = Color(0xFF121212),
        onSurfaceVariant = Color(0xFFC5C5C5),
        outline = Color(0xFF8C8C8C),
        outlineVariant = Color(0xFF414141),
        inverseSurface = Color(0xFFE5E5E5),
        inverseOnSurface = Color(0xFF202020),
        error = Color(0xFFFFB4AB),
        onError = Color(0xFF690005),
        errorContainer = Color(0xFF93000A),
        onErrorContainer = Color(0xFFFFDAD6)
    )
}

@Composable
fun NNAGATheme(
    @Suppress("UNUSED_PARAMETER") darkTheme: Boolean = true,
    content: @Composable () -> Unit
) {
    val context = LocalContext.current
    AppearancePreferences.initialize(context)
    val accentArgb by AppearancePreferences.accentArgb.collectAsState()
    val colorScheme = remember(accentArgb) { amoledColorScheme(accentArgb) }
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = Color.Black.toArgb()
            window.navigationBarColor = Color.Black.toArgb()
            WindowCompat.getInsetsController(window, view).apply {
                isAppearanceLightStatusBars = false
                isAppearanceLightNavigationBars = false
            }
        }
    }
    MaterialTheme(
        colorScheme = colorScheme,
        typography = androidx.compose.material3.Typography(),
        content = content
    )
}
